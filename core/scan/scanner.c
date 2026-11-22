/*
 * Full-frame discovery seeds the camera session's confirmed grids. Following frames
 * sample the camera Y plane at the confirmed grids, with local reacquisition
 * on a miss and periodic discovery to pick up additional codes. All paths
 * verify the currently sampled QR payload before passing it to cp_rx_push().
 *
 * Contrast retry (after Decimen 0.5.3-fixed receive/contrast-retry.ts): when a
 * frame decodes nothing, or only some of the codes it located (a sharp code
 * next to a blurred one), re-binarise it once at the original resolution with
 * a global Otsu threshold applied to a high-boost sharpened luminance
 * (12*Y - 4 neighbours at distance 2, i.e. gain 0.5) and run quirc again. This
 * rescues moderately blurred captures with moire stripes, which quirc's own
 * thresholding loses. Throttled per scanner, and only attempted while codes
 * are in view or a transfer is running, so an idle camera costs nothing extra.
 *
 * Optical boards (CP_PROTO_OB, experimental non-QR format, core/src/ob*.c):
 * boards found by cp_ob_locate are kept per scanner and re-fitted on every
 * frame with cp_ob_track; each locked board is sampled and handed to
 * cp_rx_push_board. A cold search runs while nothing is locked (every
 * OB_SEARCH_ACTIVE_MS while codes are seen or a transfer is running, else
 * every OB_SEARCH_IDLE_MS), at once when a locked board is lost, and every
 * OB_SEARCH_MORE_MS to pick up further boards. Locked boards count as codes in
 * view, so they do not trigger the QR full-frame paths on every frame.
 */
#include "scanner.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include "quirc.h"
#include "tracked_grid.h"
#include "camera_scan.h"
#ifdef CP_WITH_ZXING
#include "qr_fallback.h"
#endif

#define RETRY_INTERVAL_MS 100
#define RETRY_MAX_PIXELS  (2304u * 1296u)   /* up to 1080p/1296p analysis frames */
#define RETRY_ACTIVE_MS   2000u             /* "a transfer is running" window   */
#define DISCOVERY_INTERVAL_MS 350u
#define DISCOVERY_FRAMES 12
#define OB_MAX_BOARDS 6
#define OB_SEARCH_ACTIVE_MS 120u
#define OB_SEARCH_IDLE_MS 400u
#define OB_SEARCH_MORE_MS 1000u
#define EXPECT_MS 1500u   /* how long a frame of n codes keeps n "expected" */

struct cp_scanner {
    struct quirc      *q;
    struct quirc      *crop;
    int                crop_w, crop_h;
    cp_grid            anchors[CP_SCAN_MAX_QUADS];
    int                nanchors, tracked_frames, missing;
    uint64_t           last_discovery;
    uint64_t           last_fallback;
    int                w, h;
    int                detected;  /* quirc_count of the last frame */
    int                retry_ms;  /* < 0: off, 0: every frame       */
    uint64_t           last_retry, last_ok;
    unsigned           retry_hits;
    /* most codes decoded in one frame lately (~ the sender's layout); a frame
     * reading fewer is partial even when quirc did not locate the others */
    int                expect;
    uint64_t           expect_at;
    struct quirc_code  code;   /* ~3.9 KB: kept here, not on the stack */
    struct quirc_data  data;   /* ~8.9 KB */
    /* codes located in the last frame (for the on-screen boxes); the contrast
     * retry pass records into pass[1] and replaces pass[0] if it decoded more */
    cp_scan_quad       pass[2][CP_SCAN_MAX_QUADS];
    int                npass[2], shown;
    cp_scan_cb         cb;
    void              *user;
    /* optical boards */
    cp_ob_quad         ob[OB_MAX_BOARDS];
    int                ob_units[OB_MAX_BOARDS];
    int                nob;
    uint64_t           last_ob_search;
    float             *ob_luma;   /* CP_OB_SIDE^2 samples */
};

static uint64_t mono_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

cp_scanner *cp_scanner_new(void)
{
    cp_scanner *s = (cp_scanner *)calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->q = quirc_new();
    if (!s->q) {
        free(s);
        return NULL;
    }
    s->retry_ms = RETRY_INTERVAL_MS;
    return s;
}

