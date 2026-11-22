/* recv_window.c - see recv_window.h
 *
 * Layout: the camera picture (mirrored, like a mirror, so moving the phone to
 * the right moves it right) with the outlines of the codes the scanner sees
 * (green = read, amber = seen but unreadable) and a progress bar, then two
 * status lines. Results are saved under Downloads\CodeTrans and, when
 * RecvAutoOpen is on, opened (text / images) or shown in Explorer (files). */
#include "recv_window.h"
#include "app.h"
#include "compat.h"
#include "receiver.h"
#include "recv_open.h"
#include "util.h"
#include <shellapi.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CPW_RECV_CLASS L"CodeTrans.Recv"
#define WM_APP_RECV    (WM_APP + 20)
#define IDT_RECV_TICK  1
#define NOTE_MS        8000

static struct {
    HWND      hwnd;
    receiver *rx;
    char      out_dir[MAX_PATH * 3];   /* UTF-8 */
    wchar_t   out_dir_w[MAX_PATH];
    uint8_t  *pv;
    size_t    pv_cap;
    uint8_t  *padded;
    size_t    padded_cap;
    int       pw, ph, nq;
    cp_scan_quad quads[CP_SCAN_MAX_QUADS];
    wchar_t   note[200];
    DWORD     note_at;
    BOOL      note_warn;
} R;

/* Downloads\CodeTrans (falls back to %TEMP%\CodeTrans). */
static void resolve_out_dir(void)
{
    /* FOLDERID_Downloads; SHGetKnownFolderPath is Vista+ */
    static const GUID downloads = { 0x374DE290, 0x123F, 0x4565, { 0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B } };
    typedef HRESULT (WINAPI *PFN_SHGetKnownFolderPath)(REFGUID, DWORD, HANDLE, PWSTR *);
    PFN_SHGetKnownFolderPath get;
    PWSTR p = NULL;
    wchar_t base[MAX_PATH] = L"";
    char *u;
    size_t ul = 0;
    *(FARPROC *)&get = GetProcAddress(GetModuleHandleW(L"shell32.dll"), "SHGetKnownFolderPath");
    if (get && SUCCEEDED(get(&downloads, 0, NULL, &p)) && p) {
        lstrcpynW(base, p, MAX_PATH - 16);
        CoTaskMemFree(p);
    } else if (!GetTempPathW(MAX_PATH - 16, base)) {
        lstrcpyW(base, L".");
    }
    if (base[0] && base[lstrlenW(base) - 1] == L'\\')
        base[lstrlenW(base) - 1] = 0;
    wfmt(R.out_dir_w, CPW_COUNTOF(R.out_dir_w), L"%ls\\CodeTrans", base);
    CreateDirectoryW(R.out_dir_w, NULL);
    u = utf16_to_utf8(R.out_dir_w, wcslen(R.out_dir_w), &ul);
    if (u && ul < sizeof R.out_dir) {
        memcpy(R.out_dir, u, ul + 1);
    } else {
        strcpy(R.out_dir, ".");
    }
    free(u);
}

static void note(BOOL warn, const wchar_t *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(R.note, CPW_COUNTOF(R.note) - 1, fmt, ap);
    va_end(ap);
    R.note[CPW_COUNTOF(R.note) - 1] = 0;
    R.note_at = GetTickCount();
    R.note_warn = warn;
    InvalidateRect(R.hwnd, NULL, FALSE);
}

static const wchar_t *kind_word(int kind)
{
    switch (kind) {
    case CP_KIND_TEXT: return L"文本";
    case CP_KIND_IMAGE: return L"图片";
    case CP_KIND_FILES: return L"文件";
    }
    return L"内容";
}

