/* pacer.h - frame pacing thread (PLAN.md §6.6).
 *
 * With DWM on, frame switches are aligned to compositions (DwmFlush) and the
 * effective rate is R / round(R / fps); with DWM off a high-resolution
 * waitable timer (or timeBeginPeriod(1)) is used instead. Each switch pops the
 * next frame from the producer and blits it straight into the code view. */
#ifndef CPW_PACER_H
#define CPW_PACER_H

#include <windows.h>

BOOL pacer_start(HWND view, HWND notify, int fps, int module_px);
void pacer_stop(void);
void pacer_set_fps(int fps);
void pacer_set_module_px(int px);
void pacer_set_paused(BOOL paused);
BOOL pacer_paused(void);
/* Forget the displayed frame (content changed / idle) and repaint. */
void pacer_clear(void);
/* WM_PAINT of the view: background + current frame. */
void pacer_paint(HDC dc, const RECT *client);
/* Display refresh rate changed (WM_DISPLAYCHANGE). */
void pacer_refresh_changed(void);
/* Effective frames per second x 10 for the current settings. */
int  pacer_effective_fps_x10(void);

#endif
