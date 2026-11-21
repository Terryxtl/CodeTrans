/* snip.c - see snip.h */
#include "snip.h"
#include "app.h"
#include "clipboard.h"
#include "compat.h"
#include "image_enc.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

static struct {
    HWND     owner, wnd;
    int      vx, vy, vw, vh;
    HDC      dc_src, dc_dark, dc_back;
    HBITMAP  bmp_src, bmp_dark, bmp_back;
    HGDIOBJ  old_src, old_dark, old_back;
    uint8_t *bits_src;           /* top-down BGRX, vw * vh */
    BOOL     dragging, has_sel;
    POINT    a, b;               /* client coordinates */
    HFONT    font;
} S;

static HBITMAP make_dib(HDC dc, int w, int h, uint8_t **bits)
{
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

static void release_gdi(void)
{
    if (S.dc_src) { SelectObject(S.dc_src, S.old_src); DeleteDC(S.dc_src); }
    if (S.dc_dark) { SelectObject(S.dc_dark, S.old_dark); DeleteDC(S.dc_dark); }
    if (S.dc_back) { SelectObject(S.dc_back, S.old_back); DeleteDC(S.dc_back); }
    if (S.bmp_src) DeleteObject(S.bmp_src);
    if (S.bmp_dark) DeleteObject(S.bmp_dark);
    if (S.bmp_back) DeleteObject(S.bmp_back);
    if (S.font) DeleteObject(S.font);
    S.dc_src = S.dc_dark = S.dc_back = NULL;
    S.bmp_src = S.bmp_dark = S.bmp_back = NULL;
    S.font = NULL;
    S.bits_src = NULL;
}

static RECT sel_rect(void)
{
    RECT r;
    r.left = CPW_MIN(S.a.x, S.b.x);
    r.top = CPW_MIN(S.a.y, S.b.y);
    r.right = CPW_MAX(S.a.x, S.b.x);
    r.bottom = CPW_MAX(S.a.y, S.b.y);
    return r;
}

static void compose(void)
{
    RECT r = sel_rect();
    wchar_t label[64];
    SIZE ts;
    BitBlt(S.dc_back, 0, 0, S.vw, S.vh, S.dc_dark, 0, 0, SRCCOPY);
    SelectObject(S.dc_back, S.font);
    SetBkMode(S.dc_back, OPAQUE);
    SetBkColor(S.dc_back, RGB(32, 32, 32));
    SetTextColor(S.dc_back, RGB(255, 255, 255));
    if (S.has_sel && r.right > r.left && r.bottom > r.top) {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0, 120, 215));
        HGDIOBJ op = SelectObject(S.dc_back, pen), ob = SelectObject(S.dc_back, GetStockObject(NULL_BRUSH));
        int ly;
        BitBlt(S.dc_back, r.left, r.top, r.right - r.left, r.bottom - r.top, S.dc_src, r.left, r.top, SRCCOPY);
        Rectangle(S.dc_back, r.left - 1, r.top - 1, r.right + 1, r.bottom + 1);
        SelectObject(S.dc_back, op);
        SelectObject(S.dc_back, ob);
        DeleteObject(pen);
        wfmt(label, CPW_COUNTOF(label), L" %d × %d ", (int)(r.right - r.left), (int)(r.bottom - r.top));
        GetTextExtentPoint32W(S.dc_back, label, lstrlenW(label), &ts);
        ly = r.top - ts.cy - 4;
        if (ly < 0)
            ly = r.top + 4;
        TextOutW(S.dc_back, r.left, ly, label, lstrlenW(label));
    } else {
        /* hint centred on the primary monitor */
        const wchar_t *hint = L"  拖动鼠标选择截图区域 · Esc 或右键取消  ";
        int px = -S.vx + GetSystemMetrics(SM_CXSCREEN) / 2, py = -S.vy + 40;
        GetTextExtentPoint32W(S.dc_back, hint, lstrlenW(hint), &ts);
        TextOutW(S.dc_back, px - ts.cx / 2, py, hint, lstrlenW(hint));
    }
}

