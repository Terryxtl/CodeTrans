#include "quad_tracker.h"

#include <math.h>
#include <string.h>

#define SMOOTH 0.5f

void cp_quad_tracker_init(cp_quad_tracker *t)
{
    memset(t, 0, sizeof *t);
    t->hold_ms = 400;
    t->read_hold_ms = 700;
    t->confirm_hits = 3;
}

void cp_quad_tracker_reset(cp_quad_tracker *t)
{
    t->n = 0;
    t->has_frame = 0;
}

static void centre(const float *q, float *x, float *y)
{
    *x = (q[0] + q[2] + q[4] + q[6]) / 4.0f;
    *y = (q[1] + q[3] + q[5] + q[7]) / 4.0f;
}

static float side(const float *q)
{
    float a = hypotf(q[2] - q[0], q[3] - q[1]), b = hypotf(q[6] - q[0], q[7] - q[1]);
    return a > b ? a : b;
}

/* q rotated (cyclically, either winding) to best match ref corner by corner */
static void align_corners(const float *ref, const float *q, float *out)
{
    float best = INFINITY;
    for (int dir = 1; dir >= -1; dir -= 2)
        for (int shift = 0; shift < 4; shift++) {
            float cand[8], d = 0;
            for (int k = 0; k < 4; k++) {
                int src = ((shift + dir * k) % 4 + 4) % 4;
                cand[2 * k] = q[2 * src];
                cand[2 * k + 1] = q[2 * src + 1];
                d += hypotf(cand[2 * k] - ref[2 * k], cand[2 * k + 1] - ref[2 * k + 1]);
            }
            if (d < best) {
                best = d;
                memcpy(out, cand, sizeof cand);
            }
        }
}

static void expire(cp_quad_tracker *t, uint64_t now)
{
    int kept = 0;
    for (int i = 0; i < t->n; i++)
        if (now < t->t[i].last_seen || now - t->t[i].last_seen <= t->hold_ms)
            t->t[kept++] = t->t[i];
    t->n = kept;
}

int cp_quad_tracker_update(cp_quad_tracker *t, uint64_t frame_no, int w, int h, int rotation,
                           const cp_scan_quad *q, int n, uint64_t now)
{
    unsigned char used[CP_TRACK_MAX];
    int existing, pass, i;
    if (t->has_frame && frame_no < t->last_frame)
        return 0;
    t->has_frame = 1;
    t->last_frame = frame_no;
    if (w != t->frame_w || h != t->frame_h || rotation != t->rotation) {
        t->n = 0;
        t->frame_w = w;
        t->frame_h = h;
        t->rotation = rotation;
    }
    expire(t, now);
    /* match only against the tracks that existed before this frame */
    existing = t->n;
    memset(used, 0, sizeof used);
    /* decoded sightings first, so they claim their tracks before unread ones */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < n; i++) {
            float cur[8], cx, cy, size, best_d = INFINITY;
            int decoded = q[i].decoded != 0, best = -1;
            if (decoded != (pass == 0))
                continue;
            for (int k = 0; k < 4; k++) {
                cur[2 * k] = (float)q[i].x[k];
                cur[2 * k + 1] = (float)q[i].y[k];
            }
            centre(cur, &cx, &cy);
            size = side(cur);
            for (int j = 0; j < existing; j++) {
                float tx, ty, d, s2;
                if (used[j])
                    continue;
                centre(t->t[j].q, &tx, &ty);
                d = hypotf(cx - tx, cy - ty);
                s2 = side(t->t[j].q);
                if (d < best_d && d < 0.5f * (size > s2 ? size : s2)) {
                    best = j;
                    best_d = d;
                }
            }
            if (best >= 0) {
                cp_quad_track *tr = &t->t[best];
                used[best] = 1;
                /* a read track only moves on decoded sightings */
                if (decoded || tr->read_at == 0) {
                    float aligned[8];
                    align_corners(tr->q, cur, aligned);
                    if (decoded && tr->read_at == 0)
                        memcpy(tr->q, aligned, sizeof aligned);
                    else
                        for (int k = 0; k < 8; k++)
                            tr->q[k] += SMOOTH * (aligned[k] - tr->q[k]);
                }
                tr->last_seen = now;
                tr->hits++;
                if (decoded)
                    tr->read_at = now;
            } else if (t->n < CP_TRACK_MAX) {
                cp_quad_track *tr = &t->t[t->n++];
                memcpy(tr->q, cur, sizeof cur);
                tr->last_seen = now;
                tr->read_at = decoded ? now : 0;
                tr->hits = 1;
            }
        }
    return 1;
}

static int confirmed(const cp_quad_tracker *t, const cp_quad_track *tr)
{
    return tr->read_at != 0 || tr->hits >= t->confirm_hits;
}

static int is_read(const cp_quad_tracker *t, const cp_quad_track *tr, uint64_t now)
{
    return tr->read_at != 0 && now >= tr->read_at && now - tr->read_at <= t->read_hold_ms;
}

int cp_quad_tracker_view(cp_quad_tracker *t, uint64_t now, cp_quad_view *out, int max)
{
    int count = 0;
    expire(t, now);
    for (int i = 0; i < t->n && count < max; i++) {
        const cp_quad_track *tr = &t->t[i];
        cp_quad_view *o;
        float age, opacity;
        if (!confirmed(t, tr))
            continue;
        o = &out[count++];
        memcpy(o->q, tr->q, sizeof o->q);
        o->ever_read = tr->read_at != 0;
        o->read = is_read(t, tr, now);
        age = now > tr->last_seen ? (float)(now - tr->last_seen) : 0.0f;
        opacity = 2.0f * (1.0f - age / (float)(t->hold_ms ? t->hold_ms : 1));
        o->opacity = opacity < 0 ? 0 : opacity > 1 ? 1 : opacity;
    }
    return count;
}

int cp_quad_tracker_get(cp_quad_tracker *t, uint64_t now, cp_scan_quad *out, int max)
{
    int count = 0;
    expire(t, now);
    for (int i = 0; i < t->n && count < max; i++) {
        const cp_quad_track *tr = &t->t[i];
        if (!confirmed(t, tr))
            continue;
        cp_scan_quad *o = &out[count++];
        for (int k = 0; k < 4; k++) {
            o->x[k] = (int)lroundf(tr->q[2 * k]);
            o->y[k] = (int)lroundf(tr->q[2 * k + 1]);
        }
        o->decoded = is_read(t, tr, now);
    }
    return count;
}
