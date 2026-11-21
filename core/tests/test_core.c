/*
 * CodeTrans core desktop tests (PLAN.md §8 W1, §9).
 *
 *   cp_core_tests [--quick] [--big]
 *
 * - CRC32 / SHA-256 vectors
 * - CPO1 container build / decode for text, image and files
 * - CodeTrans v1 frame header boundaries
 * - RaptorQ block decode success rate with K+2 symbols
 * - full sender -> cp_rx loopback with random frame loss, 1 B .. 5 MB
 *   (--big adds 50 MB)
 * - render loopback: qrcodegen -> greyscale image -> quirc -> cp_rx
 */
#include "cp_internal.h"

#include "qrcodegen.h"
#include "quirc.h"

#include <stdarg.h>

#define CPW_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int g_fail, g_pass;
static const char *g_out = "cp_test_out";

#define CHECK(c) do { if (c) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* ---- deterministic PRNG ------------------------------------------------ */

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}
static double rnd01(void) { return (rnd() & 0xFFFFFF) / (double)0x1000000; }

static void fill_random(uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        p[i] = (uint8_t)rnd();
}

/* compressible but not trivial */
static void fill_text(uint8_t *p, size_t n)
{
    static const char *words[] = { "alpha ", "beta ", "gamma ", "delta ", "码 ", "二维 ", "\n" };
    size_t i = 0;
    while (i < n) {
        const char *w = words[rnd() % 7];
        size_t l = strlen(w);
        if (l > n - i) {                       /* never end inside a UTF-8 sequence */
            w = "xxxxxxx";
            l = n - i;
        }
        memcpy(p + i, w, l);
        i += l;
    }
}

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = cp_fopen_utf8(path, "rb");
    uint8_t *b;
    long sz;
    *n = 0;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
    if (b && sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b)
        *n = (size_t)sz;
    return b;
}

/* ---- vectors ----------------------------------------------------------- */

static void test_vectors(void)
{
    static const uint8_t abc_sha[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad };
    static const uint8_t empty_sha[32] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55 };
    static const uint8_t long_sha[32] = {   /* 448-bit NIST message */
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26, 0x93, 0x0c, 0x3e, 0x60, 0x39,
        0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff, 0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1 };
    uint8_t h[32];
    cp_sha256_ctx c;
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    size_t i;

    CHECK(cp_crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    CHECK(cp_crc32(NULL, 0) == 0);
    cp_sha256((const uint8_t *)"abc", 3, h);
    CHECK(!memcmp(h, abc_sha, 32));
    cp_sha256((const uint8_t *)"", 0, h);
    CHECK(!memcmp(h, empty_sha, 32));
    cp_sha256_init(&c);                       /* byte-at-a-time streaming */
    for (i = 0; m[i]; i++)
        cp_sha256_update(&c, (const uint8_t *)m + i, 1);
    cp_sha256_final(&c, h);
    CHECK(!memcmp(h, long_sha, 32));
}

/* ---- container --------------------------------------------------------- */

static void test_container(void)
{
    cp_container_hdr h;
    uint8_t *owned;
    const uint8_t *raw, *bytes;
    size_t n;
    cp_payload *p;
    cp_payload_info info;
    char mime[64];
    uint8_t *big = (uint8_t *)malloc(200000);

    /* text, compressible */
    fill_text(big, 200000);
    p = cp_payload_text((const char *)big, 200000);
    CHECK(p != NULL);
    cp_payload_info_get(p, &info);
    CHECK(info.kind == CP_KIND_TEXT && info.compressed && info.raw_len == 200000);
    bytes = cp_payload_bytes(p, &n);
    CHECK(n == info.container_len && n < 100000);
    CHECK(cp_container_decode(bytes, n, 0, &h, &owned, &raw));
    CHECK(h.kind == CP_KIND_TEXT && h.raw_len == 200000 && !memcmp(raw, big, 200000));
    CHECK(cp_meta_get(h.meta, h.meta_len, "mime", mime, sizeof mime) && !strcmp(mime, "text/plain"));
    free(owned);
    /* corrupt one body byte -> must fail (inflate or SHA-256) */
    {
        uint8_t *bad = (uint8_t *)malloc(n);
        memcpy(bad, bytes, n);
        bad[n - 10] ^= 0x40;
        CHECK(!cp_container_decode(bad, n, 0, &h, &owned, &raw));
        free(bad);
    }
    cp_payload_free(p);

    /* text with BOM: BOM stripped, short text not compressed */
    p = cp_payload_text("\xEF\xBB\xBFhi", 5);
    bytes = cp_payload_bytes(p, &n);
    CHECK(cp_container_decode(bytes, n, 0, &h, &owned, &raw));
    CHECK(h.raw_len == 2 && !memcmp(raw, "hi", 2) && owned == NULL);
    cp_payload_free(p);

    /* empty text is legal */
    p = cp_payload_text("", 0);
    CHECK(p != NULL);
    cp_payload_free(p);

    /* image: PNG magic -> never compressed, mime sniffed */
    fill_random(big, 5000);
    memcpy(big, "\x89PNG\r\n\x1a\n", 8);
    p = cp_payload_image(big, 5000, NULL);
    cp_payload_info_get(p, &info);
    CHECK(info.kind == CP_KIND_IMAGE && !info.compressed);
    bytes = cp_payload_bytes(p, &n);
    CHECK(cp_container_decode(bytes, n, 0, &h, &owned, &raw));
    CHECK(cp_meta_get(h.meta, h.meta_len, "mime", mime, sizeof mime) && !strcmp(mime, "image/png"));
    cp_payload_free(p);
    CHECK(cp_payload_image(big, 0, NULL) == NULL);
    CHECK(cp_payload_image(big, 10, "bad mime") == NULL);

    /* files: two entries, path stripped */
    {
        const char *names[3] = { "C:\\dir\\a.txt", "sub/b.bin", "empty" };
        const uint8_t *datas[3] = { (const uint8_t *)"hello", big, NULL };
        uint64_t sizes[3] = { 5, 3000, 0 };
        size_t off = 0;
        cp_file_entry e;
        p = cp_payload_files(3, names, datas, sizes);
        CHECK(p != NULL);
        bytes = cp_payload_bytes(p, &n);
        CHECK(cp_container_decode(bytes, n, 0, &h, &owned, &raw));
        CHECK(h.kind == CP_KIND_FILES);
        CHECK(cp_meta_get(h.meta, h.meta_len, "count", mime, sizeof mime) && !strcmp(mime, "3"));
        CHECK(cp_files_next(raw, (size_t)h.raw_len, &off, &e) == 1 && e.name_len == 5 &&
              !memcmp(e.name, "a.txt", 5) && e.size == 5 && !memcmp(e.data, "hello", 5));
        CHECK(cp_files_next(raw, (size_t)h.raw_len, &off, &e) == 1 && e.name_len == 5 &&
              !memcmp(e.name, "b.bin", 5) && e.size == 3000 && !memcmp(e.data, big, 3000));
        CHECK(cp_files_next(raw, (size_t)h.raw_len, &off, &e) == 1 && e.size == 0);
        CHECK(cp_files_next(raw, (size_t)h.raw_len, &off, &e) == 0);
        free(owned);
        cp_payload_free(p);
    }

    /* header validation */
    {
        uint8_t hdr[CPO_HDR_LEN + 4];
        memset(hdr, 0, sizeof hdr);
        memcpy(hdr, "CPO1", 4);
        hdr[4] = 9;                                   /* unknown kind */
        CHECK(!cp_container_parse(hdr, sizeof hdr, 0, &h));
        hdr[4] = CP_KIND_TEXT;
        hdr[5] = 0x80;                                /* unknown flag */
        CHECK(!cp_container_parse(hdr, sizeof hdr, 0, &h));
        hdr[5] = 0;
        cp_be64w(hdr + 8, 4);
        CHECK(cp_container_parse(hdr, sizeof hdr, 0, &h));
        cp_be16w(hdr + 6, 100);                       /* meta_len beyond end */
        CHECK(!cp_container_parse(hdr, sizeof hdr, 0, &h));
    }
    free(big);
}

