/* CodeTrans optical board: layout, rendering, registered read and the sender
 * carousel. See cp_internal.h ("OB"); port of CodeTrans-Optical-Reference
 * protocol.py (same pilots, masks, bit placement, schedule and wire bytes).
 *
 * Layout (177 x 177, origin top-left, 1 = white):
 * - pilots: xorshift32 PRBS (seed 0x73513251), bit 0 of each output byte;
 * - corner marks at (0,0) (170,0) (170,170) (0,170): 7x7 black ring, white
 *   ring, then a corner-specific 3x3 pattern (so orientation is unambiguous);
 * - data rectangle j (0..23, row-major 6 x 4): top-left (7 + 41c, 2 + 29r),
 *   40 x 28 modules; octet b sits at patch (b % 10, b / 10) of 4 x 2 modules;
 *   rectangle j's octets are XOR-ed with PRBS(0xa7135c01 ^ j);
 * - profile marks: 6 x 2 blocks of 3 x 4 modules at columns 2 and 172; the
 *   bands profile inverts them, the reader compares them against the pilots.
 *
 * Carousel: groups of <= 32 source rows; first pass row-interleaved over
 * windows of 48 groups (tiles add two repair rows per group), then 32 sweeps
 * of fresh repair rows over all groups, repeated with rotating repair ids.
 * Slot i of refresh t is drawn at position (5i + 7t) mod L, L = total units on
 * screen. The session manifest takes one unit every 16 (bands) / 8 (tiles)
 * refreshes, at a rotating position. */
#include "cp_internal.h"

#include <math.h>
#include <stdatomic.h>

#define SIDE   OB_SIDE
#define AREA   (SIDE * SIDE)
#define RIM    (40 + 40 + 28 + 28)
#define NMODE  (OB_ROWS * 2 * 4 * 3)

uint8_t ob_pilots[AREA];
uint8_t ob_pilot_score[AREA];
static uint8_t band_pilots[AREA];
static uint8_t masks[OB_SLOTS][OB_N];
static int     tile_x[OB_SLOTS], tile_y[OB_SLOTS];
static int     rim_idx[OB_SLOTS][RIM];
static uint8_t rim_white[OB_SLOTS][RIM];
static int     mode_idx[NMODE];
static uint8_t mode_white[NMODE];       /* pilot value at the profile marks  */
static uint8_t is_mode[AREA];
static int    *pilot_idx;               /* PILOT_SCORE positions             */
static float  *pilot_sign;
static int     npilot;
static atomic_int layout_ready;

static void prbs(uint32_t seed, size_t n, uint8_t *out)
{
    size_t i;
    for (i = 0; i < n; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        out[i] = (uint8_t)seed;
    }
}

