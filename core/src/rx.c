/*
 * Receiver: session management, dedup, RaptorQ decode, verification and
 * result output. See core/PROTOCOL.md §5.
 *
 * Locking: one mutex per cp_rx protects all bookkeeping. The expensive work
 * (RaptorQ block decode, container assembly, inflate, SHA-256, file writes)
 * runs WITHOUT the lock:
 *   - a block being decoded is marked busy; the owning thread holds a session
 *     reference. Symbols for a busy block (or one that already has a decoder)
 *     are queued in the block's pending list and fed by the owner, which
 *     retries as long as new symbols keep arriving.
 *   - a fully decoded session is marked finalizing (frames for it are ignored
 *     through the completed-session LRU), finished by one thread, and then
 *     published as the pending result.
 *   - sessions that are evicted / reset while referenced are marked dead and
 *     freed by the last reference holder.
 *
 * Decimen v3 sessions (d3.c) live in a second list. Their decoders are cheap
 * per frame except the occasional Cauchy group solve, so each session has its
 * own mutex around the decoder; rx->mu is never held while taking it.
 */
#include "cp_internal.h"

#define RX_MAX_ACTIVE      2
#define RX_DONE_LRU        32
#define RX_MAX_EXTRA       64u        /* unique symbols beyond K before giving up on a block */
#define RX_PLAIN_MAX       16384u
#define RX_PLAIN_QUEUE     4
#define RX_PLAIN_RECENT    4
#define RX_RATE_BUCKETS    20
#define RX_RATE_BUCKET_MS  100u
#define RX_TEXT_FULL_MAX   (1u << 20)
#define RX_TEXT_PREVIEW    (64u << 10)

typedef struct rx_pend {
    struct rx_pend *next;
    uint32_t esi;
    uint8_t data[1];               /* T bytes (allocated past the struct) */
} rx_pend;

typedef struct rx_block {
    uint32_t  K;
    uint64_t  bytes;               /* transfer bytes in this block            */
    uint8_t  *buf;                 /* K*T bytes, source symbols / decoded data */
    uint8_t  *have;                /* K-bit source ESI bitmap (dedup)          */
    uint32_t *rep;                 /* sorted unique repair ESIs (dedup)        */
    uint32_t  n_rep, cap_rep;
    uint32_t  received;            /* unique symbols                          */
    uint32_t  tried_at;            /* `received` when the last attempt started */
    rx_pend  *ph, *pt;             /* symbols not yet fed to the decoder      */
    fec_dec  *dec;
    uint8_t   busy, done;
} rx_block;

typedef struct rx_key {
    int      proto;                /* CP_PROTO_RQ / CP_PROTO_D3                */
    uint32_t id;
    cp_oti   oti;                  /* RQ                                       */
    uint16_t d3_k, d3_bl;          /* D3 stream identity (with id = sessionId) */
    uint32_t d3_total, d3_fnv;
    uint8_t  d3_flags;             /* critical half only                       */
    uint8_t  ob_tag[8];            /* OB: session tag (id = its low 32 bits)   */
} rx_key;

typedef struct rx_session {
    struct rx_session *next;
    rx_key    key;
    rx_block *blocks;
    uint32_t  blocks_done;
    uint64_t  last_seen;
    uint64_t  have, need;
    int       kind;
    int       refs;
    int       dead;
    int       finalizing;
} rx_session;

typedef struct d3_session {
    struct d3_session *next;
    rx_key    key;
    d3_dec   *dec;                 /* guarded by mu                            */
    cp_mutex  mu;
    int       claimed;             /* (mu) complete, one thread assembles it   */
    uint64_t  last_seen;
    uint64_t  have, need;          /* progress snapshot                        */
    int       kind;
    int       refs;
    int       dead;
    int       finalizing;
} d3_session;

/* Optical board session, bound by its manifest. All bookkeeping is guarded
 * by rx->mu (feeding a unit is a copy; a group solve is a few tens of us);
 * the final SHA-256 + container decode run without the lock. */
typedef struct ob_group {
    uint8_t  *rows;                 /* up to 32 received rows (lazy)            */
    uint8_t   ids[OB_GROUP];
    uint8_t   n, solved;
} ob_group;

typedef struct ob_session {
    struct ob_session *next;
    rx_key    key;
    uint8_t   mode;                 /* OB_TYPE_TILE / OB_TYPE_BAND              */
    uint32_t  width, length, nrows, ngroups, solved;
    uint8_t   digest[32];
    ob_group *groups;
    uint8_t  *data;                 /* nrows * width recovered source rows     */
    uint64_t  last_seen;
    uint64_t  have, need;
    int       kind;
    int       refs;
    int       dead;
    int       finalizing;
} ob_session;

typedef struct ob_pend {
    size_t  len;
    uint8_t body[OB_BAND_BODY];
} ob_pend;

struct cp_rx {
    cp_mutex    mu;
    char       *out_dir;
    uint64_t    max_bytes;
    int         enable_d3, enable_plain;
    rx_session *sessions;          /* active + finalizing                     */
    d3_session *d3s;               /* Decimen: active + finalizing            */
    ob_session *obs;               /* optical board: active + finalizing      */
    ob_pend    *ob_pend;           /* ring of units seen before a manifest    */
    int         ob_pend_head, ob_pend_n;
    uint64_t    tick;
    uint64_t    generation;        /* bumped by cp_rx_reset                   */
    rx_key      done[RX_DONE_LRU]; /* MRU first                               */
    int         n_done;
    cp_result  *result;
    uint8_t    *plain_q[RX_PLAIN_QUEUE];
    size_t      plain_len[RX_PLAIN_QUEUE];
    int         plain_n;
    uint8_t     recent[RX_PLAIN_RECENT][32];
    int         n_recent, recent_pos;
    int         codes_in_frame;
    uint64_t    rate_epoch[RX_RATE_BUCKETS];
    uint64_t    rate_bytes[RX_RATE_BUCKETS];
    uint64_t    rate_start_ms, rate_last_ms;
    cp_rx_stats stats;             /* diagnostics (cp_rx_stats_get)            */
};

/* ---- small helpers ------------------------------------------------------ */

static int key_eq(const rx_key *a, const rx_key *b)
{
    return a->proto == b->proto && a->id == b->id && a->oti.F == b->oti.F && a->oti.T == b->oti.T &&
           a->oti.Z == b->oti.Z && a->oti.N == b->oti.N && a->oti.Al == b->oti.Al && a->d3_k == b->d3_k &&
           a->d3_bl == b->d3_bl && a->d3_total == b->d3_total && a->d3_fnv == b->d3_fnv &&
           a->d3_flags == b->d3_flags && !memcmp(a->ob_tag, b->ob_tag, sizeof a->ob_tag);
}

/* Result directory / session id of a Decimen stream: its 16-bit session id,
 * tagged so it cannot collide with a CodeTrans session directory. */
static uint32_t d3_tag(uint32_t sid) { return 0xD3000000u | (sid & 0xFFFFu); }

static char *xstrdup(const char *s)
{
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (d)
        memcpy(d, s, n + 1);
    return d;
}

static void pend_free_list(rx_pend *p)
{
    while (p) {
        rx_pend *n = p->next;
        free(p);
        p = n;
    }
}

static void block_release(rx_block *b)
{
    free(b->buf);
    free(b->have);
    free(b->rep);
    pend_free_list(b->ph);
    fec_dec_free(b->dec);
    b->buf = NULL;
    b->have = NULL;
    b->rep = NULL;
    b->ph = b->pt = NULL;
    b->dec = NULL;
    b->n_rep = b->cap_rep = 0;
}

static void session_free(rx_session *s)
{
    uint32_t i;
    if (!s)
        return;
    if (s->blocks)
        for (i = 0; i < s->key.oti.Z; i++)
            block_release(&s->blocks[i]);
    free(s->blocks);
    free(s);
}