/* ---- frames ------------------------------------------------------------ */

static void test_frames(void)
{
    cp_oti o = { 1000, 100, 1, 1, 4 };
    uint8_t sym[100], buf[200];
    cp_frame f;
    size_t n;
    fill_random(sym, sizeof sym);
    n = cp_frame_write(buf, sizeof buf, 0x12345678u, &o, 0, 0xABCDEF, sym);
    CHECK(n == 128);
    CHECK(buf[0] == 0xC0 && buf[1] == 0xDE && buf[2] == 1 && buf[3] == 0);
    CHECK(cp_frame_parse(buf, n, 0, &f) == CP_FRAME_OK);
    CHECK(f.session_id == 0x12345678u && f.oti.F == 1000 && f.oti.T == 100 && f.oti.Z == 1 &&
          f.oti.N == 1 && f.oti.Al == 4 && f.sbn == 0 && f.esi == 0xABCDEF && !memcmp(f.symbol, sym, 100));
    CHECK(cp_frame_parse(buf, n - 1, 0, &f) == CP_FRAME_ERR_LENGTH);
    CHECK(cp_frame_parse(buf, 27, 0, &f) == CP_FRAME_ERR_SHORT);
    CHECK(cp_frame_parse(buf, n, 999, &f) == CP_FRAME_ERR_TOO_BIG);
    buf[50] ^= 1;
    CHECK(cp_frame_parse(buf, n, 0, &f) == CP_FRAME_ERR_CRC);
    buf[50] ^= 1;
    buf[3] = 0x10;                                    /* ignorable bit: CRC now wrong */
    CHECK(cp_frame_parse(buf, n, 0, &f) == CP_FRAME_ERR_CRC);
    cp_be32w(buf + 124, cp_crc32(buf, 124));
    CHECK(cp_frame_parse(buf, n, 0, &f) == CP_FRAME_OK);
    buf[3] = 0x01;                                    /* must-understand bit */
    CHECK(cp_frame_parse(buf, n, 0, &f) == CP_FRAME_ERR_FLAGS);
    buf[3] = 0;
    buf[2] = 2;
    CHECK(cp_frame_parse(buf, n, 0, &f) == CP_FRAME_ERR_VERSION);
    buf[2] = 1;
    buf[0] = 0xD1;
    CHECK(cp_frame_parse(buf, n, 0, &f) == CP_FRAME_ERR_MAGIC);
    buf[0] = 0xC0;

    /* OTI limits */
    o.Al = 3;
    CHECK(!cp_oti_valid(&o));
    o.Al = 4;
    o.T = 102;                                        /* not a multiple of Al */
    CHECK(!cp_oti_valid(&o));
    o.T = 100;
    o.Z = 11;                                         /* more blocks than symbols */
    CHECK(!cp_oti_valid(&o));
    o.F = 4097ull * 100;                              /* KL 4097 > CP_MAX_K */
    o.Z = 1;
    CHECK(!cp_oti_valid(&o));
    o.Z = 2;
    CHECK(cp_oti_valid(&o));
    CHECK(cp_oti_block_K(&o, 0) == 2049 && cp_oti_block_K(&o, 1) == 2048);
    CHECK(cp_oti_block_offset(&o, 1) == 2049ull * 100);
    CHECK(cp_oti_block_bytes(&o, 1) == 4097ull * 100 - 2049ull * 100);
}

/* ---- RaptorQ overhead -------------------------------------------------- */

static void test_rq_overhead(int trials)
{
    int ok = 0, t;
    for (t = 0; t < trials; t++) {
        uint32_t K = 10 + rnd() % 600, T = 16, i, got = 0, esi;
        cp_oti o;
        uint8_t *data, *out, *sym, *have;
        fec_enc *e;
        fec_dec *d;
        int r = 0;
        o.F = (uint64_t)K * T - (rnd() % T);
        o.T = (uint16_t)T;
        o.Z = 1;
        o.N = 1;
        o.Al = 4;
        K = (uint32_t)cp_oti_Kt(&o);
        data = (uint8_t *)malloc((size_t)o.F);
        out = (uint8_t *)calloc(1, (size_t)K * T);
        sym = (uint8_t *)malloc(T);
        have = (uint8_t *)calloc(1, K);
        fill_random(data, (size_t)o.F);
        e = fec_enc_new(data, &o);
        d = fec_dec_new(&o, 0, out);
        if (!e || !d) {
            CHECK(e && d);
            break;
        }
        /* drop ~30 % of source symbols, top up with repair symbols to K+2 */
        for (i = 0; i < K; i++)
            if (rnd01() >= 0.3) {
                fec_enc_symbol(e, 0, i, sym);
                fec_dec_add(d, i, sym);
                got++;
            }
        for (esi = K + (rnd() % 1000); got < K + 2; esi++) {
            fec_enc_symbol(e, 0, esi, sym);
            if (fec_dec_add(d, esi, sym) == 1)
                got++;
        }
        r = fec_dec_try(d);
        if (r == 1 && !memcmp(out, data, (size_t)o.F))
            ok++;
        fec_enc_free(e);
        fec_dec_free(d);
        free(data);
        free(out);
        free(sym);
        free(have);
    }
    note("  RaptorQ K+2 decode success: %d / %d\n", ok, trials);
    CHECK(ok * 100 >= trials * 99);
}

