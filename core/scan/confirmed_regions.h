/* Session-local, un-smoothed sampling geometry of the codes a camera has read.
 * A confirmed grid seeds the next frames' tracked read, so a code stays
 * readable through blur / glare that defeats a fresh finder search. Shared by
 * the Windows receiver and the Android JNI bridge.
 *
 * cp_scan_session_step() is the one call a decoder thread makes per frame:
 * seeds out of the cache, scan, cache updated with what was read. The cache
 * is shared by all decoder threads, so the step takes the caller's lock (see
 * cp_scan_lock) around its two cache accesses only, never across the scan.
 * Nothing in core creates a lock of its own. */
#ifndef CODETRANS_CONFIRMED_REGIONS_H
#define CODETRANS_CONFIRMED_REGIONS_H

#include "camera_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cp_confirmed_region {
    cp_grid grid;
    double x, y, side;
    uint64_t frame, seen_at;
} cp_confirmed_region;

typedef struct cp_confirmed_regions {
    cp_confirmed_region items[CP_SCAN_MAX_QUADS];
    int count;
    uint64_t latest;
    int has_latest;
} cp_confirmed_regions;

/* Grids usable as seeds for frame `frame` taken at `time` (ms): confirmed by
 * an earlier frame, within the 1.5 s hold. Returns the count (<= CP_SCAN_MAX_QUADS). */
int cp_regions_seeds(const cp_confirmed_regions *cache, uint64_t frame, uint64_t time, cp_grid *out);

/* Record the result of scanning `frame`. grids/count are cp_scan_seeded()'s
 * output (count < 0 = more symbols than the cache holds: forget everything,
 * count == 0 = empty frame: keep what is cached). quads/nq are the frame's
 * located codes: an unread one next to a cached grid extends its life but
 * never moves or creates one. Results older than the newest one applied are
 * ignored (decoder threads finish out of order). */
void cp_regions_observe(cp_confirmed_regions *cache, uint64_t frame, uint64_t time,
                        const cp_grid *grids, int count, const cp_scan_quad *quads, int nq);

/* Caller-provided mutex for the cache. Either function may be NULL. */
typedef struct cp_scan_lock {
    void (*lock)(void *user);
    void (*unlock)(void *user);
    void *user;
} cp_scan_lock;

typedef struct cp_scan_session {
    cp_confirmed_regions regions;
    uint64_t min_frame;     /* frames numbered below this are from an older geometry */
    cp_scan_lock lock;
} cp_scan_session;

void cp_scan_session_init(cp_scan_session *s, const cp_scan_lock *lock);

/* The camera frame size or rotation changed (or scanning restarted): forget
 * every grid. Frames numbered below first_frame, possibly still in flight,
 * neither read nor write the cache any more. The caller holds the lock. */
void cp_scan_session_reset(cp_scan_session *s, uint64_t first_frame);

/* Scan one frame (y: w*h luminance, tightly packed) with this session's seeds.
 * `frame` is a strictly increasing number, `time_ms` the capture time on a
 * monotonic clock. Returns CP_EV_* bits like cp_scanner_scan(). Optional
 * outputs: *decoded = codes read (-1 on invalid arguments); quads/nquads =
 * where the located codes are (room for CP_SCAN_MAX_QUADS). Thread-safe across
 * threads that each use their own cp_scanner, given a working lock. */
int cp_scan_session_step(cp_scan_session *s, cp_scanner *scanner, cp_rx *rx, const uint8_t *y, int w, int h,
                         uint64_t frame, uint64_t time_ms, int *decoded,
                         cp_scan_quad *quads, int *nquads);

#ifdef __cplusplus
}
#endif
#endif /* CODETRANS_CONFIRMED_REGIONS_H */
