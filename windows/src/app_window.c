/* app_window.c - main window: layout and size calculation, status line,
 * keyboard, drag & drop, clipboard flow, tray, settings application
 * (PLAN.md §6.1, §6.3, §6.8). */
#include "app.h"
#include "app_window.h"
#include "clipboard.h"
#include "compat.h"
#include "dbglog.h"
#include "hotkey.h"
#include "pacer.h"
#include "producer.h"
#include "qr_geom.h"
#include "snip.h"
#include "tray.h"
#include "util.h"
#include <commctrl.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdlib.h>

app_ctx g_app;

#define MAIN_STYLE    (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN)
#define MAIN_EX_STYLE (WS_EX_CONTROLPARENT)

static struct {
    job_status job;              /* last status from the producer          */
    BOOL       has_cur;
    content    cur;              /* identity of what is being sent (no data) */
    wchar_t    cur_name[80];     /* single dropped file: its name          */
    content   *pending;          /* large content waiting for Enter        */
    wchar_t    note[200];
    wchar_t    hotkey_active[48];
    BOOL       clamped;
    DWORD      last_seq;
    BOOL       restore_after_snip;
    int        status_h;
    RECT       status_rc;
    BOOL       quitting;
} A;

/* ---- helpers ------------------------------------------------------------ */

static job_params params_from_cfg(void)
{
    job_params p;
    const app_settings *s = &g_app.cfg;
    p.proto = s->protocol;
    p.version = s->version;
    p.ecc = s->ecc;
    p.image_format = s->image_format;
    p.jpeg_quality = s->jpeg_quality;
    p.count = s->code_count;
    p.quiet = s->quiet_zone;
    p.fps = s->fps;
    return p;
}

static void invalidate_status(void)
{
    if (g_app.hwnd)
        InvalidateRect(g_app.hwnd, &A.status_rc, FALSE);
}

void app_note(const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(A.note, CPW_COUNTOF(A.note) - 1, fmt, ap);
    va_end(ap);
    A.note[CPW_COUNTOF(A.note) - 1] = 0;
    SetTimer(g_app.hwnd, IDT_NOTE, 6000, NULL);
    invalidate_status();
}

static void fmt_eta(double sec, wchar_t *b, size_t cap)
{
    if (sec < 10)
        wfmt(b, cap, L"≈%.1fs", sec);
    else if (sec < 60)
        wfmt(b, cap, L"≈%.0fs", sec);
    else if (sec < 3600)
        wfmt(b, cap, L"≈%d分%02d秒", (int)sec / 60, (int)sec % 60);
    else
        wfmt(b, cap, L"≈%d时%02d分", (int)(sec / 3600), (int)sec / 60 % 60);
}

static void fmt_fps(wchar_t *b, size_t cap)
{
    int f = pacer_effective_fps_x10();
    if (f % 10 == 0)
        wfmt(b, cap, L"%d", f / 10);
    else
        wfmt(b, cap, L"%d.%d", f / 10, f % 10);
}

static const wchar_t *kind_name(int kind)
{
    switch (kind) {
    case CP_KIND_TEXT: return L"文本";
    case CP_KIND_IMAGE: return L"图片";
    case CP_KIND_FILES: return L"文件";
    }
    return L"内容";
}