/* ---- sender -> receiver loopback --------------------------------------- */

typedef struct loop_stats {
    uint64_t frames_sent, frames_pushed;
    uint64_t need;
} loop_stats;

/* Plays tx frames into rx with the given loss rate until a result appears.
 * Returns the result (caller frees) or NULL after max_frames. */
static cp_result *play(cp_tx *tx, cp_rx *rx, double loss, uint64_t max_frames, size_t frame_cap,
                       loop_stats *st)
{
    uint8_t *buf = (uint8_t *)malloc(frame_cap);
    cp_result *res = NULL;
    memset(st, 0, sizeof *st);
    while (st->frames_sent < max_frames) {
        size_t n = cp_tx_next(tx, buf, frame_cap);
        int ev;
        if (!n)
            break;
        st->frames_sent++;
        if (rnd01() < loss)
            continue;
        st->frames_pushed++;
        ev = cp_rx_push(rx, buf, n);
        if (ev & CP_EV_FAILED)
            break;
        if (ev & CP_EV_COMPLETE) {
            res = cp_rx_take_result(rx);
            break;
        }
    }
    {
        cp_progress pr;
        cp_rx_progress(rx, &pr);
        st->need = pr.need;
    }
    free(buf);
    return res;
}

static int result_matches(const cp_result *r, int kind, const uint8_t *data, size_t n)
{
    size_t got;
    uint8_t *file;
    int ok;
    if (!r || r->kind != kind || r->count < 1)
        return 0;
    file = read_file(r->paths[0], &got);
    ok = file && got == n && (n == 0 || !memcmp(file, data, n));
    free(file);
    return ok;
}

static void test_loopback(int big)
{
    /* size, QR byte capacity, loss, random (else compressible text) */
    static const struct { size_t n; size_t cap; double loss; int random; } cases[] = {
        { 1, 520, 0.0, 0 },
        { 27, 1273, 0.1, 1 },
        { 1000, 520, 0.3, 0 },
        { 4096, 1273, 0.5, 1 },
        { 65536, 520, 0.0, 0 },
        { 300000, 1273, 0.1, 1 },
        { 5u << 20, 520, 0.3, 0 },
        { 3u << 20, 520, 0.2, 1 },       /* Kt 6394 -> Z = 2  */
        { 2u << 20, 120, 0.5, 1 },       /* T 92 -> Z = 6     */
        { 50u << 20, 2953, 0.1, 1 },     /* --big: Z = 5      */
    };
    cp_rx_config cfg;
    size_t si, nsizes = CPW_COUNT(cases) - (big ? 0 : 1);
    cp_rx *rx;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = g_out;
    rx = cp_rx_new(&cfg);
    CHECK(rx != NULL);
    for (si = 0; si < nsizes; si++) {
        size_t n = cases[si].n;
        double loss = cases[si].loss;
        size_t cap = cases[si].cap;
        uint8_t *data = (uint8_t *)malloc(n);
        cp_payload *p;
        cp_tx *tx;
        cp_tx_info ti;
        cp_result *r;
        loop_stats st;
        uint64_t t0 = cp_mono_ms();
        int ok;
        if (cases[si].random)
            fill_random(data, n);
        else
            fill_text(data, n);
        p = cp_payload_files(1, (const char *const[]){ "blob.bin" }, (const uint8_t *const[]){ data },
                             (const uint64_t[]){ n });
        tx = cp_tx_new(CP_PROTO_RQ, p, cap, NULL);
        CHECK(tx != NULL);
        if (!tx) {
            cp_payload_free(p);
            free(data);
            continue;
        }
        cp_tx_info_get(tx, &ti);
        r = play(tx, rx, loss, (ti.total_source_symbols + 50ull * ti.Z) * 3 + 100, cap, &st);
        ok = result_matches(r, CP_KIND_FILES, data, n);
        CHECK(ok);
        note("  %9zu B  loss %2.0f%%  T=%-4u Z=%-3u K=%-6u frames %llu (pushed %llu, overhead %+lld)  %llums %s\n",
             n, loss * 100, ti.T, ti.Z, ti.total_source_symbols, (unsigned long long)st.frames_sent,
             (unsigned long long)st.frames_pushed,
             (long long)st.frames_pushed - (long long)ti.total_source_symbols,
             (unsigned long long)(cp_mono_ms() - t0), ok ? "ok" : "FAILED");
        cp_result_free(r);
        cp_tx_free(tx);
        cp_payload_free(p);
        free(data);
    }

    /* text kind result carries the text inline */
    {
        const char *msg = "你好,CodeTrans!";
        cp_payload *p = cp_payload_text(msg, strlen(msg));
        cp_tx *tx = cp_tx_new(CP_PROTO_RQ, p, 300, NULL);
        loop_stats st;
        cp_result *r = play(tx, rx, 0.2, 1000, 300, &st);
        CHECK(r && r->kind == CP_KIND_TEXT && r->text && !strcmp(r->text, msg) && !r->text_truncated);
        cp_result_free(r);
        cp_tx_free(tx);
        cp_payload_free(p);
    }

    /* same session started mid-stream (receiver joins late) */
    {
        size_t n = 20000;
        uint8_t *data = (uint8_t *)malloc(n), buf[600];
        cp_payload *p;
        cp_tx *tx;
        loop_stats st;
        cp_result *r;
        int i;
        fill_random(data, n);
        p = cp_payload_image(data, n, "image/jpeg");
        tx = cp_tx_new(CP_PROTO_RQ, p, 520, NULL);
        for (i = 0; i < 37; i++)                       /* frames nobody saw */
            cp_tx_next(tx, buf, sizeof buf);
        r = play(tx, rx, 0.0, 1000, 600, &st);
        CHECK(result_matches(r, CP_KIND_IMAGE, data, n));
        cp_result_free(r);
        cp_tx_free(tx);
        cp_payload_free(p);
        free(data);
    }

    CHECK(cp_tx_new(CP_PROTO_D3, NULL, 1000, NULL) == NULL);
    cp_rx_free(rx);
}

