/* pacer.c - see pacer.h */
#include "pacer.h"
#include "app.h"
#include "compat.h"
#include "producer.h"
#include "qr_geom.h"
#include "util.h"
#include <dwmapi.h>
#include <mmsystem.h>
#include <stdlib.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

typedef struct bmi2 {
    BITMAPINFOHEADER h;
    RGBQUAD pal[2];
} bmi2;

static struct {
    HANDLE           thread, wake;
    HWND             view, notify;
    volatile LONG    quit;
    volatile LONG    fps, px, paused, refresh_dirty;
    CRITICAL_SECTION cs;          /* display buffer */
    uint8_t         *pix;
    size_t           pix_cap;
    frame_geom       g;
    BOOL             has_frame;
    bmi2             bmi;
    double           refresh_hz;  /* pacer thread */
    LONG             eff_fps_x10;
} K;

static double query_refresh_hz(void)
{
    DWM_TIMING_INFO ti;
    DEVMODEW dm;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    if (SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &ti)) && ti.rateRefresh.uiDenominator &&
        ti.rateRefresh.uiNumerator) {
        double hz = (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator;
        if (hz >= 20 && hz <= 1000)
            return hz;
    }
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return dm.dmDisplayFrequency;
    return 60.0;
}

/* vblanks per frame for the DWM path */
static int vblanks_per_frame(double hz, int fps)
{
    int n = (int)(hz / fps + 0.5);
    return n < 1 ? 1 : n;
}

static void update_effective(BOOL dwm)
{
    int fps = K.fps;
    LONG eff = dwm ? (LONG)(K.refresh_hz * 10.0 / vblanks_per_frame(K.refresh_hz, fps) + 0.5) : fps * 10;
    if (eff != K.eff_fps_x10) {
        InterlockedExchange(&K.eff_fps_x10, eff);
        PostMessageW(K.notify, WM_APP_FPS, (WPARAM)eff, 0);
    }
}

static void fill_white(HDC dc, int l, int t, int r, int b)
{
    RECT f;
    if (r > l && b > t) {
        SetRect(&f, l, t, r, b);
        FillRect(dc, &f, (HBRUSH)GetStockObject(WHITE_BRUSH));
    }
}

/* cs held. Paints the whole client rect: the frame centred, white around it.
 * The margins must be repainted on every frame: when the view is resized
 * (e.g. the settings bar widens the window) the frame moves, and any pixels
 * of the previous position left outside the new frame rect would show up as
 * broken code fragments next to the real codes. */
static void draw_locked(HDC dc, const RECT *rc)
{
    int px = K.px, w, h, x0, y0, x1, y1;
    if (!K.has_frame) {
        fill_white(dc, rc->left, rc->top, rc->right, rc->bottom);
        return;
    }
    w = K.g.w * px;
    h = K.g.h * px;
    x0 = rc->left + CPW_MAX(0, (rc->right - rc->left - w) / 2);
    y0 = rc->top + CPW_MAX(0, (rc->bottom - rc->top - h) / 2);
    x1 = x0 + K.g.stride * px;        /* the blit covers the stride padding too (white) */
    y1 = y0 + h;
    fill_white(dc, rc->left, rc->top, rc->right, y0);
    fill_white(dc, rc->left, y1, rc->right, rc->bottom);
    fill_white(dc, rc->left, y0, x0, y1);
    fill_white(dc, x1, y0, rc->right, y1);
    K.bmi.h.biWidth = K.g.stride;     /* rows are stride bytes; extra columns are white */
    K.bmi.h.biHeight = -K.g.h;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, x0, y0, K.g.stride * px, h, 0, 0, K.g.stride, K.g.h, K.pix,
                  (const BITMAPINFO *)&K.bmi, DIB_RGB_COLORS, SRCCOPY);
}

static void advance(void)
{
    HDC dc;
    RECT rc;
    EnterCriticalSection(&K.cs);
    if (producer_pop(&K.pix, &K.pix_cap, &K.g)) {
        K.has_frame = TRUE;
        dc = GetDC(K.view);
        if (dc) {
            GetClientRect(K.view, &rc);
            draw_locked(dc, &rc);
            ReleaseDC(K.view, dc);
        }
    }
    LeaveCriticalSection(&K.cs);
}

static uint64_t qpc_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (uint64_t)t.QuadPart; }

