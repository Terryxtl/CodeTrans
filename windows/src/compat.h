/* compat.h - dynamically loaded APIs newer than Windows 7 + DPI helpers.
 *
 * The exe must start on a bare Win7 SP1, so every API introduced after Win7
 * (GetDpiForWindow, AdjustWindowRectExForDpi, SystemParametersInfoForDpi,
 * GetSystemMetricsForDpi, GetDpiForMonitor, SetProcessInformation, ...) is
 * resolved with GetProcAddress here and wrapped with a Win7 fallback.
 * Call compat_init() once at startup (any thread, before the others). */
#ifndef CPW_COMPAT_H
#define CPW_COMPAT_H

#include <windows.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef USER_DEFAULT_SCREEN_DPI
#define USER_DEFAULT_SCREEN_DPI 96
#endif

void  compat_init(void);

/* Real OS version via RtlGetVersion (not subject to manifest lies). */
DWORD compat_os_major(void);
DWORD compat_os_minor(void);
DWORD compat_os_build(void);

/* Load a DLL from System32 only (works without KB2533623 on Win7). */
HMODULE compat_load_system_dll(const wchar_t *name);

/* DPI. All values are in physical pixels for a PerMonitorV2 process. */
UINT  dpi_system(void);
UINT  dpi_for_window(HWND hwnd);
UINT  dpi_for_monitor(HMONITOR mon);
int   dpi_scale(int v96, UINT dpi);
int   dpi_metric(int index, UINT dpi);
BOOL  dpi_adjust_window_rect(RECT *rc, DWORD style, DWORD ex_style, UINT dpi);
/* NONCLIENTMETRICS.lfMessageFont scaled for `dpi`. */
BOOL  dpi_message_font(UINT dpi, LOGFONTW *out);

/* Disable DWM show/hide animations for a top-level window (Vista+). */
void  compat_disable_dwm_transitions(HWND hwnd);
/* Block until the next DWM composition (or ~frames*16ms when DWM is off). */
void  compat_wait_composition(int frames);
BOOL  compat_dwm_enabled(void);

/* Win10 1709+: opt this process out of EcoQoS / execution-speed throttling.
 * Returns FALSE when unsupported. For the pacer (PLAN.md §6.6). */
BOOL  compat_disable_power_throttling(void);

#endif