static void build_status(wchar_t *out, size_t cap)
{
    wchar_t a[32], b[32], eta[32], fps[16], tail[256] = L"";
    const job_status *j = &A.job;
    const app_settings *s = &g_app.cfg;

    if (A.pending) {
        size_t T = qr_byte_capacity(s->version, s->ecc) - CP_FRAME_OVERHEAD;
        double k = (double)A.pending->raw_size / (double)(T ? T : 1);
        fmt_size(A.pending->raw_size, a, CPW_COUNTOF(a));
        fmt_eta(k / (s->code_count * (pacer_effective_fps_x10() / 10.0)), eta, CPW_COUNTOF(eta));
        wfmt(out, cap, L"%ls %ls 较大,预计 %ls · 按 Enter 开始发送", kind_name(content_kind(A.pending)), a, eta);
    } else if (!A.has_cur || j->state == JOB_IDLE) {
        const wchar_t *snip = L"Win+Shift+S 截图";
        wchar_t sb[80];
        if (hotkey_builtin_wanted(s)) {
            if (A.hotkey_active[0]) {
                wfmt(sb, CPW_COUNTOF(sb), L"%ls 截图", A.hotkey_active);
                snip = sb;
            } else {
                snip = L"托盘菜单截图";
            }
        }
        wfmt(out, cap, L"等待剪切板… · %ls · 拖入文件", snip);
    } else if (j->state == JOB_PREPARING) {
        fmt_size(j->estimate, a, CPW_COUNTOF(a));
        wfmt(out, cap, L"正在准备%ls %ls…", kind_name(j->kind), a);
    } else if (j->state == JOB_ERROR) {
        wfmt(out, cap, L"⚠ %ls", j->err);
    } else {
        double need = (double)j->tinfo.total_source_symbols + 2.0 * j->tinfo.Z;
        double rate = s->code_count * (pacer_effective_fps_x10() / 10.0);
        wchar_t what[160];
        fmt_size(j->pinfo.raw_len, a, CPW_COUNTOF(a));
        fmt_size(j->pinfo.container_len, b, CPW_COUNTOF(b));
        fmt_eta(need / (rate > 0 ? rate : 1), eta, CPW_COUNTOF(eta));
        fmt_fps(fps, CPW_COUNTOF(fps));
        switch (j->kind) {
        case CP_KIND_TEXT:
            if (j->pinfo.compressed)
                wfmt(what, CPW_COUNTOF(what), L"文本 %ls→%ls", a, b);
            else
                wfmt(what, CPW_COUNTOF(what), L"文本 %ls", a);
            break;
        case CP_KIND_IMAGE:
            wfmt(what, CPW_COUNTOF(what), L"图片 %ls %ls · K=%u", !strcmp(j->mime, "image/jpeg") ? L"JPEG" : L"PNG",
                 a, j->tinfo.total_source_symbols);
            break;
        default:
            if (j->nfiles == 1 && A.cur_name[0])
                wfmt(what, CPW_COUNTOF(what), L"%ls %ls%ls%ls · K=%u", A.cur_name, a, j->pinfo.compressed ? L"→" : L"",
                     j->pinfo.compressed ? b : L"", j->tinfo.total_source_symbols);
            else
                wfmt(what, CPW_COUNTOF(what), L"文件 %u个 %ls%ls%ls · K=%u", (unsigned)j->nfiles, a,
                     j->pinfo.compressed ? L"→" : L"", j->pinfo.compressed ? b : L"", j->tinfo.total_source_symbols);
            break;
        }
        wfmt(out, cap, L"%ls · %d码×%lsfps · %ls%ls", what, s->code_count, fps, eta,
             pacer_paused() ? L" · 已暂停(空格继续)" : L"");
    }
    if (s->pause_monitor)
        wcsncat(tail, L" · 监听已暂停", CPW_COUNTOF(tail) - wcslen(tail) - 1);
    if (A.clamped)
        wcsncat(tail, L" · 超出屏幕,请减小模块像素或码数", CPW_COUNTOF(tail) - wcslen(tail) - 1);
    if (A.note[0]) {
        wcsncat(tail, L" · ", CPW_COUNTOF(tail) - wcslen(tail) - 1);
        wcsncat(tail, A.note, CPW_COUNTOF(tail) - wcslen(tail) - 1);
    }
    wcsncat(out, tail, cap - wcslen(out) - 1);
}

/* ---- fonts / layout ---------------------------------------------------- */

static void make_fonts(void)
{
    LOGFONTW lf;
    if (g_app.font)
        DeleteObject(g_app.font);
    if (g_app.gear_font)
        DeleteObject(g_app.gear_font);
    dpi_message_font(g_app.dpi, &lf);
    g_app.font = CreateFontIndirectW(&lf);
    lf.lfHeight = lf.lfHeight * 5 / 4;
    lstrcpyW(lf.lfFaceName, L"Segoe UI Symbol");
    g_app.gear_font = CreateFontIndirectW(&lf);
    if (g_app.gear)
        SendMessageW(g_app.gear, WM_SETFONT, (WPARAM)g_app.gear_font, TRUE);
    {
        HDC dc = GetDC(g_app.hwnd);
        HGDIOBJ of = SelectObject(dc, g_app.font);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, of);
        ReleaseDC(g_app.hwnd, dc);
        A.status_h = tm.tmHeight + dpi_scale(12, g_app.dpi);
    }
}

static SIZE s_bar_size;

