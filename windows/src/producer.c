/* producer.c - see producer.h */
#include "producer.h"
#include "app.h"
#include "config.h"
#include "frame_render.h"
#include "dbglog.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

static struct {
    SRWLOCK            lock;
    CONDITION_VARIABLE cv;
    HANDLE             thread;
    HWND               notify;
    BOOL               quit;

    /* requests from the UI thread */
    content           *req_content;
    BOOL               content_dirty;
    job_params         req;
    BOOL               params_dirty;

    /* ring (protected by lock) */
    uint8_t           *ring;
    int                ring_cap;     /* slots allocated               */
    int                ring_target;  /* slots to keep filled          */
    int                head, count;
    size_t             frame_bytes;
    frame_geom         geom;
    BOOL               busy;         /* preparing or playing          */
} P;

static void post_status(const job_status *st)
{
    job_status *m = (job_status *)malloc(sizeof *m);
    if (!m)
        return;
    *m = *st;
    if (!PostMessageW(P.notify, WM_APP_JOB, 0, (LPARAM)m))
        free(m);
}

static int ring_depth(int fps)
{
    return CPW_CLAMP(fps * 2, 4, 120);
}

/* lock held */
static BOOL ring_relayout(const job_params *p)
{
    frame_geom g;
    size_t fb;
    int cap = ring_depth(p->fps);
    frame_geom_calc(&g, p->version, p->count, p->quiet);
    fb = (size_t)g.stride * g.h;
    P.head = P.count = 0;
    if (fb != P.frame_bytes || cap > P.ring_cap) {
        uint8_t *r = (uint8_t *)realloc(P.ring, fb * cap);
        if (!r)
            return FALSE;
        P.ring = r;
        P.ring_cap = cap;
        P.frame_bytes = fb;
    }
    P.ring_target = cap;
    P.geom = g;
    return TRUE;
}

static BOOL session_differs(const job_params *a, const job_params *b)
{
    return a->proto != b->proto || a->version != b->version || a->ecc != b->ecc ||
           a->image_format != b->image_format || a->jpeg_quality != b->jpeg_quality;
}

static BOOL layout_differs(const job_params *a, const job_params *b)
{
    return a->count != b->count || a->quiet != b->quiet || a->version != b->version;
}

/* Builds payload + sender for `c` (producer thread, no lock). */
static cp_tx *make_session(const content *c, const job_params *p, cp_payload **out_pl)
{
    job_status st;
    build_result br;
    build_opts bo;
    cp_tx *tx = NULL;

    *out_pl = NULL;
    memset(&st, 0, sizeof st);
    st.kind = content_kind(c);
    st.nfiles = c->nfiles;
    st.estimate = c->raw_size;

    st.state = JOB_PREPARING;
    post_status(&st);

    bo.image_format = p->image_format;
    bo.jpeg_quality = p->jpeg_quality;
    bo.max_bytes = CFG_HARD_CAP_BYTES;
    if (!content_build(c, &bo, &br)) {
        st.state = JOB_ERROR;
        lstrcpynW(st.err, br.err[0] ? br.err : L"打包失败", (int)CPW_COUNTOF(st.err));
        post_status(&st);
        return NULL;
    }
    if (CFG_PROTO_IS_DECIMEN(p->proto)) {
        /* Decimen v3: every frame fills the QR version at the chosen ECC level */
        cp_tx_opts o;
        memset(&o, 0, sizeof o);
        o.d3_fec = p->proto == CFG_PROTO_DECIMEN3_LT ? CP_D3_FEC_LT : CP_D3_FEC_RS96;
        tx = cp_tx_new(CP_PROTO_D3, br.payload, qr_byte_capacity(p->version, p->ecc), &o);
    } else {
        tx = cp_tx_new(CP_PROTO_RQ, br.payload, qr_byte_capacity(p->version, p->ecc), NULL);
    }
    if (!tx) {
        cp_payload_free(br.payload);
        st.state = JOB_ERROR;
        if (CFG_PROTO_IS_DECIMEN(p->proto))
            wfmt(st.err, CPW_COUNTOF(st.err), L"无法建立 Decimen 会话(内容为空、超过 64 MB,或二维码版本太小)");
        else
            wfmt(st.err, CPW_COUNTOF(st.err), L"无法建立发送会话(内容过大或二维码版本太小)");
        post_status(&st);
        return NULL;
    }
    st.state = JOB_PLAYING;
    lstrcpynA(st.mime, br.mime, sizeof st.mime);
    st.img_w = br.img_w;
    st.img_h = br.img_h;
    cp_payload_info_get(br.payload, &st.pinfo);
    cp_tx_info_get(tx, &st.tinfo);
    if (st.tinfo.proto == CP_PROTO_D3) {   /* the status line shows what is on the wire */
        st.pinfo.container_len = st.tinfo.F;
        st.pinfo.compressed = st.tinfo.d3_gzip;
    }
    post_status(&st);
    DBG("session %08x F=%llu T=%u Z=%u K=%u", st.tinfo.session_id, (unsigned long long)st.tinfo.F,
        st.tinfo.T, st.tinfo.Z, st.tinfo.total_source_symbols);
    *out_pl = br.payload;
    return tx;
}

