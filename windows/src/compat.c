/* compat.c - see compat.h */
#include "compat.h"
#include <dwmapi.h>

#define LOADFN(mod, var, name) (*(FARPROC *)&(var) = (mod) ? GetProcAddress((mod), (name)) : NULL)

typedef LONG    (WINAPI *PFN_RtlGetVersion)(OSVERSIONINFOW *);
typedef UINT    (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef UINT    (WINAPI *PFN_GetDpiForSystem)(void);
typedef BOOL    (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef BOOL    (WINAPI *PFN_SystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);
typedef int     (WINAPI *PFN_GetSystemMetricsForDpi)(int, UINT);
typedef HRESULT (WINAPI *PFN_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);
typedef BOOL    (WINAPI *PFN_SetProcessInformation)(HANDLE, int, LPVOID, DWORD);

static PFN_GetDpiForWindow            p_GetDpiForWindow;
static PFN_GetDpiForSystem            p_GetDpiForSystem;
static PFN_AdjustWindowRectExForDpi   p_AdjustWindowRectExForDpi;
static PFN_SystemParametersInfoForDpi p_SystemParametersInfoForDpi;
static PFN_GetSystemMetricsForDpi     p_GetSystemMetricsForDpi;
static PFN_GetDpiForMonitor           p_GetDpiForMonitor;
static PFN_SetProcessInformation      p_SetProcessInformation;

static DWORD s_major, s_minor, s_build;
static UINT  s_sys_dpi = USER_DEFAULT_SCREEN_DPI;
static BOOL  s_inited;

HMODULE compat_load_system_dll(const wchar_t *name)
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n + 2 + lstrlenW(name) >= MAX_PATH)
        return NULL;
    if (path[n - 1] != L'\\')
        path[n++] = L'\\';
    lstrcpyW(path + n, name);
    return LoadLibraryW(path);
}

void compat_init(void)
{
    if (s_inited)
        return;
    s_inited = TRUE;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    PFN_RtlGetVersion p_RtlGetVersion = NULL;
    LOADFN(ntdll, p_RtlGetVersion, "RtlGetVersion");
    if (p_RtlGetVersion) {
        OSVERSIONINFOW vi;
        ZeroMemory(&vi, sizeof vi);
        vi.dwOSVersionInfoSize = sizeof vi;
        if (p_RtlGetVersion(&vi) == 0) {
            s_major = vi.dwMajorVersion;
            s_minor = vi.dwMinorVersion;
            s_build = vi.dwBuildNumber;
        }
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    LOADFN(user32, p_GetDpiForWindow, "GetDpiForWindow");
    LOADFN(user32, p_GetDpiForSystem, "GetDpiForSystem");
    LOADFN(user32, p_AdjustWindowRectExForDpi, "AdjustWindowRectExForDpi");
    LOADFN(user32, p_SystemParametersInfoForDpi, "SystemParametersInfoForDpi");
    LOADFN(user32, p_GetSystemMetricsForDpi, "GetSystemMetricsForDpi");

    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    LOADFN(kernel32, p_SetProcessInformation, "SetProcessInformation");

    /* shcore.dll exists on 8.1+; Win7 simply has none. */
    if (s_major > 6 || (s_major == 6 && s_minor >= 3)) {
        HMODULE shcore = compat_load_system_dll(L"shcore.dll");
        LOADFN(shcore, p_GetDpiForMonitor, "GetDpiForMonitor");
    }

    if (p_GetDpiForSystem) {
        s_sys_dpi = p_GetDpiForSystem();
    } else {
        HDC dc = GetDC(NULL);
        if (dc) {
            s_sys_dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(NULL, dc);
        }
    }
    if (s_sys_dpi < 48)
        s_sys_dpi = USER_DEFAULT_SCREEN_DPI;
}

DWORD compat_os_major(void) { return s_major; }
DWORD compat_os_minor(void) { return s_minor; }
DWORD compat_os_build(void) { return s_build; }

UINT dpi_system(void) { return s_sys_dpi; }

UINT dpi_for_monitor(HMONITOR mon)
{
    if (mon && p_GetDpiForMonitor) {
        UINT x = 0, y = 0;
        if (SUCCEEDED(p_GetDpiForMonitor(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && y >= 48)
            return y;
    }
    return s_sys_dpi;
}

UINT dpi_for_window(HWND hwnd)
{
    if (hwnd && p_GetDpiForWindow) {
        UINT d = p_GetDpiForWindow(hwnd);
        if (d >= 48)
            return d;
    }
    if (hwnd && p_GetDpiForMonitor)
        return dpi_for_monitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
    return s_sys_dpi;
}

int dpi_scale(int v96, UINT dpi)
{
    return MulDiv(v96, (int)dpi, USER_DEFAULT_SCREEN_DPI);
}

int dpi_metric(int index, UINT dpi)
{
    if (p_GetSystemMetricsForDpi)
        return p_GetSystemMetricsForDpi(index, dpi);
    return MulDiv(GetSystemMetrics(index), (int)dpi, (int)s_sys_dpi);
}

BOOL dpi_adjust_window_rect(RECT *rc, DWORD style, DWORD ex_style, UINT dpi)
{
    if (p_AdjustWindowRectExForDpi)
        return p_AdjustWindowRectExForDpi(rc, style, FALSE, ex_style, dpi);
    /* Win7 / 8.1: the non-client area is not DPI-scaled per monitor, so the
     * system-DPI frame is what the window actually gets. */
    return AdjustWindowRectEx(rc, style, FALSE, ex_style);
}

BOOL dpi_message_font(UINT dpi, LOGFONTW *out)
{
    NONCLIENTMETRICSW ncm;
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (p_SystemParametersInfoForDpi &&
        p_SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi)) {
        *out = ncm.lfMessageFont;
        return TRUE;
    }
    ZeroMemory(&ncm, sizeof ncm);
    ncm.cbSize = sizeof ncm;
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0)) {
        ZeroMemory(out, sizeof *out);
        out->lfHeight = -MulDiv(9, (int)dpi, 72);
        out->lfWeight = FW_NORMAL;
        out->lfCharSet = DEFAULT_CHARSET;
        lstrcpyW(out->lfFaceName, L"Segoe UI");
        return FALSE;
    }
    *out = ncm.lfMessageFont;
    out->lfHeight = MulDiv(out->lfHeight, (int)dpi, (int)s_sys_dpi);
    return TRUE;
}

BOOL compat_dwm_enabled(void)
{
    BOOL on = FALSE;
    return SUCCEEDED(DwmIsCompositionEnabled(&on)) && on;
}

void compat_disable_dwm_transitions(HWND hwnd)
{
    BOOL yes = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &yes, sizeof yes);
}

void compat_wait_composition(int frames)
{
    if (frames < 1)
        frames = 1;
    if (compat_dwm_enabled()) {
        for (int i = 0; i < frames; i++)
            if (FAILED(DwmFlush())) {
                Sleep(16);
            }
    } else {
        Sleep((DWORD)(16 * frames + 20));
    }
}

typedef struct cpw_power_throttling_state {
    ULONG Version, ControlMask, StateMask;
} cpw_power_throttling_state;

BOOL compat_disable_power_throttling(void)
{
    if (!p_SetProcessInformation)
        return FALSE;
    cpw_power_throttling_state st;
    st.Version = 1;          /* PROCESS_POWER_THROTTLING_CURRENT_VERSION */
    st.ControlMask = 0x1;    /* PROCESS_POWER_THROTTLING_EXECUTION_SPEED */
    st.StateMask = 0;        /* 0 = never throttle */
    return p_SetProcessInformation(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof st);
}