static void on_result(cp_result *r)
{
    wchar_t path[MAX_PATH], size[32];
    uint64_t total = 0;
    int action = recv_classify(r, path, CPW_COUNTOF(path));
    for (size_t i = 0; i < r->count; i++)
        total += r->sizes[i];
    fmt_size(total, size, CPW_COUNTOF(size));
    if (action == RECV_OPEN_NONE) {
        note(TRUE, L"收到%ls,但无法保存", kind_word(r->kind));
        return;
    }
    if (!g_app.cfg.recv_auto_open) {
        note(FALSE, L"✔ 已收到%ls %ls,保存在 %ls", kind_word(r->kind), size, R.out_dir_w);
        return;
    }
    if (!recv_perform(action, path))
        note(TRUE, L"✔ 已收到%ls %ls,但无法打开:%ls", kind_word(r->kind), size, path);
    else if (action == RECV_OPEN_DEFAULT_APP)
        note(FALSE, L"✔ 已收到%ls %ls,已用默认程序打开", kind_word(r->kind), size);
    else if (r->kind == CP_KIND_FILES && r->count > 1)
        note(FALSE, L"✔ 已收到 %u 个文件 %ls,已在资源管理器中显示", (unsigned)r->count, size);
    else
        note(FALSE, L"✔ 已收到%ls %ls,已在资源管理器中显示", kind_word(r->kind), size);
}

static void on_plain(const recv_plain *p)
{
    wchar_t path[MAX_PATH];
    if (!recv_save_plain(R.out_dir, p->text, p->n, path, CPW_COUNTOF(path))) {
        note(TRUE, L"识别到普通二维码,但无法保存");
        return;
    }
    if (g_app.cfg.recv_auto_open && recv_perform(RECV_OPEN_DEFAULT_APP, path))
        note(FALSE, L"✔ 识别到普通二维码,已用默认程序打开");
    else
        note(FALSE, L"✔ 识别到普通二维码,保存在 %ls", path);
}

static void restart_camera(int index)
{
    if (R.rx)
        receiver_stop(R.rx);
    R.rx = receiver_start(R.hwnd, WM_APP_RECV, index, R.out_dir);
    R.pw = R.ph = R.nq = 0;
    if (!R.rx)
        note(TRUE, L"无法启动接收");
    InvalidateRect(R.hwnd, NULL, FALSE);
}

/* ---- painting ------------------------------------------------------------- */

