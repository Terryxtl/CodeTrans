/* receiver.c - see receiver.h */
#include "receiver.h"
#include "confirmed_regions.h"
#include "quad_tracker.h"
#include <stdlib.h>
#include <string.h>

enum { MAX_WORKERS = 4, FRAME_SLOTS = MAX_WORKERS + 2 };
typedef struct {
    uint8_t *pixels;
    size_t capacity;
    int w, h;
    enum { FREE_FRAME, QUEUED, SCANNING } state;
    uint64_t number, captured_at;
} scan_frame;

typedef struct {
    receiver *owner;
    cp_scanner *scanner;
    HANDLE thread;
} decode_worker;

struct receiver {
    HWND          notify;
    UINT          msg;
    camera       *cam;
    cp_rx        *rx;
    decode_worker workers[MAX_WORKERS];
    int           nworkers;
    CONDITION_VARIABLE ready;
    volatile LONG frame_posted;   /* RECV_MSG_FRAME in flight */

    SRWLOCK       lock;           /* everything below */
    int           quit;
    scan_frame    frames[FRAME_SLOTS];
    cp_scan_session session;
    cp_quad_tracker tracker;
    uint64_t      geometry_start, last_display;
    int           width, height;
    DWORD         last_state;
    uint8_t      *preview;        /* newest frame, for painting */
    size_t        preview_cap;
    int           vw, vh;
    cp_scan_quad  quads[CP_SCAN_MAX_QUADS];
    int           nquads, qw, qh;
    recv_status   st;
    uint64_t      scan_ns;
};

static void post(receiver *r, WPARAM what, void *payload)
{
    if (!PostMessageW(r->notify, r->msg, what, (LPARAM)payload)) {
        if (what == RECV_MSG_RESULT)
            cp_result_free((cp_result *)payload);
        else
            free(payload);
    }
}

static int grow(uint8_t **buf, size_t *cap, size_t need)
{
    if (*cap >= need)
        return 1;
    uint8_t *p = (uint8_t *)realloc(*buf, need);
    if (!p)
        return 0;
    *buf = p;
    *cap = need;
    return 1;
}

static void session_lock(void *user)
{
    AcquireSRWLockExclusive(&((receiver *)user)->lock);
}

static void session_unlock(void *user)
{
    ReleaseSRWLockExclusive(&((receiver *)user)->lock);
}

static void on_frame(void *user, const uint8_t *y, int w, int h)
{
    receiver_feed((receiver *)user, y, w, h);
}

void receiver_feed(receiver *r, const uint8_t *y, int w, int h)
{
    if (!y || w <= 0 || h <= 0 || w > CP_SCAN_MAX_DIM || h > CP_SCAN_MAX_DIM) return;
    size_t n = (size_t)w * h;
    AcquireSRWLockExclusive(&r->lock);
    r->st.frames++;
    if (w != r->width || h != r->height) {
        r->width = w;
        r->height = h;
        r->geometry_start = r->st.frames;
        cp_scan_session_reset(&r->session, r->st.frames);
        cp_quad_tracker_reset(&r->tracker);
        r->nquads = 0;
    }
    for (int i = 0; i < r->nworkers + 2; ++i) {
        scan_frame *f = &r->frames[i];
        if (f->state != FREE_FRAME) continue;
        if (!grow(&f->pixels, &f->capacity, n)) break;
        memcpy(f->pixels, y, n);
        f->w = w;
        f->h = h;
        f->number = r->st.frames;
        f->captured_at = GetTickCount64();
        f->state = QUEUED;
        WakeConditionVariable(&r->ready);
        break;
    }
    if (grow(&r->preview, &r->preview_cap, n)) {
        memcpy(r->preview, y, n);
        r->vw = w;
        r->vh = h;
    }
    ReleaseSRWLockExclusive(&r->lock);
    if (r->notify && InterlockedCompareExchange(&r->frame_posted, 1, 0) == 0)
        if (!PostMessageW(r->notify, r->msg, RECV_MSG_FRAME, 0))
            InterlockedExchange(&r->frame_posted, 0);
}

static void on_state(void *user, const camera_info *info)
{
    receiver *r = (receiver *)user;
    AcquireSRWLockExclusive(&r->lock);
    r->st.cam = *info;
    ReleaseSRWLockExclusive(&r->lock);
    if (r->notify)
        PostMessageW(r->notify, r->msg, RECV_MSG_STATE, 0);
}

/* Called under the receiver lock. Workers take the oldest accepted frame. */
static scan_frame *next_frame(receiver *r)
{
    scan_frame *oldest = NULL;
    for (int i = 0; i < r->nworkers + 2; ++i) {
        scan_frame *f = &r->frames[i];
        if (f->state == QUEUED && (!oldest || f->number < oldest->number))
            oldest = f;
    }
    return oldest;
}