/* ---- render loopback --------------------------------------------------- */

/* Renders one QR code into a greyscale image (0 = black) with integer module
 * size and quiet zone, the way the Windows producer does. */
static void render_qr(const uint8_t *qr, int px, int quiet, uint8_t *img, int stride)
{
    int size = qrcodegen_getSize(qr), x, y, dx, dy;
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++) {
            uint8_t v = qrcodegen_getModule(qr, x, y) ? 0 : 255;
            for (dy = 0; dy < px; dy++)
                for (dx = 0; dx < px; dx++)
                    img[(size_t)((quiet + y) * px + dy) * stride + (quiet + x) * px + dx] = v;
        }
}

static size_t qr_byte_capacity(int ver, enum qrcodegen_Ecc ecl)
{
    /* largest payload that still fits: probe downward from the version maximum */
    static uint8_t tmp[qrcodegen_BUFFER_LEN_MAX], qr[qrcodegen_BUFFER_LEN_MAX];
    size_t n;
    for (n = 3000; n > 0; n--) {
        memset(tmp, 0xA5, n);
        if (qrcodegen_encodeBinary(tmp, n, qr, ecl, ver, ver, qrcodegen_Mask_0, false))
            return n;
    }
    return 0;
}

static void test_render(void)
{
    static const struct { int ver; enum qrcodegen_Ecc ecl; int px; size_t cap; } cases[] = {
        { 5, qrcodegen_Ecc_LOW, 3, 0 },
        { 15, qrcodegen_Ecc_LOW, 2, 520 },
        { 20, qrcodegen_Ecc_LOW, 2, 858 },
        { 25, qrcodegen_Ecc_LOW, 2, 1273 },
        { 25, qrcodegen_Ecc_MEDIUM, 3, 0 },
        { 30, qrcodegen_Ecc_LOW, 2, 1732 },
        { 40, qrcodegen_Ecc_LOW, 2, 2953 },
        { 40, qrcodegen_Ecc_HIGH, 2, 0 },
    };
    size_t ci;
    struct quirc *q = quirc_new();
    cp_rx_config cfg;
    cp_rx *rx;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = g_out;
    rx = cp_rx_new(&cfg);
    for (ci = 0; ci < CPW_COUNT(cases); ci++) {
        int ver = cases[ci].ver, px = cases[ci].px, quiet = 4;
        size_t cap = qr_byte_capacity(ver, cases[ci].ecl);
        int modules = ver * 4 + 17, side = (modules + 2 * quiet) * px;
        size_t n = 3000 + rnd() % 3000, frames = 0, decoded = 0;
        uint8_t *data = (uint8_t *)malloc(n);
        uint8_t tmp[qrcodegen_BUFFER_LEN_MAX], qr[qrcodegen_BUFFER_LEN_MAX];
        cp_payload *p;
        cp_tx *tx;
        cp_result *r = NULL;
        if (cases[ci].cap)
            CHECK(cap == cases[ci].cap);
        fill_text(data, n);
        p = cp_payload_text((const char *)data, n);
        tx = cp_tx_new(CP_PROTO_RQ, p, cap, NULL);
        CHECK(tx != NULL);
        if (!tx || quirc_resize(q, side, side) < 0) {
            cp_payload_free(p);
            free(data);
            continue;
        }
        while (!r && frames < 400) {
            size_t len = cp_tx_next(tx, tmp, sizeof tmp);
            int w, h, i;
            uint8_t *img;
            frames++;
            if (!len || len > cap ||
                !qrcodegen_encodeBinary(tmp, len, qr, cases[ci].ecl, ver, ver, qrcodegen_Mask_AUTO, false)) {
                CHECK(!"encode");
                break;
            }
            img = quirc_begin(q, &w, &h);
            memset(img, 255, (size_t)w * h);
            render_qr(qr, px, quiet, img, w);
            quirc_end(q);
            for (i = 0; i < quirc_count(q); i++) {
                struct quirc_code code;
                struct quirc_data qd;
                quirc_extract(q, i, &code);
                if (quirc_decode(&code, &qd) == QUIRC_SUCCESS) {
                    decoded++;
                    if (cp_rx_push(rx, qd.payload, (size_t)qd.payload_len) & CP_EV_COMPLETE)
                        r = cp_rx_take_result(rx);
                }
            }
        }
        note("  v%-2d ecc%d px%d cap %4zu: %zu frames, %zu decoded, %s\n", ver, (int)cases[ci].ecl, px, cap,
             frames, decoded, r ? "ok" : "FAILED");
        CHECK(decoded == frames);
        CHECK(r && r->kind == CP_KIND_TEXT && r->text_len == n && !memcmp(r->text, data, n));
        cp_result_free(r);
        cp_tx_free(tx);
        cp_payload_free(p);
        free(data);
    }
    cp_rx_free(rx);
    quirc_destroy(q);
}

/* ---- Decimen v3 ----------------------------------------------------------- */

#ifndef CP_TEST_DATA
#define CP_TEST_DATA "tests/data"
#endif

static uint8_t *read_data(const char *name, size_t *n)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", CP_TEST_DATA, name);
    return read_file(path, n);
}

/* Feeds a frames file ([u16 LE len][frame]...) into rx; returns the result. */
static cp_result *push_frames(cp_rx *rx, const uint8_t *buf, size_t n, int *ev_all)
{
    size_t off = 0;
    *ev_all = 0;
    while (off + 2 <= n) {
        size_t len = (size_t)buf[off] | ((size_t)buf[off + 1] << 8);
        int ev;
        if (off + 2 + len > n)
            break;
        ev = cp_rx_push(rx, buf + off + 2, len);
        *ev_all |= ev;
        off += 2 + len;
        if (ev & CP_EV_COMPLETE)
            return cp_rx_take_result(rx);
    }
    return NULL;
}

