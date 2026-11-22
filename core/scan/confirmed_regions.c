#include "confirmed_regions.h"

#include <math.h>
#include <string.h>

#define HOLD_MS 1500

static cp_confirmed_region measure(cp_grid grid, uint64_t frame, uint64_t time)
{
    cp_confirmed_region r = {.grid = grid, .frame = frame, .seen_at = time};
    int x0 = grid.corners[0].x, x1 = x0, y0 = grid.corners[0].y, y1 = y0;
    for (int k = 0; k < 4; ++k) {
        int x = grid.corners[k].x, y = grid.corners[k].y;
        r.x += x * 0.25;
        r.y += y * 0.25;
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    r.side = fmax(x1 - x0, y1 - y0);
    return r;
}

int cp_regions_seeds(const cp_confirmed_regions *cache, uint64_t frame, uint64_t time, cp_grid *out)
{
    int n = 0;
    for (int i = 0; i < cache->count; ++i) {
        const cp_confirmed_region *r = &cache->items[i];
        if (r->frame < frame && time >= r->seen_at && time - r->seen_at <= HOLD_MS)
            out[n++] = r->grid;
    }
    return n;
}

void cp_regions_observe(cp_confirmed_regions *cache, uint64_t frame, uint64_t time,
                        const cp_grid *grids, int count, const cp_scan_quad *quads, int nq)
{
    if (cache->has_latest && frame <= cache->latest) return;
    int kept = 0;
    for (int i = 0; i < cache->count; ++i) {
        cp_confirmed_region r = cache->items[i];
        if (time <= r.seen_at || time - r.seen_at <= HOLD_MS)
            cache->items[kept++] = r;
    }
    cache->count = kept;
    /* An unread sighting may keep a nearby grid alive, never move it. */
    for (int i = 0; i < nq && kept; ++i) {
        if (quads[i].decoded) continue;
        cp_grid grid = {0};
        for (int k = 0; k < 4; ++k)
            grid.corners[k] = (struct quirc_point){quads[i].x[k], quads[i].y[k]};
        cp_confirmed_region sighting = measure(grid, frame, time);
        int best = 0;
        double distance = HUGE_VAL;
        for (int j = 0; j < kept; ++j) {
            double d = hypot(sighting.x - cache->items[j].x, sighting.y - cache->items[j].y);
            if (d < distance) { best = j; distance = d; }
        }
        cp_confirmed_region *r = &cache->items[best];
        if (sighting.side >= 0.7 * r->side && sighting.side <= 1.4 * r->side &&
            distance < 0.25 * r->side && time > r->seen_at)
            r->seen_at = time;
    }
    /* A faster empty frame must allow an older successful decode to publish. */
    if (!count) return;
    cp_confirmed_regions next = {.latest = frame, .has_latest = 1};
    if (count > 0) {
        int used[CP_SCAN_MAX_QUADS] = {0};
        if (count > CP_SCAN_MAX_QUADS) count = CP_SCAN_MAX_QUADS;
        for (int i = 0; i < count; ++i) {
            cp_confirmed_region r = measure(grids[i], frame, time);
            int best = -1;
            double distance = HUGE_VAL;
            for (int j = 0; j < kept; ++j) {
                const cp_confirmed_region *p = &cache->items[j];
                double d = hypot(r.x - p->x, r.y - p->y);
                if (!used[j] && d < distance && d < 0.5 * fmin(r.side, p->side)) {
                    distance = d;
                    best = j;
                }
            }
            if (best >= 0) {
                used[best] = 1;
                r.grid.drift = (int)lround((cache->items[best].grid.drift + distance) * 0.5);
            }
            next.items[next.count++] = r;
        }
        for (int j = 0; j < kept && next.count < CP_SCAN_MAX_QUADS; ++j)
            if (!used[j]) next.items[next.count++] = cache->items[j];
    }
    *cache = next;
}

/* ---- session ---------------------------------------------------------------- */

void cp_scan_session_init(cp_scan_session *s, const cp_scan_lock *lock)
{
    memset(s, 0, sizeof *s);
    if (lock) s->lock = *lock;
}

void cp_scan_session_reset(cp_scan_session *s, uint64_t first_frame)
{
    memset(&s->regions, 0, sizeof s->regions);
    s->min_frame = first_frame;
}

static void lock(cp_scan_session *s)   { if (s->lock.lock) s->lock.lock(s->lock.user); }
static void unlock(cp_scan_session *s) { if (s->lock.unlock) s->lock.unlock(s->lock.user); }

int cp_scan_session_step(cp_scan_session *s, cp_scanner *scanner, cp_rx *rx, const uint8_t *y, int w, int h,
                         uint64_t frame, uint64_t time_ms, int *decoded,
                         cp_scan_quad *quads, int *nquads)
{
    cp_grid grids[CP_SCAN_MAX_QUADS];
    cp_scan_quad local[CP_SCAN_MAX_QUADS];
    cp_scan_quad *q = quads ? quads : local;
    int count = 0, codes = -1, nq = 0, ev;
    if (decoded) *decoded = -1;
    if (nquads) *nquads = 0;
    if (!s || !scanner) return 0;

    lock(s);
    if (frame >= s->min_frame)
        count = cp_regions_seeds(&s->regions, frame, time_ms, grids);
    unlock(s);

    ev = cp_scan_seeded(scanner, rx, y, w, h, grids, &count, &codes);
    if (codes < 0)   /* invalid arguments: nothing was scanned, nothing to learn */
        return ev;
    nq = cp_scanner_last_quads(scanner, q, CP_SCAN_MAX_QUADS);

    lock(s);
    if (frame >= s->min_frame)
        cp_regions_observe(&s->regions, frame, time_ms, grids, count, q, nq);
    unlock(s);
    if (decoded) *decoded = codes;
    if (nquads) *nquads = nq;
    return ev;
}