void cp_scanner_free(cp_scanner *s)
{
    if (!s)
        return;
    quirc_destroy(s->q);
    if (s->crop)
        quirc_destroy(s->crop);
    free(s->ob_luma);
    free(s);
}

int cp_scanner_last_detected(const cp_scanner *s)
{
    return s ? s->detected : 0;
}

/* strictly inside the (convex) quad, either winding */
static int point_in_quad(const cp_scan_quad *q, int px, int py)
{
    int pos = 0, neg = 0;
    for (int k = 0; k < 4; k++) {
        int j = (k + 1) & 3;
        long long c = (long long)(q->x[j] - q->x[k]) * (py - q->y[k]) -
                      (long long)(q->y[j] - q->y[k]) * (px - q->x[k]);
        if (c > 0)
            pos++;
        else if (c < 0)
            neg++;
        else
            return 0;
    }
    return pos == 4 || neg == 4;
}

static int quads_overlap(const cp_scan_quad *a, const cp_scan_quad *b)
{
    for (int k = 0; k < 4; k++)
        if (point_in_quad(b, a->x[k], a->y[k]) || point_in_quad(a, b->x[k], b->y[k]))
            return 1;
    return 0;
}

static long long quad_area2(const cp_scan_quad *q)
{
    long long a = 0;
    for (int k = 0; k < 4; k++) {
        int j = (k + 1) & 3;
        a += (long long)q->x[k] * q->y[j] - (long long)q->x[j] * q->y[k];
    }
    return a < 0 ? -a : a;
}

/* quirc sometimes groups finder-like patterns of two neighbouring codes into a
 * phantom third code lying across both. Real codes never overlap, so while two
 * located codes overlap, drop one: an unread one before a read one, then the
 * one overlapping the most others, then the larger one. */
static int drop_phantoms(cp_scan_quad *q, int n)
{
    for (;;) {
        int worst = -1, worst_ov = 0;
        for (int i = 0; i < n; i++) {
            int ov = 0;
            if (q[i].decoded)
                continue;
            for (int j = 0; j < n; j++)
                if (j != i && quads_overlap(&q[i], &q[j]))
                    ov++;
            if (ov == 0)
                continue;
            if (worst < 0 || ov > worst_ov || (ov == worst_ov && quad_area2(&q[i]) > quad_area2(&q[worst]))) {
                worst = i;
                worst_ov = ov;
            }
        }
        if (worst < 0)
            return n;
        q[worst] = q[--n];
    }
}

int cp_scanner_last_quads(const cp_scanner *s, cp_scan_quad *out, int max)
{
    cp_scan_quad q[CP_SCAN_MAX_QUADS];
    int n, i;
    if (!s || !out || max <= 0)
        return 0;
    n = s->npass[s->shown];
    for (i = 0; i < n; i++)
        q[i] = s->pass[s->shown][i];
    n = drop_phantoms(q, n);
    if (n > max)
        n = max;
    for (i = 0; i < n; i++)
        out[i] = q[i];
    return n;
}

void cp_scanner_set_callback(cp_scanner *s, cp_scan_cb cb, void *user)
{
    if (!s)
        return;
    s->cb = cb;
    s->user = user;
}

void cp_scanner_set_retry_interval(cp_scanner *s, int ms)
{
    if (s)
        s->retry_ms = ms;
}

unsigned cp_scanner_retry_hits(const cp_scanner *s)
{
    return s ? s->retry_hits : 0;
}

typedef struct { int x0, y0, x1, y1; } scan_box;

/* Otsu on every 16th pixel, then threshold the high-boost image. Writes a
 * 0/255 image into dst; 0 when the frame has no usable contrast. Pixels inside
 * `skip` (bounding boxes of codes already read) are left out of the histogram:
 * a sharp code's black and white would otherwise pull the threshold away from
 * what the blurred codes still missing need. */