static size_t d3_test_frame(uint8_t *buf, uint8_t flags, uint16_t bl, uint32_t total, uint16_t k)
{
    d3_header h;
    uint8_t blk[3000];
    memset(&h, 0, sizeof h);
    memset(blk, 0x5A, sizeof blk);
    h.flags = flags;
    h.session_id = 9;
    h.k = k;
    h.block_len = bl;
    h.total_len = total;
    return d3_frame_write(buf, 4096, &h, blk);
}

static void test_d3_header(void)
{
    /* docs/technical/golden-vectors.md "Canonical frame" */
    static const uint8_t golden[] = { 0xd1, 0xc3, 0x03, 0x00, 0xef, 0xbe, 0x04, 0x03, 0x02, 0x01, 0x11, 0x01, 0x06, 0x00,
                                      0xba, 0xdc, 0xfe, 0x00, 0xef, 0xcd, 0xab, 0x89, 1, 2, 3, 4, 5, 6 };
    static const uint8_t blk[6] = { 1, 2, 3, 4, 5, 6 };
    d3_header h, g;
    uint8_t buf[4096];
    size_t n;

    memset(&h, 0, sizeof h);
    h.session_id = 0xBEEF;
    h.seq = 0x01020304u;
    h.k = 0x0111;
    h.block_len = 6;
    h.total_len = 0x00FEDCBAu;
    h.fnv = 0x89ABCDEFu;
    CHECK(d3_frame_write(buf, sizeof buf, &h, blk) == sizeof golden && !memcmp(buf, golden, sizeof golden));
    /* stricter than the reference on purpose: k must equal ceil(totalLen / blockLen) */
    CHECK(d3_classify(golden, sizeof golden, &g) == D3_MALFORMED);
    h.total_len = 0x0111u * 6u - 5u;
    d3_frame_write(buf, sizeof buf, &h, blk);
    CHECK(d3_classify(buf, sizeof golden, &g) == D3_OK);
    CHECK(g.session_id == 0xBEEF && g.seq == 0x01020304u && g.k == 0x0111 && g.block_len == 6 &&
          g.total_len == 0x0111u * 6u - 5u && g.fnv == 0x89ABCDEFu && g.flags == 0);

    /* classification table (golden-vectors.md) on a self-consistent frame */
    n = d3_test_frame(buf, 0, 6, 12, 2);
    CHECK(d3_classify(buf, n, NULL) == D3_OK);
    buf[1] = 0x0C; CHECK(d3_classify(buf, n, NULL) == D3_OLDER_SENDER);
    buf[1] = 0x0D; CHECK(d3_classify(buf, n, NULL) == D3_OLDER_SENDER);
    buf[1] = 0x42; CHECK(d3_classify(buf, n, NULL) == D3_FOREIGN);
    buf[1] = 0xC3; buf[0] = 0xD2; CHECK(d3_classify(buf, n, NULL) == D3_FOREIGN);
    buf[0] = 0xD1; buf[2] = 4; CHECK(d3_classify(buf, n, NULL) == D3_NEWER_SENDER);
    buf[2] = 2; CHECK(d3_classify(buf, n, NULL) == D3_OLDER_SENDER);
    buf[2] = 0; CHECK(d3_classify(buf, n, NULL) == D3_MALFORMED);
    buf[2] = 3; buf[3] = 0x01; CHECK(d3_classify(buf, n, NULL) == D3_UNSUPPORTED_FLAGS);
    buf[3] = 0x04; CHECK(d3_classify(buf, n, NULL) == D3_UNSUPPORTED_FLAGS);
    buf[3] = 0x10; CHECK(d3_classify(buf, n, NULL) == D3_OK);
    buf[3] = 0x02; CHECK(d3_classify(buf, n, NULL) == D3_OK);
    buf[3] = 0;
    CHECK(d3_classify(buf, n - 1, NULL) == D3_MALFORMED);
    CHECK(d3_classify(buf, D3_HDR_LEN, NULL) == D3_MALFORMED);
    CHECK(d3_classify(buf, 3, NULL) == D3_FOREIGN);
    n = d3_test_frame(buf, 0, 6, 12, 0);
    CHECK(d3_classify(buf, n, NULL) == D3_MALFORMED);
    n = d3_test_frame(buf, 0, 6, 0, 1);
    CHECK(d3_classify(buf, n, NULL) == D3_MALFORMED);
    n = d3_test_frame(buf, 0, 2932, 2932, 1);                 /* > one V40-L QR */
    CHECK(d3_classify(buf, n, NULL) == D3_OK);
    n = d3_test_frame(buf, D3_FLAG_CAUCHY96, 2932, 2932, 1);
    CHECK(d3_classify(buf, n, NULL) == D3_MALFORMED);

    CHECK(d3_fnv1a((const uint8_t *)"", 0) == 0x811c9dc5u);
    CHECK(d3_fnv1a((const uint8_t *)"a", 1) == 0xe40c292cu);
    CHECK(d3_fnv1a((const uint8_t *)"foobar", 6) == 0xbf9cf968u);
}

