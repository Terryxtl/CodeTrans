/*
 * Stable code outlines for a camera preview (the one tracker for
 * Windows and Android). Per-frame detections flicker: a code is missed for a
 * frame, quirc reports the corners in another order, or invents a phantom
 * across two neighbours. The tracker turns them into steady boxes:
 *  - each located code is matched to a track by its centre; matched tracks
 *    move smoothly (corner order aligned first),
 *  - a track survives hold_ms without being seen, so one miss does not blink,
 *  - a decode-confirmed track only moves on decoded sightings,
 *  - an unread track is shown only after confirm_hits sightings,
 *  - a track counts as read for read_hold_ms after its last decode,
 *  - frames numbered below the last one processed are ignored (decoder threads
 *    finish out of order); a size / rotation change drops every track.
 * Pure C, no allocation, not thread-safe (feed it from one thread).
 */
#ifndef CODETRANS_QUAD_TRACKER_H
#define CODETRANS_QUAD_TRACKER_H

#include <stdint.h>

#include "scanner.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CP_TRACK_MAX (2 * CP_SCAN_MAX_QUADS)

typedef struct cp_quad_track {
    float    q[8];        /* x0, y0 .. x3, y3 in frame pixels */
    uint64_t last_seen;
    uint64_t read_at;     /* 0 = never decoded */
    int      hits;
} cp_quad_track;

typedef struct cp_quad_tracker {
    cp_quad_track t[CP_TRACK_MAX];
    int           n;
    int           frame_w, frame_h, rotation;
    uint64_t      last_frame;
    int           has_frame;
    unsigned      hold_ms, read_hold_ms;
    int           confirm_hits;
} cp_quad_tracker;

/* Defaults as on Android: hold 400 ms, read 700 ms, 3 sightings. */
void cp_quad_tracker_init(cp_quad_tracker *t);
void cp_quad_tracker_reset(cp_quad_tracker *t);

/* One scanned frame (w x h, shown rotated by `rotation` degrees) with its
 * cp_scanner_last_quads() result. frame_no orders frames: one below the last
 * processed is dropped (returns 0; the tracker is untouched). A frame size or
 * rotation change drops every track. Returns 1 when the frame was applied. */
int  cp_quad_tracker_update(cp_quad_tracker *t, uint64_t frame_no, int w, int h, int rotation,
                            const cp_scan_quad *q, int n, uint64_t now_ms);

/* The boxes to draw at now_ms: decode-confirmed tracks and confirmed unread
 * ones; decoded = 1 while the track counts as read. Returns the count. */
int  cp_quad_tracker_get(cp_quad_tracker *t, uint64_t now_ms, cp_scan_quad *out, int max);

/* Same tracks with sub-pixel corners and fade: opacity falls from 1 to 0 over
 * the second half of hold_ms after a track was last seen. */
typedef struct cp_quad_view {
    float q[8];
    int   ever_read;   /* decoded at least once (Android draws only these) */
    int   read;        /* counts as read at now_ms */
    float opacity;     /* 0..1 */
} cp_quad_view;

/* Confirmed tracks (those cp_quad_tracker_get returns) at now_ms. Returns the
 * count; that count is also the "sighted" figure: codes seen, read or not. */
int  cp_quad_tracker_view(cp_quad_tracker *t, uint64_t now_ms, cp_quad_view *out, int max);

#ifdef __cplusplus
}
#endif
#endif /* CODETRANS_QUAD_TRACKER_H */
