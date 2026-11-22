/* camtest.c - opens a webcam through camera.c, captures a few seconds and
 * reports device, format, frame rate and luminance statistics. Not part of
 * the automated tests (CI machines have no camera).
 *   cpwin_camtest [index] [seconds] [out.pgm]
 * The last frame is written as a binary PGM when out.pgm is given. */
#include "camera.h"
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    SRWLOCK lock;
    int frames, w, h, state, done;
    uint8_t *last;
    camera_info info;
    double mean;
} T = { SRWLOCK_INIT };

static void on_frame(void *u, const uint8_t *y, int w, int h)
{
    uint64_t sum = 0;
    (void)u;
    for (int i = 0; i < w * h; i += 7)
        sum += y[i];
    AcquireSRWLockExclusive(&T.lock);
    T.frames++;
    T.mean = (double)sum / ((w * h + 6) / 7);
    if (T.w != w || T.h != h) {
        free(T.last);
        T.last = (uint8_t *)malloc((size_t)w * h);
        T.w = w;
        T.h = h;
    }
    if (T.last)
        memcpy(T.last, y, (size_t)w * h);
    ReleaseSRWLockExclusive(&T.lock);
}

static void on_state(void *u, const camera_info *i)
{
    (void)u;
    AcquireSRWLockExclusive(&T.lock);
    T.info = *i;
    ReleaseSRWLockExclusive(&T.lock);
    printf("state %d: %d device(s), #%d \"%ls\" %dx%d %s %dfps %ls\n", i->state, i->count, i->index, i->name,
           i->width, i->height, i->format, i->fps, i->err);
    fflush(stdout);
}

int wmain(int argc, wchar_t **argv)
{
    int index = argc > 1 ? _wtoi(argv[1]) : 0, secs = argc > 2 ? _wtoi(argv[2]) : 3;
    camera_callbacks cb = { NULL, on_frame, on_state };
    camera *c;
    DWORD t0;
    compat_init();
    c = camera_open(index, &cb);
    if (!c) {
        printf("camera_open failed\n");
        return 1;
    }
    t0 = GetTickCount();
    while (GetTickCount() - t0 < (DWORD)secs * 1000) {
        Sleep(100);
        if (T.info.state == CAM_FAILED)
            break;
    }
    camera_close(c);
    printf("frames %d in %.1f s (%.1f fps), %dx%d, mean luma %.1f\n", T.frames, (GetTickCount() - t0) / 1000.0,
           T.frames * 1000.0 / (GetTickCount() - t0), T.w, T.h, T.mean);
    if (argc > 3 && T.last) {
        FILE *f = _wfopen(argv[3], L"wb");
        if (f) {
            fprintf(f, "P5\n%d %d\n255\n", T.w, T.h);
            fwrite(T.last, 1, (size_t)T.w * T.h, f);
            fclose(f);
        }
    }
    return T.frames > 0 ? 0 : 2;
}