static int contrast_retry(const uint8_t *y, int w, int h, uint8_t *dst, const scan_box *skip, int nskip)
{
    size_t len = (size_t)w * (size_t)h, i;
    uint32_t hist[256];
    double sum = 0, samples = 0, lower_n = 0, lower_sum = 0, best = 0;
    int thr = 0, v, r, x;

    if (len < 64 || len > RETRY_MAX_PIXELS || w < 5 || h < 5)
        return 0;
    memset(hist, 0, sizeof hist);
    for (i = 0; i < len; i += 16) {
        if (nskip) {
            int px = (int)(i % (size_t)w), py = (int)(i / (size_t)w), k;
            for (k = 0; k < nskip; k++)
                if (px >= skip[k].x0 && px <= skip[k].x1 && py >= skip[k].y0 && py <= skip[k].y1)
                    break;
            if (k < nskip)
                continue;
        }
        hist[y[i]]++;
        sum += y[i];
        samples++;
    }
    if (samples < 64)
        return 0;
    for (v = 0; v < 255; v++) {
        double diff, score;
        lower_n += hist[v];
        lower_sum += (double)v * hist[v];
        if (lower_n == 0 || lower_n == samples)
            continue;
        diff = lower_n * sum - samples * lower_sum;
        score = diff * diff / (lower_n * (samples - lower_n));
        if (score > best) {
            best = score;
            thr = v;
        }
    }
    if (best < 36.0 * samples * samples)   /* class means closer than ~12 levels */
        return 0;
    for (r = 0; r < h; r++) {
        const uint8_t *row = y + (size_t)r * w;
        const uint8_t *above = y + (size_t)(r >= 2 ? r - 2 : 0) * w;
        const uint8_t *below = y + (size_t)(r + 2 < h ? r + 2 : h - 1) * w;
        uint8_t *out = dst + (size_t)r * w;
        for (x = 0; x < w; x++) {
            int nb = above[x] + below[x] + row[x >= 2 ? x - 2 : 0] + row[x + 2 < w ? x + 2 : w - 1];
            out[x] = 12 * row[x] - nb <= 8 * thr ? 0 : 255;
        }
    }
    return 1;
}

static int decode_sample(cp_scanner *s)
{
    quirc_decode_error_t err;
    err = quirc_decode(&s->code, &s->data);
    if (err == QUIRC_ERROR_FORMAT_ECC || err == QUIRC_ERROR_DATA_ECC) {
        quirc_flip(&s->code);
        err = quirc_decode(&s->code, &s->data);
    }
    return err == QUIRC_SUCCESS && s->data.payload_len > 0 && s->data.payload_len <= QUIRC_MAX_PAYLOAD;
}

static void record_code(cp_scanner *s, int pass, int good)
{
    if (s->npass[pass] == CP_SCAN_MAX_QUADS) {
        if (!good) return;
        int i = 0;
        while (i < CP_SCAN_MAX_QUADS && s->pass[pass][i].decoded) i++;
        if (i == CP_SCAN_MAX_QUADS) return;
        s->pass[pass][i] = s->pass[pass][--s->npass[pass]];
    }
    cp_scan_quad *q = &s->pass[pass][s->npass[pass]];
    q->decoded = good;
    for (int k = 0; k < 4; k++) {
        q->x[k] = s->code.corners[k].x;
        q->y[k] = s->code.corners[k].y;
        if (!good && (q->x[k] < -s->w/2 || q->x[k] > s->w + s->w/2 ||
                      q->y[k] < -s->h/2 || q->y[k] > s->h + s->h/2)) return;
    }
    s->npass[pass]++;
}

static void accept_code(cp_scanner *s, cp_rx *rx, int *ev)
{
    if (s->nanchors < CP_SCAN_MAX_QUADS) {
        cp_grid *grid = &s->anchors[s->nanchors++];
        memcpy(grid->corners, s->code.corners, sizeof grid->corners);
        grid->size = s->code.size;
        grid->drift = 0;
    }
    if (s->cb) s->cb(s->user, s->data.payload, (size_t)s->data.payload_len);
    if (rx) *ev |= cp_rx_push(rx, s->data.payload, (size_t)s->data.payload_len);
}

/* A local retry and a discovery scan can see the same physical symbol.
 * De-duplicate by position only within this frame; neighbouring equal payloads
 * are still two distinct detections. */