/* positions the children inside the current client rect */
static void layout_children(void)
{
    RECT rc;
    int cw, ch, bar_h, view_h, gs;
    GetClientRect(g_app.hwnd, &rc);
    cw = rc.right;
    ch = rc.bottom;
    bar_h = g_app.cfg.settings_expanded ? s_bar_size.cy : 0;
    view_h = CPW_MAX(0, ch - A.status_h - bar_h);
    MoveWindow(g_app.view, 0, 0, cw, view_h, TRUE);
    gs = A.status_h - dpi_scale(6, g_app.dpi);
    MoveWindow(g_app.gear, dpi_scale(4, g_app.dpi), view_h + (A.status_h - gs) / 2, gs, gs, TRUE);
    SetRect(&A.status_rc, 0, view_h, cw, view_h + A.status_h);
    if (g_app.cfg.settings_expanded) {
        MoveWindow(g_app.bar, 0, view_h + A.status_h, cw, bar_h, TRUE);
        ShowWindow(g_app.bar, SW_SHOWNA);
    } else {
        ShowWindow(g_app.bar, SW_HIDE);
    }
    InvalidateRect(g_app.hwnd, NULL, FALSE);
}

/* Window size follows from the settings (not user-resizable). */
static void app_layout(const RECT *suggested)
{
    const app_settings *s = &g_app.cfg;
    frame_geom g;
    RECT r, wr;
    MONITORINFO mi;
    int cw, ch, ww, wh, x, y, minw;
    BOOL clamped = FALSE;

    frame_geom_calc(&g, s->version, s->code_count, s->quiet_zone);
    s_bar_size = settings_bar_layout();
    minw = dpi_scale(380, g_app.dpi);
    cw = CPW_MAX(g.w * s->module_px, minw);
    if (s->settings_expanded)
        cw = CPW_MAX(cw, s_bar_size.cx);
    ch = g.h * s->module_px + A.status_h + (s->settings_expanded ? s_bar_size.cy : 0);
    SetRect(&r, 0, 0, cw, ch);
    dpi_adjust_window_rect(&r, MAIN_STYLE, MAIN_EX_STYLE | (s->topmost ? WS_EX_TOPMOST : 0), g_app.dpi);
    ww = r.right - r.left;
    wh = r.bottom - r.top;

    if (suggested)
        wr = *suggested;
    else
        GetWindowRect(g_app.hwnd, &wr);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST), &mi);
    if (ww > mi.rcWork.right - mi.rcWork.left) {
        ww = mi.rcWork.right - mi.rcWork.left;
        clamped = TRUE;
    }
    if (wh > mi.rcWork.bottom - mi.rcWork.top) {
        wh = mi.rcWork.bottom - mi.rcWork.top;
        clamped = TRUE;
    }
    x = CPW_CLAMP(wr.left, mi.rcWork.left, mi.rcWork.right - ww);
    y = CPW_CLAMP(wr.top, mi.rcWork.top, mi.rcWork.bottom - wh);
    A.clamped = clamped;
    SetWindowPos(g_app.hwnd, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    layout_children();
}

/* ---- show / hide ------------------------------------------------------- */

static void save_window_pos(void)
{
    RECT r;
    if (!IsWindowVisible(g_app.hwnd) || IsIconic(g_app.hwnd))
        return;
    GetWindowRect(g_app.hwnd, &r);
    g_app.cfg.has_window_pos = TRUE;
    g_app.cfg.window_x = r.left;
    g_app.cfg.window_y = r.top;
    config_save(&g_app.cfg);
}