static void test_d3_codes(void)
{
    /* tests/fixed-geometry.test.ts: "experimental repair vectors are stable" */
    static const uint8_t expect[5][3] = { { 169, 219, 122 }, { 234, 60, 135 }, { 97, 141, 39 }, { 111, 72, 22 },
                                          { 252, 142, 170 } };
    uint8_t data[14], out[3];
    d3_enc *e;
    int i, k;
    for (i = 0; i < 14; i++)
        data[i] = (uint8_t)(i + 1);
    e = d3_enc_new(data, 14, 3, 0, 1);
    CHECK(e && d3_enc_k(e) == 5);
    for (i = 0; e && i < 5; i++) {
        d3_enc_block(e, 5 + i, out);
        CHECK(!memcmp(out, expect[i], 3));
    }
    d3_enc_free(e);

    /* Cauchy: any k of the first 2k rows decode, reverse order with repeats */
    for (k = 1; k <= 5; k++) {
        size_t n = (size_t)k * 29 - 7;
        uint8_t src[200], blk[29];
        unsigned mask;
        int all_ok = 1;
        d3_header h;
        fill_random(src, n);
        e = d3_enc_new(src, n, 29, 0, 1);
        memset(&h, 0, sizeof h);
        h.flags = D3_FLAG_CAUCHY96;
        h.k = (uint16_t)k;
        h.block_len = 29;
        h.total_len = (uint32_t)n;
        for (mask = 0; mask < (1u << (2 * k)); mask++) {
            d3_dec *d;
            uint8_t *res;
            int bits = 0, seq;
            for (seq = 0; seq < 2 * k; seq++)
                bits += (mask >> seq) & 1;
            if (bits != k)
                continue;
            d = d3_dec_new(&h);
            for (seq = 2 * k - 1; seq >= 0; seq--)
                if (mask & (1u << seq)) {
                    d3_enc_block(e, (uint32_t)seq, blk);
                    d3_dec_add(d, (uint32_t)seq, blk);
                    d3_dec_add(d, (uint32_t)seq, blk);
                }
            res = d3_dec_assemble(d);
            if (!res || memcmp(res, src, n))
                all_ok = 0;
            free(res);
            d3_dec_free(d);
        }
        CHECK(all_ok);
        d3_enc_free(e);
    }

    /* Cauchy: many groups, repair-only cold start in later cycles; LT with loss */
    {
        static const uint32_t ks[] = { 13, 96, 97, 358, 2001 };
        size_t ki;
        for (ki = 0; ki < CPW_COUNT(ks); ki++) {
            int fec;
            for (fec = 0; fec < 2; fec++) {
                uint32_t kk = ks[ki], i2, start = fec ? 13 * kk + 3 : 3 * kk;
                size_t n = (size_t)kk * 47 - 13;
                uint8_t *src = (uint8_t *)malloc(n), blk[47], *res;
                d3_header h;
                d3_dec *d;
                fill_random(src, n);
                e = d3_enc_new(src, n, 47, 0x77, fec == 0);
                memset(&h, 0, sizeof h);
                h.flags = fec == 0 ? D3_FLAG_CAUCHY96 : 0;
                h.session_id = 0x77;
                h.k = (uint16_t)kk;
                h.block_len = 47;
                h.total_len = (uint32_t)n;
                d = d3_dec_new(&h);
                for (i2 = 0; !d3_dec_complete(d) && i2 < 12 * kk; i2++)
                    if (i2 % 4 != 1) {
                        d3_enc_block(e, start + i2, blk);
                        d3_dec_add(d, start + i2, blk);
                    }
                res = d3_dec_assemble(d);
                CHECK(res && !memcmp(res, src, n));
                free(res);
                d3_dec_free(d);
                d3_enc_free(e);
                free(src);
            }
        }
    }
}

static void test_d3_golden(void)
{
    static const char *const vec[] = { "d3_rs.frames", "d3_lt.frames" };
    cp_rx_config cfg;
    cp_rx *rx;
    size_t n, en, i;
    uint8_t *frames, *expect;
    cp_result *r;
    int ev;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = g_out;
    cfg.enable_d3 = 1;
    rx = cp_rx_new(&cfg);
    expect = read_data("d3_file.expect", &en);
    CHECK(expect && en == 5000);
    for (i = 0; i < CPW_COUNT(vec); i++) {
        frames = read_data(vec[i], &n);
        CHECK(frames != NULL);
        r = frames ? push_frames(rx, frames, n, &ev) : NULL;
        CHECK(r && r->kind == CP_KIND_FILES && r->count == 1 && !strcmp(r->names[0], "测试 data.bin") &&
              !strcmp(r->mime, "application/octet-stream") && r->session_id == 0xD3001234u);
        CHECK(result_matches(r, CP_KIND_FILES, expect, en));
        CHECK(!(ev & (CP_EV_FAILED | CP_EV_UNSUPPORTED)));
        cp_result_free(r);
        /* finished stream: frames for it are ignored now */
        CHECK(frames && push_frames(rx, frames, n, &ev) == NULL && ev == 0);
        free(frames);
        cp_rx_reset(rx);
    }
    free(expect);

    frames = read_data("d3_snip.frames", &n);       /* gzip from the browser's CompressionStream */
    expect = read_data("d3_snip.expect", &en);
    r = frames ? push_frames(rx, frames, n, &ev) : NULL;
    CHECK(r && r->kind == CP_KIND_TEXT && !strcmp(r->mime, "text/plain") && r->text && expect &&
          r->text_len == en && !memcmp(r->text, expect, en));
    cp_result_free(r);
    free(frames);
    free(expect);
    cp_rx_free(rx);
}

/* Checks a stored zip: UTF-8 flag, names and contents. */
static int zip_matches(const char *path, size_t count, const char *const *names, const uint8_t *const *datas,
                       const uint64_t *sizes)
{
    size_t n, i, p;
    uint8_t *z = read_file(path, &n);
    uint32_t cd, entries;
    int ok = z && n >= 22;
    if (!ok)
        goto out;
    p = n - 22;
    ok = z[p] == 0x50 && z[p + 1] == 0x4b && z[p + 2] == 5 && z[p + 3] == 6;
    entries = (uint32_t)z[p + 10] | ((uint32_t)z[p + 11] << 8);
    cd = (uint32_t)z[p + 16] | ((uint32_t)z[p + 17] << 8) | ((uint32_t)z[p + 18] << 16) | ((uint32_t)z[p + 19] << 24);
    ok = ok && entries == count;
    for (i = 0, p = cd; ok && i < count; i++) {
        size_t nl = (size_t)z[p + 28] | ((size_t)z[p + 29] << 8), lo, ln;
        uint32_t sz = (uint32_t)z[p + 24] | ((uint32_t)z[p + 25] << 8) | ((uint32_t)z[p + 26] << 16) |
                      ((uint32_t)z[p + 27] << 24);
        lo = (size_t)z[p + 42] | ((size_t)z[p + 43] << 8) | ((size_t)z[p + 44] << 16) | ((size_t)z[p + 45] << 24);
        ok = (z[p + 9] & 0x08) && nl == strlen(names[i])   /* general purpose bit 11: UTF-8 */ && !memcmp(z + p + 46, names[i], nl) && sz == sizes[i];
        ln = (size_t)z[lo + 26] | ((size_t)z[lo + 27] << 8);
        ok = ok && cp_crc32(z + lo + 30 + ln, sz) == ((uint32_t)z[p + 16] | ((uint32_t)z[p + 17] << 8) |
                                                       ((uint32_t)z[p + 18] << 16) | ((uint32_t)z[p + 19] << 24));
        ok = ok && (sz == 0 || !memcmp(z + lo + 30 + ln, datas[i], sz));
        p += 46 + nl;
    }
out:
    free(z);
    return ok;
}