static DWORD WINAPI decode_main(void *arg)
{
    decode_worker *worker = (decode_worker *)arg;
    receiver *r = worker->owner;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    for (;;) {
        scan_frame *frame;
        int codes, ev, nq;
        cp_scan_quad q[CP_SCAN_MAX_QUADS];
        AcquireSRWLockExclusive(&r->lock);
        while (!r->quit && !(frame = next_frame(r)))
            SleepConditionVariableSRW(&r->ready, &r->lock, INFINITE, 0);
        if (r->quit) {
            ReleaseSRWLockExclusive(&r->lock);
            break;
        }
        frame->state = SCANNING;
        ReleaseSRWLockExclusive(&r->lock);

        QueryPerformanceCounter(&t0);
        ev = cp_scan_session_step(&r->session, worker->scanner, r->rx, frame->pixels, frame->w, frame->h,
                                  frame->number, frame->captured_at, &codes, q, &nq);
        QueryPerformanceCounter(&t1);

        AcquireSRWLockExclusive(&r->lock);
        if (frame->number >= r->geometry_start) {
            if (frame->number > r->last_display) {
                r->last_display = frame->number;
                r->qw = frame->w;
                r->qh = frame->h;
                cp_quad_tracker_update(&r->tracker, frame->number, frame->w, frame->h, 0, q, nq, frame->captured_at);
                r->nquads = cp_quad_tracker_get(&r->tracker, frame->captured_at, r->quads, CP_SCAN_MAX_QUADS);
                r->st.located = cp_scanner_last_detected(worker->scanner);
                r->st.decoded = codes < 0 ? 0 : codes;
            }
        }
        frame->state = FREE_FRAME;
        r->st.scanned++;
        r->scan_ns += (uint64_t)((t1.QuadPart - t0.QuadPart) * 1000000000.0 / (double)f.QuadPart);
        r->st.scan_ms = r->scan_ns / 1e6 / (double)r->st.scanned;
        cp_rx_progress(r->rx, &r->st.progress);
        r->st.receiving = r->st.progress.need > 0;
        if ((ev & CP_EV_PROGRESS) && GetTickCount() - r->last_state >= 100) {
            r->last_state = GetTickCount();
            post(r, RECV_MSG_STATE, NULL);
        }
        ReleaseSRWLockExclusive(&r->lock);

        if (ev & CP_EV_COMPLETE) {
            cp_result *res = cp_rx_take_result(r->rx);
            if (res)
                post(r, RECV_MSG_RESULT, res);
        }
        if (ev & CP_EV_PLAIN) {
            size_t n = 0;
            uint8_t *p;
            while ((p = cp_rx_take_plain(r->rx, &n)) != NULL) {
                recv_plain *m = (recv_plain *)malloc(sizeof *m + n);
                if (m) {
                    m->n = n;
                    memcpy(m->text, p, n);
                    m->text[n] = 0;
                    post(r, RECV_MSG_PLAIN, m);
                }
                cp_free(p);
            }
        }
        if (ev & CP_EV_FAILED)
            post(r, RECV_MSG_FAILED, NULL);
    }
    return 0;
}

receiver *receiver_start(HWND notify, UINT msg, int camera_index, const char *out_dir)
{
    receiver *r = (receiver *)calloc(1, sizeof *r);
    cp_rx_config cfg;
    camera_callbacks cb;
    SYSTEM_INFO sys;
    if (!r)
        return NULL;
    InitializeSRWLock(&r->lock);
    InitializeConditionVariable(&r->ready);
    r->notify = notify;
    r->msg = msg;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = out_dir;
    cfg.enable_d3 = 1;
    cfg.enable_plain = 1;
    r->rx = cp_rx_new(&cfg);
    cp_quad_tracker_init(&r->tracker);
    cp_scan_session_init(&r->session, &(cp_scan_lock){session_lock, session_unlock, r});
    if (!r->rx)
        goto fail;
    GetSystemInfo(&sys);
    r->nworkers = sys.dwNumberOfProcessors < MAX_WORKERS ? (int)sys.dwNumberOfProcessors : MAX_WORKERS;
    for (int i = 0; i < r->nworkers; ++i) {
        decode_worker *worker = &r->workers[i];
        worker->owner = r;
        worker->scanner = cp_scanner_new();
        if (!worker->scanner) goto fail;
        worker->thread = CreateThread(NULL, 0, decode_main, worker, 0, NULL);
        if (!worker->thread) goto fail;
    }
    if (camera_index >= 0) {   /* < 0: no camera (tests feed frames) */
        cb.user = r;
        cb.on_frame = on_frame;
        cb.on_state = on_state;
        r->cam = camera_open(camera_index, &cb);
        if (!r->cam) goto fail;
    }
    return r;
fail:
    receiver_stop(r);
    return NULL;
}

void receiver_stop(receiver *r)
{
    if (!r)
        return;
    camera_close(r->cam);          /* no frame callback after this */
    AcquireSRWLockExclusive(&r->lock);
    r->quit = 1;
    WakeAllConditionVariable(&r->ready);
    ReleaseSRWLockExclusive(&r->lock);
    for (int i = 0; i < r->nworkers; ++i) {
        decode_worker *worker = &r->workers[i];
        if (worker->thread) {
            WaitForSingleObject(worker->thread, INFINITE);
            CloseHandle(worker->thread);
        }
        cp_scanner_free(worker->scanner);
    }
    cp_rx_free(r->rx);
    for (int i = 0; i < FRAME_SLOTS; ++i) free(r->frames[i].pixels);
    free(r->preview);
    free(r);
}

void receiver_status(receiver *r, recv_status *out)
{
    AcquireSRWLockShared(&r->lock);
    *out = r->st;
    ReleaseSRWLockShared(&r->lock);
}

int receiver_preview(receiver *r, uint8_t **buf, size_t *cap, int *w, int *h, cp_scan_quad *quads, int *nquads)
{
    int ok = 0;
    AcquireSRWLockShared(&r->lock);
    if (r->vw > 0 && grow(buf, cap, (size_t)r->vw * r->vh)) {
        memcpy(*buf, r->preview, (size_t)r->vw * r->vh);
        *w = r->vw;
        *h = r->vh;
        /* outlines from the last scan; only meaningful for frames of that size */
        *nquads = (r->qw == r->vw && r->qh == r->vh) ? r->nquads : 0;
        memcpy(quads, r->quads, sizeof quads[0] * (size_t)*nquads);
        ok = 1;
    }
    ReleaseSRWLockShared(&r->lock);
    return ok;
}

void receiver_frame_seen(receiver *r)
{
    InterlockedExchange(&r->frame_posted, 0);
}