static int already_read(const cp_scanner *s, int count)
{
    if (!count) return 0;
    int64_t x = 0, y = 0;
    for (int k = 0; k < 4; k++) {
        x += s->code.corners[k].x;
        y += s->code.corners[k].y;
    }
    for (int i = 0; i < count; i++)
        if (point_in_quad(&s->pass[0][i], (int)(x/4), (int)(y/4))) return 1;
    return 0;
}

/* The centre of s->code lies in a code of pass[0] that was already read. */
static int covered_by_decoded(const cp_scanner *s, int *cx, int *cy)
{
    int64_t x = 0, y = 0;
    for (int k = 0; k < 4; k++) {
        x += s->code.corners[k].x;
        y += s->code.corners[k].y;
    }
    *cx = (int)(x / 4);
    *cy = (int)(y / 4);
    for (int i = 0; i < s->npass[0]; i++)
        if (s->pass[0][i].decoded && point_in_quad(&s->pass[0][i], *cx, *cy)) return 1;
    return 0;
}

/* Contrast retry of a frame where only some located codes decoded: quirc ran
 * on the re-binarised image; decode only the codes not read yet and turn their
 * undecoded boxes of pass[0] into decoded ones (the codes read before keep
 * theirs). Returns the codes rescued. */
static int decode_missing(cp_scanner *s, cp_rx *rx, int *ev)
{
    int n = quirc_count(s->q), ok = 0, cx, cy;
    for (int i = 0; i < n; i++) {
        quirc_extract(s->q, i, &s->code);
        if (covered_by_decoded(s, &cx, &cy) || !decode_sample(s))
            continue;
        for (int j = s->npass[0] - 1; j >= 0; j--)
            if (!s->pass[0][j].decoded && point_in_quad(&s->pass[0][j], cx, cy))
                s->pass[0][j] = s->pass[0][--s->npass[0]];
        record_code(s, 0, 1);
        accept_code(s, rx, ev);
        ok++;
    }
    return ok;
}

static int decode_all(cp_scanner *s, cp_rx *rx, int *ev, int p, int already)
{
    int n = quirc_count(s->q), ok = 0;
    if (n > s->detected) s->detected = n;
    for (int i = 0; i < n; i++) {
        quirc_extract(s->q, i, &s->code);
        if (already_read(s, already)) continue;
        int good = decode_sample(s);
        record_code(s, p, good);
        if (!good)
            continue;
        ok++;
        accept_code(s, rx, ev);
    }
    return ok;
}

#ifdef CP_WITH_ZXING
struct fallback_result {
    cp_scanner *scanner;
    cp_rx *rx;
    int *events;
    int left, top, count;
    int crop;
};

static void fallback_found(void *ctx, const cp_grid *grid, const uint8_t *bytes, size_t n)
{
    struct fallback_result *r = ctx;
    cp_scanner *s = r->scanner;
    if ((r->crop && r->count) || (!r->crop && s->nanchors >= CP_SCAN_MAX_QUADS)) return;
    s->code.size = grid->size;
    int cx = 0, cy = 0;
    for (int k = 0; k < 4; k++) {
        s->code.corners[k].x = grid->corners[k].x + r->left;
        s->code.corners[k].y = grid->corners[k].y + r->top;
        cx += s->code.corners[k].x;
        cy += s->code.corners[k].y;
    }
    int p = s->shown;
    for (int i = 0; i < s->npass[p]; i++) {
        cp_scan_quad *q = &s->pass[p][i];
        if (q->decoded && point_in_quad(q, cx / 4, cy / 4)) return;
    }
    memcpy(s->data.payload, bytes, n);
    s->data.payload_len = (int)n;
    r->count++;
    if (r->crop) return; /* the tracked loop records and delivers this result */
    for (int i = s->npass[p] - 1; i >= 0; i--) {
        cp_scan_quad *q = &s->pass[p][i];
        if (!q->decoded && point_in_quad(q, cx / 4, cy / 4))
            *q = s->pass[p][--s->npass[p]];
    }
    record_code(s, p, 1);
    accept_code(s, r->rx, r->events);
}
#endif

/* A hand movement can exceed the finder search window. Detect again within
 * a padded crop and convert its measured corners back into frame coordinates. */
