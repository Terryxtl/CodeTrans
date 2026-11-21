/* app.h - application-wide state shared by the UI modules (UI thread only
 * unless noted), window messages and command ids. */
#ifndef CPW_APP_H
#define CPW_APP_H

#include <windows.h>
#include "config.h"
#include "content.h"

#define CPW_MAIN_CLASS   L"CodeTrans.Main"
#define CPW_VIEW_CLASS   L"CodeTrans.QrView"
#define CPW_BAR_CLASS    L"CodeTrans.SettingsBar"
#define CPW_SNIP_CLASS   L"CodeTrans.Snip"

/* window messages */
#define WM_APP_TRAY       (WM_APP + 1)   /* Shell_NotifyIcon callback                  */
#define WM_APP_JOB        (WM_APP + 2)   /* producer -> UI, lParam = job_status* (free) */
#define WM_APP_FPS        (WM_APP + 3)   /* pacer -> UI, wParam = actual fps x 10       */
#define WM_APP_SNIP_DONE  (WM_APP + 4)   /* snip overlay closed, wParam = 1 if captured */

/* timers (main window) */
#define IDT_CLIP_DEBOUNCE 1
#define IDT_NOTE          2

/* command ids: tray menu, gear button, settings controls */
enum {
    IDM_SHOWHIDE = 100,
    IDM_SNIP,
    IDM_PAUSE_MONITOR,
    IDM_FORCE_SNIP,
    IDM_AUTOSTART,
    IDM_EXIT,
    IDC_GEAR = 200,
    IDC_SB_FIRST = 300
};

#define HOTKEY_ID_SNIP 0x0C01

typedef struct app_ctx {
    HINSTANCE    hinst;
    HWND         hwnd;          /* main window              */
    HWND         view;          /* QR code area             */
    HWND         bar;           /* settings bar             */
    HWND         gear;          /* ⚙ button                 */
    HICON        icon_big, icon_small;
    app_settings cfg;
    UINT         dpi;
    HFONT        font;          /* message font for the current DPI */
    HFONT        gear_font;
    UINT         msg_taskbar_created;
    UINT         msg_show;      /* second instance -> "show yourself" */
    BOOL         ini_writable;
} app_ctx;

extern app_ctx g_app;

/* Settings bar / tray changed g_app.cfg; old = values before the change.
 * Applies the difference (new session, relayout, hotkeys, ...) and saves. */
void app_settings_changed(const app_settings *old);

/* Short-lived message appended to the status line (≈6 s). */
void app_note(const wchar_t *fmt, ...);

void app_show(BOOL activate);
void app_hide(void);
void app_toggle_settings(void);
void app_start_snip(void);
void app_exit(void);

/* window procedures / registration of the child modules */
BOOL qr_view_register(HINSTANCE hi);
BOOL settings_bar_register(HINSTANCE hi);
HWND settings_bar_create(HWND parent);
/* Lays the controls out for g_app.font / g_app.dpi; returns the bar size. */
SIZE settings_bar_layout(void);
/* Copies g_app.cfg into the controls (no change notifications). */
void settings_bar_sync(void);
/* Refreshes the "1273B" label next to the version box. */
void settings_bar_update_capacity(void);

#endif