static void test_d3_loopback(void)
{
    static const struct { int kind; size_t n; size_t cap; int lt; double loss; } cases[] = {
        { CP_KIND_TEXT, 50000, 2953, 0, 0.2 },
        { CP_KIND_TEXT, 7, 300, 1, 0.0 },
        { CP_KIND_FILES, 300000, 2953, 0, 0.3 },
        { CP_KIND_FILES, 300000, 2953, 1, 0.3 },
        { CP_KIND_FILES, 3u << 20, 1273, 0, 0.1 },     /* k 2440: 26 groups */
        { CP_KIND_FILES, 200000, 520, 1, 0.5 },
        { CP_KIND_IMAGE, 20000, 858, 0, 0.0 },
        { CP_KIND_IMAGE, 20000, 858, 1, 0.25 },
    };
    cp_rx_config cfg;
    cp_rx *rx;
    size_t ci;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = g_out;
    cfg.enable_d3 = 1;
    rx = cp_rx_new(&cfg);
    for (ci = 0; ci < CPW_COUNT(cases); ci++) {
        size_t n = cases[ci].n;
        uint8_t *data = (uint8_t *)malloc(n);
        cp_payload *p;
        cp_tx_opts o;
        cp_tx *tx;
        cp_tx_info ti;
        cp_result *r;
        loop_stats st;
        int ok;
        if (cases[ci].kind == CP_KIND_TEXT)
            fill_text(data, n);
        else
            fill_random(data, n);
        if (cases[ci].kind == CP_KIND_TEXT)
            p = cp_payload_text((const char *)data, n);
        else if (cases[ci].kind == CP_KIND_IMAGE)
            p = cp_payload_image(data, n, "image/jpeg");
        else
            p = cp_payload_files(1, (const char *const[]){ "blob.bin" }, (const uint8_t *const[]){ data },
                                 (const uint64_t[]){ n });
        memset(&o, 0, sizeof o);
        o.d3_fec = cases[ci].lt ? CP_D3_FEC_LT : CP_D3_FEC_RS96;
        tx = cp_tx_new(CP_PROTO_D3, p, cases[ci].cap, &o);
        CHECK(tx != NULL);
        if (!tx) {
            cp_payload_free(p);
            free(data);
            continue;
        }
        cp_tx_info_get(tx, &ti);
        CHECK(ti.proto == CP_PROTO_D3 && ti.T == cases[ci].cap - D3_HDR_LEN && ti.Z == 0 && ti.session_id > 0 &&
              ti.session_id <= 0xFFFF);
        r = play(tx, rx, cases[ci].loss, ti.total_source_symbols * 12ull + 300, cases[ci].cap, &st);
        if (cases[ci].kind == CP_KIND_TEXT)
            ok = r && r->kind == CP_KIND_TEXT && r->text_len == n && !memcmp(r->text, data, n) &&
                 result_matches(r, CP_KIND_TEXT, data, n);
        else if (cases[ci].kind == CP_KIND_IMAGE)
            ok = result_matches(r, CP_KIND_IMAGE, data, n) && !strcmp(r->mime, "image/jpeg") &&
                 !strcmp(r->names[0], "image.jpg");
        else
            ok = result_matches(r, CP_KIND_FILES, data, n) && !strcmp(r->names[0], "blob.bin");
        CHECK(ok);
        note("  D3 %-2s %8zu B  loss %2.0f%%  blockLen=%-4u k=%-5u gzip=%d frames %llu (pushed %llu)  %s\n",
             cases[ci].lt ? "LT" : "RS", n, cases[ci].loss * 100, ti.T, ti.total_source_symbols, ti.d3_gzip,
             (unsigned long long)st.frames_sent, (unsigned long long)st.frames_pushed, ok ? "ok" : "FAILED");
        cp_result_free(r);
        cp_tx_free(tx);
        cp_payload_free(p);
        free(data);
    }

    /* several files (Unicode names, one empty) travel as one stored zip */
    {
        static const char *const names[3] = { "报告 😀.txt", "data.bin", "空.txt" };
        uint8_t a[3000], b[20000];
        const uint8_t *datas[3] = { a, b, NULL };
        uint64_t sizes[3] = { sizeof a, sizeof b, 0 };
        cp_payload *p;
        cp_tx *tx;
        cp_result *r;
        loop_stats st;
        fill_text(a, sizeof a);
        fill_random(b, sizeof b);
        p = cp_payload_files(3, names, datas, sizes);
        tx = cp_tx_new(CP_PROTO_D3, p, 1273, NULL);
        r = play(tx, rx, 0.1, 2000, 1273, &st);
        CHECK(r && r->kind == CP_KIND_FILES && r->count == 1 && !strcmp(r->names[0], "CodeTrans-3files.zip") &&
              !strcmp(r->mime, "application/zip"));
        CHECK(r && zip_matches(r->paths[0], 3, names, datas, sizes));
        cp_result_free(r);
        cp_tx_free(tx);
        cp_payload_free(p);
    }

    /* receiver joins late (repair rows only), then a corrupted frame */
    {
        size_t n = 40000, i;
        uint8_t *data = (uint8_t *)malloc(n), buf[1300];
        cp_payload *p;
        cp_tx *tx;
        cp_result *r;
        loop_stats st;
        int ev = 0;
        fill_random(data, n);
        p = cp_payload_files(1, (const char *const[]){ "late.bin" }, (const uint8_t *const[]){ data },
                             (const uint64_t[]){ n });
        tx = cp_tx_new(CP_PROTO_D3, p, 1273, NULL);
        for (i = 0; i < 1000; i++)
            cp_tx_next(tx, buf, sizeof buf);
        r = play(tx, rx, 0.3, 2000, sizeof buf, &st);
        CHECK(result_matches(r, CP_KIND_FILES, data, n));
        cp_result_free(r);
        cp_tx_free(tx);

        tx = cp_tx_new(CP_PROTO_D3, p, 1273, NULL);
        for (i = 0; i < 200 && !(ev & (CP_EV_COMPLETE | CP_EV_FAILED)); i++) {
            size_t len = cp_tx_next(tx, buf, sizeof buf);
            if (i == 5)
                buf[D3_HDR_LEN + 100] ^= 0x01;           /* QR ECC let a bad byte through */
            ev = cp_rx_push(rx, buf, len);
        }
        CHECK(ev & CP_EV_FAILED);                        /* FNV catches it */
        r = play(tx, rx, 0.0, 200, sizeof buf, &st);     /* the carousel goes on: recollect */
        CHECK(result_matches(r, CP_KIND_FILES, data, n));
        cp_result_free(r);
        cp_tx_free(tx);
        cp_payload_free(p);
        free(data);
    }

    /* loud and silent rejects, and the enable switch */
    {
        uint8_t buf[4096];
        size_t n = d3_test_frame(buf, 0x01, 6, 12, 2);   /* encrypted */
        CHECK(cp_rx_push(rx, buf, n) == CP_EV_UNSUPPORTED);
        buf[3] = 0;
        buf[2] = 4;
        CHECK(cp_rx_push(rx, buf, n) == CP_EV_UNSUPPORTED);
        buf[2] = 3;
        buf[1] = 0x0D;
        CHECK(cp_rx_push(rx, buf, n) == CP_EV_UNSUPPORTED);
        buf[1] = 0xC3;
        CHECK(cp_rx_push(rx, buf, n - 1) == 0);          /* malformed: silent */
        CHECK(cp_rx_push(rx, buf, n) == CP_EV_PROGRESS);
        CHECK(cp_rx_push(rx, buf, n) == 0);              /* duplicate seq */
        buf[3] = 0x10;                                   /* ignorable bit: same stream, same seq */
        CHECK(cp_rx_push(rx, buf, n) == 0);
        {
            cp_progress pr;
            cp_rx_progress(rx, &pr);
            CHECK(pr.proto == CP_PROTO_D3 && pr.need == 2 && pr.have <= 1 && pr.total_bytes == 12 &&
                  pr.session_id == 0xD3000009u);
        }
        cp_rx_set_options(rx, 0, 0);
        buf[3] = 0x01;
        CHECK(cp_rx_push(rx, buf, n) == 0);
        cp_rx_set_options(rx, 1, 0);
    }
    cp_rx_free(rx);
}