static int read_crop(cp_scanner *s, const cp_grid *grid, const uint8_t *y)
{
    int left = s->w, right = 0, top = s->h, bottom = 0;
    for (int k = 0; k < 4; k++) {
        int x = grid->corners[k].x, v = grid->corners[k].y;
        if ((unsigned)x >= (unsigned)s->w || (unsigned)v >= (unsigned)s->h) return 0;
        if (x < left) left = x;
        if (x > right) right = x;
        if (v < top) top = v;
        if (v > bottom) bottom = v;
    }
    int side = right-left > bottom-top ? right-left : bottom-top;
    int margin = (side * 35) / 100 + (2 * grid->drift < side ? 2 * grid->drift : side) + 4;
    int pad_x = margin, pad_y = margin;
    left = left > pad_x ? left-pad_x : 0;
    top = top > pad_y ? top-pad_y : 0;
    right = right+pad_x+1 < s->w ? right+pad_x+1 : s->w;
    bottom = bottom+pad_y+1 < s->h ? bottom+pad_y+1 : s->h;
    int w = right-left, h = bottom-top;
    if (w*h > s->w*s->h/2) {
#ifdef CP_WITH_ZXING
        /* A large known symbol can need lens-aware sampling on every frame.
         * Read the strided crop directly without copying half the camera image. */
        struct fallback_result result = {.scanner=s, .left=left, .top=top, .crop=1};
        cp_find_qr(y + top*s->w + left, w, h, s->w, fallback_found, &result);
        return result.count;
#else
        return 0;
#endif
    }
    if (!s->crop) s->crop = quirc_new();
    if (!s->crop) return 0;
    if (s->crop_w != w || s->crop_h != h) {
        if (quirc_resize(s->crop, w, h) < 0) return 0;
        s->crop_w = w;
        s->crop_h = h;
    }
    uint8_t *dst = quirc_begin(s->crop, NULL, NULL);
    for (int row = 0; row < h; row++)
        memcpy(dst + row*w, y + (top+row)*s->w + left, w);
    quirc_end(s->crop);
    for (int i = 0; i < quirc_count(s->crop); i++) {
        quirc_extract(s->crop, i, &s->code);
        if (!decode_sample(s)) continue;
        for (int k = 0; k < 4; k++) {
            s->code.corners[k].x += left;
            s->code.corners[k].y += top;
        }
        if (!already_read(s, s->npass[0])) return 1;
    }
#ifdef CP_WITH_ZXING
    struct fallback_result result = {.scanner=s, .left=left, .top=top, .crop=1};
    cp_find_qr(y + top*s->w + left, w, h, s->w, fallback_found, &result);
    return result.count;
#else
    return 0;
#endif
}

static int quad_contains_centre(const cp_ob_quad *a, const cp_ob_quad *b)
{
    cp_scan_quad q;
    int cx = 0, cy = 0;
    for (int k = 0; k < 4; k++) {
        q.x[k] = (int)a->x[k];
        q.y[k] = (int)a->y[k];
        cx += (int)b->x[k];
        cy += (int)b->y[k];
    }
    return point_in_quad(&q, cx / 4, cy / 4);
}

/* Tracks, searches and decodes optical boards. Returns the boards locked in
 * this frame; their overlay quads are recorded later by record_boards(). */
