/* qr_view.c - the code area child window. WM_PAINT only blits the current
 * frame (the pacer thread draws every new frame directly). */
#include "app.h"
#include "pacer.h"

static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &ps);
        GetClientRect(hwnd, &rc);
        pacer_paint(dc, &rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        /* give keyboard focus back to the main window (Space, +/-, Ctrl+V) */
        SetFocus(GetParent(hwnd));
        return 0;
    case WM_NCHITTEST:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL qr_view_register(HINSTANCE hi)
{
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;   /* resizing re-centres the frame: repaint all of it */
    wc.lpfnWndProc = view_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CPW_VIEW_CLASS;
    return RegisterClassExW(&wc) != 0;
}