static void session_unlink(cp_rx *rx, rx_session *s)
{
    rx_session **pp = &rx->sessions;
    while (*pp) {
        if (*pp == s) {
            *pp = s->next;
            s->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* lock held */
static void session_kill(cp_rx *rx, rx_session *s)
{
    session_unlink(rx, s);
    s->dead = 1;
    if (s->refs == 0)
        session_free(s);
}

/* lock held */
static void session_unref(rx_session *s)
{
    s->refs--;
    if (s->dead && s->refs == 0)
        session_free(s);
}

static rx_session *session_new(const cp_frame *f)
{
    rx_session *s = (rx_session *)calloc(1, sizeof *s);
    uint32_t i;
    if (!s)
        return NULL;
    s->key.proto = CP_PROTO_RQ;
    s->key.id = f->session_id;
    s->key.oti = f->oti;
    s->blocks = (rx_block *)calloc(f->oti.Z, sizeof(rx_block));
    if (!s->blocks) {
        free(s);
        return NULL;
    }
    for (i = 0; i < f->oti.Z; i++) {
        s->blocks[i].K = cp_oti_block_K(&f->oti, (uint8_t)i);
        s->blocks[i].bytes = cp_oti_block_bytes(&f->oti, (uint8_t)i);
    }
    s->need = cp_oti_Kt(&f->oti);
    return s;
}

static int done_find(cp_rx *rx, const rx_key *k)
{
    int i;
    for (i = 0; i < rx->n_done; i++)
        if (key_eq(&rx->done[i], k))
            return i;
    return -1;
}

static void done_add(cp_rx *rx, const rx_key *k)
{
    int i = done_find(rx, k);
    if (i < 0)
        i = rx->n_done < RX_DONE_LRU ? rx->n_done++ : RX_DONE_LRU - 1;
    memmove(&rx->done[1], &rx->done[0], (size_t)i * sizeof(rx_key));
    rx->done[0] = *k;
}

static void done_remove(cp_rx *rx, const rx_key *k)
{
    int i = done_find(rx, k);
    if (i < 0)
        return;
    memmove(&rx->done[i], &rx->done[i + 1], (size_t)(rx->n_done - i - 1) * sizeof(rx_key));
    rx->n_done--;
}

static void rate_add(cp_rx *rx, uint64_t bytes)
{
    uint64_t now = cp_mono_ms(), ep = now / RX_RATE_BUCKET_MS;
    int idx = (int)(ep % RX_RATE_BUCKETS);
    if (rx->rate_last_ms == 0 || now - rx->rate_last_ms > 2000u)
        rx->rate_start_ms = now;
    rx->rate_last_ms = now;
    if (rx->rate_epoch[idx] != ep) {
        rx->rate_epoch[idx] = ep;
        rx->rate_bytes[idx] = 0;
    }
    rx->rate_bytes[idx] += bytes;
}

static uint64_t rate_get(cp_rx *rx)
{
    uint64_t now = cp_mono_ms(), ep = now / RX_RATE_BUCKET_MS, sum = 0, win;
    int i;
    for (i = 0; i < RX_RATE_BUCKETS; i++)
        if (rx->rate_epoch[i] + RX_RATE_BUCKETS > ep && rx->rate_epoch[i] <= ep)
            sum += rx->rate_bytes[i];
    if (sum == 0)
        return 0;
    win = now - rx->rate_start_ms;
    if (win > RX_RATE_BUCKETS * RX_RATE_BUCKET_MS)
        win = RX_RATE_BUCKETS * RX_RATE_BUCKET_MS;
    if (win < 250u)
        win = 250u;
    return sum * 1000u / win;
}

static void detect_kind(rx_session *s, const uint8_t *first, size_t n)
{
    if (s->kind == CP_KIND_NONE && n >= 5 && !memcmp(first, "CPO1", 4) && first[4] >= CP_KIND_TEXT &&
        first[4] <= CP_KIND_FILES)
        s->kind = first[4];
}

/* ---- dedup sets ---------------------------------------------------------- */

static int rep_find(const rx_block *b, uint32_t esi, uint32_t *pos)
{
    uint32_t lo = 0, hi = b->n_rep;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (b->rep[mid] < esi)
            lo = mid + 1;
        else
            hi = mid;
    }
    *pos = lo;
    return lo < b->n_rep && b->rep[lo] == esi;
}

static int rep_insert(rx_block *b, uint32_t esi, uint32_t pos)
{
    if (b->n_rep == b->cap_rep) {
        uint32_t nc = b->cap_rep ? b->cap_rep * 2 : 16;
        uint32_t *n = (uint32_t *)realloc(b->rep, (size_t)nc * sizeof(uint32_t));
        if (!n)
            return 0;
        b->rep = n;
        b->cap_rep = nc;
    }
    memmove(b->rep + pos + 1, b->rep + pos, (size_t)(b->n_rep - pos) * sizeof(uint32_t));
    b->rep[pos] = esi;
    b->n_rep++;
    return 1;
}

/* ---- result output ------------------------------------------------------- */

static int write_file(const char *path, const uint8_t *data, uint64_t n)
{
    FILE *f = cp_fopen_utf8(path, "wb");
    int ok;
    if (!f)
        return 0;
    ok = 1;
    while (n > 0 && ok) {
        size_t chunk = n > (1u << 20) ? (1u << 20) : (size_t)n;
        if (fwrite(data, 1, chunk, f) != chunk)
            ok = 0;
        data += chunk;
        n -= chunk;
    }
    if (fclose(f) != 0)
        ok = 0;
    return ok;
}

static int mkdirs(const char *path)
{
    char *p = xstrdup(path), *c;
    int r;
    if (!p)
        return -1;
    for (c = p + 1; *c; c++) {
        if (*c == '/' || *c == '\\') {
            char save = *c;
            *c = 0;
            (void)cp_mkdir_utf8(p);   /* ignore: may exist or be a drive root */
            *c = save;
        }
    }
    r = cp_mkdir_utf8(p);
    free(p);
    return r;
}

static char *path_join(const char *dir, const char *name)
{
    size_t a = strlen(dir), b = strlen(name);
    char *p = (char *)malloc(a + b + 2);
    if (!p)
        return NULL;
    memcpy(p, dir, a);
    p[a] = '/';
    memcpy(p + a + 1, name, b + 1);
    return p;
}

/* valid UTF-8 copy for display: invalid sequences and NUL become U+FFFD */
static char *utf8_display(const uint8_t *s, size_t n, size_t *out_len)
{
    char *o = (char *)malloc(n * 3 + 1);
    size_t i = 0, j = 0;
    if (!o)
        return NULL;
    while (i < n) {
        uint32_t cp = 0;
        size_t len = cp_utf8_decode(s + i, n - i, &cp);
        if (len && cp != 0) {
            memcpy(o + j, s + i, len);
            j += len;
            i += len;
            continue;
        }
        o[j++] = (char)0xEF;
        o[j++] = (char)0xBF;
        o[j++] = (char)0xBD;
        i++;
    }
    o[j] = 0;
    *out_len = j;
    return o;
}

static const char *image_ext(const char *mime, const uint8_t *d, size_t n)
{
    static const char *const map[][2] = { { "image/png", "png" },   { "image/jpeg", "jpg" }, { "image/jpg", "jpg" },
                                          { "image/gif", "gif" },   { "image/webp", "webp" }, { "image/bmp", "bmp" },
                                          { "image/heic", "heic" }, { "image/heif", "heif" }, { "image/avif", "avif" },
                                          { "image/tiff", "tif" },  { NULL, NULL } };
    const char *sniffed;
    int i;
    for (i = 0; map[i][0]; i++)
        if (!strcmp(mime, map[i][0]))
            return map[i][1];
    sniffed = cp_image_mime_sniff(d, n);
    if (sniffed)
        for (i = 0; map[i][0]; i++)
            if (!strcmp(sniffed, map[i][0]))
                return map[i][1];
    return "bin";
}

/* lower-case, restricted charset; empty if nothing usable */
static void clean_mime(const char *in, char *out, size_t cap)
{
    size_t j = 0;
    for (; *in && j + 1 < cap; in++) {
        char c = *in;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("/.+-_!#$&^", c))
            out[j++] = c;
        else
            break;   /* stop at ';', spaces, etc. */
    }
    out[j] = 0;
    if (!strchr(out, '/'))
        out[0] = 0;
}

static cp_result *result_alloc(size_t count)
{
    cp_result *r = (cp_result *)calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->paths = (char **)calloc(count ? count : 1, sizeof(char *));
    r->names = (char **)calloc(count ? count : 1, sizeof(char *));
    r->sizes = (uint64_t *)calloc(count ? count : 1, sizeof(uint64_t));
    if (!r->paths || !r->names || !r->sizes) {
        cp_result_free(r);
        return NULL;
    }
    return r;
}

static int result_add_file(cp_result *r, const char *dir, const char *name, const uint8_t *data, uint64_t n)
{
    char *path = path_join(dir, name);
    char *nm = xstrdup(name);
    if (!path || !nm || !write_file(path, data, n)) {
        free(path);
        free(nm);
        return 0;
    }
    r->paths[r->count] = path;
    r->names[r->count] = nm;
    r->sizes[r->count] = n;
    r->count++;
    return 1;
}

/* Decodes the container and writes its files. NULL on any failure. */
static cp_result *build_result(const char *out_dir, uint32_t sid, const uint8_t *cont, size_t n, uint64_t max_raw)
{
    cp_container_hdr h;
    uint8_t *owned = NULL;
    const uint8_t *raw = NULL;
    cp_result *r = NULL;
    char sdir[16], *dir = NULL;
    int ok = 0;

    if (!cp_container_decode(cont, n, max_raw, &h, &owned, &raw))
        return NULL;
    snprintf(sdir, sizeof sdir, "%08x", (unsigned)sid);
    dir = path_join(out_dir, sdir);
    if (!dir || mkdirs(dir) != 0)
        goto out;

    if (h.kind == CP_KIND_TEXT) {
        size_t show = (size_t)h.raw_len;
        r = result_alloc(1);
        if (!r)
            goto out;
        r->kind = CP_KIND_TEXT;
        snprintf(r->mime, sizeof r->mime, "text/plain");
        if (h.raw_len > RX_TEXT_FULL_MAX) {
            show = cp_utf8_cut(raw, RX_TEXT_PREVIEW);
            r->text_truncated = 1;
        }
        r->text = utf8_display(raw, show, &r->text_len);
        if (!r->text || !result_add_file(r, dir, "text.txt", raw, h.raw_len))
            goto out;
    } else if (h.kind == CP_KIND_IMAGE) {
        char m[128], mm[128], name[32];
        cp_meta_get(h.meta, h.meta_len, "mime", m, sizeof m);
        clean_mime(m, mm, sizeof mm);
        r = result_alloc(1);
        if (!r)
            goto out;
        r->kind = CP_KIND_IMAGE;
        snprintf(name, sizeof name, "image.%s", image_ext(mm, raw, (size_t)h.raw_len));
        if (!mm[0])
            snprintf(mm, sizeof mm, "%s", "application/octet-stream");
        snprintf(r->mime, sizeof r->mime, "%s", mm);
        if (!result_add_file(r, dir, name, raw, h.raw_len))
            goto out;
    } else {
        size_t off = 0, count = 0;
        cp_file_entry e;
        int st;
        char cnt[24];
        /* pass 1: validate structure and count */
        while ((st = cp_files_next(raw, (size_t)h.raw_len, &off, &e)) == 1)
            if (++count > CPO_MAX_FILES)
                goto out;
        if (st < 0 || count == 0)
            goto out;
        if (cp_meta_get(h.meta, h.meta_len, "count", cnt, sizeof cnt) && strtoul(cnt, NULL, 10) != count)
            goto out;
        r = result_alloc(count);
        if (!r)
            goto out;
        r->kind = CP_KIND_FILES;
        snprintf(r->mime, sizeof r->mime, "application/octet-stream");
        off = 0;
        while (cp_files_next(raw, (size_t)h.raw_len, &off, &e) == 1) {
            char name[CP_NAME_MAX + 1];
            cp_sanitize_name(e.name, e.name_len, name, sizeof name);
            cp_unique_name(name, sizeof name, r->names, r->count);
            if (!result_add_file(r, dir, name, e.data, e.size))
                goto out;
        }
    }
    r->session_id = sid;
    ok = 1;
out:
    free(owned);
    free(dir);
    if (!ok) {
        cp_result_free(r);
        r = NULL;
    }
    return r;
}

void cp_result_free(cp_result *r)
{
    size_t i;
    if (!r)
        return;
    if (r->paths)
        for (i = 0; i < r->count; i++)
            free(r->paths[i]);
    if (r->names)
        for (i = 0; i < r->count; i++)
            free(r->names[i]);
    free(r->paths);
    free(r->names);
    free(r->sizes);
    free(r->text);
    free(r);
}

/* ---- decode / finalize (enter and leave with the lock held) ------------- */

static int finalize(cp_rx *rx, rx_session *s)
{
    uint64_t gen = rx->generation;
    uint64_t F = s->key.oti.F, off = 0;
    uint8_t *cont;
    cp_result *res = NULL;
    uint32_t i;
    int ev;

    s->finalizing = 1;
    done_add(rx, &s->key);
    cp_mutex_unlock(&rx->mu);

    /* every block is done: nobody else touches block buffers any more */
    cont = (uint8_t *)malloc((size_t)F);
    if (cont) {
        for (i = 0; i < s->key.oti.Z; i++) {
            rx_block *b = &s->blocks[i];
            memcpy(cont + off, b->buf, (size_t)b->bytes);
            off += b->bytes;
            free(b->buf);
            b->buf = NULL;
        }
        res = build_result(rx->out_dir, s->key.id, cont, (size_t)F, rx->max_bytes);
        free(cont);
    }

    cp_mutex_lock(&rx->mu);
    if (res) {
        if (gen == rx->generation) {
            cp_result_free(rx->result);
            rx->result = res;
            ev = CP_EV_COMPLETE;
        } else {
            cp_result_free(res);   /* reset happened meanwhile */
            ev = 0;
        }
    } else {
        done_remove(rx, &s->key);  /* allow a fresh attempt */
        ev = CP_EV_FAILED;
    }
    session_unlink(rx, s);
    s->dead = 1;
    session_unref(s);
    return ev;
}

static void fail_session(cp_rx *rx, rx_session *s) { session_kill(rx, s); }

static int run_decode(cp_rx *rx, rx_session *s, rx_block *b, uint8_t sbn)
{
    const uint32_t T = s->key.oti.T;
    fec_dec *dec = b->dec;
    for (;;) {
        rx_pend *list = b->ph, *p;
        uint32_t snapshot = b->received;
        uint8_t *have_copy = NULL;
        int r = -1;

        b->ph = b->pt = NULL;
        if (!b->buf)
            b->buf = (uint8_t *)calloc(b->K, T);
        if (!dec && b->buf && b->have) {
            have_copy = (uint8_t *)malloc((b->K + 7u) / 8u);
            if (have_copy)
                memcpy(have_copy, b->have, (b->K + 7u) / 8u);
        }
        cp_mutex_unlock(&rx->mu);

        if (b->buf) {
            r = 0;
            if (!dec) {
                uint32_t e;
                dec = fec_dec_new(&s->key.oti, sbn, b->buf);
                if (!dec)
                    r = -1;
                for (e = 0; dec && have_copy && e < b->K; e++)
                    if (have_copy[e >> 3] & (1u << (e & 7)))
                        if (fec_dec_add(dec, e, b->buf + (size_t)e * T) < 0)
                            r = -1;
            }
            for (p = list; p && r == 0; p = p->next)
                if (fec_dec_add(dec, p->esi, p->data) < 0)
                    r = -1;
            if (r == 0)
                r = fec_dec_try(dec);
        }
        free(have_copy);
        pend_free_list(list);

        cp_mutex_lock(&rx->mu);
        b->dec = dec;
        if (s->dead) {
            b->busy = 0;
            session_unref(s);
            return 0;
        }
        if (r == 1) {
            uint32_t had = b->received < b->K ? b->received : b->K;
            b->done = 1;
            b->busy = 0;
            fec_dec_free(b->dec);
            b->dec = NULL;
            pend_free_list(b->ph);
            b->ph = b->pt = NULL;
            free(b->have);
            free(b->rep);
            b->have = NULL;
            b->rep = NULL;
            b->n_rep = b->cap_rep = 0;
            s->have += b->K - had;
            s->blocks_done++;
            if (sbn == 0)
                detect_kind(s, b->buf, (size_t)b->bytes);
            if (s->blocks_done == s->key.oti.Z)
                return finalize(rx, s);
            session_unref(s);
            return 0;
        }
        if (r < 0) {
            b->busy = 0;
            fail_session(rx, s);
            session_unref(s);
            return CP_EV_FAILED;
        }
        b->tried_at = snapshot;
        if (b->received > snapshot && b->ph)
            continue;              /* more symbols arrived while decoding: retry */
        b->busy = 0;
        if (b->received >= b->K + RX_MAX_EXTRA) {
            fail_session(rx, s);
            session_unref(s);
            return CP_EV_FAILED;
        }
        session_unref(s);
        return 0;
    }
}

/* ---- push paths ---------------------------------------------------------- */

/* ---- Decimen session lifetime (lock held) -------------------------------- */

static void d3s_free(d3_session *s)
{
    d3_dec_free(s->dec);
    cp_mutex_destroy(&s->mu);
    free(s);
}

static void d3s_unlink(cp_rx *rx, d3_session *s)
{
    d3_session **pp = &rx->d3s;
    while (*pp) {
        if (*pp == s) {
            *pp = s->next;
            s->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void d3s_kill(cp_rx *rx, d3_session *s)
{
    d3s_unlink(rx, s);
    s->dead = 1;
    if (s->refs == 0)
        d3s_free(s);
}

static void d3s_unref(d3_session *s)
{
    s->refs--;
    if (s->dead && s->refs == 0)
        d3s_free(s);
}

static void obs_kill(cp_rx *rx, ob_session *s);

/* all protocols share the RX_MAX_ACTIVE budget */
static int active_count(cp_rx *rx)
{
    int n = 0;
    rx_session *s;
    d3_session *d;
    ob_session *o;
    for (s = rx->sessions; s; s = s->next)
        if (!s->finalizing)
            n++;
    for (d = rx->d3s; d; d = d->next)
        if (!d->finalizing)
            n++;
    for (o = rx->obs; o; o = o->next)
        if (!o->finalizing)
            n++;
    return n;
}

/* Drops the active session that has gone longest without a frame. */
static int evict_lru(cp_rx *rx)
{
    rx_session *v = NULL, *it;
    d3_session *dv = NULL, *dt;
    ob_session *ov = NULL, *ot;
    uint64_t best = UINT64_MAX;
    for (it = rx->sessions; it; it = it->next)
        if (!it->finalizing && (!v || it->last_seen < v->last_seen))
            v = it;
    for (dt = rx->d3s; dt; dt = dt->next)
        if (!dt->finalizing && (!dv || dt->last_seen < dv->last_seen))
            dv = dt;
    for (ot = rx->obs; ot; ot = ot->next)
        if (!ot->finalizing && (!ov || ot->last_seen < ov->last_seen))
            ov = ot;
    if (v)
        best = v->last_seen;
    if (dv && dv->last_seen < best)
        best = dv->last_seen;
    if (ov && ov->last_seen < best)
        best = ov->last_seen;
    if (ov && ov->last_seen == best) {
        obs_kill(rx, ov);
        return 1;
    }
    if (dv && dv->last_seen == best) {
        d3s_kill(rx, dv);
        return 1;
    }
    if (v) {
        session_kill(rx, v);
        return 1;
    }
    return 0;
}

static int push_cp(cp_rx *rx, const uint8_t *data, size_t n)
{
    cp_frame f;
    rx_key key;
    rx_session *s;
    rx_block *b;
    int r, ev = 0, dup = 0;
    uint32_t T, pos = 0;

    r = cp_frame_parse(data, n, rx->max_bytes, &f);
    if (r == CP_FRAME_ERR_TOO_BIG)
        return CP_EV_UNSUPPORTED;
    if (r != CP_FRAME_OK)
        return 0;
    T = f.oti.T;
    memset(&key, 0, sizeof key);
    key.proto = CP_PROTO_RQ;
    key.id = f.session_id;
    key.oti = f.oti;

    cp_mutex_lock(&rx->mu);
    rx->stats.frames++;
    if (done_find(rx, &key) >= 0) {
        rx->stats.duplicates++;      /* transfer already received */
        cp_mutex_unlock(&rx->mu);
        return 0;
    }
    for (s = rx->sessions; s; s = s->next)
        if (key_eq(&s->key, &key))
            break;
    if (s && s->finalizing) {
        rx->stats.duplicates++;
        cp_mutex_unlock(&rx->mu);
        return 0;
    }
    if (!s) {
        while (active_count(rx) >= RX_MAX_ACTIVE && evict_lru(rx))
            ;
        s = session_new(&f);
        if (!s) {
            cp_mutex_unlock(&rx->mu);
            return 0;
        }
        s->next = rx->sessions;
        rx->sessions = s;
    }
    s->last_seen = ++rx->tick;
    b = &s->blocks[f.sbn];
    if (b->done) {
        rx->stats.duplicates++;      /* block already decoded */
        cp_mutex_unlock(&rx->mu);
        return 0;
    }

    /* dedup */
    if (f.esi < b->K) {
        if (!b->have) {
            b->have = (uint8_t *)calloc((b->K + 7u) / 8u, 1);
            if (!b->have)
                goto out;
        }
        dup = (b->have[f.esi >> 3] >> (f.esi & 7)) & 1;
    } else {
        dup = rep_find(b, f.esi, &pos);
    }
    if (dup) {
        rx->stats.duplicates++;
        goto out;
    }
    if (b->received >= b->K + RX_MAX_EXTRA) {   /* hopeless block (should not happen) */
        if (!b->busy) {
            fail_session(rx, s);
            ev = CP_EV_FAILED;
        }
        goto out;
    }

    /* store */
    if (f.esi < b->K && !b->dec && !b->busy) {
        size_t off = (size_t)f.esi * T, len = T;
        if (!b->buf) {
            b->buf = (uint8_t *)calloc(b->K, T);
            if (!b->buf)
                goto out;
        }
        if (off + len > b->bytes)
            len = (size_t)(b->bytes - off);
        memcpy(b->buf + off, f.symbol, len);
    } else {
        rx_pend *p = (rx_pend *)malloc(sizeof(rx_pend) + T);
        if (!p)
            goto out;
        if (f.esi >= b->K && !rep_insert(b, f.esi, pos)) {
            free(p);
            goto out;
        }
        p->next = NULL;
        p->esi = f.esi;
        memcpy(p->data, f.symbol, T);
        if (b->pt)
            b->pt->next = p;
        else
            b->ph = p;
        b->pt = p;
    }
    if (f.esi < b->K)
        b->have[f.esi >> 3] |= (uint8_t)(1u << (f.esi & 7));
    b->received++;
    rx->stats.unique++;
    if (b->received <= b->K)
        s->have++;
    rate_add(rx, T);
    ev |= CP_EV_PROGRESS;
    if (f.sbn == 0 && f.esi == 0)
        detect_kind(s, f.symbol, T);

    if (!b->busy && b->received >= b->K && b->received > b->tried_at) {
        b->busy = 1;
        s->refs++;
        ev |= run_decode(rx, s, b, f.sbn);
    }
out:
    cp_mutex_unlock(&rx->mu);
    return ev;
}

/* ---- Decimen v3 ------------------------------------------------------------ */

/* Kind from the DCF2 header in source block 0 (NONE if the type is not in it). */
static int d3_kind_of(const uint8_t *b0, size_t n)
{
    static const char snip[] = D3_SNIPPET_TYPE;
    uint32_t nl, tl;
    if (n < D3_FILE_HDR_LEN || memcmp(b0, "DCF2", 4) != 0)
        return CP_KIND_NONE;
    nl = (uint32_t)b0[5] | ((uint32_t)b0[6] << 8);
    tl = (uint32_t)b0[7] | ((uint32_t)b0[8] << 8);
    if ((size_t)D3_FILE_HDR_LEN + nl + tl > n)
        return CP_KIND_NONE;
    b0 += D3_FILE_HDR_LEN + nl;
    if (tl == sizeof snip - 1 && !memcmp(b0, snip, tl))
        return CP_KIND_TEXT;
    if (tl > 6 && !memcmp(b0, "image/", 6))
        return CP_KIND_IMAGE;
    return CP_KIND_FILES;
}

/* LT progress, after Decimen's estimateTransferProgress(): useful frames give
 * 0..86 % up to k, 86..96 % through the expected 2 % redundancy, then creep
 * towards 99 %; solved blocks cap the lead and can always push it forward. */
static uint64_t d3_lt_have(const d3_dec_stats *st)
{
    double k = st->k ? st->k : 1, fr = st->useful, so = st->solved;
    double exp_fr = (double)(uint64_t)(k * (k <= 1 ? 1.0 : 1.02) + 0.999999), red, ff, dec, f;
    if (exp_fr < k + 1)
        exp_fr = k + 1;
    red = exp_fr - k;
    if (fr < k) {
        ff = 0.86 * fr / k;
    } else if (fr <= exp_fr) {
        ff = 0.86 + 0.1 * (fr - k) / red;
    } else {
        double x = (fr - exp_fr) / red;
        ff = 0.96 + 0.03 * (x / (1.0 + x));
    }
    if (so > 0) {
        double cap = (so + 0.12 * k) / k;
        if (cap > 1)
            cap = 1;
        if (ff > 0.86 * cap)
            ff = 0.86 * cap;
    }
    dec = 0.99 * (so < k ? so / k : 1.0);
    f = ff > dec ? ff : dec;
    if (f > 0.99)
        f = 0.99;
    return (uint64_t)(f * k);
}

static cp_result *build_result_d3(const char *out_dir, uint32_t tag, const uint8_t *cont, size_t n,
                                  uint64_t max_raw)
{
    d3_file f;
    uint8_t *owned = NULL;
    cp_result *r = NULL;
    char sdir[16], *dir = NULL, type[128], mime[128], name[CP_NAME_MAX + 1];
    size_t tl;
    int ok = 0;

    if (!d3_container_unpack(cont, n, &f, &owned))
        return NULL;
    if ((uint64_t)f.len > max_raw)
        goto out;
    snprintf(sdir, sizeof sdir, "%08x", (unsigned)tag);
    dir = path_join(out_dir, sdir);
    if (!dir || mkdirs(dir) != 0)
        goto out;
    tl = f.type_len < sizeof type - 1 ? f.type_len : sizeof type - 1;
    memcpy(type, f.type, tl);
    type[tl] = 0;
    r = result_alloc(1);
    if (!r)
        goto out;
    if (f.type_len == sizeof D3_SNIPPET_TYPE - 1 && !strcmp(type, D3_SNIPPET_TYPE)) {
        size_t show = f.len;
        r->kind = CP_KIND_TEXT;
        snprintf(r->mime, sizeof r->mime, "text/plain");
        if (f.len > RX_TEXT_FULL_MAX) {
            show = cp_utf8_cut(f.data, RX_TEXT_PREVIEW);
            r->text_truncated = 1;
        }
        r->text = utf8_display(f.data, show, &r->text_len);
        if (!r->text || !result_add_file(r, dir, "text.txt", f.data, f.len))
            goto out;
    } else {
        clean_mime(type, mime, sizeof mime);
        cp_sanitize_name(f.name, f.name_len, name, sizeof name);
        if (!strncmp(mime, "image/", 6)) {
            r->kind = CP_KIND_IMAGE;
            snprintf(r->mime, sizeof r->mime, "%s", mime);
            if (!strcmp(name, "file"))
                snprintf(name, sizeof name, "image.%s", image_ext(mime, f.data, f.len));
        } else {
            r->kind = CP_KIND_FILES;
            snprintf(r->mime, sizeof r->mime, "%s", mime[0] ? mime : "application/octet-stream");
        }
        if (!result_add_file(r, dir, name, f.data, f.len))
            goto out;
    }
    r->session_id = tag;
    ok = 1;
out:
    free(owned);
    free(dir);
    if (!ok) {
        cp_result_free(r);
        r = NULL;
    }
    return r;
}

/* Enter with the lock held and a reference on s; returns with the lock held
 * and the reference dropped. */
static int d3_finalize(cp_rx *rx, d3_session *s)
{
    uint64_t gen = rx->generation;
    cp_result *res = NULL;
    uint8_t *cont;
    d3_dec *dec;
    int ev;

    s->finalizing = 1;
    done_add(rx, &s->key);
    cp_mutex_unlock(&rx->mu);

    cp_mutex_lock(&s->mu);           /* claimed: nobody feeds the decoder any more */
    dec = s->dec;
    s->dec = NULL;
    cp_mutex_unlock(&s->mu);
    cont = d3_dec_assemble(dec);
    d3_dec_free(dec);
    if (cont && d3_fnv1a(cont, s->key.d3_total) == s->key.d3_fnv)
        res = build_result_d3(rx->out_dir, d3_tag(s->key.id), cont, s->key.d3_total, rx->max_bytes);
    free(cont);

    cp_mutex_lock(&rx->mu);
    if (res) {
        if (gen == rx->generation) {
            cp_result_free(rx->result);
            rx->result = res;
            ev = CP_EV_COMPLETE;
        } else {
            cp_result_free(res);     /* reset happened meanwhile */
            ev = 0;
        }
    } else {
        done_remove(rx, &s->key);    /* checksum / container failure: collect again */
        ev = CP_EV_FAILED;
    }
    d3s_unlink(rx, s);
    s->dead = 1;
    d3s_unref(s);
    return ev;
}

static int push_d3(cp_rx *rx, const uint8_t *data, size_t n)
{
    d3_header h;
    d3_dec_stats st, st0;
    rx_key key;
    d3_session *s;
    uint64_t cap;
    int v, r = 0, ev = 0, claim = 0, kind = CP_KIND_NONE, fed = 0;

    memset(&st, 0, sizeof st);
    memset(&st0, 0, sizeof st0);
    v = d3_classify(data, n, &h);
    cp_mutex_lock(&rx->mu);
    if (!rx->enable_d3)
        goto unlock;
    if (v == D3_OLDER_SENDER || v == D3_NEWER_SENDER || v == D3_UNSUPPORTED_FLAGS) {
        ev = CP_EV_UNSUPPORTED;      /* one of the two ends needs an update */
        goto unlock;
    }
    if (v != D3_OK)
        goto unlock;
    cap = rx->max_bytes < CP_MAX_TOTAL_BYTES_D3 ? rx->max_bytes : CP_MAX_TOTAL_BYTES_D3;
    if ((uint64_t)h.total_len > cap + D3_FILE_HDR_LEN + 2u * 0xFFFFu) {
        ev = CP_EV_UNSUPPORTED;
        goto unlock;
    }
    memset(&key, 0, sizeof key);
    key.proto = CP_PROTO_D3;
    key.id = h.session_id;
    key.d3_k = h.k;
    key.d3_bl = h.block_len;
    key.d3_total = h.total_len;
    key.d3_fnv = h.fnv;
    key.d3_flags = (uint8_t)(h.flags & D3_FLAGS_MUST);   /* ignorable bits never split a stream */
    rx->stats.frames++;
    if (done_find(rx, &key) >= 0) {
        rx->stats.duplicates++;
        goto unlock;
    }
    for (s = rx->d3s; s; s = s->next)
        if (key_eq(&s->key, &key))
            break;
    if (s && s->finalizing) {
        rx->stats.duplicates++;
        goto unlock;
    }
    if (!s) {
        while (active_count(rx) >= RX_MAX_ACTIVE && evict_lru(rx))
            ;
        s = (d3_session *)calloc(1, sizeof *s);
        if (!s)
            goto unlock;
        s->dec = d3_dec_new(&h);
        if (!s->dec) {
            free(s);
            goto unlock;
        }
        cp_mutex_init(&s->mu);
        s->key = key;
        s->need = h.k;
        s->next = rx->d3s;
        rx->d3s = s;
    }
    s->last_seen = ++rx->tick;
    s->refs++;
    kind = s->kind;
    cp_mutex_unlock(&rx->mu);

    cp_mutex_lock(&s->mu);
    if (!s->claimed && s->dec) {
        d3_dec_stats_get(s->dec, &st0);
        r = d3_dec_add(s->dec, h.seq, data + D3_HDR_LEN);
        d3_dec_stats_get(s->dec, &st);
        fed = 1;
        if (kind == CP_KIND_NONE && r == 1) {
            const uint8_t *b0 = d3_dec_block0(s->dec);
            if (b0)
                kind = d3_kind_of(b0, h.block_len);
        }
        if (d3_dec_complete(s->dec)) {
            s->claimed = 1;
            claim = 1;
        }
    }
    cp_mutex_unlock(&s->mu);

    cp_mutex_lock(&rx->mu);
    if (fed) {
        if (r == 1)
            rx->stats.unique++;
        else if (r == 0 && st.frames_new > st0.frames_new)
            rx->stats.redundant++;
        else if (r == 0)
            rx->stats.duplicates++;
        if (h.flags & D3_FLAG_CAUCHY96) {
            rx->stats.rs96_k = st.k;
            rx->stats.rs96_rows = st.frames_new;
            rx->stats.rs96_useful = st.useful;
            rx->stats.rs96_solved = st.solved;
        }
    } else {
        rx->stats.duplicates++;      /* stream complete, being assembled */
    }
    if (fed) {
        if (claim)
            s->have = st.k;
        else if (h.flags & D3_FLAG_CAUCHY96)   /* any distinct row is worth a block */
            s->have = st.useful < st.k ? st.useful : st.k;
        else
            s->have = d3_lt_have(&st);
        s->kind = kind;
    }
    if (r == 1) {
        rate_add(rx, h.block_len);
        ev |= CP_EV_PROGRESS;
    }
    if (r < 0 && !s->dead) {         /* out of memory: give the stream up */
        d3s_kill(rx, s);
        ev |= CP_EV_FAILED;
    }
    if (claim && !s->dead) {
        ev |= d3_finalize(rx, s);
        goto unlock;
    }
    d3s_unref(s);
unlock:
    cp_mutex_unlock(&rx->mu);
    return ev;
}

/* ---- optical board ----------------------------------------------------------- */

static uint32_t ob_tag_id(const uint8_t *tag)
{
    return (uint32_t)tag[0] | ((uint32_t)tag[1] << 8) | ((uint32_t)tag[2] << 16) | ((uint32_t)tag[3] << 24);
}

static void obs_free(ob_session *s)
{
    uint32_t g;
    if (s->groups)
        for (g = 0; g < s->ngroups; g++)
            free(s->groups[g].rows);
    free(s->groups);
    free(s->data);
    free(s);
}

static void obs_unlink(cp_rx *rx, ob_session *s)
{
    ob_session **pp = &rx->obs;
    while (*pp) {
        if (*pp == s) {
            *pp = s->next;
            s->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* lock held */
static void obs_kill(cp_rx *rx, ob_session *s)
{
    obs_unlink(rx, s);
    s->dead = 1;
    if (s->refs == 0)
        obs_free(s);
}

static void obs_unref(ob_session *s)
{
    s->refs--;
    if (s->dead && s->refs == 0)
        obs_free(s);
}

static void ob_key(rx_key *k, const uint8_t *tag)
{
    memset(k, 0, sizeof *k);
    k->proto = CP_PROTO_OB;
    k->id = ob_tag_id(tag);
    memcpy(k->ob_tag, tag, 8);
}

/* Diagnostics: s is the optical board stream most recently fed (lock held). */
static void ob_note_stats(cp_rx *rx, const ob_session *s)
{
    rx->stats.ob_mode = (uint32_t)(s->mode == OB_TYPE_TILE ? CP_OB_TILES : CP_OB_BANDS) + 1u;
    rx->stats.ob_k = s->nrows;
    rx->stats.ob_rows = (uint32_t)s->have;
    rx->stats.ob_groups = s->ngroups;
    rx->stats.ob_solved = s->solved;
}

/* Enter with the lock held; returns with it held. s is gone afterwards. */
static int ob_finalize(cp_rx *rx, ob_session *s)
{
    uint64_t gen = rx->generation;
    cp_result *res = NULL;
    uint8_t digest[32];
    int ev;

    s->finalizing = 1;
    s->refs++;
    done_add(rx, &s->key);
    cp_mutex_unlock(&rx->mu);

    cp_sha256(s->data, s->length, digest);
    if (!memcmp(digest, s->digest, 32))
        res = build_result(rx->out_dir, s->key.id, s->data, s->length, rx->max_bytes);

    cp_mutex_lock(&rx->mu);
    if (res) {
        if (gen == rx->generation) {
            cp_result_free(rx->result);
            rx->result = res;
            ev = CP_EV_COMPLETE;
        } else {
            cp_result_free(res);     /* reset happened meanwhile */
            ev = 0;
        }
    } else {
        done_remove(rx, &s->key);    /* checksum / container failure: collect again */
        ev = CP_EV_FAILED;
    }
    obs_unlink(rx, s);
    s->dead = 1;
    obs_unref(s);
    return ev;
}

/* One data unit for a bound session (lock held). *gone = 1 when s was
 * finalized or dropped and must not be touched again. */
static int ob_feed_locked(cp_rx *rx, ob_session *s, const uint8_t *body, size_t n, int *gone)
{
    uint32_t g = (uint32_t)body[8] | ((uint32_t)body[9] << 8);
    int r = body[10], k, i;
    const uint8_t *payload = body + OB_HDR_LEN;
    ob_group *grp;

    *gone = 0;
    if ((body[11] & 0xF0u) != s->mode || n != OB_HDR_LEN + s->width || g >= s->ngroups)
        return 0;
    grp = &s->groups[g];
    if (grp->solved) {
        rx->stats.duplicates++;
        return 0;
    }
    k = (int)(s->nrows - g * OB_GROUP < OB_GROUP ? s->nrows - g * OB_GROUP : OB_GROUP);
    if (r >= k && r < OB_GROUP)
        return 0;
    for (i = 0; i < grp->n; i++)
        if (grp->ids[i] == r) {
            if (memcmp(grp->rows + (size_t)i * s->width, payload, s->width) != 0) {
                obs_kill(rx, s);    /* two different rows with one id: not trustworthy */
                *gone = 1;
                return CP_EV_FAILED;
            }
            rx->stats.duplicates++;
            return 0;
        }
    if (!grp->rows) {
        grp->rows = (uint8_t *)malloc((size_t)OB_GROUP * s->width);
        if (!grp->rows)
            return 0;
    }
    memcpy(grp->rows + (size_t)grp->n * s->width, payload, s->width);
    grp->ids[grp->n++] = (uint8_t)r;
    s->have++;
    rx->stats.unique++;
    s->last_seen = ++rx->tick;
    rate_add(rx, s->width);
    ob_note_stats(rx, s);
    if (g == 0 && r == 0 && s->kind == CP_KIND_NONE && s->width >= 5 && !memcmp(payload, "CPO1", 4) &&
        payload[4] >= CP_KIND_TEXT && payload[4] <= CP_KIND_FILES)
        s->kind = payload[4];
    if (grp->n < k)
        return CP_EV_PROGRESS;
    if (!ob_outer_recover(grp->ids, grp->rows, k, (int)s->width, s->data + (size_t)g * OB_GROUP * s->width)) {
        obs_kill(rx, s);
        *gone = 1;
        return CP_EV_FAILED;
    }
    grp->solved = 1;
    free(grp->rows);
    grp->rows = NULL;
    s->solved++;
    ob_note_stats(rx, s);
    if (g == 0 && s->kind == CP_KIND_NONE && !memcmp(s->data, "CPO1", 4) && s->data[4] >= CP_KIND_TEXT &&
        s->data[4] <= CP_KIND_FILES)
        s->kind = s->data[4];
    if (s->solved < s->ngroups)
        return CP_EV_PROGRESS;
    *gone = 1;
    return CP_EV_PROGRESS | ob_finalize(rx, s);
}

static ob_session *ob_find(cp_rx *rx, const uint8_t *tag)
{
    ob_session *s;
    for (s = rx->obs; s; s = s->next)
        if (!memcmp(s->key.ob_tag, tag, 8))
            return s;
    return NULL;
}

static void ob_pend_push(cp_rx *rx, const uint8_t *body, size_t n)
{
    ob_pend *p;
    if (!rx->ob_pend) {
        rx->ob_pend = (ob_pend *)malloc(sizeof(ob_pend) * OB_PENDING_MAX);
        if (!rx->ob_pend)
            return;
    }
    if (rx->ob_pend_n == OB_PENDING_MAX) {   /* drop the oldest */
        rx->ob_pend_head = (rx->ob_pend_head + 1) % OB_PENDING_MAX;
        rx->ob_pend_n--;
    }
    p = &rx->ob_pend[(rx->ob_pend_head + rx->ob_pend_n) % OB_PENDING_MAX];
    p->len = n;
    memcpy(p->body, body, n);
    rx->ob_pend_n++;
}

/* Moves the pending units of tag out of the ring (lock held). Caller frees. */
static ob_pend *ob_pend_take(cp_rx *rx, const uint8_t *tag, int *count)
{
    ob_pend *out = NULL;
    int i, kept = 0, m = 0;
    *count = 0;
    if (!rx->ob_pend_n)
        return NULL;
    out = (ob_pend *)malloc(sizeof(ob_pend) * (size_t)rx->ob_pend_n);
    if (!out)
        return NULL;
    for (i = 0; i < rx->ob_pend_n; i++) {
        ob_pend *p = &rx->ob_pend[(rx->ob_pend_head + i) % OB_PENDING_MAX];
        if (!memcmp(p->body, tag, 8))
            out[m++] = *p;
        else
            rx->ob_pend[(rx->ob_pend_head + kept++) % OB_PENDING_MAX] = *p;
    }
    rx->ob_pend_n = kept;
    *count = m;
    return out;
}

int ob_rx_feed(cp_rx *rx, const uint8_t *body, size_t n)
{
    uint32_t g, width, length;
    uint8_t fmt, base, digest[32];
    rx_key key;
    ob_session *s;
    int ev = 0, gone, i, npend;
    ob_pend *pend;
    uint64_t cap;

    if (!rx || !body || (n != OB_TILE_BODY && n != OB_BAND_BODY))
        return 0;
    g = (uint32_t)body[8] | ((uint32_t)body[9] << 8);
    fmt = body[11];
    base = (uint8_t)(fmt & 0xF0u);
    width = base == OB_TYPE_TILE ? OB_TILE_DATA : base == OB_TYPE_BAND ? OB_BAND_DATA : 0;
    if (!width || n != OB_HDR_LEN + width)
        return 0;

    if ((fmt & 0x0Fu) == 1 && g == OB_MANIFEST_GROUP && body[10] == 0) {
        const uint8_t *m = body + OB_HDR_LEN;
        length = (uint32_t)m[4] | ((uint32_t)m[5] << 8) | ((uint32_t)m[6] << 16) | ((uint32_t)m[7] << 24);
        if (memcmp(m, "CTB1", 4) != 0 || length < 1)
            return 0;
        for (i = 40; i < (int)width; i++)
            if (m[i])
                return 0;
        cp_sha256(m, width, digest);
        if (memcmp(digest, body, 8) != 0)      /* the tag names this exact manifest */
            return 0;
        cap = rx->max_bytes < CP_OB_MAX_BYTES ? rx->max_bytes : CP_OB_MAX_BYTES;
        if (length > cap)
            return CP_EV_UNSUPPORTED;
        ob_key(&key, body);
        cp_mutex_lock(&rx->mu);
        if (done_find(rx, &key) >= 0 || ob_find(rx, body)) {
            cp_mutex_unlock(&rx->mu);
            return 0;
        }
        while (active_count(rx) >= RX_MAX_ACTIVE && evict_lru(rx))
            ;
        s = (ob_session *)calloc(1, sizeof *s);
        if (!s) {
            cp_mutex_unlock(&rx->mu);
            return 0;
        }
        s->key = key;
        s->mode = base;
        s->width = width;
        s->length = length;
        s->nrows = (length + width - 1) / width;
        s->ngroups = (s->nrows + OB_GROUP - 1) / OB_GROUP;
        memcpy(s->digest, m + 8, 32);
        s->need = s->nrows;
        s->groups = (ob_group *)calloc(s->ngroups, sizeof *s->groups);
        s->data = (uint8_t *)calloc(s->nrows, width);
        if (!s->groups || !s->data) {
            obs_free(s);
            cp_mutex_unlock(&rx->mu);
            return 0;
        }
        s->last_seen = ++rx->tick;
        s->next = rx->obs;
        rx->obs = s;
        ob_note_stats(rx, s);
        /* replay what arrived before the manifest */
        pend = ob_pend_take(rx, body, &npend);
        for (i = 0, gone = 0; i < npend && !gone; i++)
            ev |= ob_feed_locked(rx, s, pend[i].body, pend[i].len, &gone);
        free(pend);
        cp_mutex_unlock(&rx->mu);
        return ev;
    }
    if (fmt != base || g == OB_MANIFEST_GROUP)
        return 0;
    cp_mutex_lock(&rx->mu);
    rx->stats.frames++;
    s = ob_find(rx, body);
    if (!s) {
        ob_key(&key, body);
        if (done_find(rx, &key) < 0)
            ob_pend_push(rx, body, n);
        else
            rx->stats.duplicates++;
    } else if (!s->finalizing) {
        ev = ob_feed_locked(rx, s, body, n, &gone);
    } else {
        rx->stats.duplicates++;
    }
    cp_mutex_unlock(&rx->mu);
    return ev;
}

int cp_rx_push_board(cp_rx *rx, const float *luma, int *units)
{
    ob_unit *u;
    int n, i, ev = 0;
    if (units)
        *units = 0;
    if (!rx || !luma)
        return 0;
    u = (ob_unit *)malloc(sizeof(ob_unit) * OB_SLOTS);
    if (!u)
        return 0;
    n = ob_read_board(luma, u);     /* optical work: no lock held */
    for (i = 0; i < n; i++)
        ev |= ob_rx_feed(rx, u[i].body, u[i].len);
    free(u);
    if (units)
        *units = n;
    return ev;
}

static int push_plain(cp_rx *rx, const uint8_t *data, size_t n)
{
    uint8_t h[32], *copy;
    int i;
    if (n > RX_PLAIN_MAX)
        return 0;
    cp_sha256(data, n, h);
    copy = (uint8_t *)malloc(n + 1);
    if (!copy)
        return 0;
    memcpy(copy, data, n);
    copy[n] = 0;
    cp_mutex_lock(&rx->mu);
    if (!rx->enable_plain)
        goto drop;
    for (i = 0; i < rx->n_recent; i++)
        if (!memcmp(rx->recent[i], h, 32))
            goto drop;
    memcpy(rx->recent[rx->recent_pos], h, 32);
    rx->recent_pos = (rx->recent_pos + 1) % RX_PLAIN_RECENT;
    if (rx->n_recent < RX_PLAIN_RECENT)
        rx->n_recent++;
    if (rx->plain_n == RX_PLAIN_QUEUE) {        /* drop the oldest */
        free(rx->plain_q[0]);
        memmove(rx->plain_q, rx->plain_q + 1, (RX_PLAIN_QUEUE - 1) * sizeof(rx->plain_q[0]));
        memmove(rx->plain_len, rx->plain_len + 1, (RX_PLAIN_QUEUE - 1) * sizeof(rx->plain_len[0]));
        rx->plain_n--;
    }
    rx->plain_q[rx->plain_n] = copy;
    rx->plain_len[rx->plain_n] = n;
    rx->plain_n++;
    cp_mutex_unlock(&rx->mu);
    return CP_EV_PLAIN;
drop:
    cp_mutex_unlock(&rx->mu);
    free(copy);
    return 0;
}

/* ---- public API ---------------------------------------------------------- */

cp_rx *cp_rx_new(const cp_rx_config *cfg)
{
    cp_rx *rx;
    size_t len;
    if (!cfg || !cfg->out_dir || !cfg->out_dir[0])
        return NULL;
    rx = (cp_rx *)calloc(1, sizeof *rx);
    if (!rx)
        return NULL;
    rx->out_dir = xstrdup(cfg->out_dir);
    if (!rx->out_dir) {
        free(rx);
        return NULL;
    }
    len = strlen(rx->out_dir);
    while (len > 1 && (rx->out_dir[len - 1] == '/' || rx->out_dir[len - 1] == '\\'))
        rx->out_dir[--len] = 0;
    rx->max_bytes = cfg->max_bytes ? cfg->max_bytes : CP_MAX_TOTAL_BYTES;
    if (rx->max_bytes > CP_MAX_TOTAL_BYTES)
        rx->max_bytes = CP_MAX_TOTAL_BYTES;
    rx->enable_d3 = cfg->enable_d3 ? 1 : 0;
    rx->enable_plain = cfg->enable_plain ? 1 : 0;
    cp_mutex_init(&rx->mu);
    return rx;
}

static void clear_locked(cp_rx *rx)
{
    int i;
    while (rx->sessions)
        session_kill(rx, rx->sessions);
    while (rx->d3s)
        d3s_kill(rx, rx->d3s);
    while (rx->obs)
        obs_kill(rx, rx->obs);
    rx->ob_pend_n = rx->ob_pend_head = 0;
    rx->n_done = 0;
    cp_result_free(rx->result);
    rx->result = NULL;
    for (i = 0; i < rx->plain_n; i++)
        free(rx->plain_q[i]);
    rx->plain_n = 0;
    rx->n_recent = 0;
    rx->recent_pos = 0;
    memset(rx->rate_epoch, 0, sizeof rx->rate_epoch);
    memset(rx->rate_bytes, 0, sizeof rx->rate_bytes);
    rx->rate_start_ms = rx->rate_last_ms = 0;
    rx->generation++;
}

void cp_rx_free(cp_rx *rx)
{
    if (!rx)
        return;
    cp_mutex_lock(&rx->mu);
    clear_locked(rx);
    cp_mutex_unlock(&rx->mu);
    cp_mutex_destroy(&rx->mu);
    free(rx->ob_pend);
    free(rx->out_dir);
    free(rx);
}

void cp_rx_reset(cp_rx *rx)
{
    if (!rx)
        return;
    cp_mutex_lock(&rx->mu);
    clear_locked(rx);
    cp_mutex_unlock(&rx->mu);
}

void cp_rx_set_options(cp_rx *rx, int enable_d3, int enable_plain)
{
    if (!rx)
        return;
    cp_mutex_lock(&rx->mu);
    rx->enable_d3 = enable_d3 ? 1 : 0;
    rx->enable_plain = enable_plain ? 1 : 0;
    cp_mutex_unlock(&rx->mu);
}

int cp_rx_push(cp_rx *rx, const uint8_t *data, size_t n)
{
    if (!rx || !data || n == 0)
        return 0;
    if (n >= 2 && data[0] == CP_MAGIC0 && data[1] == CP_MAGIC1)
        return push_cp(rx, data, n);
    /* Decimen: v3 (0xD1 0xC3) and the pre-versioning v1 / v2 magics, which are
     * reported as "update the sender" rather than shown as plain text */
    if (n >= 4 && data[0] == D3_MAGIC0 && (data[1] == D3_MAGIC1 || data[1] == 0x0C || data[1] == 0x0D))
        return push_d3(rx, data, n);
    return push_plain(rx, data, n);
}

void cp_rx_note_frame(cp_rx *rx, int codes_in_frame)
{
    if (!rx)
        return;
    cp_mutex_lock(&rx->mu);
    rx->codes_in_frame = codes_in_frame;
    cp_mutex_unlock(&rx->mu);
}

void cp_rx_progress(cp_rx *rx, cp_progress *out)
{
    rx_session *s, *best = NULL;
    d3_session *d, *dbest = NULL;
    ob_session *o, *obest = NULL;
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!rx)
        return;
    cp_mutex_lock(&rx->mu);
    for (s = rx->sessions; s; s = s->next)
        if (!best || s->last_seen > best->last_seen)
            best = s;
    for (d = rx->d3s; d; d = d->next)
        if (!dbest || d->last_seen > dbest->last_seen)
            dbest = d;
    for (o = rx->obs; o; o = o->next)
        if (!obest || o->last_seen > obest->last_seen)
            obest = o;
    if (obest && (!best || obest->last_seen > best->last_seen) && (!dbest || obest->last_seen > dbest->last_seen)) {
        out->proto = CP_PROTO_OB;
        out->kind = obest->kind;
        out->have = obest->have;
        out->need = obest->need;
        out->total_bytes = obest->length;
        out->session_id = obest->key.id;
    } else if (dbest && (!best || dbest->last_seen > best->last_seen)) {
        out->proto = CP_PROTO_D3;
        out->kind = dbest->kind;
        out->have = dbest->have;
        out->need = dbest->need;
        out->total_bytes = dbest->key.d3_total;
        out->session_id = d3_tag(dbest->key.id);
    } else if (best) {
        out->proto = CP_PROTO_RQ;
        out->kind = best->kind;
        out->have = best->have;
        out->need = best->need;
        out->total_bytes = best->key.oti.F;
        out->session_id = best->key.id;
    }
    out->bytes_per_sec = rate_get(rx);
    out->codes_in_frame = rx->codes_in_frame;
    cp_mutex_unlock(&rx->mu);
}

void cp_rx_stats_get(cp_rx *rx, cp_rx_stats *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!rx)
        return;
    cp_mutex_lock(&rx->mu);
    *out = rx->stats;
    cp_mutex_unlock(&rx->mu);
}

uint8_t *cp_rx_take_plain(cp_rx *rx, size_t *n)
{
    uint8_t *p = NULL;
    if (n)
        *n = 0;
    if (!rx)
        return NULL;
    cp_mutex_lock(&rx->mu);
    if (rx->plain_n > 0) {
        p = rx->plain_q[0];
        if (n)
            *n = rx->plain_len[0];
        memmove(rx->plain_q, rx->plain_q + 1, (RX_PLAIN_QUEUE - 1) * sizeof(rx->plain_q[0]));
        memmove(rx->plain_len, rx->plain_len + 1, (RX_PLAIN_QUEUE - 1) * sizeof(rx->plain_len[0]));
        rx->plain_n--;
    }
    cp_mutex_unlock(&rx->mu);
    return p;
}

cp_result *cp_rx_take_result(cp_rx *rx)
{
    cp_result *r;
    if (!rx)
        return NULL;
    cp_mutex_lock(&rx->mu);
    r = rx->result;
    rx->result = NULL;
    cp_mutex_unlock(&rx->mu);
    return r;
}
