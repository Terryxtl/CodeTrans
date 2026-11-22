/* receiver.h - phone -> PC: camera frames -> scanner -> cp_rx (PLAN: reverse direction).
 *
 * One capture thread and up to four decoder workers. A full worker-count + 2
 * buffer pool drops incoming analysis frames while preview stays live. Results
 * reach the notify window as WM_APP_RECV messages:
 *   wParam RECV_MSG_FRAME   new preview frame (repaint; then receiver_frame_seen)
 *   wParam RECV_MSG_STATE   camera state / progress changed (re-read receiver_status)
 *   wParam RECV_MSG_RESULT  lParam = cp_result * (the window frees it, cp_result_free)
 *   wParam RECV_MSG_PLAIN   lParam = recv_plain * (free())
 *   wParam RECV_MSG_FAILED  a transfer failed verification and restarts */
#ifndef CPW_RECEIVER_H
#define CPW_RECEIVER_H

#include <windows.h>
#include "camera.h"
#include "cp.h"
#include "scanner.h"

enum { RECV_MSG_FRAME = 1, RECV_MSG_STATE, RECV_MSG_RESULT, RECV_MSG_PLAIN, RECV_MSG_FAILED };

typedef struct recv_plain {
    size_t n;
    char   text[1];   /* n bytes + NUL */
} recv_plain;

typedef struct recv_status {
    camera_info cam;
    cp_progress progress;
    int         receiving;     /* a transfer is in progress (symbols arriving)  */
    int         located, decoded; /* codes in the last scanned frame          */
    uint64_t    frames, scanned;  /* camera frames / frames scanned            */
    double      scan_ms;          /* average scan time                         */
} recv_status;

typedef struct receiver receiver;

/* out_dir: UTF-8, results go to <out_dir>/<session>/. NULL on failure. */
receiver *receiver_start(HWND notify, UINT msg, int camera_index, const char *out_dir);
void      receiver_stop(receiver *r);
void      receiver_status(receiver *r, recv_status *out);
/* Copies the newest frame (w*h luminance) and the code outlines as of the last
 * scan (steadied by cp_quad_tracker: held through missed frames, smoothed,
 * one-off unread sightings hidden), scaled to that frame. Returns 0 when there
 * is no frame yet. */
int       receiver_preview(receiver *r, uint8_t **buf, size_t *cap, int *w, int *h,
                           cp_scan_quad *quads, int *nquads);
/* The window painted the last RECV_MSG_FRAME: the next frame may post again. */
void      receiver_frame_seen(receiver *r);

/* Feeds one luminance frame as if the camera had delivered it (tests). */
void      receiver_feed(receiver *r, const uint8_t *y, int w, int h);

#endif
