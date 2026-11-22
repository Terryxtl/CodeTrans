/* settings_bar.c - collapsible settings bar (PLAN.md §6.1 / §6.2).
 *
 *   协议[CodeTrans▾] 版本[25▾]1273B 纠错[L▾] 模块[4▴▾]px
 *   码数[2▾] FPS[15▴▾] 图片[PNG▾] ☑置顶 ☑自动弹出 ☐暂停监听
 *
 * Every change applies immediately via app_settings_changed() (which also
 * writes the INI). */
#include "app.h"
#include "compat.h"
#include "util.h"
#include <commctrl.h>
#include <stdlib.h>

enum {
    ID_PROTO = IDC_SB_FIRST, ID_VERSION, ID_CAP, ID_ECC, ID_MODULE, ID_MODULE_UD, ID_PX,
    ID_COUNT, ID_FPS, ID_FPS_UD, ID_IMAGE, ID_TOPMOST, ID_POPUP, ID_PAUSE,
    ID_L_PROTO, ID_L_VERSION, ID_L_ECC, ID_L_MODULE, ID_L_COUNT, ID_L_FPS, ID_L_IMAGE
};

static const int k_counts[] = { 1, 2, 3, 4, 6 };

static struct {
    HWND bar;
    HWND proto, version, cap, ecc, module, module_ud, px, count, fps, fps_ud, image, topmost, popup, pause;
    HWND l_proto, l_version, l_ecc, l_module, l_count, l_fps, l_image;
    BOOL syncing;
} B;

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int id)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, B.bar,
                             (HMENU)(INT_PTR)id, g_app.hinst, NULL);
    return h;
}

static HWND mk_label(const wchar_t *text, int id)
{
    return mk(L"STATIC", text, SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, id);
}

static HWND mk_combo(int id)
{
    return mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
}

static void combo_add(HWND c, const wchar_t *s) { SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s); }

static HWND mk_updown(HWND edit, int id, int lo, int hi)
{
    HWND ud = CreateWindowExW(0, UPDOWN_CLASSW, L"",
                              WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS |
                                  UDS_NOTHOUSANDS | UDS_HOTTRACK,
                              0, 0, 10, 10, B.bar, (HMENU)(INT_PTR)id, g_app.hinst, NULL);
    SendMessageW(ud, UDM_SETBUDDY, (WPARAM)edit, 0);
    SendMessageW(ud, UDM_SETRANGE32, (WPARAM)lo, (LPARAM)hi);
    return ud;
}

static void create_controls(void)
{
    wchar_t b[16];
    B.l_proto = mk_label(L"协议", ID_L_PROTO);
    B.proto = mk_combo(ID_PROTO);
    combo_add(B.proto, L"CodeTrans");
    combo_add(B.proto, L"Decimen v3 · RS96");
    combo_add(B.proto, L"Decimen v3 · LT");
    combo_add(B.proto, L"光传码面 · 横带(实验)");   /* CFG_PROTO_OB_BANDS */
    combo_add(B.proto, L"光传码面 · 分块(实验)");   /* CFG_PROTO_OB_TILES */
    B.l_version = mk_label(L"版本", ID_L_VERSION);
    B.version = mk_combo(ID_VERSION);
    for (int v = CFG_VERSION_MIN; v <= CFG_VERSION_MAX; v++) {
        wfmt(b, CPW_COUNTOF(b), L"%d", v);
        combo_add(B.version, b);
    }
    B.cap = mk_label(L"1273B", ID_CAP);
    B.l_ecc = mk_label(L"纠错", ID_L_ECC);
    B.ecc = mk_combo(ID_ECC);
    combo_add(B.ecc, L"L");
    combo_add(B.ecc, L"M");
    combo_add(B.ecc, L"Q");
    combo_add(B.ecc, L"H");
    B.l_module = mk_label(L"模块", ID_L_MODULE);
    B.module = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER |
                               ES_CENTER, 0, 0, 10, 10, B.bar, (HMENU)(INT_PTR)ID_MODULE, g_app.hinst, NULL);
    SendMessageW(B.module, EM_LIMITTEXT, 2, 0);
    B.module_ud = mk_updown(B.module, ID_MODULE_UD, CFG_MODULE_MIN, CFG_MODULE_MAX);
    B.px = mk_label(L"px", ID_PX);

    B.l_count = mk_label(L"码数", ID_L_COUNT);
    B.count = mk_combo(ID_COUNT);
    for (size_t i = 0; i < CPW_COUNTOF(k_counts); i++) {
        wfmt(b, CPW_COUNTOF(b), L"%d", k_counts[i]);
        combo_add(B.count, b);
    }
    B.l_fps = mk_label(L"FPS", ID_L_FPS);
    B.fps = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER |
                            ES_CENTER, 0, 0, 10, 10, B.bar, (HMENU)(INT_PTR)ID_FPS, g_app.hinst, NULL);
    SendMessageW(B.fps, EM_LIMITTEXT, 2, 0);
    B.fps_ud = mk_updown(B.fps, ID_FPS_UD, CFG_FPS_MIN, CFG_FPS_MAX);
    B.l_image = mk_label(L"图片", ID_L_IMAGE);
    B.image = mk_combo(ID_IMAGE);
    combo_add(B.image, L"PNG");
    combo_add(B.image, L"JPEG");
    combo_add(B.image, L"自动");   /* CFG_IMG_AUTO: photos as JPEG, screenshots as PNG */
    B.topmost = mk(L"BUTTON", L"置顶", BS_AUTOCHECKBOX | WS_TABSTOP, ID_TOPMOST);
    B.popup = mk(L"BUTTON", L"自动弹出", BS_AUTOCHECKBOX | WS_TABSTOP, ID_POPUP);
    B.pause = mk(L"BUTTON", L"暂停监听", BS_AUTOCHECKBOX | WS_TABSTOP, ID_PAUSE);
}