static void test_d3_render(void)
{
    static const struct { int ver; enum qrcodegen_Ecc ecl; size_t cap; int lt; } cases[] = {
        { 27, qrcodegen_Ecc_LOW, 1465, 0 },
        { 40, qrcodegen_Ecc_LOW, 2953, 0 },
        { 40, qrcodegen_Ecc_MEDIUM, 2331, 1 },
        { 32, qrcodegen_Ecc_LOW, 1952, 0 },     /* "fixed geometry": V32 filled to the brim */
    };
    struct quirc *q = quirc_new();
    cp_rx_config cfg;
    cp_rx *rx;
    size_t ci;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = g_out;
    cfg.enable_d3 = 1;
    rx = cp_rx_new(&cfg);
    for (ci = 0; ci < CPW_COUNT(cases); ci++) {
        int ver = cases[ci].ver, px = 2, quiet = 4, side = ((ver * 4 + 17) + 2 * quiet) * px;
        size_t n = 8000 + rnd() % 4000, frames = 0, decoded = 0;
        uint8_t *data = (uint8_t *)malloc(n), tmp[qrcodegen_BUFFER_LEN_MAX], qr[qrcodegen_BUFFER_LEN_MAX];
        cp_payload *p;
        cp_tx_opts o;
        cp_tx *tx;
        cp_result *r = NULL;
        CHECK(qr_byte_capacity(ver, cases[ci].ecl) == cases[ci].cap);
        fill_random(data, n);
        p = cp_payload_files(1, (const char *const[]){ "r.bin" }, (const uint8_t *const[]){ data },
                             (const uint64_t[]){ n });
        memset(&o, 0, sizeof o);
        o.d3_fec = cases[ci].lt ? CP_D3_FEC_LT : CP_D3_FEC_RS96;
        tx = cp_tx_new(CP_PROTO_D3, p, cases[ci].cap, &o);
        CHECK(tx && quirc_resize(q, side, side) >= 0);
        while (tx && !r && frames < 200) {
            size_t len = cp_tx_next(tx, tmp, sizeof tmp);
            int w, h, i;
            uint8_t *img;
            frames++;
            CHECK(len == cases[ci].cap);                   /* every frame fills the version */
            if (!qrcodegen_encodeBinary(tmp, len, qr, cases[ci].ecl, ver, ver, qrcodegen_Mask_4, false)) {
                CHECK(!"encode");
                break;
            }
            img = quirc_begin(q, &w, &h);
            memset(img, 255, (size_t)w * h);
            render_qr(qr, px, quiet, img, w);
            quirc_end(q);
            for (i = 0; i < quirc_count(q); i++) {
                struct quirc_code code;
                struct quirc_data qd;
                quirc_extract(q, i, &code);
                if (quirc_decode(&code, &qd) == QUIRC_SUCCESS) {
                    decoded++;
                    if (cp_rx_push(rx, qd.payload, (size_t)qd.payload_len) & CP_EV_COMPLETE)
                        r = cp_rx_take_result(rx);
                }
            }
        }
        note("  D3 v%-2d ecc%d cap %4zu %s: %zu frames, %zu decoded, %s\n", ver, (int)cases[ci].ecl, cases[ci].cap,
             cases[ci].lt ? "LT" : "RS", frames, decoded, r ? "ok" : "FAILED");
        CHECK(decoded == frames);
        CHECK(result_matches(r, CP_KIND_FILES, data, n));
        cp_result_free(r);
        cp_tx_free(tx);
        cp_payload_free(p);
        free(data);
    }
    cp_rx_free(rx);
    quirc_destroy(q);
}

int main(int argc, char **argv)
{
    int quick = 0, big = 0, i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--quick"))
            quick = 1;
        else if (!strcmp(argv[i], "--big"))
            big = 1;
    }
    cp_mkdir_utf8(g_out);
    note("vectors\n");
    test_vectors();
    note("container\n");
    test_container();
    note("frames\n");
    test_frames();
    note("raptorq overhead\n");
    test_rq_overhead(quick ? 50 : 300);
    note("loopback\n");
    test_loopback(big);
    note("render loopback\n");
    test_render();
    note("decimen v3: header\n");
    test_d3_header();
    note("decimen v3: codes\n");
    test_d3_codes();
    note("decimen v3: golden vectors from the reference implementation\n");
    test_d3_golden();
    note("decimen v3: loopback\n");
    test_d3_loopback();
    note("decimen v3: render loopback\n");
    test_d3_render();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