static DWORD WINAPI producer_main(void *arg)
{
    content *cur = NULL;
    cp_payload *pl = NULL;
    cp_tx *tx = NULL;
    job_params cp = { 0 };
    uint8_t *scratch = NULL;
    size_t scratch_cap = 0;
    (void)arg;

    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    AcquireSRWLockExclusive(&P.lock);
    cp = P.req;
    for (;;) {
        while (!P.quit && !P.content_dirty && !P.params_dirty && (!tx || P.count >= P.ring_target))
            SleepConditionVariableSRW(&P.cv, &P.lock, INFINITE, 0);
        if (P.quit)
            break;

        if (P.content_dirty || P.params_dirty) {
            BOOL rebuild = P.content_dirty || session_differs(&cp, &P.req);
            BOOL relayout = rebuild || layout_differs(&cp, &P.req) || ring_depth(cp.fps) != ring_depth(P.req.fps);
            content *old = NULL;
            if (P.content_dirty) {
                old = cur;
                cur = P.req_content;
                P.req_content = NULL;
            }
            cp = P.req;
            P.content_dirty = P.params_dirty = FALSE;
            if (relayout)
                ring_relayout(&cp);
            if (!rebuild) {
                P.count = P.head = 0;   /* flush, keep session */
                continue;
            }
            P.busy = cur != NULL;
            ReleaseSRWLockExclusive(&P.lock);

            content_free(old);
            cp_tx_free(tx);
            cp_payload_free(pl);
            tx = NULL;
            pl = NULL;
            if (cur) {
                tx = make_session(cur, &cp, &pl);
            } else {
                job_status st;
                memset(&st, 0, sizeof st);
                st.state = JOB_IDLE;
                post_status(&st);
            }

            AcquireSRWLockExclusive(&P.lock);
            P.busy = tx != NULL;
            P.count = P.head = 0;
            continue;
        }

        /* render one frame outside the lock */
        {
            frame_geom g = P.geom;
            size_t fb = P.frame_bytes;
            int fixed_mask = cp.fps * g.count >= 90;
            int ok;
            ReleaseSRWLockExclusive(&P.lock);
            if (scratch_cap < fb) {
                uint8_t *s = (uint8_t *)realloc(scratch, fb);
                if (s) {
                    scratch = s;
                    scratch_cap = fb;
                }
            }
            ok = scratch_cap >= fb && frame_render(tx, &g, cp.ecc, fixed_mask, scratch);
            if (!ok && cur) {
                /* ESI space exhausted (≈70 h of playing) or a fatal error:
                 * start over with a fresh session of the same content. */
                cp_tx_free(tx);
                cp_payload_free(pl);
                tx = make_session(cur, &cp, &pl);
            }
            AcquireSRWLockExclusive(&P.lock);
            if (!tx)
                P.busy = FALSE;
            if (ok && !P.content_dirty && !P.params_dirty && fb == P.frame_bytes &&
                P.count < P.ring_cap) {
                int slot = (P.head + P.count) % P.ring_cap;
                memcpy(P.ring + (size_t)slot * fb, scratch, fb);
                P.count++;
            }
        }
    }
    ReleaseSRWLockExclusive(&P.lock);
    cp_tx_free(tx);
    cp_payload_free(pl);
    content_free(cur);
    free(scratch);
    return 0;
}

BOOL producer_start(HWND notify, const job_params *p)
{
    InitializeSRWLock(&P.lock);
    InitializeConditionVariable(&P.cv);
    P.notify = notify;
    P.req = *p;
    AcquireSRWLockExclusive(&P.lock);
    ring_relayout(p);
    ReleaseSRWLockExclusive(&P.lock);
    P.thread = CreateThread(NULL, 0, producer_main, NULL, 0, NULL);
    return P.thread != NULL;
}

void producer_stop(void)
{
    if (!P.thread)
        return;
    AcquireSRWLockExclusive(&P.lock);
    P.quit = TRUE;
    WakeAllConditionVariable(&P.cv);
    ReleaseSRWLockExclusive(&P.lock);
    /* a large content_build cannot be interrupted; don't hang the exit on it */
    if (WaitForSingleObject(P.thread, 3000) == WAIT_OBJECT_0) {
        CloseHandle(P.thread);
        content_free(P.req_content);
        P.req_content = NULL;
        free(P.ring);
        P.ring = NULL;
    }
    P.thread = NULL;
}

void producer_submit(content *c)
{
    content *old;
    AcquireSRWLockExclusive(&P.lock);
    old = P.req_content;
    P.req_content = c;
    P.content_dirty = TRUE;
    P.busy = c != NULL;
    P.count = P.head = 0;
    WakeAllConditionVariable(&P.cv);
    ReleaseSRWLockExclusive(&P.lock);
    content_free(old);
}

void producer_set_params(const job_params *p)
{
    AcquireSRWLockExclusive(&P.lock);
    if (memcmp(&P.req, p, sizeof *p) != 0) {
        P.req = *p;
        P.params_dirty = TRUE;
        WakeAllConditionVariable(&P.cv);
    }
    ReleaseSRWLockExclusive(&P.lock);
}

int producer_pop(uint8_t **dst, size_t *cap, frame_geom *g)
{
    int got = 0;
    AcquireSRWLockExclusive(&P.lock);
    if (P.count > 0 && !P.content_dirty && !P.params_dirty) {
        if (*cap < P.frame_bytes) {
            uint8_t *n = (uint8_t *)realloc(*dst, P.frame_bytes);
            if (n) {
                *dst = n;
                *cap = P.frame_bytes;
            }
        }
        if (*cap >= P.frame_bytes) {
            memcpy(*dst, P.ring + (size_t)P.head * P.frame_bytes, P.frame_bytes);
            *g = P.geom;
            P.head = (P.head + 1) % P.ring_cap;
            P.count--;
            got = 1;
            WakeAllConditionVariable(&P.cv);
        }
    }
    ReleaseSRWLockExclusive(&P.lock);
    return got;
}

BOOL producer_busy(void)
{
    BOOL b;
    AcquireSRWLockShared(&P.lock);
    b = P.busy;
    ReleaseSRWLockShared(&P.lock);
    return b;
}
