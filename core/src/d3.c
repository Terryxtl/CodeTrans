/*
 * Decimen v3 compatibility layer: frame header, LT carousel, Cauchy RS-96
 * outer code, DCF2 container (gzip + SHA-256) and the CPO1 -> DCF2 transcoder
 * used by the sender.
 *
 * Wire compatible with Decimen Optical Transfer v0.5.3 (LT carousel, flags
 * bit 0x02 clear) and its "fixed geometry" 0.5.3-fixed.1 build (interleaved
 * systematic Cauchy code over GF(256), flags bit 0x02 set). The PRNG, block
 * schedules, GF(256) field and container layout follow that implementation
 * (shared/protocol.ts, fountain.ts, cauchy.ts, gf256.ts), which is licensed
 * AGPL-3.0-or-later; see core/PROTOCOL.md and windows/PLAN.md §5.4.
 *
 * All arithmetic here is integer-only, so no floating point determinism
 * concerns apply (the robust-soliton/dlog path of wire v1 is not used by v3).
 */
#include "cp_internal.h"

#include <stdatomic.h>

#include "miniz.h"

/* ---- little-endian helpers ---------------------------------------------- */

static uint16_t le16r(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t le32r(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void le16w(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32w(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---- GF(2^8), primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11d) ----- */

static uint8_t    gf_mul[256 * 256];   /* gf_mul[a << 8 | b] = a * b */
static uint8_t    gf_inv[256];
static atomic_int gf_ready;

static void gf_init(void)
{
    uint8_t ex[510], lg[256];
    unsigned e = 1, a, b, i;
    if (atomic_load_explicit(&gf_ready, memory_order_acquire))
        return;
    memset(lg, 0, sizeof lg);
    for (i = 0; i < 255; i++) {
        ex[i] = ex[i + 255] = (uint8_t)e;
        lg[e] = (uint8_t)i;
        e <<= 1;
        if (e & 0x100u)
            e ^= 0x11du;
    }
    /* Concurrent first calls write identical bytes; the flag publishes them. */
    for (a = 1; a < 256; a++) {
        gf_inv[a] = ex[255 - lg[a]];
        for (b = 1; b < 256; b++)
            gf_mul[(a << 8) | b] = ex[lg[a] + lg[b]];
    }
    atomic_store_explicit(&gf_ready, 1, memory_order_release);
}

/* dst ^= f * src */
static void scale_xor(uint8_t *dst, const uint8_t *src, size_t n, uint8_t f)
{
    const uint8_t *row = gf_mul + ((size_t)f << 8);
    size_t i;
    if (f == 0)
        return;
    if (f == 1) {
        for (i = 0; i < n; i++)
            dst[i] ^= src[i];
        return;
    }
    for (i = 0; i < n; i++)
        dst[i] ^= row[src[i]];
}

static void scale_inplace(uint8_t *buf, size_t n, uint8_t f)
{
    const uint8_t *row = gf_mul + ((size_t)f << 8);
    size_t i;
    if (f == 1)
        return;
    for (i = 0; i < n; i++)
        buf[i] = row[buf[i]];
}

static void xor_into(uint8_t *dst, const uint8_t *src, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        dst[i] ^= src[i];
}

/* ---- frame header -------------------------------------------------------- */

uint32_t d3_fnv1a(const uint8_t *d, size_t n)
{
    uint32_t h = 0x811c9dc5u;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= d[i];
        h *= 0x01000193u;
    }
    return h;
}

int d3_classify(const uint8_t *d, size_t n, d3_header *h)
{
    d3_header x;
    if (!d || n < 4 || d[0] != D3_MAGIC0)
        return D3_FOREIGN;
    if (d[1] != D3_MAGIC1)   /* 0x0C / 0x0D: the pre-versioning v1 / v2 magics */
        return (d[1] == 0x0C || d[1] == 0x0D) ? D3_OLDER_SENDER : D3_FOREIGN;
    if (d[2] == 0)
        return D3_MALFORMED;
    if (d[2] != D3_VERSION)
        return d[2] > D3_VERSION ? D3_NEWER_SENDER : D3_OLDER_SENDER;
    if (d[3] & D3_FLAGS_MUST & ~D3_FLAGS_KNOWN)
        return D3_UNSUPPORTED_FLAGS;
    if (n <= D3_HDR_LEN)
        return D3_MALFORMED;
    x.flags = d[3];
    x.session_id = le16r(d + 4);
    x.seq = le32r(d + 6);
    x.k = le16r(d + 10);
    x.block_len = le16r(d + 12);
    x.total_len = le32r(d + 14);
    x.fnv = le32r(d + 18);
    if (x.k == 0 || x.block_len == 0 || x.total_len == 0)
        return D3_MALFORMED;
    if (n != (size_t)D3_HDR_LEN + x.block_len)
        return D3_MALFORMED;
    /* Every sender emits k = ceil(totalLen / blockLen); the reference receiver
     * only enforces this (and the size caps) for the Cauchy stream, we enforce
     * it for both so the decoder's buffers are bounded by totalLen. */
    if ((uint64_t)x.total_len > D3_MAX_TOTAL ||
        x.k != (uint32_t)(((uint64_t)x.total_len + x.block_len - 1) / x.block_len))
        return D3_MALFORMED;
    if ((x.flags & D3_FLAG_CAUCHY96) && x.block_len > D3_MAX_BLOCK_RS)
        return D3_MALFORMED;
    if (h)
        *h = x;
    return D3_OK;
}

size_t d3_frame_write(uint8_t *buf, size_t cap, const d3_header *h, const uint8_t *block)
{
    size_t n = (size_t)D3_HDR_LEN + h->block_len;
    if (!buf || cap < n)
        return 0;
    buf[0] = D3_MAGIC0;
    buf[1] = D3_MAGIC1;
    buf[2] = D3_VERSION;
    buf[3] = h->flags;
    le16w(buf + 4, h->session_id);
    le32w(buf + 6, h->seq);
    le16w(buf + 10, h->k);
    le16w(buf + 12, h->block_len);
    le32w(buf + 14, h->total_len);
    le32w(buf + 18, h->fnv);
    memcpy(buf + D3_HDR_LEN, block, h->block_len);
    return n;
}

/* ---- schedules ------------------------------------------------------------ */

typedef struct sm32 { uint32_t s; } sm32;

static uint32_t sm32_next(sm32 *r)   /* splitmix32 */
{
    uint32_t t;
    r->s += 0x9e3779b9u;
    t = r->s ^ (r->s >> 16);
    t *= 0x21f0aaadu;
    t ^= t >> 15;
    t *= 0x735a2d97u;
    t ^= t >> 15;
    return t;
}

static uint32_t frame_seed(uint32_t session_id, uint32_t seq)
{
    uint32_t h = ((session_id + 1u) * 0x9e3779b1u) ^ (seq + 0x85ebca6bu);
    h = (h ^ (h >> 13)) * 0xc2b2ae35u;
    return h ^ (h >> 16);
}

#define LT_REPAIR_MIN 4u
#define LT_REPAIR_MAX 24u

/* Systematic carousel: positions 0..k-1 of each 2k cycle carry block `pos`,
 * the other k carry XORs of 4..24 distinct blocks seeded by the absolute seq. */
static uint32_t lt_composition(uint32_t k, uint32_t session_id, uint32_t seq, uint32_t *idx)
{
    uint32_t pos = seq % (2u * k), d, c = 0, i, v;
    sm32 r;
    if (pos < k) {
        idx[0] = pos;
        return 1;
    }
    r.s = frame_seed(session_id, seq);
    d = LT_REPAIR_MIN + sm32_next(&r) % (LT_REPAIR_MAX - LT_REPAIR_MIN + 1u);
    if (d > k)
        d = k;
    while (c < d) {
        v = sm32_next(&r) % k;
        for (i = 0; i < c && idx[i] != v; i++)
            ;
        if (i == c)
            idx[c++] = v;
    }
    return c;
}

#define RS_MAX_GROUP 96u

/* Interleaved Cauchy schedule: (group, group size n, row code) of frame seq.
 * Even sweeps send the systematic rows 0..n-1, odd sweeps repair rows n..255
 * (rotating), with the group-to-slot assignment rotating every 8 rounds. */
static void rs_symbol_at(uint32_t k, uint32_t seq, uint32_t *group, uint32_t *n, uint32_t *code)
{
    uint32_t groups = (k + RS_MAX_GROUP - 1) / RS_MAX_GROUP;
    uint32_t sweep = seq / k, slot = seq % k;
    uint32_t row = slot / groups;
    uint32_t width = k - row * groups < groups ? k - row * groups : groups;
    uint32_t g = (uint32_t)(((uint64_t)(slot % groups) + row / 8u + sweep) % width);
    uint32_t nn = (k - 1u - g) / groups + 1u;
    *group = g;
    *n = nn;
    *code = (sweep % 2u == 0) ? row : nn + (uint32_t)(((uint64_t)(sweep / 2u) * nn + row) % (256u - nn));
}

/* ---- encoder -------------------------------------------------------------- */

struct d3_enc {
    uint8_t *blocks;          /* k * bl, zero padded */
    uint32_t k, bl, groups;
    uint32_t session_id;
    int      cauchy;
};

d3_enc *d3_enc_new(const uint8_t *payload, size_t n, uint16_t block_len, uint16_t session_id, int cauchy)
{
    d3_enc *e;
    uint64_t k;
    if (!payload || n == 0 || block_len == 0)
        return NULL;
    k = ((uint64_t)n + block_len - 1) / block_len;
    if (k > D3_MAX_K || (cauchy && block_len > D3_MAX_BLOCK_RS))
        return NULL;
    e = (d3_enc *)calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->k = (uint32_t)k;
    e->bl = block_len;
    e->groups = (e->k + RS_MAX_GROUP - 1) / RS_MAX_GROUP;
    e->session_id = session_id;
    e->cauchy = cauchy ? 1 : 0;
    e->blocks = (uint8_t *)calloc((size_t)k, block_len);
    if (!e->blocks) {
        free(e);
        return NULL;
    }
    memcpy(e->blocks, payload, n);
    if (e->cauchy)
        gf_init();
    return e;
}

uint32_t d3_enc_k(const d3_enc *e) { return e ? e->k : 0; }

void d3_enc_block(d3_enc *e, uint32_t seq, uint8_t *out)
{
    size_t bl = e->bl;
    if (e->cauchy) {
        uint32_t g, n, code, col;
        rs_symbol_at(e->k, seq, &g, &n, &code);
        /* block (group, column) is source block column * groups + group */
        if (code < n) {
            memcpy(out, e->blocks + ((size_t)code * e->groups + g) * bl, bl);
        } else {
            memset(out, 0, bl);
            for (col = 0; col < n; col++)
                scale_xor(out, e->blocks + ((size_t)col * e->groups + g) * bl, bl, gf_inv[code ^ col]);
        }
    } else {
        uint32_t idx[LT_REPAIR_MAX], c, i;
        c = lt_composition(e->k, e->session_id, seq, idx);
        memset(out, 0, bl);
        for (i = 0; i < c; i++)
            xor_into(out, e->blocks + (size_t)idx[i] * bl, bl);
    }
}

void d3_enc_free(d3_enc *e)
{
    if (!e)
        return;
    free(e->blocks);
    free(e);
}

/* ---- decoder -------------------------------------------------------------- */

/* set of seen seqs: open addressing over seq + 1 (0 = empty) */
typedef struct seqset {
    uint64_t *slot;
    uint32_t  cap, n;        /* cap: power of two */
} seqset;

static uint32_t seq_hash(uint32_t seq, uint32_t mask)
{
    return (uint32_t)(((uint64_t)seq * 0x9E3779B97F4A7C15ull) >> 32) & mask;
}

/* 1 = inserted, 0 = already present, -1 = out of memory */
static int seqset_add(seqset *s, uint32_t seq)
{
    uint64_t key = (uint64_t)seq + 1u;
    uint32_t i;
    if ((s->n + 1u) * 2u > s->cap) {
        uint32_t nc = s->cap ? s->cap * 2u : 1024u, j;
        uint64_t *ns = (uint64_t *)calloc(nc, sizeof(uint64_t));
        if (!ns)
            return -1;
        for (j = 0; j < s->cap; j++)
            if (s->slot[j]) {
                i = seq_hash((uint32_t)(s->slot[j] - 1u), nc - 1u);
                while (ns[i])
                    i = (i + 1u) & (nc - 1u);
                ns[i] = s->slot[j];
            }
        free(s->slot);
        s->slot = ns;
        s->cap = nc;
    }
    i = seq_hash(seq, s->cap - 1u);
    while (s->slot[i]) {
        if (s->slot[i] == key)
            return 0;
        i = (i + 1u) & (s->cap - 1u);
    }
    s->slot[i] = key;
    s->n++;
    return 1;
}

typedef struct lt_pend {
    uint8_t *data;                    /* bl bytes, reduced by solved blocks   */
    uint32_t idx[LT_REPAIR_MAX];      /* unsolved blocks still in the XOR     */
    uint8_t  left;
    uint8_t  alive;
} lt_pend;

typedef struct u32vec {
    uint32_t *v;
    uint32_t  n, cap;
} u32vec;

typedef struct rs_group {
    uint32_t n, held, n_rep;
    uint8_t  have[32];                /* bitmap over row codes 0..255         */
    uint8_t  rep_code[RS_MAX_GROUP];
    uint8_t *rep_data[RS_MAX_GROUP];
} rs_group;

struct d3_dec {
    d3_header h;
    uint32_t  k, bl;
    int       cauchy;
    uint8_t  *blocks;                 /* k * bl: source blocks in file order  */
    uint32_t  solved, useful, frames_new;
    seqset    seen;
    /* LT */
    uint8_t  *solved_bits;
    lt_pend  *pend;
    uint32_t  n_pend, cap_pend;
    u32vec   *by_block;               /* pending equations per block          */
    /* Cauchy */
    uint32_t  groups;
    rs_group *grp;
};

#define BIT_GET(m, i) (((m)[(i) >> 3] >> ((i) & 7u)) & 1u)
#define BIT_SET(m, i) ((m)[(i) >> 3] |= (uint8_t)(1u << ((i) & 7u)))

d3_dec *d3_dec_new(const d3_header *h)
{
    d3_dec *d;
    if (!h || h->k == 0 || h->block_len == 0)
        return NULL;
    d = (d3_dec *)calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->h = *h;
    d->k = h->k;
    d->bl = h->block_len;
    d->cauchy = (h->flags & D3_FLAG_CAUCHY96) ? 1 : 0;
    d->blocks = (uint8_t *)calloc(d->k, d->bl);
    if (!d->blocks)
        goto fail;
    if (d->cauchy) {
        gf_init();
        d->groups = (d->k + RS_MAX_GROUP - 1) / RS_MAX_GROUP;
        d->grp = (rs_group *)calloc(d->groups, sizeof(rs_group));
        if (!d->grp)
            goto fail;
    } else {
        d->solved_bits = (uint8_t *)calloc((d->k + 7u) / 8u, 1);
        d->by_block = (u32vec *)calloc(d->k, sizeof(u32vec));
        if (!d->solved_bits || !d->by_block)
            goto fail;
    }
    return d;
fail:
    d3_dec_free(d);
    return NULL;
}

void d3_dec_free(d3_dec *d)
{
    uint32_t i, j;
    if (!d)
        return;
    free(d->blocks);
    free(d->seen.slot);
    free(d->solved_bits);
    if (d->pend)
        for (i = 0; i < d->n_pend; i++)
            free(d->pend[i].data);
    free(d->pend);
    if (d->by_block)
        for (i = 0; i < d->k; i++)
            free(d->by_block[i].v);
    free(d->by_block);
    if (d->grp)
        for (i = 0; i < d->groups; i++)
            for (j = 0; j < d->grp[i].n_rep; j++)
                free(d->grp[i].rep_data[j]);
    free(d->grp);
    free(d);
}

static int u32vec_push(u32vec *v, uint32_t x)
{
    if (v->n == v->cap) {
        uint32_t nc = v->cap ? v->cap * 2u : 4u;
        uint32_t *nv = (uint32_t *)realloc(v->v, (size_t)nc * sizeof(uint32_t));
        if (!nv)
            return 0;
        v->v = nv;
        v->cap = nc;
    }
    v->v[v->n++] = x;
    return 1;
}

/* Peeling cascade. `data` (bl bytes, malloc'd) is consumed. */
static void lt_resolve(d3_dec *d, uint32_t b0, uint8_t *data)
{
    typedef struct { uint32_t b; uint8_t *data; } item;
    item *stack = NULL;
    uint32_t sp = 0, cap = 0;
    const size_t bl = d->bl;

#define LT_PUSH(B, D)                                                                  \
    do {                                                                               \
        if (sp == cap) {                                                               \
            uint32_t nc_ = cap ? cap * 2u : 16u;                                       \
            item *ns_ = (item *)realloc(stack, (size_t)nc_ * sizeof(item));            \
            if (!ns_) { free(D); break; }                                              \
            stack = ns_;                                                               \
            cap = nc_;                                                                 \
        }                                                                              \
        stack[sp].b = (B);                                                             \
        stack[sp].data = (D);                                                          \
        sp++;                                                                          \
    } while (0)

    LT_PUSH(b0, data);
    while (sp > 0) {
        item it = stack[--sp];
        uint8_t *dst;
        u32vec list;
        uint32_t i;
        if (BIT_GET(d->solved_bits, it.b)) {
            free(it.data);
            continue;
        }
        dst = d->blocks + (size_t)it.b * bl;
        memcpy(dst, it.data, bl);
        free(it.data);
        BIT_SET(d->solved_bits, it.b);
        d->solved++;
        list = d->by_block[it.b];
        memset(&d->by_block[it.b], 0, sizeof(u32vec));
        for (i = 0; i < list.n; i++) {
            lt_pend *p = &d->pend[list.v[i]];
            uint32_t j;
            if (!p->alive)
                continue;
            xor_into(p->data, dst, bl);
            for (j = 0; j < p->left && p->idx[j] != it.b; j++)
                ;
            if (j < p->left)
                p->idx[j] = p->idx[--p->left];
            if (p->left <= 1) {
                uint8_t *pd = p->data;
                p->alive = 0;
                p->data = NULL;
                if (p->left == 1 && !BIT_GET(d->solved_bits, p->idx[0]))
                    LT_PUSH(p->idx[0], pd);
                else
                    free(pd);
            }
        }
        free(list.v);
    }
#undef LT_PUSH
    free(stack);
}

static int lt_add(d3_dec *d, uint32_t seq, const uint8_t *block)
{
    uint32_t all[LT_REPAIR_MAX], idx[LT_REPAIR_MAX], c, i, j;
    uint8_t *tmp;
    c = lt_composition(d->k, d->h.session_id, seq, all);
    /* keep the unsolved blocks; the solved ones are XORed out below */
    for (i = j = 0; i < c; i++)
        if (!BIT_GET(d->solved_bits, all[i]))
            idx[j++] = all[i];
    if (j == 0)
        return 0;                    /* redundant: everything already known */
    tmp = (uint8_t *)malloc(d->bl);
    if (!tmp)
        return -1;
    memcpy(tmp, block, d->bl);
    for (i = 0; i < c && j != c; i++)
        if (BIT_GET(d->solved_bits, all[i]))
            xor_into(tmp, d->blocks + (size_t)all[i] * d->bl, d->bl);
    c = j;
    if (c == 1) {
        lt_resolve(d, idx[0], tmp);
        return 1;
    }
    if (d->n_pend == d->cap_pend) {
        uint32_t nc = d->cap_pend ? d->cap_pend * 2u : 64u;
        lt_pend *np = (lt_pend *)realloc(d->pend, (size_t)nc * sizeof(lt_pend));
        if (!np) {
            free(tmp);
            return -1;
        }
        d->pend = np;
        d->cap_pend = nc;
    }
    {
        uint32_t id = d->n_pend;
        lt_pend *p = &d->pend[id];
        p->data = tmp;
        memcpy(p->idx, idx, c * sizeof(uint32_t));
        p->left = (uint8_t)c;
        p->alive = 1;
        d->n_pend++;
        for (i = 0; i < c; i++)
            if (!u32vec_push(&d->by_block[idx[i]], id)) {
                /* cannot index it everywhere: forget the equation entirely */
                p->alive = 0;
                free(p->data);
                p->data = NULL;
                return -1;
            }
    }
    return 1;
}

/* Solves the missing columns of a group that holds n distinct rows. */
static uint32_t rs_recover(d3_dec *d, uint32_t gi)
{
    rs_group *g = &d->grp[gi];
    const size_t bl = d->bl;
    uint32_t missing[RS_MAX_GROUP], r = 0, c, i, pivot, row;
    uint8_t m[RS_MAX_GROUP][RS_MAX_GROUP];

    for (c = 0; c < g->n; c++)
        if (!BIT_GET(g->have, c))
            missing[r++] = c;
    if (r != g->n_rep)
        return 0;                    /* cannot happen: held == n */
    for (i = 0; i < r; i++) {
        uint32_t code = g->rep_code[i];
        uint8_t *blk = g->rep_data[i];
        for (c = 0; c < g->n; c++)
            if (BIT_GET(g->have, c))
                scale_xor(blk, d->blocks + ((size_t)c * d->groups + gi) * bl, bl, gf_inv[code ^ c]);
        for (c = 0; c < r; c++)
            m[i][c] = gf_inv[code ^ missing[c]];
    }
    /* Every square submatrix of a Cauchy matrix is non-singular, so plain
     * elimination without pivoting cannot hit a zero pivot. */
    for (pivot = 0; pivot < r; pivot++) {
        uint8_t norm = gf_inv[m[pivot][pivot]];
        for (c = pivot; c < r; c++)
            m[pivot][c] = gf_mul[((unsigned)norm << 8) | m[pivot][c]];
        scale_inplace(g->rep_data[pivot], bl, norm);
        for (row = pivot + 1; row < r; row++) {
            uint8_t f = m[row][pivot];
            scale_xor(g->rep_data[row], g->rep_data[pivot], bl, f);
            for (c = pivot + 1; c < r; c++)
                m[row][c] ^= gf_mul[((unsigned)f << 8) | m[pivot][c]];
        }
    }
    for (row = r; row-- > 0;) {
        uint8_t *v = g->rep_data[row];
        for (c = row + 1; c < r; c++)
            scale_xor(v, g->rep_data[c], bl, m[row][c]);
    }
    for (i = 0; i < r; i++) {
        memcpy(d->blocks + ((size_t)missing[i] * d->groups + gi) * bl, g->rep_data[i], bl);
        BIT_SET(g->have, missing[i]);
        free(g->rep_data[i]);
        g->rep_data[i] = NULL;
    }
    g->n_rep = 0;
    return r;
}

static int rs_add(d3_dec *d, uint32_t seq, const uint8_t *block)
{
    uint32_t gi, n, code;
    rs_group *g;
    rs_symbol_at(d->k, seq, &gi, &n, &code);
    g = &d->grp[gi];
    g->n = n;
    if (g->held == n || BIT_GET(g->have, code))
        return 0;
    if (code < n) {
        memcpy(d->blocks + ((size_t)code * d->groups + gi) * d->bl, block, d->bl);
        d->solved++;
    } else {
        uint8_t *copy = (uint8_t *)malloc(d->bl);
        if (!copy)
            return -1;
        memcpy(copy, block, d->bl);
        g->rep_code[g->n_rep] = (uint8_t)code;
        g->rep_data[g->n_rep] = copy;
        g->n_rep++;
    }
    BIT_SET(g->have, code);
    g->held++;
    if (g->held == n && g->n_rep > 0)
        d->solved += rs_recover(d, gi);
    return 1;
}

int d3_dec_add(d3_dec *d, uint32_t seq, const uint8_t *block)
{
    int r;
    if (!d || !block)
        return 0;
    r = seqset_add(&d->seen, seq);
    if (r <= 0)
        return r;                    /* duplicate seq (or OOM) */
    d->frames_new++;
    if (d->solved >= d->k)
        return 0;
    r = d->cauchy ? rs_add(d, seq, block) : lt_add(d, seq, block);
    if (r == 1)
        d->useful++;
    return r;
}

int d3_dec_complete(const d3_dec *d) { return d && d->solved >= d->k; }

void d3_dec_stats_get(const d3_dec *d, d3_dec_stats *out)
{
    memset(out, 0, sizeof *out);
    if (!d)
        return;
    out->k = d->k;
    out->solved = d->solved;
    out->useful = d->useful;
    out->frames_new = d->frames_new;
}

const uint8_t *d3_dec_block0(const d3_dec *d)
{
    if (!d)
        return NULL;
    if (d->cauchy ? BIT_GET(d->grp[0].have, 0) : BIT_GET(d->solved_bits, 0))
        return d->blocks;
    return NULL;
}

uint8_t *d3_dec_assemble(const d3_dec *d)
{
    uint8_t *out;
    if (!d3_dec_complete(d))
        return NULL;
    out = (uint8_t *)malloc(d->h.total_len);
    if (out)   /* both layouts keep source block i at offset i * bl */
        memcpy(out, d->blocks, d->h.total_len);
    return out;
}

/* ---- gzip ------------------------------------------------------------------ */

/* NULL when deflate cannot save 5 % (the caller then stores the content). */
static uint8_t *gzip_mem(const uint8_t *src, size_t n, size_t *out_n)
{
    size_t dl = 0;
    int method;
    uint8_t *def = cp_deflate_auto(src, n, &dl, &method), *out;   /* same chooser as CPO1 */
    static const uint8_t hdr[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff };
    *out_n = 0;
    if (!def)
        return NULL;
    out = (uint8_t *)malloc(10 + dl + 8);
    if (out) {
        memcpy(out, hdr, 10);
        memcpy(out + 10, def, dl);
        le32w(out + 10 + dl, cp_crc32(src, n));
        le32w(out + 14 + dl, (uint32_t)n);
        *out_n = 10 + dl + 8;
    }
    mz_free(def);
    return out;
}

/* Inflates one gzip member into exactly out_len bytes; checks CRC and ISIZE. */
static int gunzip_exact(const uint8_t *in, size_t n, uint8_t *out, size_t out_len)
{
    size_t p = 10, in_sz, out_sz = out_len;
    uint8_t flg;
    tinfl_decompressor *r;
    tinfl_status st;
    if (n < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
        return 0;
    flg = in[3];
    if (flg & 0xE0)
        return 0;
    if (flg & 0x04) {                                   /* FEXTRA */
        if (n - p < 2)
            return 0;
        p += 2u + le16r(in + p);
    }
    if (flg & 0x08) {                                   /* FNAME */
        while (p < n && in[p])
            p++;
        p++;
    }
    if (flg & 0x10) {                                   /* FCOMMENT */
        while (p < n && in[p])
            p++;
        p++;
    }
    if (flg & 0x02)                                     /* FHCRC */
        p += 2;
    if (p >= n || n - p < 8)
        return 0;
    r = tinfl_decompressor_alloc();
    if (!r)
        return 0;
    in_sz = n - p - 8;
    st = tinfl_decompress(r, in + p, &in_sz, out, out, &out_sz, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    tinfl_decompressor_free(r);
    if (st != TINFL_STATUS_DONE || out_sz != out_len)
        return 0;   /* truncated, corrupt, or expands past the declared length */
    p += in_sz;
    if (n - p < 8)
        return 0;
    return le32r(in + p) == cp_crc32(out, out_len) && le32r(in + p + 4) == (uint32_t)out_len;
}

/* ---- DCF2 container --------------------------------------------------------- */

int d3_container_pack(const char *name, const char *type, const uint8_t *data, size_t n, int try_gzip,
                      uint8_t **out, size_t *out_n, int *gzipped)
{
    size_t nl = strlen(name), tl = strlen(type), gl = 0, tn;
    uint8_t *gz = NULL, *o;
    const uint8_t *tx = data;
    int use_gz = 0;
    *out = NULL;
    *out_n = 0;
    if (gzipped)
        *gzipped = 0;
    if (!data || n == 0 || (uint64_t)n > CP_MAX_TOTAL_BYTES_D3 || nl > 0xFFFFu || tl > 0xFFFFu)
        return 0;
    if (try_gzip && n >= 768) {
        gz = gzip_mem(data, n, &gl);
        if (gz && gl + 64 < n) {
            use_gz = 1;
            tx = gz;
        }
    }
    tn = use_gz ? gl : n;
    o = (uint8_t *)malloc(D3_FILE_HDR_LEN + nl + tl + tn);
    if (!o) {
        free(gz);
        return 0;
    }
    memcpy(o, "DCF2", 4);
    o[4] = (uint8_t)use_gz;
    le16w(o + 5, (uint32_t)nl);
    le16w(o + 7, (uint32_t)tl);
    le32w(o + 9, (uint32_t)n);
    le32w(o + 13, (uint32_t)tn);
    cp_sha256(data, n, o + 17);
    memcpy(o + D3_FILE_HDR_LEN, name, nl);
    memcpy(o + D3_FILE_HDR_LEN + nl, type, tl);
    memcpy(o + D3_FILE_HDR_LEN + nl + tl, tx, tn);
    free(gz);
    *out = o;
    *out_n = D3_FILE_HDR_LEN + nl + tl + tn;
    if (gzipped)
        *gzipped = use_gz;
    return 1;
}

int d3_container_unpack(const uint8_t *c, size_t n, d3_file *f, uint8_t **owned)
{
    uint32_t nl, tl, flen, tlen;
    size_t off;
    uint8_t digest[32], *buf = NULL;
    const uint8_t *data;
    *owned = NULL;
    if (!c || n < D3_FILE_HDR_LEN || memcmp(c, "DCF2", 4) != 0 || c[4] > 1)
        return 0;
    nl = le16r(c + 5);
    tl = le16r(c + 7);
    flen = le32r(c + 9);
    tlen = le32r(c + 13);
    off = (size_t)D3_FILE_HDR_LEN + nl + tl;
    if (flen == 0 || flen > CP_MAX_TOTAL_BYTES_D3 || tlen == 0 || tlen > CP_MAX_TOTAL_BYTES_D3 || off > n ||
        n - off != tlen)
        return 0;
    data = c + off;
    if (c[4] == 1) {
        if (tlen < 18 || le32r(data + tlen - 4) != flen)
            return 0;
        buf = (uint8_t *)malloc(flen);
        if (!buf)
            return 0;
        if (!gunzip_exact(data, tlen, buf, flen)) {
            free(buf);
            return 0;
        }
        data = buf;
    } else if (tlen != flen) {
        return 0;
    }
    cp_sha256(data, flen, digest);
    if (memcmp(digest, c + 17, 32) != 0) {
        free(buf);
        return 0;
    }
    f->name = c + D3_FILE_HDR_LEN;
    f->name_len = nl;
    f->type = c + D3_FILE_HDR_LEN + nl;
    f->type_len = tl;
    f->data = data;
    f->len = flen;
    f->gzipped = c[4] == 1;
    *owned = buf;
    return 1;
}

/* ---- CPO1 -> DCF2 (sender) --------------------------------------------------- */

static const char *ext_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && dot[1] && !strchr(dot, '/') && !strchr(dot, '\\') ? dot + 1 : "";
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static const char *type_for_name(const char *name)
{
    static const char *const map[][2] = {
        { "txt", "text/plain" },        { "md", "text/markdown" },     { "csv", "text/csv" },
        { "htm", "text/html" },         { "html", "text/html" },       { "json", "application/json" },
        { "xml", "application/xml" },   { "pdf", "application/pdf" },  { "png", "image/png" },
        { "jpg", "image/jpeg" },        { "jpeg", "image/jpeg" },      { "gif", "image/gif" },
        { "webp", "image/webp" },       { "bmp", "image/bmp" },        { "svg", "image/svg+xml" },
        { "mp3", "audio/mpeg" },        { "mp4", "video/mp4" },        { "mov", "video/quicktime" },
        { "zip", "application/zip" },   { "7z", "application/x-7z-compressed" },
        { "gz", "application/gzip" },   { "apk", "application/vnd.android.package-archive" },
        { NULL, NULL } };
    const char *e = ext_of(name);
    int i;
    for (i = 0; map[i][0]; i++)
        if (ieq(e, map[i][0]))
            return map[i][1];
    return "application/octet-stream";
}

static const char *image_ext_for(const char *mime)
{
    static const char *const map[][2] = { { "image/png", "png" },   { "image/jpeg", "jpg" }, { "image/gif", "gif" },
                                          { "image/webp", "webp" }, { "image/bmp", "bmp" },  { NULL, NULL } };
    int i;
    for (i = 0; map[i][0]; i++)
        if (ieq(mime, map[i][0]))
            return map[i][1];
    return "bin";
}

/* Stored (method 0) zip with UTF-8 names; entries already sanitized/unique. */
static uint8_t *zip_stored(size_t count, char *const *names, const cp_file_entry *e, size_t *out_n)
{
    uint64_t total = 22;
    size_t i, p = 0, cd_start;
    uint8_t *z;
    uint32_t *crc, *ofs;
    *out_n = 0;
    for (i = 0; i < count; i++)
        total += 30u + 46u + 2u * strlen(names[i]) + e[i].size;
    if (total > 0xFFFFFFFFu || count > 0xFFFFu)
        return NULL;
    z = (uint8_t *)malloc((size_t)total);
    crc = (uint32_t *)malloc(count * sizeof(uint32_t));
    ofs = (uint32_t *)malloc(count * sizeof(uint32_t));
    if (!z || !crc || !ofs) {
        free(z);
        free(crc);
        free(ofs);
        return NULL;
    }
    for (i = 0; i < count; i++) {
        size_t nl = strlen(names[i]);
        crc[i] = cp_crc32(e[i].data, (size_t)e[i].size);
        ofs[i] = (uint32_t)p;
        le32w(z + p, 0x04034b50u);
        le16w(z + p + 4, 20);
        le16w(z + p + 6, 0x0800);              /* UTF-8 names */
        le16w(z + p + 8, 0);                   /* stored      */
        le16w(z + p + 10, 0);
        le16w(z + p + 12, 0x21);               /* 1980-01-01  */
        le32w(z + p + 14, crc[i]);
        le32w(z + p + 18, (uint32_t)e[i].size);
        le32w(z + p + 22, (uint32_t)e[i].size);
        le16w(z + p + 26, (uint32_t)nl);
        le16w(z + p + 28, 0);
        memcpy(z + p + 30, names[i], nl);
        p += 30 + nl;
        if (e[i].size)
            memcpy(z + p, e[i].data, (size_t)e[i].size);
        p += (size_t)e[i].size;
    }
    cd_start = p;
    for (i = 0; i < count; i++) {
        size_t nl = strlen(names[i]);
        le32w(z + p, 0x02014b50u);
        le16w(z + p + 4, 20);
        le16w(z + p + 6, 20);
        le16w(z + p + 8, 0x0800);
        le16w(z + p + 10, 0);
        le16w(z + p + 12, 0);
        le16w(z + p + 14, 0x21);
        le32w(z + p + 16, crc[i]);
        le32w(z + p + 20, (uint32_t)e[i].size);
        le32w(z + p + 24, (uint32_t)e[i].size);
        le16w(z + p + 28, (uint32_t)nl);
        memset(z + p + 30, 0, 12);             /* extra, comment, disk, attrs */
        le32w(z + p + 42, ofs[i]);
        memcpy(z + p + 46, names[i], nl);
        p += 46 + nl;
    }
    le32w(z + p, 0x06054b50u);
    le16w(z + p + 4, 0);
    le16w(z + p + 6, 0);
    le16w(z + p + 8, (uint32_t)count);
    le16w(z + p + 10, (uint32_t)count);
    le32w(z + p + 12, (uint32_t)(p - cd_start));
    le32w(z + p + 16, (uint32_t)cd_start);
    le16w(z + p + 20, 0);
    p += 22;
    free(crc);
    free(ofs);
    *out_n = p;
    return z;
}

int d3_from_cpo(const uint8_t *cpo, size_t n, uint8_t **out, size_t *out_n, int *gzipped)
{
    cp_container_hdr h;
    uint8_t *owned = NULL;
    const uint8_t *raw = NULL;
    int ok = 0;
    *out = NULL;
    *out_n = 0;
    if (!cp_container_decode(cpo, n, CP_MAX_TOTAL_BYTES, &h, &owned, &raw))
        return 0;
    if (h.kind == CP_KIND_TEXT) {
        if (h.raw_len > 0)
            ok = d3_container_pack(D3_SNIPPET_NAME, D3_SNIPPET_TYPE, raw, (size_t)h.raw_len, 1, out, out_n, gzipped);
    } else if (h.kind == CP_KIND_IMAGE) {
        char mime[128], name[32];
        int pre;
        cp_meta_get(h.meta, h.meta_len, "mime", mime, sizeof mime);
        if (!mime[0])
            snprintf(mime, sizeof mime, "application/octet-stream");
        snprintf(name, sizeof name, "image.%s", image_ext_for(mime));
        pre = cp_is_compressed_magic(raw, (size_t)h.raw_len) || !strcmp(image_ext_for(mime), "jpg") ||
              !strcmp(image_ext_for(mime), "png") || !strcmp(image_ext_for(mime), "gif") ||
              !strcmp(image_ext_for(mime), "webp");
        ok = d3_container_pack(name, mime, raw, (size_t)h.raw_len, !pre, out, out_n, gzipped);
    } else {
        size_t off = 0, count = 0, i;
        cp_file_entry e, *list = NULL;
        int st;
        while ((st = cp_files_next(raw, (size_t)h.raw_len, &off, &e)) == 1)
            count++;
        if (st < 0 || count == 0 || count > CPO_MAX_FILES)
            goto out;
        list = (cp_file_entry *)malloc(count * sizeof *list);
        if (!list)
            goto out;
        off = 0;
        for (i = 0; i < count; i++)
            cp_files_next(raw, (size_t)h.raw_len, &off, &list[i]);
        if (count == 1 && list[0].size > 0 && list[0].name_len > 0 && list[0].name_len < 4096) {
            char *nm = (char *)malloc(list[0].name_len + 1);
            if (nm) {
                int pre;
                memcpy(nm, list[0].name, list[0].name_len);
                nm[list[0].name_len] = 0;
                pre = cp_is_compressed_name(nm) || cp_is_compressed_magic(list[0].data, (size_t)list[0].size);
                ok = d3_container_pack(nm, type_for_name(nm), list[0].data, (size_t)list[0].size, !pre, out,
                                       out_n, gzipped);
                free(nm);
            }
        } else {
            /* several files (or one empty one: DCF2 cannot carry 0 bytes) -> stored zip */
            char **names = (char **)calloc(count, sizeof(char *));
            int all_pre = 1;
            if (names) {
                size_t zn = 0;
                uint8_t *zip;
                char zname[48];
                for (i = 0; i < count; i++) {
                    names[i] = (char *)malloc(CP_NAME_MAX + 1);
                    if (!names[i])
                        break;
                    cp_sanitize_name(list[i].name, list[i].name_len, names[i], CP_NAME_MAX + 1);
                    cp_unique_name(names[i], CP_NAME_MAX + 1, names, i);
                    if (!(cp_is_compressed_name(names[i]) ||
                          cp_is_compressed_magic(list[i].data, (size_t)list[i].size)))
                        all_pre = 0;
                }
                if (i == count) {
                    zip = zip_stored(count, names, list, &zn);
                    snprintf(zname, sizeof zname, "CodeTrans-%ufiles.zip", (unsigned)count);
                    if (zip)
                        ok = d3_container_pack(zname, "application/zip", zip, zn, !all_pre, out, out_n, gzipped);
                    free(zip);
                }
                for (i = 0; i < count; i++)
                    free(names[i]);
                free(names);
            }
        }
        free(list);
    }
out:
    free(owned);
    return ok;
}