void ob_layout_init(void)
{
    static const char *const corner_bits[4] = { "111101111", "110100110", "010101010", "000000000" };
    static const int corner_xy[4][2] = { { 0, 0 }, { 170, 0 }, { 170, 170 }, { 0, 170 } };
    static int   s_pidx[AREA];
    static float s_psign[AREA];
    int i, j, x, y, c, n;

    if (atomic_load_explicit(&layout_ready, memory_order_acquire))
        return;
    /* Concurrent first calls write identical bytes; the flag publishes them. */
    prbs(0x73513251u, AREA, ob_pilots);
    for (i = 0; i < AREA; i++)
        ob_pilots[i] &= 1;
    for (c = 0; c < 4; c++) {
        for (y = 0; y < 7; y++)
            for (x = 0; x < 7; x++) {
                uint8_t v = (x >= 1 && x < 6 && y >= 1 && y < 6) ? 1 : 0;
                if (x >= 2 && x < 5 && y >= 2 && y < 5)
                    v = (uint8_t)(corner_bits[c][(y - 2) * 3 + (x - 2)] - '0');
                ob_pilots[(corner_xy[c][1] + y) * SIDE + corner_xy[c][0] + x] = v;
            }
    }
    for (j = 0; j < OB_SLOTS; j++) {
        prbs(0xa7135c01u ^ (uint32_t)j, OB_N, masks[j]);
        tile_x[j] = 7 + 41 * (j % OB_COLS);
        tile_y[j] = 2 + 29 * (j / OB_COLS);
    }
    memset(is_mode, 0, sizeof is_mode);
    n = 0;
    for (i = 0; i < OB_ROWS; i++)
        for (y = 13 + 29 * i; y < 17 + 29 * i; y++)
            for (x = 0; x < SIDE; x++)
                if ((x >= 2 && x < 5) || (x >= 172 && x < 175))
                    is_mode[y * SIDE + x] = 1;
    for (i = 0; i < AREA; i++)            /* row-major, as numpy's boolean index */
        if (is_mode[i]) {
            mode_idx[n] = i;
            mode_white[n] = ob_pilots[i];
            n++;
        }
    for (i = 0; i < AREA; i++)
        band_pilots[i] = (uint8_t)(ob_pilots[i] ^ is_mode[i]);
    for (j = 0; j < OB_SLOTS; j++) {
        int x0 = tile_x[j], y0 = tile_y[j], k = 0;
        for (x = 0; x < 40; x++)
            rim_idx[j][k++] = (y0 - 1) * SIDE + x0 + x;
        for (x = 0; x < 40; x++)
            rim_idx[j][k++] = (y0 + 28) * SIDE + x0 + x;
        for (y = 0; y < 28; y++)
            rim_idx[j][k++] = (y0 + y) * SIDE + x0 - 1;
        for (y = 0; y < 28; y++)
            rim_idx[j][k++] = (y0 + y) * SIDE + x0 + 40;
        for (k = 0; k < RIM; k++)
            rim_white[j][k] = ob_pilots[rim_idx[j][k]];
    }
    memset(ob_pilot_score, 1, sizeof ob_pilot_score);
    for (j = 0; j < OB_SLOTS; j++)
        for (y = 0; y < 28; y++)
            memset(ob_pilot_score + (tile_y[j] + y) * SIDE + tile_x[j], 0, 40);
    for (i = 0; i < AREA; i++) {
        x = i % SIDE;
        y = i / SIDE;
        if (is_mode[i] || x < 2 || y < 2 || x >= SIDE - 2 || y >= SIDE - 2)
            ob_pilot_score[i] = 0;
    }
    n = 0;
    for (i = 0; i < AREA; i++)
        if (ob_pilot_score[i]) {
            s_pidx[n] = i;
            s_psign[n] = 2.0f * ob_pilots[i] - 1.0f;
            n++;
        }
    pilot_idx = s_pidx;
    pilot_sign = s_psign;
    npilot = n;
    atomic_store_explicit(&layout_ready, 1, memory_order_release);
}

/* ---- rendering ----------------------------------------------------------- */

static void put_word(uint8_t *board, int j, const uint8_t *word)
{
    int b, bit;
    for (b = 0; b < OB_N; b++) {
        uint8_t v = (uint8_t)(word[b] ^ masks[j][b]);
        int x0 = tile_x[j] + 4 * (b % 10), y0 = tile_y[j] + 2 * (b / 10);
        for (bit = 0; bit < 8; bit++)
            board[(y0 + bit / 4) * SIDE + x0 + bit % 4] = (uint8_t)((v >> (7 - bit)) & 1);
    }
}

void ob_draw(int profile, const uint8_t *bodies, uint8_t *white)
{
    uint8_t word[OB_N], stripe[4 * OB_N];
    int i, j, a, c, col;
    ob_layout_init();
    if (profile == CP_OB_TILES) {
        memcpy(white, ob_pilots, AREA);
        for (j = 0; j < OB_SLOTS; j++) {
            ob_tile_encode(bodies + (size_t)j * OB_TILE_BODY, word);
            put_word(white, j, word);
        }
        return;
    }
    memcpy(white, band_pilots, AREA);
    for (i = 0; i < OB_ROWS; i++) {
        ob_band_encode(bodies + (size_t)i * OB_BAND_BODY, stripe);
        /* stripe octet a*40 + col*10 + c -> rectangle (i, col), octet a*10 + c */
        for (col = 0; col < OB_COLS; col++) {
            for (a = 0; a < 14; a++)
                for (c = 0; c < 10; c++)
                    word[a * 10 + c] = stripe[a * 40 + col * 10 + c];
            put_word(white, i * OB_COLS + col, word);
        }
    }
}