void app_show(BOOL activate)
{
    if (activate) {
        ShowWindow(g_app.hwnd, IsIconic(g_app.hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_app.hwnd);
        SetFocus(g_app.hwnd);
        return;
    }
    /* pop up without stealing the keyboard focus (§6.8) */
    ShowWindow(g_app.hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(g_app.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (!g_app.cfg.topmost)
        SetWindowPos(g_app.hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void app_hide(void)
{
    save_window_pos();
    ShowWindow(g_app.hwnd, SW_HIDE);
}

void app_toggle_settings(void)
{
    app_settings old = g_app.cfg;
    g_app.cfg.settings_expanded = !g_app.cfg.settings_expanded;
    app_settings_changed(&old);
    SetFocus(g_app.hwnd);
}

void app_exit(void)
{
    A.quitting = TRUE;
    save_window_pos();
    DestroyWindow(g_app.hwnd);
}

void app_start_snip(void)
{
    if (snip_active())
        return;
    A.restore_after_snip = IsWindowVisible(g_app.hwnd) && !IsIconic(g_app.hwnd);
    if (A.restore_after_snip)
        ShowWindow(g_app.hwnd, SW_HIDE);
    if (!snip_start(g_app.hwnd)) {
        if (A.restore_after_snip)
            ShowWindow(g_app.hwnd, SW_SHOWNOACTIVATE);
        app_note(L"截图失败");
    }
}

/* ---- content flow ------------------------------------------------------ */

static void submit(content *c)
{
    A.cur = *c;
    A.cur.data = NULL;
    A.cur.paths = NULL;
    A.cur.sizes = NULL;
    A.has_cur = TRUE;
    A.cur_name[0] = 0;
    if (c->type == CONTENT_FILES && c->nfiles == 1)
        lstrcpynW(A.cur_name, path_basename(c->paths[0]), (int)CPW_COUNTOF(A.cur_name));
    memset(&A.job, 0, sizeof A.job);
    A.job.state = JOB_PREPARING;
    A.job.kind = content_kind(c);
    A.job.estimate = c->raw_size;
    A.job.nfiles = c->nfiles;
    pacer_set_paused(FALSE);
    pacer_clear();
    producer_submit(c);
    invalidate_status();
}

static void accept_content(content *c, BOOL from_clipboard, BOOL forced)
{
    if (!A.pending && A.has_cur && content_same(&A.cur, c)) {
        /* same text copied again: don't restart, the phone keeps its progress */
        content_free(c);
        if (forced)
            app_note(L"内容未变化,继续发送");
        return;
    }
    if (A.pending) {
        content_free(A.pending);
        A.pending = NULL;
    }
    if (c->raw_size > (uint64_t)g_app.cfg.confirm_mb << 20) {
        A.pending = c;
        invalidate_status();
    } else {
        submit(c);
    }
    if (from_clipboard && g_app.cfg.auto_popup &&
        (!IsWindowVisible(g_app.hwnd) || IsIconic(g_app.hwnd) || GetForegroundWindow() != g_app.hwnd))
        app_show(FALSE);
}

static void read_clipboard(BOOL forced)
{
    content *c = NULL;
    DWORD seq = GetClipboardSequenceNumber();
    int r;
    if (!forced && seq == A.last_seq)
        return;
    A.last_seq = seq;
    r = clip_read(g_app.hwnd, g_app.cfg.skip_password_managers, &c);
    switch (r) {
    case CLIP_OK:
        accept_content(c, TRUE, forced);
        break;
    case CLIP_SKIPPED:
        app_note(L"已跳过密码管理器标记的内容");
        break;
    case CLIP_BUSY:
        app_note(L"剪切板被占用,按 Ctrl+V 重试");
        break;
    case CLIP_ERROR:
        app_note(L"无法读取剪切板内容");
        break;
    default:
        if (forced)
            app_note(L"剪切板里没有文本或图片");
        break;
    }
}

static void on_drop(HDROP drop)
{
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0), nf = 0, dirs = 0;
    wchar_t **paths = (wchar_t **)calloc(n ? n : 1, sizeof *paths);
    const wchar_t *bad = NULL;
    content *c;
    for (UINT i = 0; paths && i < n; i++) {
        UINT len = DragQueryFileW(drop, i, NULL, 0);
        wchar_t *p = (wchar_t *)malloc((len + 1) * sizeof(wchar_t));
        DWORD attr;
        if (!p)
            continue;
        DragQueryFileW(drop, i, p, len + 1);
        attr = GetFileAttributesW(p);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            dirs++;
            free(p);
            continue;
        }
        paths[nf++] = p;
    }
    DragFinish(drop);
    if (nf == 0) {
        if (dirs)
            app_note(L"暂不支持文件夹");
    } else {
        c = content_new_files(nf, (const wchar_t *const *)paths, &bad);
        if (c) {
            accept_content(c, FALSE, TRUE);
            if (dirs)
                app_note(L"已跳过 %u 个文件夹", dirs);
        } else {
            app_note(L"无法打开 %ls", bad ? path_basename(bad) : L"文件");
        }
    }
    for (UINT i = 0; i < nf; i++)
        free(paths[i]);
    free(paths);
}

/* ---- settings ---------------------------------------------------------- */

static void apply_hotkey(BOOL announce)
{
    wchar_t note[160];
    hotkey_apply(g_app.hwnd, &g_app.cfg, A.hotkey_active, CPW_COUNTOF(A.hotkey_active), note, CPW_COUNTOF(note));
    if (note[0])
        app_note(L"%ls", note);
    else if (announce && A.hotkey_active[0])
        app_note(L"截图热键:%ls", A.hotkey_active);
}

void app_settings_changed(const app_settings *old)
{
    const app_settings *s = &g_app.cfg;
    BOOL session = s->protocol != old->protocol || s->version != old->version || s->ecc != old->ecc ||
                   s->image_format != old->image_format || s->jpeg_quality != old->jpeg_quality;
    BOOL relayout = s->version != old->version || s->code_count != old->code_count ||
                    s->quiet_zone != old->quiet_zone || s->module_px != old->module_px ||
                    s->settings_expanded != old->settings_expanded;

    job_params p = params_from_cfg();

    producer_set_params(&p);
    if (session && A.has_cur)
        pacer_clear();
    if (s->fps != old->fps)
        pacer_set_fps(s->fps);
    if (s->module_px != old->module_px)
        pacer_set_module_px(s->module_px);
    if (s->topmost != old->topmost)
        SetWindowPos(g_app.hwnd, s->topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (s->force_builtin_snip != old->force_builtin_snip)
        apply_hotkey(TRUE);
    if (relayout)
        app_layout(NULL);
    if (!g_app.ini_writable || !config_save(s))
        g_app.ini_writable = FALSE;
    invalidate_status();
}

static void change_setting_from_key(int dpx)
{
    app_settings old = g_app.cfg;
    g_app.cfg.module_px = CPW_CLAMP(g_app.cfg.module_px + dpx, CFG_MODULE_MIN, CFG_MODULE_MAX);
    if (g_app.cfg.module_px != old.module_px) {
        app_settings_changed(&old);
        settings_bar_sync();
    }
}

static void toggle_cfg(BOOL *field)
{
    app_settings old = g_app.cfg;
    *field = !*field;
    app_settings_changed(&old);
    settings_bar_sync();
}

/* ---- keyboard (called from the message loop) --------------------------- */

BOOL app_pretranslate(MSG *m)
{
    HWND focus;
    BOOL in_ctrl, ctrl;
    if (m->message != WM_KEYDOWN || !g_app.hwnd || (m->hwnd != g_app.hwnd && !IsChild(g_app.hwnd, m->hwnd)))
        return FALSE;
    focus = GetFocus();
    in_ctrl = focus && focus != g_app.hwnd && focus != g_app.view && focus != g_app.gear;
    ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (m->wParam) {
    case VK_F2:
        app_toggle_settings();
        return TRUE;
    case VK_ESCAPE:
        if (focus && SendMessageW(focus, CB_GETDROPPEDSTATE, 0, 0))
            return FALSE;
        app_hide();
        return TRUE;
    }
    if (in_ctrl)
        return FALSE;
    switch (m->wParam) {
    case VK_SPACE:
        if (A.has_cur) {
            pacer_set_paused(!pacer_paused());
            invalidate_status();
        }
        return TRUE;
    case VK_RETURN:
        if (A.pending) {
            content *c = A.pending;
            A.pending = NULL;
            submit(c);
        }
        return TRUE;
    case 'V':
        if (!ctrl)
            return FALSE;
        read_clipboard(TRUE);
        return TRUE;
    case VK_OEM_PLUS:
    case VK_ADD:
        change_setting_from_key(+1);
        return TRUE;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        change_setting_from_key(-1);
        return TRUE;
    }
    return FALSE;
}

/* ---- window procedure -------------------------------------------------- */

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc = A.status_rc, tr;
    wchar_t text[512];
    HGDIOBJ of;
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    build_status(text, CPW_COUNTOF(text));
    tr = rc;
    tr.left = A.status_h + dpi_scale(6, g_app.dpi);
    tr.right -= dpi_scale(6, g_app.dpi);
    of = SelectObject(dc, g_app.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, A.job.state == JOB_ERROR && A.has_cur && !A.pending ? RGB(176, 32, 32)
                                                                         : GetSysColor(COLOR_BTNTEXT));
    DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_app.msg_taskbar_created && msg) {
        tray_add(hwnd, g_app.icon_small, L"CodeTrans · 剪切板转二维码");
        return 0;
    }
    if (msg == g_app.msg_show && msg) {
        app_show(TRUE);
        return 0;
    }
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED && g_app.view)
            layout_children();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_GEAR:
            app_toggle_settings();
            return 0;
        case IDM_SHOWHIDE:
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd))
                app_hide();
            else
                app_show(TRUE);
            return 0;
        case IDM_SNIP:
            app_start_snip();
            return 0;
        case IDM_PAUSE_MONITOR:
            toggle_cfg(&g_app.cfg.pause_monitor);
            if (!g_app.cfg.pause_monitor)
                A.last_seq = GetClipboardSequenceNumber();   /* don't resend what was copied meanwhile */
            return 0;
        case IDM_FORCE_SNIP:
            toggle_cfg(&g_app.cfg.force_builtin_snip);
            return 0;
        case IDM_AUTOSTART:
            if (!autostart_set(!autostart_is_enabled()))
                app_note(L"无法修改开机自启");
            return 0;
        case IDM_EXIT:
            app_exit();
            return 0;
        }
        break;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
            if (IsWindowVisible(hwnd) && !IsIconic(hwnd))
                app_hide();
            else
                app_show(TRUE);
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            tray_menu(hwnd, &g_app.cfg, IsWindowVisible(hwnd) && !IsIconic(hwnd), !snip_active());
            break;
        }
        return 0;
    case WM_CLIPBOARDUPDATE:
        if (!g_app.cfg.pause_monitor)
            SetTimer(hwnd, IDT_CLIP_DEBOUNCE, 80, NULL);   /* writers often set formats in steps */
        return 0;
    case WM_TIMER:
        if (wp == IDT_CLIP_DEBOUNCE) {
            KillTimer(hwnd, IDT_CLIP_DEBOUNCE);
            if (!g_app.cfg.pause_monitor)
                read_clipboard(FALSE);
        } else if (wp == IDT_NOTE) {
            KillTimer(hwnd, IDT_NOTE);
            A.note[0] = 0;
            invalidate_status();
        }
        return 0;
    case WM_DROPFILES:
        on_drop((HDROP)wp);
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_ID_SNIP)
            app_start_snip();
        return 0;
    case WM_APP_SNIP_DONE:
        if (A.restore_after_snip && !IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        A.restore_after_snip = FALSE;
        return 0;
    case WM_APP_JOB: {
        job_status *j = (job_status *)lp;
        if (j) {
            if (A.has_cur)
                A.job = *j;
            free(j);
            invalidate_status();
        }
        return 0;
    }
    case WM_APP_FPS:
        invalidate_status();
        return 0;
    case WM_DPICHANGED: {
        const RECT *sug = (const RECT *)lp;
        g_app.dpi = HIWORD(wp);
        make_fonts();
        app_layout(sug);
        return 0;
    }
    case WM_DISPLAYCHANGE:
        pacer_refresh_changed();
        app_layout(NULL);
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETNONCLIENTMETRICS || wp == SPI_SETWORKAREA) {
            make_fonts();
            app_layout(NULL);
        }
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE && g_app.cfg.topmost) {
            /* a topmost minimised window is useless; go to the tray instead */
            app_hide();
            return 0;
        }
        break;
    case WM_CLOSE:
        if (!A.quitting) {
            app_hide();   /* close = to tray; exit via the tray menu */
            return 0;
        }
        break;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp)
            save_window_pos();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_CLIP_DEBOUNCE);
        KillTimer(hwnd, IDT_NOTE);
        clip_unlisten(hwnd);
        hotkey_release(hwnd);
        tray_remove(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- creation ---------------------------------------------------------- */

typedef BOOL (WINAPI *PFN_ChangeWindowMessageFilterEx)(HWND, UINT, DWORD, void *);

static void allow_drop_from_lower_integrity(HWND hwnd)
{
    /* only matters if someone runs us elevated anyway (manifest: asInvoker) */
    PFN_ChangeWindowMessageFilterEx f;
    *(FARPROC *)&f = GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx");
    if (f) {
        f(hwnd, WM_DROPFILES, 1 /* MSGFLT_ALLOW */, NULL);
        f(hwnd, WM_COPYDATA, 1, NULL);
        f(hwnd, 0x0049 /* WM_COPYGLOBALDATA */, 1, NULL);
    }
}

BOOL app_create(HINSTANCE hi, BOOL start_hidden)
{
    WNDCLASSEXW wc;
    RECT wr;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;

    g_app.hinst = hi;
    g_app.icon_big = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                       GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    g_app.icon_small = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_app.icon_big)
        g_app.icon_big = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    if (!g_app.icon_small)
        g_app.icon_small = g_app.icon_big;

    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hi;
    wc.hIcon = g_app.icon_big;
    wc.hIconSm = g_app.icon_small;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CPW_MAIN_CLASS;
    if (!RegisterClassExW(&wc) || !qr_view_register(hi) || !settings_bar_register(hi) || !snip_register(hi))
        return FALSE;
    g_app.msg_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_app.msg_show = RegisterWindowMessageW(L"CodeTrans.ShowWindow");

    if (g_app.cfg.has_window_pos) {
        SetRect(&wr, g_app.cfg.window_x, g_app.cfg.window_y, g_app.cfg.window_x + 200, g_app.cfg.window_y + 200);
        if (MonitorFromRect(&wr, MONITOR_DEFAULTTONULL)) {   /* still on some monitor? */
            x = g_app.cfg.window_x;
            y = g_app.cfg.window_y;
        }
    }
    g_app.hwnd = CreateWindowExW(MAIN_EX_STYLE | (g_app.cfg.topmost ? WS_EX_TOPMOST : 0), CPW_MAIN_CLASS,
                                 L"CodeTrans", MAIN_STYLE, x, y, 400, 300, NULL, NULL, hi, NULL);
    if (!g_app.hwnd)
        return FALSE;
    g_app.dpi = dpi_for_window(g_app.hwnd);
    g_app.view = CreateWindowExW(0, CPW_VIEW_CLASS, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, g_app.hwnd, NULL, hi,
                                 NULL);
    g_app.gear = CreateWindowExW(0, L"BUTTON", L"⚙", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_CENTER, 0, 0,
                                 10, 10, g_app.hwnd, (HMENU)(INT_PTR)IDC_GEAR, hi, NULL);
    g_app.bar = settings_bar_create(g_app.hwnd);
    if (!g_app.view || !g_app.gear || !g_app.bar)
        return FALSE;
    make_fonts();
    settings_bar_sync();

    if (x == CW_USEDEFAULT) {
        /* first start: centre on the primary work area */
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetRect(&wr, (mi.rcWork.left + mi.rcWork.right) / 2 - 250, (mi.rcWork.top + mi.rcWork.bottom) / 2 - 200, 0, 0);
        wr.right = wr.left + 500;
        wr.bottom = wr.top + 400;
        app_layout(&wr);
        GetWindowRect(g_app.hwnd, &wr);
        SetWindowPos(g_app.hwnd, NULL, (mi.rcWork.left + mi.rcWork.right - (wr.right - wr.left)) / 2,
                     (mi.rcWork.top + mi.rcWork.bottom - (wr.bottom - wr.top)) / 2, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    } else {
        app_layout(NULL);
    }

    DragAcceptFiles(g_app.hwnd, TRUE);
    allow_drop_from_lower_integrity(g_app.hwnd);
    compat_disable_dwm_transitions(g_app.hwnd);

    job_params p = params_from_cfg();
    if (!producer_start(g_app.hwnd, &p) ||
        !pacer_start(g_app.view, g_app.hwnd, g_app.cfg.fps, g_app.cfg.module_px))
        return FALSE;
    compat_disable_power_throttling();

    if (!clip_listen(g_app.hwnd))
        app_note(L"无法监听剪切板");
    A.last_seq = GetClipboardSequenceNumber();   /* don't send what was there before we started */
    tray_add(g_app.hwnd, g_app.icon_small, L"CodeTrans · 剪切板转二维码");
    apply_hotkey(FALSE);
    if (!g_app.ini_writable)
        app_note(L"配置文件不可写,设置不会保存");

    if (!start_hidden) {
        ShowWindow(g_app.hwnd, SW_SHOW);
        UpdateWindow(g_app.hwnd);
    }
    return TRUE;
}

void app_shutdown(void)
{
    producer_stop();
    pacer_stop();
    if (A.pending)
        content_free(A.pending);
    A.pending = NULL;
}