/* ---- layout ------------------------------------------------------------ */

typedef struct item {
    HWND h, buddy_ud;   /* buddy_ud: edit + up-down pair */
    int  w;
    int  kind;          /* 0 label, 1 combo, 2 edit, 3 check */
} item;

static int text_w(HDC dc, const wchar_t *s)
{
    SIZE sz;
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    return sz.cx;
}

static int window_text_w(HDC dc, HWND h)
{
    wchar_t b[64];
    GetWindowTextW(h, b, CPW_COUNTOF(b));
    return text_w(dc, b);
}

static int combo_w(HDC dc, HWND c, UINT dpi)
{
    int n = (int)SendMessageW(c, CB_GETCOUNT, 0, 0), w = 0;
    wchar_t b[64];
    for (int i = 0; i < n; i++) {
        SendMessageW(c, CB_GETLBTEXT, (WPARAM)i, (LPARAM)b);
        w = CPW_MAX(w, text_w(dc, b));
    }
    return w + dpi_metric(SM_CXVSCROLL, dpi) + dpi_scale(14, dpi);
}

static int place_row(item *it, int n, int x, int y, int h, int gap, int small, BOOL apply)
{
    for (int i = 0; i < n; i++) {
        if (apply) {
            if (it[i].kind == 1) {
                MoveWindow(it[i].h, x, y, it[i].w, h * 12, TRUE);
            } else if (it[i].kind == 2) {
                MoveWindow(it[i].h, x, y, it[i].w, h, TRUE);
                SendMessageW(it[i].buddy_ud, UDM_SETBUDDY, (WPARAM)it[i].h, 0);   /* re-attach to the right */
            } else {
                MoveWindow(it[i].h, x, y, it[i].w, h, TRUE);
            }
        }
        x += it[i].w + ((it[i].kind == 0 && i + 1 < n && it[i + 1].kind != 0) ? small : gap);
    }
    return x;
}

