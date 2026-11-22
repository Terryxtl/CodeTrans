/* tray.c - see tray.h */
#include "tray.h"
#include "app.h"
#include <shellapi.h>

#define TRAY_UID 1

static void fill(NOTIFYICONDATAW *nid, HWND hwnd)
{
    ZeroMemory(nid, sizeof *nid);
    nid->cbSize = sizeof *nid;     /* Vista+ layout, fine on Win7 */
    nid->hWnd = hwnd;
    nid->uID = TRAY_UID;
}

BOOL tray_add(HWND hwnd, HICON icon, const wchar_t *tip)
{
    NOTIFYICONDATAW nid;
    fill(&nid, hwnd);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = icon;
    lstrcpynW(nid.szTip, tip, (int)(sizeof nid.szTip / sizeof nid.szTip[0]));
    Shell_NotifyIconW(NIM_DELETE, &nid);
    return Shell_NotifyIconW(NIM_ADD, &nid);
}

void tray_remove(HWND hwnd)
{
    NOTIFYICONDATAW nid;
    fill(&nid, hwnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void tray_set_tip(HWND hwnd, const wchar_t *tip)
{
    NOTIFYICONDATAW nid;
    fill(&nid, hwnd);
    nid.uFlags = NIF_TIP;
    lstrcpynW(nid.szTip, tip, (int)(sizeof nid.szTip / sizeof nid.szTip[0]));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void tray_menu(HWND hwnd, const app_settings *s, BOOL window_visible, BOOL snip_available)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    if (!m)
        return;
    AppendMenuW(m, MF_STRING, IDM_SHOWHIDE, window_visible ? L"隐藏窗口" : L"显示窗口");
    AppendMenuW(m, MF_STRING | (snip_available ? 0 : MF_GRAYED), IDM_SNIP, L"截图");
    AppendMenuW(m, MF_STRING, IDM_RECEIVE, L"摄像头接收(手机 → 电脑)\tF3");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (s->pause_monitor ? MF_CHECKED : 0), IDM_PAUSE_MONITOR, L"暂停监听剪切板");
    AppendMenuW(m, MF_STRING | (s->force_builtin_snip ? MF_CHECKED : 0), IDM_FORCE_SNIP, L"强制使用内置截图");
    AppendMenuW(m, MF_STRING | (autostart_is_enabled() ? MF_CHECKED : 0), IDM_AUTOSTART, L"开机自启");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"退出");
    SetMenuDefaultItem(m, IDM_SHOWHIDE, FALSE);
    GetCursorPos(&pt);
    /* required so the menu closes when clicking elsewhere (KB135788) */
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}