static DWORD WINAPI pacer_main(void *arg)
{
    LARGE_INTEGER f;
    double qpf;
    HANDLE timer;
    BOOL hires = TRUE, period_set = FALSE, awake = FALSE, dwm = FALSE;
    uint64_t last_switch = 0, last_dwm_check = 0;
    (void)arg;

    QueryPerformanceFrequency(&f);
    qpf = (double)f.QuadPart;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {            /* before Win10 1803 */
        hires = FALSE;
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    }
    K.refresh_hz = query_refresh_hz();

    while (!K.quit) {
        BOOL active = producer_busy() && !K.paused;
        uint64_t now = qpc_now();

        if (active != awake) {
            /* keep the display on while playing (§6.6) */
            SetThreadExecutionState(active ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)
                                           : ES_CONTINUOUS);
            awake = active;
        }
        if (!active) {
            if (period_set) {
                timeEndPeriod(1);
                period_set = FALSE;
            }
            WaitForSingleObject(K.wake, 100);
            last_switch = 0;
            continue;
        }
        if (K.refresh_dirty || now - last_dwm_check > (uint64_t)qpf) {
            if (InterlockedExchange(&K.refresh_dirty, 0))
                K.refresh_hz = query_refresh_hz();
            dwm = compat_dwm_enabled();
            last_dwm_check = now;
            update_effective(dwm);
        }

        if (dwm) {
            double vblank = qpf / K.refresh_hz;
            double frame = vblank * vblanks_per_frame(K.refresh_hz, K.fps);
            uint64_t t0 = qpc_now();
            if (period_set) {
                timeEndPeriod(1);
                period_set = FALSE;
            }
            if (FAILED(DwmFlush()) || (double)(qpc_now() - t0) < vblank * 0.1)
                Sleep(1);    /* nothing to wait for: don't spin */
            now = qpc_now();
            if (last_switch == 0 || (double)(now - last_switch) >= frame - vblank * 0.5) {
                /* stay phase-locked unless we fell far behind */
                if (last_switch && (double)(now - last_switch) < frame * 2)
                    last_switch += (uint64_t)frame;
                else
                    last_switch = now;
                advance();
            }
        } else {
            double frame = qpf / K.fps;
            LARGE_INTEGER due;
            if (!hires && !period_set) {
                timeBeginPeriod(1);
                period_set = TRUE;
            }
            if (last_switch == 0)
                last_switch = now;
            last_switch += (uint64_t)frame;
            if (last_switch < now)
                last_switch = now;    /* fell behind: don't burst */
            due.QuadPart = -(LONGLONG)((double)(last_switch - now) * 1e7 / qpf);
            if (timer && due.QuadPart < 0 && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                WaitForSingleObject(timer, 1000);
            advance();
        }
    }
    if (period_set)
        timeEndPeriod(1);
    if (awake)
        SetThreadExecutionState(ES_CONTINUOUS);
    if (timer)
        CloseHandle(timer);
    return 0;
}

BOOL pacer_start(HWND view, HWND notify, int fps, int module_px)
{
    InitializeCriticalSection(&K.cs);
    K.view = view;
    K.notify = notify;
    K.fps = CPW_CLAMP(fps, 1, 60);
    K.px = CPW_CLAMP(module_px, 1, 16);
    K.bmi.h.biSize = sizeof K.bmi.h;
    K.bmi.h.biPlanes = 1;
    K.bmi.h.biBitCount = 8;
    K.bmi.h.biCompression = BI_RGB;
    K.bmi.h.biClrUsed = 2;
    K.bmi.pal[0].rgbRed = K.bmi.pal[0].rgbGreen = K.bmi.pal[0].rgbBlue = 255;   /* 0 = white */
    K.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    K.thread = CreateThread(NULL, 0, pacer_main, NULL, 0, NULL);
    return K.thread != NULL;
}

void pacer_stop(void)
{
    if (!K.thread)
        return;
    InterlockedExchange(&K.quit, 1);
    SetEvent(K.wake);
    WaitForSingleObject(K.thread, 2000);
    CloseHandle(K.thread);
    CloseHandle(K.wake);
    K.thread = NULL;
    free(K.pix);
    K.pix = NULL;
    K.pix_cap = 0;
}

void pacer_set_fps(int fps)
{
    InterlockedExchange(&K.fps, CPW_CLAMP(fps, 1, 60));
    InterlockedExchange(&K.refresh_dirty, 1);
    SetEvent(K.wake);
}

void pacer_set_module_px(int px)
{
    InterlockedExchange(&K.px, CPW_CLAMP(px, 1, 16));
    InvalidateRect(K.view, NULL, FALSE);
}

void pacer_set_paused(BOOL paused)
{
    InterlockedExchange(&K.paused, paused ? 1 : 0);
    SetEvent(K.wake);
}

BOOL pacer_paused(void) { return K.paused != 0; }

void pacer_clear(void)
{
    EnterCriticalSection(&K.cs);
    K.has_frame = FALSE;
    LeaveCriticalSection(&K.cs);
    InvalidateRect(K.view, NULL, FALSE);
    SetEvent(K.wake);
}

void pacer_paint(HDC dc, const RECT *client)
{
    EnterCriticalSection(&K.cs);
    draw_locked(dc, client);
    LeaveCriticalSection(&K.cs);
}

void pacer_refresh_changed(void)
{
    InterlockedExchange(&K.refresh_dirty, 1);
    SetEvent(K.wake);
}

int pacer_effective_fps_x10(void)
{
    LONG v = K.eff_fps_x10;
    return v ? (int)v : K.fps * 10;
}