static int scan_boards(cp_scanner *s, cp_rx *rx, const uint8_t *y, int w, int h, uint64_t now, int qr_seen,
                       int *ev)
{
    int kept = 0, lost;
    uint64_t wait;
    for (int i = 0; i < s->nob; i++) {
        cp_ob_quad q = s->ob[i];
        if (cp_ob_track(y, w, h, &q))
            s->ob[kept++] = q;
    }
    lost = kept < s->nob;
    s->nob = kept;
    wait = kept ? OB_SEARCH_MORE_MS
         : (qr_seen || now - s->last_ok < RETRY_ACTIVE_MS) ? OB_SEARCH_ACTIVE_MS
         : OB_SEARCH_IDLE_MS;
    /* during a QR transfer the board search only runs at the slow rate */
    if (qr_seen && !kept)
        wait = OB_SEARCH_MORE_MS;
    if (lost || !s->last_ob_search || now - s->last_ob_search >= wait) {
        cp_ob_quad found[OB_MAX_BOARDS];
        int n = cp_ob_locate(y, w, h, found, OB_MAX_BOARDS);
        s->last_ob_search = now;
        for (int i = 0; i < n && s->nob < OB_MAX_BOARDS; i++) {
            int dup = 0;
            for (int j = 0; j < kept && !dup; j++)
                dup = quad_contains_centre(&s->ob[j], &found[i]);
            if (!dup)
                s->ob[s->nob++] = found[i];
        }
    }
    if (s->nob && !s->ob_luma)
        s->ob_luma = (float *)malloc(sizeof(float) * CP_OB_SIDE * CP_OB_SIDE);
    for (int i = 0; i < s->nob; i++) {
        s->ob_units[i] = 0;
        if (!s->ob_luma)
            continue;
        cp_ob_sample(y, w, h, &s->ob[i], s->ob_luma);
        if (rx)
            *ev |= cp_rx_push_board(rx, s->ob_luma, &s->ob_units[i]);
    }
    return s->nob;
}

static void record_boards(cp_scanner *s)
{
    int p = s->shown;
    for (int i = 0; i < s->nob && s->npass[p] < CP_SCAN_MAX_QUADS; i++) {
        cp_scan_quad *q = &s->pass[p][s->npass[p]++];
        q->decoded = s->ob_units[i] > 0;
        for (int k = 0; k < 4; k++) {
            q->x[k] = (int)(s->ob[i].x[k] + 0.5f);
            q->y[k] = (int)(s->ob[i].y[k] + 0.5f);
        }
    }
}