SIZE settings_bar_layout(void)
{
    UINT dpi = g_app.dpi;
    HDC dc = GetDC(B.bar);
    HGDIOBJ of = SelectObject(dc, g_app.font);
    TEXTMETRICW tm;
    int pad = dpi_scale(8, dpi), gap = dpi_scale(10, dpi), small = dpi_scale(4, dpi), h, w1, w2, y1, y2;
    int ud_w = dpi_metric(SM_CXVSCROLL, dpi), chk = dpi_metric(SM_CXMENUCHECK, dpi) + dpi_scale(8, dpi);
    int edit_w = text_w(dc, L"88") + ud_w + dpi_scale(14, dpi);
    SIZE sz;
    HWND all[] = { B.proto, B.version, B.cap, B.ecc, B.module, B.module_ud, B.px, B.count, B.fps, B.fps_ud,
                   B.image, B.topmost, B.popup, B.pause, B.l_proto, B.l_version, B.l_ecc, B.l_module,
                   B.l_count, B.l_fps, B.l_image };

    for (size_t i = 0; i < CPW_COUNTOF(all); i++)
        SendMessageW(all[i], WM_SETFONT, (WPARAM)g_app.font, FALSE);
    GetTextMetricsW(dc, &tm);
    h = tm.tmHeight + dpi_scale(8, dpi);

    item r1[] = {
        { B.l_proto, NULL, window_text_w(dc, B.l_proto), 0 },
        { B.proto, NULL, combo_w(dc, B.proto, dpi), 1 },
        { B.l_version, NULL, window_text_w(dc, B.l_version), 0 },
        { B.version, NULL, combo_w(dc, B.version, dpi), 1 },
        { B.cap, NULL, text_w(dc, L"2953B"), 3 },
        { B.l_ecc, NULL, window_text_w(dc, B.l_ecc), 0 },
        { B.ecc, NULL, combo_w(dc, B.ecc, dpi), 1 },
        { B.l_module, NULL, window_text_w(dc, B.l_module), 0 },
        { B.module, B.module_ud, edit_w, 2 },
        { B.px, NULL, text_w(dc, L"px"), 3 },
    };
    item r2[] = {
        { B.l_count, NULL, window_text_w(dc, B.l_count), 0 },
        { B.count, NULL, combo_w(dc, B.count, dpi), 1 },
        { B.l_fps, NULL, window_text_w(dc, B.l_fps), 0 },
        { B.fps, B.fps_ud, edit_w, 2 },
        { B.l_image, NULL, window_text_w(dc, B.l_image), 0 },
        { B.image, NULL, combo_w(dc, B.image, dpi), 1 },
        { B.topmost, NULL, window_text_w(dc, B.topmost) + chk, 3 },
        { B.popup, NULL, window_text_w(dc, B.popup) + chk, 3 },
        { B.pause, NULL, window_text_w(dc, B.pause) + chk, 3 },
    };
    SelectObject(dc, of);
    ReleaseDC(B.bar, dc);

    y1 = pad;
    y2 = pad + h + small * 2;
    w1 = place_row(r1, (int)CPW_COUNTOF(r1), pad, y1, h, gap, small, TRUE);
    w2 = place_row(r2, (int)CPW_COUNTOF(r2), pad, y2, h, gap, small, TRUE);
    sz.cx = CPW_MAX(w1, w2) - gap + pad;
    sz.cy = y2 + h + pad;
    return sz;
}

/* ---- values ------------------------------------------------------------ */

static int count_index(int n)
{
    for (size_t i = 0; i < CPW_COUNTOF(k_counts); i++)
        if (k_counts[i] == n)
            return (int)i;
    return 1;
}

void settings_bar_update_capacity(void)
{
    wchar_t b[24];
    const app_settings *s = &g_app.cfg;
    BOOL ob = CFG_PROTO_IS_OB(s->protocol);
    /* boards: per-board payload; size and ECC are fixed by the format */
    wfmt(b, CPW_COUNTOF(b), ob ? L"%uB/面" : L"%uB", (unsigned)cfg_code_bytes(s));
    SetWindowTextW(B.cap, b);
    EnableWindow(B.version, !ob);
    EnableWindow(B.ecc, !ob);
}