/* ---- registered read ----------------------------------------------------- */

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* numpy.median: mean of the two middle values for an even count */
static float median(float *v, int n)
{
    if (n <= 0)
        return 0;
    qsort(v, (size_t)n, sizeof *v, cmp_float);
    return n & 1 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

/* Bands invert the profile marks: there, pilot-white modules read darker. */
static int is_band_board(const float *luma)
{
    float w[NMODE], b[NMODE];
    int nw = 0, nb = 0, i;
    for (i = 0; i < NMODE; i++) {
        if (mode_white[i])
            w[nw++] = luma[mode_idx[i]];
        else
            b[nb++] = luma[mode_idx[i]];
    }
    return median(w, nw) < median(b, nb);
}

/* Low-confidence octets: score < 0.24, at most `limit` (lowest first, stable). */
static int weak_symbols(const float *score, int n, int limit, uint8_t *out)
{
    int idx[OB_N], m = 0, i, j;
    for (i = 0; i < n; i++)
        if (score[i] < 0.24f)
            idx[m++] = i;
    if (m > limit) {
        for (i = 1; i < m; i++) {        /* stable insertion sort by score */
            int v = idx[i];
            for (j = i; j > 0 && score[idx[j - 1]] > score[v]; j--)
                idx[j] = idx[j - 1];
            idx[j] = v;
        }
        m = limit;
    }
    for (i = 0; i < m; i++)
        out[i] = (uint8_t)idx[i];
    return m;
}

/* Thresholds one rectangle against its known rim. 0 when it has no contrast. */
static int sample_tile(const float *luma, int j, uint8_t *word, float *score)
{
    float wv[RIM], bv[RIM], mid, gain, span;
    int nw = 0, nb = 0, k, b, bit;
    for (k = 0; k < RIM; k++) {
        if (rim_white[j][k])
            wv[nw++] = luma[rim_idx[j][k]];
        else
            bv[nb++] = luma[rim_idx[j][k]];
    }
    {
        float black = median(bv, nb), whitel = median(wv, nw);
        span = whitel - black;
        if (span < 24)
            return 0;
        mid = black + span * 0.5f;
        gain = 2 / span;
    }
    for (b = 0; b < OB_N; b++) {
        int x0 = tile_x[j] + 4 * (b % 10), y0 = tile_y[j] + 2 * (b / 10);
        uint8_t v = 0;
        float lo = INFINITY;
        for (bit = 0; bit < 8; bit++) {
            float s = (luma[(y0 + bit / 4) * SIDE + x0 + bit % 4] - mid) * gain;
            float a = fabsf(s);
            v = (uint8_t)((v << 1) | (s > 0));
            if (a < lo)
                lo = a;
        }
        word[b] = (uint8_t)(v ^ masks[j][b]);
        score[b] = lo;
    }
    return 1;
}

/* skip[index] = 1 for units already accepted. Appends to out, returns count. */
static int read_pass(const float *luma, const uint8_t *skip, ob_unit *out)
{
    uint8_t words[OB_SLOTS][OB_N], weak[16];
    float scores[OB_SLOTS][OB_N];
    int band = is_band_board(luma), n = 0, j;

    if (band) {
        memset(words, 0, sizeof words);
        memset(scores, 0, sizeof scores);
    }
    for (j = 0; j < OB_SLOTS; j++) {
        int unit = band ? j / OB_COLS : j;
        if (skip && skip[unit])
            continue;
        if (band) {
            sample_tile(luma, j, words[j], scores[j]);
        } else {
            uint8_t word[OB_N];
            float score[OB_N];
            int nweak, st;
            if (!sample_tile(luma, j, word, score))
                continue;
            nweak = weak_symbols(score, OB_N, OB_N - 124, weak);
            st = ob_tile_decode(word, weak, nweak, out[n].body);
            if (st) {
                out[n].index = j;
                out[n].status = st;
                out[n].len = OB_TILE_BODY;
                n++;
            }
        }
    }
    if (!band)
        return n;
    for (j = 0; j < OB_ROWS; j++) {
        uint8_t stripe[4 * OB_N], erase[4 * 8], counts[4];
        float conf[4 * OB_N], lane[OB_N];
        int col, a, c, l, i, st;
        if (skip && skip[j])
            continue;
        for (col = 0; col < OB_COLS; col++)
            for (a = 0; a < 14; a++)
                for (c = 0; c < 10; c++) {
                    stripe[a * 40 + col * 10 + c] = words[j * OB_COLS + col][a * 10 + c];
                    conf[a * 40 + col * 10 + c] = scores[j * OB_COLS + col][a * 10 + c];
                }
        memset(erase, 0, sizeof erase);
        for (l = 0; l < 4; l++) {
            for (i = 0; i < OB_N; i++)
                lane[i] = conf[4 * i + l];
            counts[l] = (uint8_t)weak_symbols(lane, OB_N, 8, erase + 8 * l);
        }
        st = ob_band_decode(stripe, erase, counts, out[n].body);
        if (st) {
            out[n].index = j;
            out[n].status = st;
            out[n].len = OB_BAND_BODY;
            n++;
        }
    }
    return n;
}

/* scipy uniform_filter(size=7, mode="nearest"), separable */
static void box7(const float *src, float *dst, float *tmp)
{
    int x, y, d;
    for (y = 0; y < SIDE; y++)
        for (x = 0; x < SIDE; x++) {
            float s = 0;
            for (d = -3; d <= 3; d++) {
                int xx = x + d < 0 ? 0 : x + d >= SIDE ? SIDE - 1 : x + d;
                s += src[y * SIDE + xx];
            }
            tmp[y * SIDE + x] = s / 7;
        }
    for (y = 0; y < SIDE; y++)
        for (x = 0; x < SIDE; x++) {
            float s = 0;
            for (d = -3; d <= 3; d++) {
                int yy = y + d < 0 ? 0 : y + d >= SIDE ? SIDE - 1 : y + d;
                s += tmp[yy * SIDE + x];
            }
            dst[y * SIDE + x] = s / 7;
        }
}

/* (y - local mean) / local std, floor 8: in place. w = 4 * AREA scratch */
static void local_levels(float *y, float *w)
{
    float *mean = w, *sq = w + AREA, *ysq = w + 2 * AREA, *tmp = w + 3 * AREA;
    int i;
    box7(y, mean, tmp);
    for (i = 0; i < AREA; i++)
        ysq[i] = y[i] * y[i];
    box7(ysq, sq, tmp);
    for (i = 0; i < AREA; i++) {
        float var = sq[i] - mean[i] * mean[i];
        y[i] = (y[i] - mean[i]) / sqrtf(var > 64 ? var : 64);
    }
}

static void sharpen(const float *src, float *dst, float *tmp)
{
    static const float k[5] = { 10 / 64.f, -51 / 64.f, 146 / 64.f, -51 / 64.f, 10 / 64.f };
    int x, y, d;
    for (y = 0; y < SIDE; y++)
        for (x = 0; x < SIDE; x++) {
            float s = 0;
            for (d = -2; d <= 2; d++) {
                int xx = x + d < 0 ? 0 : x + d >= SIDE ? SIDE - 1 : x + d;
                s += k[d + 2] * src[y * SIDE + xx];
            }
            tmp[y * SIDE + x] = s;
        }
    for (y = 0; y < SIDE; y++)
        for (x = 0; x < SIDE; x++) {
            float s = 0;
            for (d = -2; d <= 2; d++) {
                int yy = y + d < 0 ? 0 : y + d >= SIDE ? SIDE - 1 : y + d;
                s += k[d + 2] * tmp[yy * SIDE + x];
            }
            dst[y * SIDE + x] = s;
        }
}

static double pilot_error(const float *v)
{
    double e = 0;
    int i;
    for (i = 0; i < npilot; i++) {
        float x = v[pilot_idx[i]];
        x = x < -2 ? -2 : x > 2 ? 2 : x;
        x -= pilot_sign[i];
        e += (double)x * x;
    }
    return npilot ? e / npilot : 0;
}

int ob_read_board(const float *luma, ob_unit *out)
{
    uint8_t skip[OB_SLOTS];
    float *mem, *flat, *sharp, *work, *best;
    int n, band, i;

    ob_layout_init();
    n = read_pass(luma, NULL, out);
    band = is_band_board(luma);
    if (n * 100 >= (band ? OB_ROWS : OB_SLOTS) * 85)
        return n;
    /* Weak board: one more look at the failed units after local brightness
     * equalization, either flat or sharpened, chosen by the pilot error. */
    mem = (float *)malloc(sizeof(float) * AREA * 6);
    if (!mem)
        return n;
    flat = mem;
    sharp = mem + AREA;
    work = mem + 2 * AREA;
    memcpy(flat, luma, sizeof(float) * AREA);
    local_levels(flat, work);
    sharpen(luma, sharp, work);
    local_levels(sharp, work);
    best = pilot_error(sharp) < pilot_error(flat) ? sharp : flat;
    for (i = 0; i < AREA; i++) {
        float v = 128 + 55 * best[i];
        best[i] = is_mode[i] ? luma[i] : v < 0 ? 0 : v > 255 ? 255 : v;
    }
    memset(skip, 0, sizeof skip);
    for (i = 0; i < n; i++)
        skip[out[i].index] = 1;
    n += read_pass(best, skip, out + n);
    free(mem);
    return n;
}

/* ---- sender carousel ----------------------------------------------------- */

struct ob_enc {
    int      profile;
    uint8_t  type;           /* OB_TYPE_TILE / OB_TYPE_BAND        */
    uint32_t width;          /* payload octets per unit            */
    int      per;            /* units per board                    */
    int      interval;       /* refreshes per manifest beacon      */
    int      early;          /* repair rows per group in pass one  */
    size_t   blen;           /* unit body length                   */
    uint8_t *rows;           /* nrows * width, zero padded         */
    uint32_t nrows, ngroups;
    uint8_t  manifest[OB_BAND_DATA];
    uint8_t  tag[8];
    uint64_t t;
    /* schedule state */
    int      phase;          /* 0 = first pass, 1 = repair sweeps  */
    uint32_t first, gi, q;
    int      row;
    uint64_t cycle;
    uint8_t *bodies;         /* 16 boards of unit bodies            */
};

#define OB_MAX_BOARDS 16

static int group_k(const ob_enc *e, uint32_t g)
{
    uint32_t left = e->nrows - g * OB_GROUP;
    return left < OB_GROUP ? (int)left : OB_GROUP;
}

static void plan_next(ob_enc *e, uint32_t *g, int *r)
{
    for (;;) {
        if (e->phase == 0) {
            uint32_t end = e->first + 48 < e->ngroups ? e->first + 48 : e->ngroups;
            if (e->row < OB_GROUP + e->early) {
                if (e->gi < end) {
                    uint32_t grp = e->gi++;
                    if (e->row < group_k(e, grp) || e->row >= OB_GROUP) {
                        *g = grp;
                        *r = e->row;
                        return;
                    }
                    continue;
                }
                e->row++;
                e->gi = e->first;
                continue;
            }
            e->first += 48;
            e->row = 0;
            e->gi = e->first;
            if (e->first >= e->ngroups) {
                e->phase = 1;
                e->q = 0;
                e->gi = 0;
            }
            continue;
        }
        if (e->q < 32) {
            if (e->gi < e->ngroups) {
                uint32_t span = (uint32_t)(256 - OB_GROUP - e->early);
                *g = e->gi++;
                *r = OB_GROUP + e->early + (int)((e->cycle * 32 + e->q) % span);
                return;
            }
            e->q++;
            e->gi = 0;
            continue;
        }
        e->cycle++;
        e->phase = 0;
        e->first = 0;
        e->row = 0;
        e->gi = 0;
    }
}

static void put_header(uint8_t *h, const uint8_t tag[8], uint32_t group, int row, uint8_t type)
{
    memcpy(h, tag, 8);
    h[8] = (uint8_t)group;
    h[9] = (uint8_t)(group >> 8);
    h[10] = (uint8_t)row;
    h[11] = type;
}

ob_enc *ob_enc_new(const uint8_t *data, size_t n, int profile)
{
    ob_enc *e;
    uint8_t digest[32];
    if (!data || n < 1 || n > OB_MAX_FILE || (profile != CP_OB_TILES && profile != CP_OB_BANDS))
        return NULL;
    ob_layout_init();
    e = (ob_enc *)calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->profile = profile;
    if (profile == CP_OB_TILES) {
        e->type = OB_TYPE_TILE;
        e->width = OB_TILE_DATA;
        e->per = OB_SLOTS;
        e->interval = CP_OB_TILES_BEACON_EVERY;
        e->early = 2;
        e->blen = OB_TILE_BODY;
    } else {
        e->type = OB_TYPE_BAND;
        e->width = OB_BAND_DATA;
        e->per = OB_ROWS;
        e->interval = CP_OB_BANDS_BEACON_EVERY;
        e->early = 0;
        e->blen = OB_BAND_BODY;
    }
    e->nrows = (uint32_t)((n + e->width - 1) / e->width);
    e->ngroups = (e->nrows + OB_GROUP - 1) / OB_GROUP;
    e->rows = (uint8_t *)calloc(e->nrows, e->width);
    e->bodies = (uint8_t *)malloc(e->blen * (size_t)e->per * OB_MAX_BOARDS);
    if (!e->rows || !e->bodies || e->ngroups >= OB_MANIFEST_GROUP) {
        ob_enc_free(e);
        return NULL;
    }
    memcpy(e->rows, data, n);
    memcpy(e->manifest, "CTB1", 4);
    e->manifest[4] = (uint8_t)n;
    e->manifest[5] = (uint8_t)(n >> 8);
    e->manifest[6] = (uint8_t)(n >> 16);
    e->manifest[7] = (uint8_t)(n >> 24);
    cp_sha256(data, n, e->manifest + 8);
    cp_sha256(e->manifest, e->width, digest);
    memcpy(e->tag, digest, 8);
    return e;
}

int ob_enc_boards(ob_enc *e, int boards, uint8_t *white)
{
    uint64_t t;
    int slots, beacon, i;
    if (!e || !white || boards < 1 || boards > OB_MAX_BOARDS)
        return 0;
    slots = e->per * boards;
    if (slots % 5 == 0)       /* (5i + 7t) mod L must be a permutation */
        return 0;
    t = e->t++;
    beacon = t % (uint64_t)e->interval == 0 ? (int)((t / (uint64_t)e->interval) % (uint64_t)slots) : -1;
    for (i = 0; i < slots; i++) {
        int pos = (int)((5u * (unsigned)i + 7u * (unsigned)(t % (uint64_t)slots)) % (unsigned)slots);
        uint8_t *body = e->bodies + (size_t)pos * e->blen;
        if (pos == beacon) {
            put_header(body, e->tag, OB_MANIFEST_GROUP, 0, (uint8_t)(e->type | 1));
            memcpy(body + OB_HDR_LEN, e->manifest, e->width);
        } else {
            uint32_t g;
            int r;
            plan_next(e, &g, &r);
            put_header(body, e->tag, g, r, e->type);
            if (!ob_outer_symbol(e->rows + (size_t)g * OB_GROUP * e->width, group_k(e, g), (int)e->width, r,
                                 body + OB_HDR_LEN))
                return 0;
        }
    }
    for (i = 0; i < boards; i++)
        ob_draw(e->profile, e->bodies + (size_t)i * e->per * e->blen, white + (size_t)i * AREA);
    return 1;
}

uint32_t ob_enc_rows(const ob_enc *e) { return e ? e->nrows : 0; }
uint32_t ob_enc_width(const ob_enc *e) { return e ? e->width : 0; }
void     ob_enc_tag(const ob_enc *e, uint8_t tag[8]) { memcpy(tag, e->tag, 8); }

void ob_enc_free(ob_enc *e)
{
    if (!e)
        return;
    free(e->rows);
    free(e->bodies);
    free(e);
}