static void finish(BOOL take)
{
    BOOL ok = FALSE;
    RECT r = sel_rect();
    HWND owner = S.owner;
    if (take && S.has_sel && r.right - r.left >= 2 && r.bottom - r.top >= 2 && S.bits_src) {
        int w = r.right - r.left, h = r.bottom - r.top;
        uint8_t *crop = (uint8_t *)malloc((size_t)w * h * 4);
        if (crop) {
            uint8_t *dib, *png;
            size_t dl = 0, pl = 0;
            for (int y = 0; y < h; y++) {
                const uint8_t *s = S.bits_src + ((size_t)(r.top + y) * S.vw + r.left) * 4;
                uint8_t *d = crop + (size_t)y * w * 4;
                memcpy(d, s, (size_t)w * 4);
                for (int x = 0; x < w; x++)
                    d[x * 4 + 3] = 255;
            }
            dib = image_make_dib(crop, w, h, &dl);
            png = image_encode_png(crop, w, h, FALSE, &pl);
            ok = clip_write_image(owner, dib, dl, png, pl);
            free(dib);
            free(png);
            free(crop);
        }
    }
    if (S.wnd) {
        HWND w = S.wnd;
        S.wnd = NULL;
        ReleaseCapture();
        DestroyWindow(w);
    }
    release_gdi();
    PostMessageW(owner, WM_APP_SNIP_DONE, ok ? 1 : 0, 0);
}

static LRESULT CALLBACK snip_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (S.dc_back)
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, S.dc_back, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_CROSS));
        return TRUE;
    case WM_LBUTTONDOWN:
        S.dragging = TRUE;
        S.has_sel = TRUE;
        S.a.x = S.b.x = (short)LOWORD(lp);
        S.a.y = S.b.y = (short)HIWORD(lp);
        SetCapture(hwnd);
        compose();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (S.dragging) {
            S.b.x = CPW_CLAMP((short)LOWORD(lp), 0, S.vw);
            S.b.y = CPW_CLAMP((short)HIWORD(lp), 0, S.vh);
            compose();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (S.dragging) {
            S.dragging = FALSE;
            S.b.x = CPW_CLAMP((short)LOWORD(lp), 0, S.vw);
            S.b.y = CPW_CLAMP((short)HIWORD(lp), 0, S.vh);
            finish(TRUE);
        }
        return 0;
    case WM_RBUTTONDOWN:
        finish(FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
            finish(FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        if (S.dragging && (HWND)lp != hwnd) {
            S.dragging = FALSE;
            S.has_sel = FALSE;
            compose();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_CLOSE:
        finish(FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL snip_register(HINSTANCE hi)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = snip_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_CROSS);
    wc.lpszClassName = CPW_SNIP_CLASS;
    return RegisterClassExW(&wc) != 0;
}

BOOL snip_active(void) { return S.wnd != NULL; }

BOOL snip_start(HWND owner)
{
    HDC screen;
    uint8_t *dark_bits = NULL, *back_bits = NULL;
    LOGFONTW lf;
    if (S.wnd)
        return FALSE;
    S.owner = owner;
    S.dragging = S.has_sel = FALSE;

    compat_wait_composition(2);   /* let our own window disappear first */
    S.vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    S.vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    S.vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    S.vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (S.vw <= 0 || S.vh <= 0)
        return FALSE;

    screen = GetDC(NULL);
    S.dc_src = CreateCompatibleDC(screen);
    S.dc_dark = CreateCompatibleDC(screen);
    S.dc_back = CreateCompatibleDC(screen);
    S.bmp_src = make_dib(screen, S.vw, S.vh, &S.bits_src);
    S.bmp_dark = make_dib(screen, S.vw, S.vh, &dark_bits);
    S.bmp_back = make_dib(screen, S.vw, S.vh, &back_bits);
    if (!S.dc_src || !S.dc_dark || !S.dc_back || !S.bmp_src || !S.bmp_dark || !S.bmp_back) {
        ReleaseDC(NULL, screen);
        release_gdi();
        return FALSE;
    }
    S.old_src = SelectObject(S.dc_src, S.bmp_src);
    S.old_dark = SelectObject(S.dc_dark, S.bmp_dark);
    S.old_back = SelectObject(S.dc_back, S.bmp_back);
    BitBlt(S.dc_src, 0, 0, S.vw, S.vh, screen, S.vx, S.vy, SRCCOPY | CAPTUREBLT);
    ReleaseDC(NULL, screen);
    GdiFlush();
    for (size_t i = 0; i < (size_t)S.vw * S.vh * 4; i++)
        dark_bits[i] = (uint8_t)(S.bits_src[i] * 9 / 20);

    dpi_message_font(dpi_for_monitor(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY)), &lf);
    lf.lfHeight = lf.lfHeight * 4 / 3;
    S.font = CreateFontIndirectW(&lf);
    compose();

    S.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, CPW_SNIP_CLASS, L"CodeTrans 截图", WS_POPUP,
                            S.vx, S.vy, S.vw, S.vh, NULL, NULL, g_app.hinst, NULL);
    if (!S.wnd) {
        release_gdi();
        return FALSE;
    }
    compat_disable_dwm_transitions(S.wnd);
    ShowWindow(S.wnd, SW_SHOW);
    SetForegroundWindow(S.wnd);
    SetFocus(S.wnd);
    return TRUE;
}