void settings_bar_sync(void)
{
    const app_settings *s = &g_app.cfg;
    if (!B.bar)
        return;
    B.syncing = TRUE;
    SendMessageW(B.proto, CB_SETCURSEL, (WPARAM)s->protocol, 0);
    SendMessageW(B.version, CB_SETCURSEL, (WPARAM)(s->version - CFG_VERSION_MIN), 0);
    SendMessageW(B.ecc, CB_SETCURSEL, (WPARAM)s->ecc, 0);
    SendMessageW(B.module_ud, UDM_SETPOS32, 0, (LPARAM)s->module_px);
    SendMessageW(B.count, CB_SETCURSEL, (WPARAM)count_index(s->code_count), 0);
    SendMessageW(B.fps_ud, UDM_SETPOS32, 0, (LPARAM)s->fps);
    SendMessageW(B.image, CB_SETCURSEL, (WPARAM)s->image_format, 0);
    CheckDlgButton(B.bar, ID_TOPMOST, s->topmost ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(B.bar, ID_POPUP, s->auto_popup ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(B.bar, ID_PAUSE, s->pause_monitor ? BST_CHECKED : BST_UNCHECKED);
    settings_bar_update_capacity();
    B.syncing = FALSE;
}

static int edit_int(HWND e, int *out)
{
    wchar_t b[16];
    if (GetWindowTextW(e, b, CPW_COUNTOF(b)) <= 0)
        return 0;
    *out = _wtoi(b);
    return 1;
}

static void on_command(int id, int code, HWND ctl)
{
    app_settings old = g_app.cfg, *s = &g_app.cfg;
    int v;
    if (B.syncing)
        return;
    switch (id) {
    case ID_PROTO:
        if (code != CBN_SELCHANGE)
            return;
        s->protocol = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        /* each QR family has its own default size (RS96 <-> LT keeps the version);
         * optical boards have a fixed size and leave the QR version alone */
        if (!CFG_PROTO_IS_OB(s->protocol) && !CFG_PROTO_IS_OB(old.protocol) &&
            CFG_PROTO_IS_DECIMEN(s->protocol) != CFG_PROTO_IS_DECIMEN(old.protocol))
            s->version = CFG_PROTO_IS_DECIMEN(s->protocol) ? 40 : 25;
        break;
    case ID_VERSION:
        if (code != CBN_SELCHANGE)
            return;
        s->version = CFG_VERSION_MIN + (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        break;
    case ID_ECC:
        if (code != CBN_SELCHANGE)
            return;
        s->ecc = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        break;
    case ID_COUNT: {
        int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        if (code != CBN_SELCHANGE || i < 0 || i >= (int)CPW_COUNTOF(k_counts))
            return;
        s->code_count = k_counts[i];
        break;
    }
    case ID_IMAGE:
        if (code != CBN_SELCHANGE)
            return;
        s->image_format = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        break;
    case ID_MODULE:
    case ID_FPS:
        if (code == EN_KILLFOCUS) {          /* invalid / empty: show the real value again */
            settings_bar_sync();
            return;
        }
        if (code != EN_CHANGE || !edit_int(ctl, &v))
            return;
        if (id == ID_MODULE) {
            if (v < CFG_MODULE_MIN || v > CFG_MODULE_MAX)
                return;
            s->module_px = v;
        } else {
            if (v < CFG_FPS_MIN || v > CFG_FPS_MAX)
                return;
            s->fps = v;
        }
        break;
    case ID_TOPMOST:
        s->topmost = IsDlgButtonChecked(B.bar, ID_TOPMOST) == BST_CHECKED;
        break;
    case ID_POPUP:
        s->auto_popup = IsDlgButtonChecked(B.bar, ID_POPUP) == BST_CHECKED;
        break;
    case ID_PAUSE:
        s->pause_monitor = IsDlgButtonChecked(B.bar, ID_PAUSE) == BST_CHECKED;
        break;
    default:
        return;
    }
    config_validate(s);
    if (memcmp(&old, s, sizeof old) != 0) {
        if (s->version != old.version || s->ecc != old.ecc || s->protocol != old.protocol) {
            B.syncing = TRUE;
            SendMessageW(B.version, CB_SETCURSEL, (WPARAM)(s->version - CFG_VERSION_MIN), 0);
            B.syncing = FALSE;
            settings_bar_update_capacity();
        }
        app_settings_changed(&old);
    }
}

static LRESULT CALLBACK bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        on_command(LOWORD(wp), HIWORD(wp), (HWND)lp);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        SetTextColor((HDC)wp, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_LBUTTONDOWN:
        SetFocus(g_app.hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL settings_bar_register(HINSTANCE hi)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = bar_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CPW_BAR_CLASS;
    return RegisterClassExW(&wc) != 0;
}

HWND settings_bar_create(HWND parent)
{
    B.bar = CreateWindowExW(WS_EX_CONTROLPARENT, CPW_BAR_CLASS, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10,
                            parent, NULL, g_app.hinst, NULL);
    if (!B.bar)
        return NULL;
    create_controls();
    return B.bar;
}