static int status_height(void)
{
    HDC dc = GetDC(R.hwnd);
    HGDIOBJ of = SelectObject(dc, g_app.font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    ReleaseDC(R.hwnd, dc);
    return 2 * tm.tmHeight + dpi_scale(14, g_app.dpi);
}

static void build_lines(const recv_status *st, wchar_t *l1, size_t c1, wchar_t *l2, size_t c2, BOOL *warn)
{
    const camera_info *c = &st->cam;
    wchar_t a[32], rate[32];
    *warn = FALSE;
    if (R.note[0] && GetTickCount() - R.note_at < NOTE_MS && !st->receiving) {
        wfmt(l1, c1, L"%ls", R.note);
        *warn = R.note_warn;
    } else if (c->state == CAM_FAILED) {
        wfmt(l1, c1, L"⚠ %ls", c->err);
        *warn = TRUE;
    } else if (c->state != CAM_RUNNING) {
        wfmt(l1, c1, L"正在打开摄像头%ls%ls…", c->name[0] ? L" " : L"", c->name);
    } else if (st->receiving && st->progress.need > 0) {
        uint64_t have = st->progress.have < st->progress.need ? st->progress.have : st->progress.need;
        fmt_size(st->progress.total_bytes, a, CPW_COUNTOF(a));
        fmt_size(st->progress.bytes_per_sec, rate, CPW_COUNTOF(rate));
        wfmt(l1, c1, L"接收中 · %ls %ls · %llu/%llu (%u%%) · %ls/s · %d 码", kind_word(st->progress.kind), a,
             (unsigned long long)have, (unsigned long long)st->progress.need,
             (unsigned)(have * 100 / st->progress.need), rate, st->decoded);
    } else if (st->located > 0 && st->decoded == 0) {
        wfmt(l1, c1, L"看到 %d 个码但读不出:手机再靠近些、调高屏幕亮度,或在手机上选低码密度", st->located);
        *warn = TRUE;
    } else {
        wfmt(l1, c1, L"把手机「发送到电脑」页面上的二维码对准摄像头");
    }
    if (c->state == CAM_RUNNING)
        wfmt(l2, c2, L"%ls · %d×%d %hs %dfps · 解码 %.0f ms · 保存到 %ls%ls", c->name, c->width, c->height, c->format,
             c->fps, st->scan_ms, R.out_dir_w, c->count > 1 ? L" · 按 C 切换摄像头" : L"");
    else
        wfmt(l2, c2, L"收到的文本和图片会自动用默认程序打开,文件保存到 %ls", R.out_dir_w);
}

static void paint_preview(HDC dc, const RECT *area, const recv_status *st)
{
    struct { BITMAPINFOHEADER h; RGBQUAD pal[256]; } bi;
    int aw = area->right - area->left, ah = area->bottom - area->top, dw, dh, dx, dy, stride;
    const uint8_t *bits;
    if (R.pw <= 0 || R.ph <= 0 || aw <= 0 || ah <= 0)
        return;
    /* fit, keeping the aspect ratio */
    if ((long long)aw * R.ph <= (long long)ah * R.pw) {
        dw = aw;
        dh = (int)((long long)aw * R.ph / R.pw);
    } else {
        dh = ah;
        dw = (int)((long long)ah * R.pw / R.ph);
    }
    dx = area->left + (aw - dw) / 2;
    dy = area->top + (ah - dh) / 2;
    stride = (R.pw + 3) & ~3;
    bits = R.pv;
    if (stride != R.pw) {   /* DIB rows are DWORD aligned */
        size_t need = (size_t)stride * R.ph;
        if (R.padded_cap < need) {
            uint8_t *p = (uint8_t *)realloc(R.padded, need);
            if (!p)
                return;
            R.padded = p;
            R.padded_cap = need;
        }
        for (int y = 0; y < R.ph; y++)
            memcpy(R.padded + (size_t)y * stride, R.pv + (size_t)y * R.pw, (size_t)R.pw);
        bits = R.padded;
    }
    memset(&bi, 0, sizeof bi);
    bi.h.biSize = sizeof bi.h;
    bi.h.biWidth = R.pw;
    bi.h.biHeight = -R.ph;
    bi.h.biPlanes = 1;
    bi.h.biBitCount = 8;
    bi.h.biCompression = BI_RGB;
    bi.h.biClrUsed = 256;
    for (int i = 0; i < 256; i++)
        bi.pal[i].rgbRed = bi.pal[i].rgbGreen = bi.pal[i].rgbBlue = (BYTE)i;
    SetStretchBltMode(dc, COLORONCOLOR);
    /* negative destination width: mirrored */
    StretchDIBits(dc, dx + dw - 1, dy, -dw, dh, 0, 0, R.pw, R.ph, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS, SRCCOPY);

    for (int i = 0; i < R.nq; i++) {
        POINT pt[5];
        HPEN pen = CreatePen(PS_SOLID, dpi_scale(2, g_app.dpi), R.quads[i].decoded ? RGB(40, 200, 90) : RGB(255, 180, 0));
        HGDIOBJ op = SelectObject(dc, pen);
        for (int k = 0; k < 4; k++) {
            pt[k].x = dx + dw - 1 - (int)((long long)R.quads[i].x[k] * dw / R.pw);
            pt[k].y = dy + (int)((long long)R.quads[i].y[k] * dh / R.ph);
        }
        pt[4] = pt[0];
        Polyline(dc, pt, 5);
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    if (st->receiving && st->progress.need > 0) {
        RECT bar = { dx, dy + dh - dpi_scale(6, g_app.dpi), dx, dy + dh };
        uint64_t have = st->progress.have < st->progress.need ? st->progress.have : st->progress.need;
        HBRUSH track = CreateSolidBrush(RGB(60, 60, 60)), fill = CreateSolidBrush(RGB(0, 150, 136));
        bar.right = dx + dw;
        FillRect(dc, &bar, track);
        bar.right = dx + (int)((long long)dw * (long long)have / (long long)st->progress.need);
        FillRect(dc, &bar, fill);
        DeleteObject(track);
        DeleteObject(fill);
    }
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc;
    RECT rc, area, line;
    HBITMAP bmp;
    HGDIOBJ ob, of;
    recv_status st;
    wchar_t l1[400], l2[400];
    BOOL warn;
    int sh = status_height(), lh;

    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    ob = SelectObject(dc, bmp);
    memset(&st, 0, sizeof st);
    if (R.rx)
        receiver_status(R.rx, &st);

    area = rc;
    area.bottom = rc.bottom - sh;
    FillRect(dc, &area, (HBRUSH)GetStockObject(BLACK_BRUSH));
    paint_preview(dc, &area, &st);

    line = rc;
    line.top = area.bottom;
    FillRect(dc, &line, GetSysColorBrush(COLOR_BTNFACE));
    build_lines(&st, l1, CPW_COUNTOF(l1), l2, CPW_COUNTOF(l2), &warn);
    of = SelectObject(dc, g_app.font);
    SetBkMode(dc, TRANSPARENT);
    lh = (sh - dpi_scale(14, g_app.dpi)) / 2;
    InflateRect(&line, -dpi_scale(8, g_app.dpi), 0);
    line.top += dpi_scale(5, g_app.dpi);
    line.bottom = line.top + lh;
    SetTextColor(dc, warn ? RGB(176, 32, 32) : GetSysColor(COLOR_BTNTEXT));
    DrawTextW(dc, l1, -1, &line, DT_SINGLELINE | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    OffsetRect(&line, 0, lh + dpi_scale(4, g_app.dpi));
    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    DrawTextW(dc, l2, -1, &line, DT_SINGLELINE | DT_LEFT | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, of);

    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

/* ---- window --------------------------------------------------------------- */

static LRESULT CALLBACK recv_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        R.hwnd = hwnd;
        SetTimer(hwnd, IDT_RECV_TICK, 500, NULL);
        restart_camera(g_app.cfg.camera_index);
        return 0;
    case WM_APP_RECV:
        switch (wp) {
        case RECV_MSG_FRAME:
            if (R.rx) {
                receiver_preview(R.rx, &R.pv, &R.pv_cap, &R.pw, &R.ph, R.quads, &R.nq);
                receiver_frame_seen(R.rx);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        case RECV_MSG_RESULT:
            on_result((cp_result *)lp);
            cp_result_free((cp_result *)lp);
            break;
        case RECV_MSG_PLAIN:
            on_plain((const recv_plain *)lp);
            free((void *)lp);
            break;
        case RECV_MSG_FAILED:
            note(TRUE, L"⚠ 校验失败,正在重新接收");
            break;
        default:
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
        } else if (wp == 'C' && R.rx) {
            recv_status st;
            receiver_status(R.rx, &st);
            if (st.cam.count > 1) {
                app_settings old = g_app.cfg;
                g_app.cfg.camera_index = (st.cam.index + 1) % st.cam.count;
                app_settings_changed(&old);
                restart_camera(g_app.cfg.camera_index);
            }
        } else if (wp == 'O') {
            ShellExecuteW(NULL, L"open", R.out_dir_w, NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_RECV_TICK);
        receiver_stop(R.rx);
        R.rx = NULL;
        R.hwnd = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL recv_window_register(HINSTANCE hi)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = recv_proc;
    wc.hInstance = hi;
    wc.hIcon = g_app.icon_big;
    wc.hIconSm = g_app.icon_small;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CPW_RECV_CLASS;
    return RegisterClassExW(&wc) != 0;
}

void recv_window_show(void)
{
    RECT r;
    int w, h;
    if (R.hwnd) {
        ShowWindow(R.hwnd, IsIconic(R.hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(R.hwnd);
        return;
    }
    if (!R.out_dir[0])
        resolve_out_dir();
    SetRect(&r, 0, 0, dpi_scale(640, g_app.dpi), dpi_scale(480, g_app.dpi) + dpi_scale(48, g_app.dpi));
    dpi_adjust_window_rect(&r, WS_OVERLAPPEDWINDOW, 0, g_app.dpi);
    w = r.right - r.left;
    h = r.bottom - r.top;
    CreateWindowExW(0, CPW_RECV_CLASS, L"CodeTrans · 摄像头接收(手机 → 电脑)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, w, h, NULL, NULL, g_app.hinst, NULL);
    if (R.hwnd)
        SetForegroundWindow(R.hwnd);
}

void recv_window_close(void)
{
    if (R.hwnd)
        DestroyWindow(R.hwnd);
    free(R.pv);
    free(R.padded);
    R.pv = R.padded = NULL;
    R.pv_cap = R.padded_cap = 0;
}

BOOL recv_window_open(void)
{
    return R.hwnd != NULL;
}