static int scan_frame(cp_scanner *s, cp_rx *rx, const uint8_t *y, int w, int h, int *codes,
                      const cp_grid *seeds, int nseeds)
{
    int boards;
    uint8_t *img;
    int ok = 0, ev = 0;
    uint64_t now;

    if (codes)
        *codes = -1;
    if (!s || !y || w <= 0 || h <= 0 || w > CP_SCAN_MAX_DIM || h > CP_SCAN_MAX_DIM)
        return 0;
    if (w != s->w || h != s->h) {
        s->nanchors = 0;
        s->nob = 0;
        if (quirc_resize(s->q, w, h) < 0) {
            s->w = s->h = 0;
            return 0;
        }
        s->w = w;
        s->h = h;
    }

    if (seeds) {
        memcpy(s->anchors, seeds, nseeds * sizeof *seeds);
        s->nanchors = nseeds;
    }
    now = mono_ms();
    s->detected = 0;
    s->shown = 0;
    s->npass[0] = s->npass[1] = 0;
    boards = scan_boards(s, rx, y, w, h, now, s->nanchors > 0, &ev);
    int previous = s->nanchors;
    s->nanchors = 0;
    if (previous) {
        for (int i = 0; i < previous; i++) {
            /* Accepted anchors compact towards the front, never over a future entry. */
            cp_grid grid = s->anchors[i];
            int good = cp_sample_grid(&grid, y, w, h, &s->code) && decode_sample(s);
            if (good && already_read(s, ok)) good = 0;
            if (!good && !read_crop(s, &grid, y)) continue;
            record_code(s, 0, 1);
            accept_code(s, rx, &ev);
            ok++;
        }
        s->detected = ok;
        s->tracked_frames++;
    }
    /* Discovery supplements this frame's tracked reads. A damaged finder may
     * still decode at its known grid even when a fresh detector misses it. */
    int lost = ok < previous;
    int discover = (!ok && !boards) || (lost && (!s->missing || now - s->last_discovery >= 200)) ||
                   s->tracked_frames >= DISCOVERY_FRAMES ||
                   now - s->last_discovery >= DISCOVERY_INTERVAL_MS;
    if (discover) {
        img = quirc_begin(s->q, NULL, NULL);
        memcpy(img, y, (size_t)w * (size_t)h);
        quirc_end(s->q);
        ok += decode_all(s, rx, &ev, 0, ok);
        s->tracked_frames = 0;
        s->last_discovery = now;
        if (s->detected < ok) s->detected = ok;
    }

    /* Partial frame: codes were located (or tracked last frame, or read
     * together with these a moment ago) that did not decode next to ones that
     * did. They get the same retry as a frame that decoded nothing, otherwise
     * one readable neighbour would keep a blurred code from ever being
     * rescued. A blurred code often loses its finder patterns, so "located"
     * alone misses most of these frames. */
    int partial = ok > 0 && (s->detected > ok || ok < previous ||
                             (ok < s->expect && now - s->expect_at < EXPECT_MS));
    if ((ok == 0 || partial) && !boards && s->retry_ms >= 0 &&
        (s->detected > 0 || now - s->last_ok < RETRY_ACTIVE_MS) &&
        (s->retry_ms == 0 || now - s->last_retry >= (uint64_t)s->retry_ms)) {
        scan_box skip[CP_SCAN_MAX_QUADS];
        int nskip = 0;
        for (int i = 0; partial && i < s->npass[0]; i++) {
            const cp_scan_quad *q = &s->pass[0][i];
            if (!q->decoded)
                continue;
            skip[nskip] = (scan_box){ q->x[0], q->y[0], q->x[0], q->y[0] };
            for (int k = 1; k < 4; k++) {
                if (q->x[k] < skip[nskip].x0) skip[nskip].x0 = q->x[k];
                if (q->x[k] > skip[nskip].x1) skip[nskip].x1 = q->x[k];
                if (q->y[k] < skip[nskip].y0) skip[nskip].y0 = q->y[k];
                if (q->y[k] > skip[nskip].y1) skip[nskip].y1 = q->y[k];
            }
            nskip++;
        }
        s->last_retry = now;
        img = quirc_begin(s->q, NULL, NULL);
        if (contrast_retry(y, w, h, img, skip, nskip)) {
            quirc_end(s->q);
            if (partial) {
                int more = decode_missing(s, rx, &ev);
                if (more) {
                    ok += more;
                    s->retry_hits++;
                    if (s->detected < ok) s->detected = ok;
                }
            } else {
                ok = decode_all(s, rx, &ev, 1, 0);
                if (ok)
                    s->retry_hits++;
                /* show whichever pass saw more: decoded first, then located */
                if (ok > 0 || s->npass[1] > s->npass[0])
                    s->shown = 1;
            }
        }
    }
#ifdef CP_WITH_ZXING
    /* Known regions already get a local retry. Bound full-frame fallback work
     * while aiming, and occasionally look for neighbours missed by quirc. */
    uint64_t interval = (!ok || ok < previous || s->detected > ok) ? 100 : 1500;
    if (discover && !boards && (!s->last_fallback || now - s->last_fallback >= interval) &&
        (!ok || previous || s->detected > ok)) {
        s->last_fallback = now;
        struct fallback_result result = {.scanner=s, .rx=rx, .events=&ev};
        cp_find_qr(y, w, h, w, fallback_found, &result);
        ok += result.count;
        if (ok > s->detected) s->detected = ok;
    }
#endif
    s->missing = ok < previous;
    if (ok || boards)
        s->last_ok = now;
    if (ok >= s->expect || now - s->expect_at >= EXPECT_MS) {
        s->expect = ok;
        s->expect_at = now;
    }
    if (ok > CP_SCAN_MAX_QUADS)
        s->nanchors = 0; /* preserve full discovery for layouts larger than the cache */
    record_boards(s);
    if (rx)
        cp_rx_note_frame(rx, ok + boards);
    if (codes)
        *codes = ok;
    return ev;
}

int cp_scanner_scan(cp_scanner *s, cp_rx *rx, const uint8_t *y, int w, int h, int *codes)
{
    return scan_frame(s, rx, y, w, h, codes, NULL, 0);
}

int cp_scan_seeded(cp_scanner *s, cp_rx *rx, const uint8_t *y, int w, int h,
                   cp_grid seeds[CP_SCAN_MAX_QUADS], int *count, int *decoded)
{
    int codes;
    int ev = scan_frame(s, rx, y, w, h, &codes, seeds, *count);
    if (decoded) *decoded = codes;
    *count = codes > CP_SCAN_MAX_QUADS ? -1 : codes < 0 ? 0 : s->nanchors;
    if (*count > 0) memcpy(seeds, s->anchors, *count * sizeof *seeds);
    return ev;
}
