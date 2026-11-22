/* camera.h - webcam capture for the phone -> PC receiver.
 *
 * Media Foundation source reader (Windows 7+), loaded at run time: the exe
 * keeps importing only system DLLs and still starts on Windows N editions
 * without the Media Feature Pack (the camera then reports CAM_FAILED).
 *
 * Everything runs on one capture thread per camera: opening the device,
 * choosing a format and delivering frames as tightly packed 8-bit luminance. */
#ifndef CPW_CAMERA_H
#define CPW_CAMERA_H

#include <windows.h>
#include <stdint.h>

enum { CAM_OPENING = 0, CAM_RUNNING = 1, CAM_FAILED = 2, CAM_STOPPED = 3 };

typedef struct camera_info {
    int     state;           /* CAM_*                                   */
    int     count;           /* video capture devices found             */
    int     index;           /* the one opened (index % count)          */
    int     width, height;   /* frame size once running                 */
    int     fps;             /* nominal frame rate of the chosen format */
    char    format[8];       /* native format: "NV12", "YUY2", "MJPG"... */
    wchar_t name[128];       /* device friendly name                    */
    wchar_t err[160];        /* CAM_FAILED: why                         */
} camera_info;

typedef struct camera_callbacks {
    void *user;
    /* capture thread; y is w*h bytes (row stride w), valid during the call */
    void (*on_frame)(void *user, const uint8_t *y, int w, int h);
    /* capture thread; state changes (opening -> running / failed / stopped) */
    void (*on_state)(void *user, const camera_info *info);
} camera_callbacks;

typedef struct camera camera;

/* Starts the capture thread for device `index` (wraps around the device count).
 * Never blocks on the device; failures arrive through on_state. NULL only if
 * the thread cannot be created. */
camera *camera_open(int index, const camera_callbacks *cb);
/* Stops capturing and waits for the thread (bounded); no callback runs after
 * this returns. */
void camera_close(camera *c);

/* Exposed for tests: luminance of one frame in a native layout. */
enum { CAM_FMT_NV12 = 1, CAM_FMT_YUY2 = 2, CAM_FMT_RGB32 = 3 };
void camera_extract_luma(int fmt, const uint8_t *scan0, LONG pitch, int w, int h, uint8_t *y);

typedef struct { int w, h, fps, native_luma; } camera_mode;
int camera_prefer_mode(camera_mode candidate, camera_mode current);

#endif
