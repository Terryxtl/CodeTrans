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
 * - optical board: inner / outer code bounds, lossy reordered late-join
 *   loopback, synthetic camera frames (locate, track, decode)
 */
#include "cp_internal.h"

#include "miniz.h"
#include "qrcodegen.h"
#include "quirc.h"
#include "scanner.h"
#include "quad_tracker.h"
#include "confirmed_regions.h"

#include <math.h>
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

/* ---- text / names / image sniffing ---------------------------------------- */

static void san(const char *in, char *out)
{
    cp_sanitize_name((const uint8_t *)in, strlen(in), out, CP_NAME_MAX + 1);
}

static void test_text_and_names(void)
{
    char o[CP_NAME_MAX + 1];

    /* UTF-8 validation and BOM */
    CHECK(cp_utf8_valid((const uint8_t *)"", 0));
    CHECK(cp_utf8_valid((const uint8_t *)"a\xC3\xA4\xE4\xB8\xAD\xF0\x9F\x98\x80", 10));
    CHECK(!cp_utf8_valid((const uint8_t *)"\xC0\x80", 2));             /* overlong */
    CHECK(!cp_utf8_valid((const uint8_t *)"\xED\xA0\x80", 3));         /* surrogate */
    CHECK(!cp_utf8_valid((const uint8_t *)"\xF4\x90\x80\x80", 4));     /* > U+10FFFF */
    CHECK(!cp_utf8_valid((const uint8_t *)"\xC3", 1));                 /* truncated */
    CHECK(!cp_utf8_valid((const uint8_t *)"\xD6\xD0", 2));             /* GBK bytes */
    CHECK(cp_text_bom_len((const uint8_t *)"\xEF\xBB\xBFhi", 5) == 3);
    CHECK(cp_text_bom_len((const uint8_t *)"\xEF\xBB", 2) == 0);
    CHECK(cp_text_bom_len((const uint8_t *)"hi", 2) == 0);

    /* sanitize */
    san("a..b", o);          CHECK(!strcmp(o, "a_b"));
    san("..", o);            CHECK(!strcmp(o, "_"));
    san("a/b\\c:d", o);      CHECK(!strcmp(o, "a_b_c_d"));
    san("  x. ", o);         CHECK(!strcmp(o, "x"));
    san("", o);              CHECK(!strcmp(o, "file"));
    san("CON", o);           CHECK(!strcmp(o, "_CON"));
    san("nul.txt", o);       CHECK(!strcmp(o, "_nul.txt"));
    san("Com1.log", o);      CHECK(!strcmp(o, "_Com1.log"));
    san("LPT9", o);          CHECK(!strcmp(o, "_LPT9"));
    san("console.txt", o);   CHECK(!strcmp(o, "console.txt"));
    san("a\xE2\x80\xAE" "txt.exe\xE2\x81\xA6", o);   CHECK(!strcmp(o, "atxt.exe"));   /* bidi dropped */
    san("a\x01\x7F" "b\xEF\xBB\xBF", o);             CHECK(!strcmp(o, "ab"));
    san("bad\xFF" "name", o);                          CHECK(!strcmp(o, "bad_name"));

    /* unique: ASCII + Unicode case-insensitive, suffix numbering */
    {
        char a[CP_NAME_MAX + 1], b[CP_NAME_MAX + 1], c[CP_NAME_MAX + 1], d[CP_NAME_MAX + 1];
        char *list[4] = { a, b, c, d };
        strcpy(a, "x.txt");
        strcpy(b, "X.TXT");  cp_unique_name(b, sizeof b, list, 1);   CHECK(!strcmp(b, "X (1).TXT"));
        strcpy(c, "x.TxT");  cp_unique_name(c, sizeof c, list, 2);   CHECK(!strcmp(c, "x (2).TxT"));
        strcpy(d, "noext");  cp_unique_name(d, sizeof d, list, 3);   CHECK(!strcmp(d, "noext"));
        strcpy(a, "\xC3\x84.txt");                                   /* A-umlaut.txt */
        strcpy(b, "\xC3\xA4.txt");                                   /* a-umlaut.txt */
        cp_unique_name(b, sizeof b, list, 1);                        CHECK(!strcmp(b, "\xC3\xA4 (1).txt"));
        strcpy(a, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");   /* Privet, capital P */
        strcpy(b, "\xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");   /* privet */
        cp_unique_name(b, sizeof b, list, 1);                        CHECK(strstr(b, " (1)") != NULL);
        strcpy(a, "\xCE\x91\xCE\xA3");                               /* Alpha Sigma */
        strcpy(b, "\xCE\xB1\xCF\x82");                               /* alpha final-sigma */
        cp_unique_name(b, sizeof b, list, 1);                        CHECK(strstr(b, " (1)") != NULL);
        strcpy(a, "\xC4\x80");                                       /* A-macron */
        strcpy(b, "\xC4\x81");
        cp_unique_name(b, sizeof b, list, 1);                        CHECK(strstr(b, " (1)") != NULL);
        strcpy(a, "\xC3\x84");                                       /* different letters stay distinct */
        strcpy(b, "\xC3\xB6");
        cp_unique_name(b, sizeof b, list, 1);                        CHECK(!strcmp(b, "\xC3\xB6"));
        strcpy(a, "\xC3\x97");                                       /* multiplication vs division sign */
        strcpy(b, "\xC3\xB7");
        cp_unique_name(b, sizeof b, list, 1);                        CHECK(!strcmp(b, "\xC3\xB7"));
    }

    /* cp_payload_files sanitizes and de-duplicates */
    {
        const char *names[5] = { "a..b.txt", "CON", "Dup.txt", "dup.TXT", ("\xE2\x80\xAE" "x") };
        const uint8_t *datas[5] = { (const uint8_t *)"1", (const uint8_t *)"2", (const uint8_t *)"3",
                                    (const uint8_t *)"4", (const uint8_t *)"5" };
        uint64_t sizes[5] = { 1, 1, 1, 1, 1 };
        const char *want[5] = { "a_b.txt", "_CON", "Dup.txt", "dup (1).TXT", "x" };
        cp_payload *p = cp_payload_files(5, names, datas, sizes);
        cp_container_hdr h;
        const uint8_t *bytes, *raw = NULL;
        uint8_t *owned = NULL;
        size_t n = 0, off = 0, i;
        cp_file_entry e;
        CHECK(p != NULL);
        if (p) {
            bytes = cp_payload_bytes(p, &n);
            CHECK(cp_container_decode(bytes, n, 0, &h, &owned, &raw));
            for (i = 0; i < 5; i++) {
                CHECK(cp_files_next(raw, (size_t)h.raw_len, &off, &e) == 1);
                CHECK(e.name_len == strlen(want[i]) && !memcmp(e.name, want[i], e.name_len));
            }
            free(owned);
            cp_payload_free(p);
        }
    }
}

static void test_image_sniff(void)
{
    static const uint8_t png[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0 };
    static const uint8_t png_loose[] = { 0x89, 'P', 'N', 'G', 'x', 'x', 'x', 'x' };
    static const uint8_t jpg[] = { 0xFF, 0xD8, 0xFF, 0xE0 };
    static const uint8_t jpg_loose[] = { 0xFF, 0xD8, 0x00, 0x00 };
    CHECK(!strcmp(cp_image_mime_sniff(png, sizeof png), "image/png"));
    CHECK(cp_image_mime_sniff(png_loose, sizeof png_loose) == NULL);
    CHECK(!strcmp(cp_image_mime_sniff(jpg, sizeof jpg), "image/jpeg"));
    CHECK(cp_image_mime_sniff(jpg_loose, sizeof jpg_loose) == NULL);
    CHECK(!strcmp(cp_image_mime_sniff((const uint8_t *)"GIF89a", 6), "image/gif"));
    CHECK(cp_image_mime_sniff((const uint8_t *)"GIF8xx", 6) == NULL);
    CHECK(!strcmp(cp_image_mime_sniff((const uint8_t *)"RIFF\0\0\0\0WEBP", 12), "image/webp"));
    CHECK(cp_image_mime_sniff(NULL, 0) == NULL);
    /* payload without a mime: sniffed; a loose JPEG falls back to octet-stream */
    {
        cp_payload *p = cp_payload_image(jpg, sizeof jpg, NULL);
        cp_container_hdr h;
        const uint8_t *raw = NULL, *b;
        uint8_t *owned = NULL;
        size_t n = 0;
        char mime[64];
        CHECK(p != NULL);
        if (p) {
            b = cp_payload_bytes(p, &n);
            CHECK(cp_container_decode(b, n, 0, &h, &owned, &raw));
            CHECK(cp_meta_get(h.meta, h.meta_len, "mime", mime, sizeof mime) && !strcmp(mime, "image/jpeg"));
            free(owned);
            cp_payload_free(p);
        }
        p = cp_payload_image(jpg_loose, sizeof jpg_loose, NULL);
        CHECK(p != NULL);
        if (p) {
            b = cp_payload_bytes(p, &n);
            owned = NULL;
            CHECK(cp_container_decode(b, n, 0, &h, &owned, &raw));
            CHECK(cp_meta_get(h.meta, h.meta_len, "mime", mime, sizeof mime) &&
                  !strcmp(mime, "application/octet-stream"));
            free(owned);
            cp_payload_free(p);
        }
    }
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
        /* after a reset (the Android preview's reset button) the same stream is received again */
        cp_rx_reset(rx);
        r = frames ? push_frames(rx, frames, n, &ev) : NULL;
        CHECK(r && result_matches(r, CP_KIND_FILES, expect, en));
        cp_result_free(r);
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

/* ---- optical board ------------------------------------------------------ */

#define OBA (OB_SIDE * OB_SIDE)

/* Port of CodeTrans-Optical-Reference/check.py: CRC vector, every error /
 * erasure split within the algebraic bound, random words rejected, and the
 * outer code from every / random choices of k distinct rows. */
static void test_ob_codes(void)
{
    uint8_t body[OB_BAND_BODY], out[OB_BAND_BODY], wire[4 * OB_N];
    int nsym, s, e, rep, cases = 0, bad = 0, i, k;
    CHECK(ob_crc32c((const uint8_t *)"123456789", 9) == 0xE3069283u);
    for (nsym = 16; nsym >= 8; nsym -= 8)
        for (s = 0; s <= nsym; s++)
            for (e = 0; e <= (nsym - s) / 2; e += (nsym - s) / 2 ? (nsym - s) / 2 : 1)
                for (rep = 0; rep < 20; rep++) {
                    uint8_t pos[OB_N], perm[OB_N];
                    for (i = 0; i < OB_N; i++)
                        perm[i] = (uint8_t)i;
                    if (nsym == 16) {
                        fill_random(body, OB_TILE_BODY);
                        ob_tile_encode(body, wire);
                        for (i = 0; i < e + s; i++) {       /* distinct positions */
                            int j = i + (int)(rnd() % (OB_N - i));
                            uint8_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
                            pos[i] = perm[i];
                            wire[pos[i]] ^= (uint8_t)(1 + rnd() % 255);
                        }
                        if (ob_tile_decode(wire, pos, s, out) == 0 || memcmp(out, body, OB_TILE_BODY))
                            bad++;
                    } else {
                        uint8_t weak[32], counts[4];
                        int l;
                        fill_random(body, OB_BAND_BODY);
                        ob_band_encode(body, wire);
                        memset(weak, 0, sizeof weak);
                        for (l = 0; l < 4; l++) {
                            for (i = 0; i < OB_N; i++)
                                perm[i] = (uint8_t)i;
                            for (i = 0; i < e + s; i++) {
                                int j = i + (int)(rnd() % (OB_N - i));
                                uint8_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
                                if (i < s)
                                    weak[8 * l + i] = perm[i];
                                wire[4 * perm[i] + l] ^= (uint8_t)(1 + rnd() % 255);
                            }
                            counts[l] = (uint8_t)s;
                        }
                        if (ob_band_decode(wire, weak, counts, out) == 0 || memcmp(out, body, OB_BAND_BODY))
                            bad++;
                    }
                    cases++;
                }
    note("  inner: %d error/erasure cases, %d wrong\n", cases, bad);
    CHECK(cases > 400 && bad == 0);
    for (i = 0, bad = 0; i < 200; i++) {
        fill_random(wire, OB_N);
        bad += ob_tile_decode(wire, NULL, 0, out) != 0;
    }
    CHECK(bad == 0);

    /* outer */
    {
        static const int ks[] = { 1, 2, 3, 4, 5, 17, 31, 32 };
        static const int widths[] = { 108, 512 };
        uint8_t *src = (uint8_t *)malloc(32 * 512), *sym = (uint8_t *)malloc(32 * 512), *rec = (uint8_t *)malloc(32 * 512);
        int ki, wi, ok = 0, total = 0;
        fill_random(src, 32 * 512);
        for (ki = 0; ki < (int)CPW_COUNT(ks); ki++)
            for (wi = 0; wi < 2; wi++) {
                int kk = ks[ki], w = widths[wi], trial;
                uint8_t x[512];
                memset(x, 0, sizeof x);
                for (i = 0; i < kk; i++)
                    for (k = 0; k < w; k++)
                        x[k] ^= src[i * w + k];
                ob_outer_symbol(src, kk, w, 32, sym);
                CHECK(!memcmp(sym, x, (size_t)w));       /* row 32 is the XOR */
                for (trial = 0; trial < 60; trial++) {
                    uint8_t ids[32], pool[256];
                    int np = 0, j;
                    for (j = 0; j < kk; j++)
                        pool[np++] = (uint8_t)j;
                    for (j = 32; j < 256; j++)
                        pool[np++] = (uint8_t)j;
                    if (trial == 0)
                        for (j = 0; j < kk; j++) ids[j] = (uint8_t)j;
                    else if (trial == 1)
                        for (j = 0; j < kk; j++) ids[j] = (uint8_t)(224 + j);
                    else
                        for (j = 0; j < kk; j++) {
                            int r = j + (int)(rnd() % (unsigned)(np - j));
                            uint8_t t = pool[j]; pool[j] = pool[r]; pool[r] = t;
                            ids[j] = pool[j];
                        }
                    for (j = 0; j < kk; j++)
                        ob_outer_symbol(src, kk, w, ids[j], sym + j * w);
                    memset(rec, 0, 32 * 512);
                    ok += ob_outer_recover(ids, sym, kk, w, rec) && !memcmp(rec, src, (size_t)(kk * w));
                    total++;
                }
            }
        note("  outer: %d/%d row selections recovered\n", ok, total);
        CHECK(ok == total);
        free(src);
        free(sym);
        free(rec);
    }
}

/* Clean module image -> luminance at module centres: dark 28, light 225. */
static void ob_luma_of(const uint8_t *dark, float *luma)
{
    int i;
    for (i = 0; i < OBA; i++)
        luma[i] = dark[i] ? 28.0f : 225.0f;
}

static void test_ob_loopback(void)
{
    static const size_t sizes[] = { 1, 108, 512, 3457, 16384, 65539, 300000 };
    cp_rx_config cfg;
    int profile;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = g_out;
    for (profile = CP_OB_BANDS; profile <= CP_OB_TILES; profile++) {
        size_t si;
        for (si = 0; si < CPW_COUNT(sizes); si++) {
            size_t n = sizes[si];
            uint8_t *data = (uint8_t *)malloc(n), *boards = (uint8_t *)malloc(2 * OBA);
            float *luma = (float *)malloc(sizeof(float) * OBA);
            ob_unit *u = (ob_unit *)malloc(sizeof(ob_unit) * 2 * OB_SLOTS);
            cp_payload *p;
            cp_tx_opts o;
            cp_tx_info info;
            cp_tx *tx;
            cp_rx *rx = cp_rx_new(&cfg);
            cp_result *r = NULL;
            int t, failed = 0;
            fill_random(data, n);
            p = cp_payload_files(1, (const char *const[]){ "ob.bin" }, (const uint8_t *const[]){ data },
                                 (const uint64_t[]){ n });
            memset(&o, 0, sizeof o);
            o.ob_profile = profile;
            tx = cp_tx_new(CP_PROTO_OB, p, 0, &o);
            CHECK(tx != NULL);
            cp_tx_info_get(tx, &info);
            CHECK(info.proto == CP_PROTO_OB && info.T == (profile == CP_OB_TILES ? 108 : 512));
            CHECK(cp_tx_next(tx, boards, 2 * OBA) == 0);   /* boards only */
            for (t = 0; tx && t < 2000 && !r && !failed; t++) {
                int nu = 0, b, i, ev = 0;
                CHECK(cp_tx_next_board(tx, 2, boards));
                if (t < 3 || t % 13 == 6 || t % 13 == 7)   /* late join + bursts of lost refreshes */
                    continue;
                for (b = 0; b < 2; b++) {
                    ob_unit tmp[OB_SLOTS];
                    int c;
                    ob_luma_of(boards + b * OBA, luma);
                    c = ob_read_board(luma, tmp);
                    for (i = 0; i < c; i++)
                        if (rnd01() > 0.12)                 /* 12 % random unit loss */
                            u[nu++] = tmp[i];
                }
                for (i = nu - 1; i > 0; i--) {              /* reorder */
                    int j = (int)(rnd() % (unsigned)(i + 1));
                    ob_unit x = u[i]; u[i] = u[j]; u[j] = x;
                }
                for (i = 0; i < nu + 2 && i < 2 * OB_SLOTS; i++)   /* plus two duplicates */
                    ev |= ob_rx_feed(rx, u[i % (nu ? nu : 1)].body, u[i % (nu ? nu : 1)].len);
                if (nu == 0)
                    continue;
                failed = (ev & CP_EV_FAILED) != 0;
                if (ev & CP_EV_COMPLETE)
                    r = cp_rx_take_result(rx);
            }
            note("  OB %s %7zu B: %d refreshes, %s\n", profile == CP_OB_TILES ? "tiles" : "bands", n, t,
                 r ? "ok" : "FAILED");
            CHECK(!failed && result_matches(r, CP_KIND_FILES, data, n));
            {
                cp_rx_stats st;
                cp_rx_stats_get(rx, &st);       /* the finished stream stays reported */
                CHECK(st.ob_mode == (uint32_t)profile + 1u && st.ob_k > 0u && st.ob_rows == st.ob_k);
                CHECK(st.ob_groups > 0u && st.ob_solved == st.ob_groups);
            }
            cp_result_free(r);
            cp_tx_free(tx);
            cp_payload_free(p);
            cp_rx_free(rx);
            free(data);
            free(boards);
            free(luma);
            free(u);
        }
    }

    /* bounds: oversize manifest ignored; a conflicting row fails the session */
    {
        uint8_t body[OB_BAND_BODY], man[OB_BAND_DATA], digest[32];
        cp_rx *rx = cp_rx_new(&cfg);
        cp_progress pr;
        memset(man, 0, sizeof man);
        memcpy(man, "CTB1", 4);
        man[7] = 0x10;                               /* 256 MiB > 64 MiB cap */
        cp_sha256(man, OB_BAND_DATA, digest);
        memcpy(body, digest, 8);
        body[8] = body[9] = 0xFF;
        body[10] = 0;
        body[11] = OB_TYPE_BAND | 1;
        memcpy(body + OB_HDR_LEN, man, OB_BAND_DATA);
        CHECK(ob_rx_feed(rx, body, OB_BAND_BODY) == CP_EV_UNSUPPORTED);
        man[7] = 0;
        man[4] = 100;                                 /* 100-byte container  */
        cp_sha256(man, OB_BAND_DATA, digest);
        memcpy(body, digest, 8);
        memcpy(body + OB_HDR_LEN, man, OB_BAND_DATA);
        CHECK(ob_rx_feed(rx, body, OB_BAND_BODY) == 0);
        man[9] ^= 1;                                  /* tag no longer matches */
        memcpy(body + OB_HDR_LEN, man, OB_BAND_DATA);
        CHECK(ob_rx_feed(rx, body, OB_BAND_BODY) == 0);
        cp_rx_progress(rx, &pr);
        CHECK(pr.proto == CP_PROTO_OB && pr.need == 1 && pr.total_bytes == 100);
        {
            cp_rx_stats st;
            cp_rx_stats_get(rx, &st);
            CHECK(st.ob_mode == CP_OB_BANDS + 1u && st.ob_k == 1u && st.ob_rows == 0u);
            CHECK(st.ob_groups == 1u && st.ob_solved == 0u);
        }
        body[8] = body[9] = 0;                        /* group 0 row 0, data */
        body[11] = OB_TYPE_BAND;
        memset(body + OB_HDR_LEN, 'a', OB_BAND_DATA);
        body[OB_HDR_LEN] = 'x';
        /* k = 1: one row completes the group -> SHA-256 mismatch -> FAILED */
        CHECK(ob_rx_feed(rx, body, OB_BAND_BODY) & CP_EV_FAILED);
        cp_rx_free(rx);
    }
}

/* ---- synthetic camera frames for the optical board ------------------------ */

static int t_homography(const double c[4][2], double h[9])
{
    static const double sq[4][2] = { { 0, 0 }, { OB_SIDE, 0 }, { OB_SIDE, OB_SIDE }, { 0, OB_SIDE } };
    double a[8][9];
    int i, j, k;
    for (i = 0; i < 4; i++) {
        double sx = sq[i][0], sy = sq[i][1], u = c[i][0], v = c[i][1];
        double r0[9] = { sx, sy, 1, 0, 0, 0, -u * sx, -u * sy, u }, r1[9] = { 0, 0, 0, sx, sy, 1, -v * sx, -v * sy, v };
        memcpy(a[2 * i], r0, sizeof r0);
        memcpy(a[2 * i + 1], r1, sizeof r1);
    }
    for (i = 0; i < 8; i++) {
        int p = i;
        for (k = i + 1; k < 8; k++)
            if (fabs(a[k][i]) > fabs(a[p][i]))
                p = k;
        if (fabs(a[p][i]) < 1e-12)
            return 0;
        for (j = 0; j < 9; j++) {
            double t = a[i][j]; a[i][j] = a[p][j]; a[p][j] = t;
        }
        for (k = 0; k < 8; k++)
            if (k != i) {
                double f = a[k][i] / a[i][i];
                for (j = i; j < 9; j++)
                    a[k][j] -= f * a[i][j];
            }
    }
    for (i = 0; i < 8; i++)
        h[i] = a[i][8] / a[i][i];
    h[8] = 1;
    return 1;
}

/* Board (1 = dark) projected at corners c into a w x h frame: background with
 * gentle shading, light quiet zone of 4 modules, blur ~0.8 px, noise. */
static void t_raster(const uint8_t *dark, const double c[4][2], uint8_t *frame, int w, int h)
{
    /* Gaussian, sigma 0.8 px, radius 3 (as acquire.py's gaussian_filter(scene, 0.8)) */
    static const float g[7] = { 0.000441f, 0.021910f, 0.228311f, 0.498677f, 0.228311f, 0.021910f, 0.000441f };
    double hm[9], inv[9], det;
    float *f = (float *)malloc(sizeof(float) * w * h), *tmp = (float *)malloc(sizeof(float) * w * h);
    int x, y, d;
    t_homography(c, hm);
    /* inverse of the 3x3 matrix: frame -> board */
    det = hm[0] * (hm[4] * hm[8] - hm[5] * hm[7]) - hm[1] * (hm[3] * hm[8] - hm[5] * hm[6]) +
          hm[2] * (hm[3] * hm[7] - hm[4] * hm[6]);
    inv[0] = (hm[4] * hm[8] - hm[5] * hm[7]) / det;
    inv[1] = (hm[2] * hm[7] - hm[1] * hm[8]) / det;
    inv[2] = (hm[1] * hm[5] - hm[2] * hm[4]) / det;
    inv[3] = (hm[5] * hm[6] - hm[3] * hm[8]) / det;
    inv[4] = (hm[0] * hm[8] - hm[2] * hm[6]) / det;
    inv[5] = (hm[2] * hm[3] - hm[0] * hm[5]) / det;
    inv[6] = (hm[3] * hm[7] - hm[4] * hm[6]) / det;
    inv[7] = (hm[1] * hm[6] - hm[0] * hm[7]) / det;
    inv[8] = (hm[0] * hm[4] - hm[1] * hm[3]) / det;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            double den = inv[6] * x + inv[7] * y + inv[8];
            double u = (inv[0] * x + inv[1] * y + inv[2]) / den, v = (inv[3] * x + inv[4] * y + inv[5]) / den;
            float val = 205.0f + 7.0f * (float)sin(x / 230.0) + 9.0f * (float)cos(y / 240.0);
            if (u >= -4 && v >= -4 && u < OB_SIDE + 4 && v < OB_SIDE + 4)
                val = 225;
            if (u >= 0 && v >= 0 && u < OB_SIDE && v < OB_SIDE)
                val = dark[(int)v * OB_SIDE + (int)u] ? 28.0f : 225.0f;
            f[y * w + x] = val;
        }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float s = 0;
            for (d = -3; d <= 3; d++)
                s += g[d + 3] * f[y * w + (x + d < 0 ? 0 : x + d >= w ? w - 1 : x + d)];
            tmp[y * w + x] = s;
        }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float s = 0, noise;
            for (d = -3; d <= 3; d++)
                s += g[d + 3] * tmp[(y + d < 0 ? 0 : y + d >= h ? h - 1 : y + d) * w + x];
            noise = (float)((rnd01() + rnd01() + rnd01() + rnd01() - 2.0) * 5.2);   /* sigma ~3 */
            s += noise;
            frame[y * w + x] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s + 0.5f);
        }
    free(f);
    free(tmp);
}

static void t_quad(double c[4][2], double cx, double cy, double size, double deg, int roll)
{
    double th = deg * 3.14159265358979 / 180, base[4][2] = { { -.5, -.5 }, { .5, -.5 }, { .5, .5 }, { -.5, .5 } };
    double tmp[4][2];
    int k;
    for (k = 0; k < 4; k++) {
        double x = base[k][0] * size, y = base[k][1] * size * (0.92 + 0.1 * rnd01());
        tmp[k][0] = cx + x * cos(th) - y * sin(th) + (rnd01() - .5) * .05 * size;
        tmp[k][1] = cy + x * sin(th) + y * cos(th) + (rnd01() - .5) * .05 * size;
    }
    for (k = 0; k < 4; k++) {               /* board corner k drawn at a rotated quad corner */
        c[k][0] = tmp[(k + roll) & 3][0];
        c[k][1] = tmp[(k + roll) & 3][1];
    }
}

static void test_ob_camera(void)
{
    const int W = 1280, H = 720;
    uint8_t *frame = (uint8_t *)malloc((size_t)W * H), *board = (uint8_t *)malloc(OBA);
    float *luma = (float *)malloc(sizeof(float) * OBA);
    cp_rx_config cfg;
    int seed, found_all = 0, units_all = 0, oriented = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = g_out;
    g_rng = 0x0B0A2D5EED5ull;   /* independent of how many draws the earlier tests made */
    /* cold acquisition: random size, rotation (+ any quadrant), perspective */
    for (seed = 0; seed < 12; seed++) {
        size_t n = 5000 + rnd() % 5000;
        uint8_t *data = (uint8_t *)malloc(n);
        cp_payload *p;
        cp_tx_opts o;
        cp_tx *tx;
        cp_rx *rx = cp_rx_new(&cfg);
        cp_ob_quad q[2];
        double c[4][2];
        int nb, units = 0, k;
        fill_random(data, n);
        p = cp_payload_files(1, (const char *const[]){ "c.bin" }, (const uint8_t *const[]){ data },
                             (const uint64_t[]){ n });
        memset(&o, 0, sizeof o);
        o.ob_profile = seed & 1;
        tx = cp_tx_new(CP_PROTO_OB, p, 0, &o);
        cp_tx_next_board(tx, 1, board);
        t_quad(c, 560 + 160 * rnd01(), 330 + 60 * rnd01(), 440 + 160 * rnd01(), (rnd01() - .5) * 34, seed & 3);
        t_raster(board, c, frame, W, H);
        nb = cp_ob_locate(frame, W, H, q, 2);
        if (nb == 1) {
            double dev = 0;
            found_all++;
            for (k = 0; k < 4; k++) {
                double dx = fabs(q[0].x[k] - c[k][0]), dy = fabs(q[0].y[k] - c[k][1]);
                dev = dx > dev ? dx : dev;
                dev = dy > dev ? dy : dev;
            }
            oriented += dev < 1.0;           /* right rotation, sub-pixel corners */
            cp_ob_sample(frame, W, H, &q[0], luma);
            cp_rx_push_board(rx, luma, &units);
            units_all += units == (seed & 1 ? OB_SLOTS : OB_ROWS);
        }
        note("  camera %2d %s %3.0f px: boards %d, units %2d, score %.3f\n", seed, seed & 1 ? "tiles" : "bands",
             hypot(c[1][0] - c[0][0], c[1][1] - c[0][1]), nb, units, nb ? q[0].score : 0);
        cp_tx_free(tx);
        cp_payload_free(p);
        cp_rx_free(rx);
        free(data);
    }
    CHECK(found_all == 12 && oriented == 12 && units_all == 12);

    /* a whole transfer through camera frames: locate once, then track a
     * slowly drifting board refresh by refresh */
    {
        size_t n = 40000;
        uint8_t *data = (uint8_t *)malloc(n);
        cp_payload *p;
        cp_tx_opts o;
        cp_tx *tx;
        cp_rx *rx = cp_rx_new(&cfg);
        cp_result *r = NULL;
        cp_ob_quad q;
        double c[4][2];
        int t, locked = 0, relocks = 0, k;
        fill_random(data, n);                /* incompressible: ~80 rows, many refreshes */
        p = cp_payload_files(1, (const char *const[]){ "t.bin" }, (const uint8_t *const[]){ data },
                             (const uint64_t[]){ n });
        memset(&o, 0, sizeof o);
        o.ob_profile = CP_OB_BANDS;
        tx = cp_tx_new(CP_PROTO_OB, p, 0, &o);
        t_quad(c, 620, 360, 560, 8, 1);
        for (t = 0; t < 200 && !r; t++) {
            int ev;
            cp_tx_next_board(tx, 1, board);
            for (k = 0; k < 4; k++) {        /* hand drift: ~1.5 px per frame */
                c[k][0] += 1.2;
                c[k][1] += 0.8;
            }
            t_raster(board, c, frame, W, H);
            if (!locked || !cp_ob_track(frame, W, H, &q)) {
                locked = cp_ob_locate(frame, W, H, &q, 1);
                relocks++;
            }
            if (!locked)
                continue;
            cp_ob_sample(frame, W, H, &q, luma);
            ev = cp_rx_push_board(rx, luma, NULL);
            if (ev & CP_EV_COMPLETE)
                r = cp_rx_take_result(rx);
        }
        note("  camera transfer: %d frames, %d cold searches, %s\n", t, relocks, r ? "ok" : "FAILED");
        CHECK(result_matches(r, CP_KIND_FILES, data, n));
        CHECK(t >= 10 && relocks <= 2);      /* tracking held across the drift */
        cp_result_free(r);
        cp_tx_free(tx);
        cp_payload_free(p);
        cp_rx_free(rx);
        free(data);
    }
    free(frame);
    free(board);
    free(luma);
}

/* ---- adaptive compression -------------------------------------------------- */

static size_t deflate6_len(const uint8_t *d, size_t n)
{
    size_t len = 0;
    int flags = (int)tdefl_create_comp_flags_from_zip_params(6, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
    void *p = tdefl_compress_mem_to_heap(d, n, &len, flags);
    mz_free(p);
    return len;
}

/* builds a FILES payload of `d`, checks the round trip, returns its info */
static cp_payload_info comp_case(const char *label, const uint8_t *d, size_t n)
{
    const char *name = "data.bin";
    uint64_t size = n;
    cp_payload *p = cp_payload_files(1, &name, &d, &size);
    cp_payload_info pi;
    cp_container_hdr h;
    const uint8_t *bytes, *raw = NULL;
    uint8_t *owned = NULL;
    size_t bn = 0, off = 0;
    cp_file_entry e;
    memset(&pi, 0, sizeof pi);
    CHECK(p != NULL);
    if (!p)
        return pi;
    cp_payload_info_get(p, &pi);
    bytes = cp_payload_bytes(p, &bn);
    CHECK(cp_container_decode(bytes, bn, CP_MAX_TOTAL_BYTES, &h, &owned, &raw) &&
          cp_files_next(raw, (size_t)h.raw_len, &off, &e) == 1 && e.size == n && !memcmp(e.data, d, n));
    note("  %-34s %8zu -> %8llu  %s\n", label, n, (unsigned long long)pi.container_len, cp_comp_name(pi.method));
    free(owned);
    cp_payload_free(p);
    return pi;
}

static void test_compression(void)
{
    const size_t n = 600 * 1024;
    uint8_t *d = (uint8_t *)malloc(n);
    cp_payload_info pi;
    size_t i;

    fill_text(d, n);                                          /* text: compresses, never worse than level 6 */
    pi = comp_case("text", d, n);
    CHECK(pi.compressed && pi.method != CP_COMP_STORE);
    CHECK(pi.container_len <= deflate6_len(d, n) + 512);

    fill_random(d, n);                                        /* noise: stored, no expansion */
    pi = comp_case("random", d, n);
    CHECK(!pi.compressed && pi.method == CP_COMP_STORE);

    fill_random(d, 64 * 1024);                                /* random header, then text: the old */
    fill_text(d + 64 * 1024, n - 64 * 1024);                  /* first-64-KiB sample stored all of it */
    pi = comp_case("random 64 KiB + text", d, n);
    CHECK(pi.compressed && pi.container_len < n / 2);

    memset(d, 0, n);                                          /* sparse: long runs */
    for (i = 0; i < n; i += 997)
        d[i] = (uint8_t)rnd();
    pi = comp_case("sparse", d, n);
    CHECK(pi.compressed && pi.container_len < n / 20);

    for (i = 0; i < n; i++)                                   /* skewed noise: Huffman pays, matches don't */
        d[i] = (uint8_t)((rnd() % 7) * (rnd() % 7));
    pi = comp_case("skewed noise", d, n);
    CHECK(pi.compressed && pi.container_len <= deflate6_len(d, n) + 512);

    for (i = 0; i < n / 4; i++) {                             /* little-endian counter table */
        uint32_t v = (uint32_t)(i * 3 + (rnd() & 3));
        memcpy(d + 4 * i, &v, 4);
    }
    pi = comp_case("counter table", d, n);
    CHECK(pi.compressed && pi.container_len <= deflate6_len(d, n) + 512);

    pi = comp_case("tiny", (const uint8_t *)"hello hello hello hello", 23);
    CHECK(!pi.compressed);
    free(d);
}

/* ---- rx diagnostics counters ---------------------------------------------- */

static void test_rx_stats(void)
{
    static const int protos[2] = { CP_PROTO_RQ, CP_PROTO_D3 };
    static uint8_t data[6000];
    uint8_t f0[2048], f[2048];
    const char *name = "stats.bin";
    const uint8_t *dp = data;
    uint64_t size = sizeof data;
    int pi;
    fill_random(data, sizeof data);                   /* incompressible: several blocks */
    for (pi = 0; pi < 2; pi++) {
        cp_payload *p = cp_payload_files(1, &name, &dp, &size);
        cp_tx *tx = cp_tx_new(protos[pi], p, 1000, NULL);
        cp_rx_config cfg;
        cp_rx *rx;
        cp_rx_stats st;
        size_t n0, n;
        int done = 0, pushes = 0, ev;
        memset(&cfg, 0, sizeof cfg);
        cfg.out_dir = g_out;
        cfg.enable_d3 = 1;
        rx = cp_rx_new(&cfg);
        n0 = cp_tx_next(tx, f0, sizeof f0);
        cp_rx_push(rx, f0, n0);
        cp_rx_push(rx, f0, n0);                       /* same code scanned twice */
        cp_rx_stats_get(rx, &st);
        CHECK(st.frames == 2 && st.unique == 1 && st.duplicates == 1);
        if (protos[pi] == CP_PROTO_D3)
            CHECK(st.rs96_k > 1 && st.rs96_rows == 1 && st.rs96_useful == 1 && st.rs96_solved <= 1);
        else
            CHECK(st.rs96_k == 0);
        while (!done && pushes < 200) {
            n = cp_tx_next(tx, f, sizeof f);
            ev = cp_rx_push(rx, f, n);
            pushes++;
            done = (ev & CP_EV_COMPLETE) != 0;
        }
        CHECK(done);
        cp_rx_stats_get(rx, &st);
        if (protos[pi] == CP_PROTO_D3)
            CHECK(st.rs96_solved == st.rs96_k && st.rs96_useful >= st.rs96_k);
        cp_rx_push(rx, f0, n0);                       /* after completion: still a duplicate */
        {
            cp_rx_stats st2;
            cp_rx_stats_get(rx, &st2);
            CHECK(st2.frames == st.frames + 1 && st2.duplicates == st.duplicates + 1 && st2.unique == st.unique);
            CHECK(st2.unique + st2.duplicates + st2.redundant == st2.frames);
            note("  %s: %llu frames, %llu unique, %llu duplicates, %llu redundant, rs96 %u/%u rows\n",
                 protos[pi] == CP_PROTO_RQ ? "CodeTrans" : "Decimen RS-96", (unsigned long long)st2.frames,
                 (unsigned long long)st2.unique, (unsigned long long)st2.duplicates,
                 (unsigned long long)st2.redundant, st2.rs96_useful, st2.rs96_k);
        }
        cp_result_free(cp_rx_take_result(rx));
        cp_rx_free(rx);
        cp_tx_free(tx);
        cp_payload_free(p);
    }
}

/* ---- camera scanner: contrast retry of a partially decoded frame ---------- */

#define SC_W 1280
#define SC_H 720

/* V25-L code, 3 px modules, 4-module quiet zone, light 232 / dark 24 */
static void sc_draw(uint8_t *img, const uint8_t *data, size_t n, int ox, int oy)
{
    uint8_t tmp[qrcodegen_BUFFER_LEN_MAX], qr[qrcodegen_BUFFER_LEN_MAX];
    int size, x, y, d;
    memcpy(tmp, data, n);
    if (!qrcodegen_encodeBinary(tmp, n, qr, qrcodegen_Ecc_LOW, 25, 25, qrcodegen_Mask_AUTO, false))
        return;
    size = qrcodegen_getSize(qr);
    for (y = -4; y < size + 4; y++)
        for (x = -4; x < size + 4; x++)
            for (d = 0; d < 9; d++)
                img[(oy + (y + 4) * 3 + d / 3) * SC_W + ox + (x + 4) * 3 + d % 3] =
                    qrcodegen_getModule(qr, x, y) ? 24 : 232;
}

/* separable Gaussian blur + moire stripes, as android/testdata-gen does */
static void sc_degrade(const uint8_t *src, uint8_t *dst, double sigma, double amp, double period)
{
    static float tmp[SC_W * SC_H];
    float k[64];
    int r = (int)(sigma * 3 + 0.5), i, x, y;
    double ks = 0;
    for (i = -r; i <= r; i++)
        ks += (k[i + r] = (float)exp(-(double)(i * i) / (2 * sigma * sigma)));
    for (i = 0; i <= 2 * r; i++)
        k[i] = (float)(k[i] / ks);
    for (y = 0; y < SC_H; y++)
        for (x = 0; x < SC_W; x++) {
            float a = 0;
            for (i = -r; i <= r; i++)
                a += k[i + r] * src[y * SC_W + (x + i < 0 ? 0 : x + i >= SC_W ? SC_W - 1 : x + i)];
            tmp[y * SC_W + x] = a;
        }
    for (y = 0; y < SC_H; y++)
        for (x = 0; x < SC_W; x++) {
            float a = 0;
            double v;
            for (i = -r; i <= r; i++)
                a += k[i + r] * tmp[(y + i < 0 ? 0 : y + i >= SC_H ? SC_H - 1 : y + i) * SC_W + x];
            v = a + amp * sin(2 * 3.14159265358979 * (x + 0.35 * y) / period);
            dst[y * SC_W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
        }
}

/* Scans `first` then `second` with one scanner; returns the codes decoded in
 * `second`. *rescued = contrast-retry hits during `second`. */
static int sc_scan2(const uint8_t *first, const uint8_t *second, int retry_ms, unsigned *rescued)
{
    cp_rx_config cfg;
    cp_rx *rx;
    cp_scanner *sc = cp_scanner_new();
    int codes = 0;
    unsigned hits;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = g_out;
    rx = cp_rx_new(&cfg);
    cp_scanner_set_retry_interval(sc, retry_ms);
    if (first)
        cp_scanner_scan(sc, rx, first, SC_W, SC_H, NULL);
    hits = cp_scanner_retry_hits(sc);
    cp_scanner_scan(sc, rx, second, SC_W, SC_H, &codes);
    if (rescued)
        *rescued = cp_scanner_retry_hits(sc) - hits;
    cp_scanner_free(sc);
    cp_rx_free(rx);
    return codes;
}

/* Two codes read together, then one of them blurs (focus hunting, motion):
 * the sharp neighbour decoding must not keep the blurred code from the
 * contrast retry. Also a frame in which quirc locates both but reads one. */
static void test_scan_partial(void)
{
    static const double sigmas[] = { 1.0, 1.2, 1.4, 1.6, 1.8, 2.0, 2.2, 2.4, 2.6, 2.8, 3.0, 3.2, 3.4, 3.6 };
    static const double amps[] = { 0, 10, 20, 30, 45, 60 };
    static const double periods[] = { 5.0, 6.5, 8.0, 12.0 };
    uint8_t *both = (uint8_t *)malloc(SC_W * SC_H), *only_b = (uint8_t *)malloc(SC_W * SC_H);
    uint8_t *img = (uint8_t *)malloc(SC_W * SC_H), a[1200], b[1200];
    size_t si, ai, pi;
    int found = 0, tried = 0, x0 = 640, seed, y;
    for (seed = 0; seed < 4 && !found; seed++) {
        g_rng = 0x5CA9E2D1F00DBA11ull + (uint64_t)seed * 0x9E3779B97F4A7C15ull;   /* fixed codes */
        fill_random(a, sizeof a);
        fill_random(b, sizeof b);
        memset(both, 200, SC_W * SC_H);
        memset(only_b, 200, SC_W * SC_H);
        sc_draw(both, a, sizeof a, 40, 150);
        sc_draw(both, b, sizeof b, 700, 150);
        sc_draw(only_b, b, sizeof b, 700, 150);
        for (pi = 0; pi < CPW_COUNT(periods) && !found; pi++)
            for (si = 0; si < CPW_COUNT(sigmas) && !found; si++)
                for (ai = 0; ai < CPW_COUNT(amps) && !found; ai++) {
                    int plain, fixed;
                    unsigned hits = 0;
                    /* a degradation of B that only the contrast retry reads (B alone) */
                    sc_degrade(only_b, img, sigmas[si], amps[ai], periods[pi]);
                    if (sc_scan2(NULL, img, -1, NULL) != 0 || sc_scan2(NULL, img, 0, NULL) != 1)
                        continue;
                    for (y = 0; y < SC_H; y++)      /* add A, sharp, on the left */
                        memcpy(img + (size_t)y * SC_W, both + (size_t)y * SC_W, (size_t)x0);
                    tried++;
                    plain = sc_scan2(both, img, -1, NULL);
                    fixed = sc_scan2(both, img, 0, &hits);
                    if (getenv("SC_DIAG"))
                        note("  seed %d sigma %.1f amp %2.0f period %4.1f: plain %d, with retry %d (%u hits)\n", seed,
                             sigmas[si], amps[ai], periods[pi], plain, fixed, hits);
                    if (plain == 1 && fixed == 2 && hits == 1) {
                        found = 1;
                        note("  neighbour blurred (sigma %.1f, stripes %.0f / %.1f px): plain 1 code, with retry 2\n",
                             sigmas[si], amps[ai], periods[pi]);
                    }
                }
    }
    CHECK(tried > 0);
    CHECK(found);
    free(both);
    free(only_b);
    free(img);
}

/* ---- preview outlines: quad tracker ------------------------------------- */

static cp_scan_quad tq(int x, int y, int s, int decoded, int rot)
{
    static const int cx[4] = { 0, 1, 1, 0 }, cy[4] = { 0, 0, 1, 1 };
    cp_scan_quad q;
    for (int k = 0; k < 4; k++) {
        int src = (k + rot) & 3;
        q.x[k] = x + cx[src] * s;
        q.y[k] = y + cy[src] * s;
    }
    q.decoded = decoded;
    return q;
}

static uint64_t g_trk_frame;

/* feeds the next frame number */
static int trk(cp_quad_tracker *t, int w, int h, const cp_scan_quad *q, int n, uint64_t now)
{
    return cp_quad_tracker_update(t, ++g_trk_frame, w, h, 0, q, n, now);
}

static void test_quad_tracker(void)
{
    cp_quad_tracker t;
    cp_scan_quad in[3], out[CP_SCAN_MAX_QUADS];
    int n;
    cp_quad_tracker_init(&t);

    /* a read code is shown at once and survives a missed frame */
    in[0] = tq(100, 100, 200, 1, 0);
    trk(&t, 640, 480, in, 1, 1000);
    n = cp_quad_tracker_get(&t, 1000, out, CP_SCAN_MAX_QUADS);
    CHECK(n == 1 && out[0].decoded && out[0].x[0] == 100 && out[0].y[2] == 300);
    trk(&t, 640, 480, in, 0, 1100);
    n = cp_quad_tracker_get(&t, 1100, out, CP_SCAN_MAX_QUADS);
    CHECK(n == 1 && out[0].decoded);

    /* corners in another order and a 4 px jump: aligned, moved half way */
    in[0] = tq(104, 100, 200, 1, 2);
    trk(&t, 640, 480, in, 1, 1200);
    n = cp_quad_tracker_get(&t, 1200, out, CP_SCAN_MAX_QUADS);
    CHECK(n == 1 && out[0].x[0] == 102 && out[0].y[0] == 100 && out[0].x[2] == 302);

    /* an unread sighting of a read code does not move it; read lapses after 700 ms */
    in[0] = tq(140, 100, 200, 0, 0);
    trk(&t, 640, 480, in, 1, 1300);
    n = cp_quad_tracker_get(&t, 1300, out, CP_SCAN_MAX_QUADS);
    CHECK(n == 1 && out[0].decoded && out[0].x[0] == 102);
    trk(&t, 640, 480, in, 1, 1600);
    trk(&t, 640, 480, in, 1, 1950);
    n = cp_quad_tracker_get(&t, 1950, out, CP_SCAN_MAX_QUADS);
    CHECK(n == 1 && !out[0].decoded && out[0].x[0] == 102);

    /* gone for longer than 400 ms: dropped */
    trk(&t, 640, 480, in, 0, 2400);
    CHECK(cp_quad_tracker_get(&t, 2400, out, CP_SCAN_MAX_QUADS) == 0);

    /* an unread code needs three sightings; a one-frame phantom never shows */
    in[0] = tq(20, 20, 150, 0, 0);
    in[1] = tq(400, 20, 150, 0, 0);
    trk(&t, 640, 480, in, 2, 3000);
    CHECK(cp_quad_tracker_get(&t, 3000, out, CP_SCAN_MAX_QUADS) == 0);
    trk(&t, 640, 480, in, 1, 3030);
    trk(&t, 640, 480, in, 1, 3060);
    n = cp_quad_tracker_get(&t, 3060, out, CP_SCAN_MAX_QUADS);
    CHECK(n == 1 && !out[0].decoded && out[0].x[0] == 20);

    /* a new frame size drops everything */
    in[0] = tq(20, 20, 150, 1, 0);
    trk(&t, 1280, 720, in, 0, 3090);
    CHECK(cp_quad_tracker_get(&t, 3090, out, CP_SCAN_MAX_QUADS) == 0);
}

/* ported from Android CodeTrackerTest: opacity, sighted count, late frames */
static void test_quad_tracker_view(void)
{
    cp_quad_tracker t;
    cp_scan_quad in[3];
    cp_quad_view v[CP_TRACK_MAX];
    int n;
    cp_quad_tracker_init(&t);
    g_trk_frame = 0;

    /* fresh read: fully opaque, fading over the second half of the hold time, gone after it */
    in[0] = tq(100, 100, 400, 1, 0);
    trk(&t, 1920, 1080, in, 1, 1000);
    n = cp_quad_tracker_view(&t, 1000, v, CP_TRACK_MAX);
    CHECK(n == 1 && v[0].ever_read && v[0].read && v[0].opacity == 1.0f);
    n = cp_quad_tracker_view(&t, 1100, v, CP_TRACK_MAX);   /* 100 of 400 ms: 2 * 0.75, clamped to 1 */
    CHECK(n == 1 && v[0].opacity == 1.0f);
    n = cp_quad_tracker_view(&t, 1300, v, CP_TRACK_MAX);   /* 300 of 400 ms: 2 * 0.25 */
    CHECK(n == 1 && v[0].opacity > 0.49f && v[0].opacity < 0.51f);
    CHECK(cp_quad_tracker_view(&t, 1401, v, CP_TRACK_MAX) == 0);

    /* an unread candidate seen three times is counted, but was never read */
    cp_quad_tracker_reset(&t);
    in[0] = tq(180, 130, 350, 0, 0);
    trk(&t, 1920, 1080, in, 1, 33);
    trk(&t, 1920, 1080, in, 1, 66);
    CHECK(cp_quad_tracker_view(&t, 66, v, CP_TRACK_MAX) == 0);
    trk(&t, 1920, 1080, in, 1, 99);
    n = cp_quad_tracker_view(&t, 99, v, CP_TRACK_MAX);
    CHECK(n == 1 && !v[0].ever_read && !v[0].read);

    /* a late frame is dropped and changes nothing; a rotation change drops the tracks */
    cp_quad_tracker_reset(&t);
    in[0] = tq(100, 100, 400, 1, 0);
    CHECK(cp_quad_tracker_update(&t, 5, 1920, 1080, 90, in, 1, 165) == 1);
    in[0] = tq(140, 100, 400, 1, 0);
    CHECK(cp_quad_tracker_update(&t, 6, 1920, 1080, 90, in, 1, 198) == 1);
    CHECK(cp_quad_tracker_view(&t, 198, v, CP_TRACK_MAX) == 1 && v[0].q[0] == 120.0f);
    in[0] = tq(900, 900, 400, 1, 0);
    CHECK(cp_quad_tracker_update(&t, 4, 1920, 1080, 90, in, 1, 200) == 0);
    CHECK(cp_quad_tracker_view(&t, 200, v, CP_TRACK_MAX) == 1 && v[0].q[0] == 120.0f);
    CHECK(cp_quad_tracker_update(&t, 6, 1920, 1080, 270, in, 0, 210) == 1);   /* same number is not late */
    CHECK(cp_quad_tracker_view(&t, 210, v, CP_TRACK_MAX) == 0);
}

/* ---- confirmed regions (Android ConfirmedRegionsTest + Windows test_region_order_and_expiry) ---- */

static cp_grid grid_at(int x)
{
    cp_grid g = {.size = 29};
    g.corners[0] = (struct quirc_point){x, 20};
    g.corners[1] = (struct quirc_point){x + 100, 20};
    g.corners[2] = (struct quirc_point){x + 100, 120};
    g.corners[3] = (struct quirc_point){x, 120};
    return g;
}

static void test_confirmed_regions(void)
{
    cp_confirmed_regions cache;
    cp_grid g[2], out[CP_SCAN_MAX_QUADS];
    cp_scan_quad unread = {.x = {43, 143, 143, 43}, .y = {20, 20, 120, 120}};

    /* late results cannot move newer geometry; an unread sighting extends life, never moves */
    memset(&cache, 0, sizeof cache);
    cp_regions_observe(&cache, 2, 100, NULL, 0, NULL, 0);     /* an empty frame does not claim frame 2 */
    g[0] = grid_at(20);
    cp_regions_observe(&cache, 1, 10, g, 1, NULL, 0);
    g[0] = grid_at(40);
    cp_regions_observe(&cache, 3, 90, g, 1, NULL, 0);
    g[0] = grid_at(25);
    cp_regions_observe(&cache, 2, 50, g, 1, NULL, 0);         /* late: ignored */
    CHECK(cp_regions_seeds(&cache, 4, 130, out) == 1);
    CHECK(out[0].corners[0].x == 40 && out[0].drift == 10);   /* half the measured displacement */
    cp_regions_observe(&cache, 4, 1000, NULL, 0, &unread, 1);
    CHECK(cp_regions_seeds(&cache, 5, 1800, out) == 1 && out[0].corners[0].x == 40);
    CHECK(cp_regions_seeds(&cache, 6, 2600, out) == 0);
    CHECK(cp_regions_seeds(&cache, 3, 1000, out) == 0);       /* only older frames seed */

    /* frame 0 is a valid first frame */
    memset(&cache, 0, sizeof cache);
    g[0] = grid_at(20);
    cp_regions_observe(&cache, 0, 5, g, 1, NULL, 0);
    CHECK(cp_regions_seeds(&cache, 1, 6, out) == 1);

    /* one good code does not evict its missing neighbour */
    memset(&cache, 0, sizeof cache);
    g[0] = grid_at(20);
    g[1] = grid_at(200);
    cp_regions_observe(&cache, 1, 0, g, 2, NULL, 0);
    g[0] = grid_at(205);
    cp_regions_observe(&cache, 2, 40, g, 1, NULL, 0);
    CHECK(cp_regions_seeds(&cache, 3, 80, out) == 2);
    /* more symbols than the cache holds: forget everything */
    cp_regions_observe(&cache, 3, 90, NULL, -1, NULL, 0);
    CHECK(cp_regions_seeds(&cache, 4, 100, out) == 0);
}

typedef struct { int locked, max_depth, calls; } lock_probe;
static void probe_lock(void *u)   { lock_probe *p = u; p->calls++; if (++p->locked > p->max_depth) p->max_depth = p->locked; }
static void probe_unlock(void *u) { lock_probe *p = u; p->locked--; }

static void test_scan_session(void)
{
    cp_scan_session ses;
    lock_probe probe = {0};
    cp_scan_lock lk = {probe_lock, probe_unlock, &probe};
    uint8_t *img = (uint8_t *)malloc(SC_W * SC_H), data[1200];
    cp_scanner *sc = cp_scanner_new();
    cp_scan_quad q[CP_SCAN_MAX_QUADS];
    cp_rx_config cfg;
    cp_rx *rx;
    int codes = -2, nq = -1, i;

    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = g_out;
    rx = cp_rx_new(&cfg);
    g_rng = 0x5E5510Full;
    fill_random(data, sizeof data);
    memset(img, 200, SC_W * SC_H);
    sc_draw(img, data, sizeof data, 100, 150);

    cp_scan_session_init(&ses, &lk);
    cp_scan_session_reset(&ses, 10);
    /* a frame numbered below the reset neither reads nor writes the cache */
    cp_scan_session_step(&ses, sc, rx, img, SC_W, SC_H, 9, 1000, &codes, q, &nq);
    CHECK(codes == 1 && nq == 1 && q[0].decoded && ses.regions.count == 0);
    cp_scan_session_step(&ses, sc, rx, img, SC_W, SC_H, 10, 1033, &codes, q, &nq);
    CHECK(codes == 1 && ses.regions.count == 1 && probe.locked == 0 && probe.max_depth == 1);
    /* later frames are read through the cached grid and keep it */
    for (i = 11; i < 14; i++) {
        cp_scan_session_step(&ses, sc, rx, img, SC_W, SC_H, (uint64_t)i, 1033 + 33u * (unsigned)(i - 10), &codes, NULL, NULL);
        CHECK(codes == 1 && ses.regions.count == 1);
    }
    /* invalid arguments: nothing scanned, cache untouched */
    CHECK(cp_scan_session_step(&ses, sc, rx, NULL, SC_W, SC_H, 20, 1500, &codes, q, &nq) == 0 && codes == -1 && nq == 0);
    CHECK(ses.regions.count == 1);
    cp_scan_session_reset(&ses, 30);
    CHECK(ses.regions.count == 0);
    cp_scanner_free(sc);
    cp_rx_free(rx);
    free(img);
}

/* ---- shared sender helpers (tx_render.c) --------------------------------- */

static void test_qr_capacity_table(void)
{
    uint8_t *data = (uint8_t *)malloc(qrcodegen_BUFFER_LEN_MAX), qr[qrcodegen_BUFFER_LEN_MAX];
    int ver, ecc;
    memset(data, 0xA5, qrcodegen_BUFFER_LEN_MAX);   /* encodeBinary also uses it as scratch */
    for (ver = 1; ver <= 40; ver++)
        for (ecc = 0; ecc < 4; ecc++) {
            size_t cap = cp_qr_capacity(ver, ecc);
            CHECK(cap > 0 && cap <= 2953);
            /* exactly the largest payload qrcodegen still fits into this version / level */
            CHECK(qrcodegen_encodeBinary(data, cap, qr, (enum qrcodegen_Ecc)ecc, ver, ver, qrcodegen_Mask_0, false));
            CHECK(!qrcodegen_encodeBinary(data, cap + 1, qr, (enum qrcodegen_Ecc)ecc, ver, ver, qrcodegen_Mask_0, false));
        }
    free(data);
    CHECK(cp_qr_capacity(1, CP_ECC_L) == 17);
    CHECK(cp_qr_capacity(10, CP_ECC_L) == 271);
    CHECK(cp_qr_capacity(40, CP_ECC_L) == 2953);
    CHECK(cp_qr_capacity(40, CP_ECC_M) == 2331);
    CHECK(cp_qr_capacity(40, CP_ECC_Q) == 1663);
    CHECK(cp_qr_capacity(40, CP_ECC_H) == 1273);
    CHECK(cp_qr_capacity(0, CP_ECC_L) == 0 && cp_qr_capacity(41, CP_ECC_L) == 0);
    CHECK(cp_qr_capacity(10, -1) == 0 && cp_qr_capacity(10, 4) == 0);
    CHECK(cp_qr_modules(25) == 117 && cp_qr_modules(40) == CP_OB_SIDE && cp_qr_modules(0) == 0);
    CHECK(CP_QR_VERSION_MIN == 5 && CP_QR_VERSION_MAX == 40);
    CHECK(cp_ob_units_per_board(CP_OB_BANDS) == 6 && cp_ob_unit_bytes(CP_OB_BANDS) == 512);
    CHECK(cp_ob_units_per_board(CP_OB_TILES) == 24 && cp_ob_unit_bytes(CP_OB_TILES) == 108);
    CHECK(cp_ob_board_bytes(CP_OB_BANDS) == 3072 && cp_ob_board_bytes(CP_OB_TILES) == 2592);
    CHECK(cp_ob_board_bytes(7) == 0);
    CHECK(cp_code_count_ok(1) && cp_code_count_ok(2) && cp_code_count_ok(3) && cp_code_count_ok(4) &&
          cp_code_count_ok(6) && !cp_code_count_ok(0) && !cp_code_count_ok(5) && !cp_code_count_ok(7));
}

static void test_frame_geom(void)
{
    cp_frame_geom g;
    int x, y, count;
    static const int cols[] = { 0, 1, 2, 3, 2, 1, 3 }, rows[] = { 0, 1, 1, 1, 2, 1, 2 };   /* by count 0..6 */
    for (count = 1; count <= 6; count++) {
        CHECK(cp_frame_geom_calc(&g, 25, count, 4, 0, 1));
        CHECK(g.cols == cols[count] && g.rows == rows[count] && g.count == g.cols * g.rows);
    }
    CHECK(cp_frame_geom_calc(&g, 25, 4, 4, 0, 4));
    CHECK(g.cols == 2 && g.rows == 2 && g.modules == 117 && g.cell == 125 && g.w == 250 && g.h == 250 && g.stride == 252);
    cp_frame_geom_origin(&g, 0, &x, &y);
    CHECK(x == 4 && y == 4);
    cp_frame_geom_origin(&g, 3, &x, &y);
    CHECK(x == 129 && y == 129);
    CHECK(cp_frame_geom_calc(&g, 40, 3, 0, 1, 1));            /* stacked, packed: contiguous matrices */
    CHECK(g.cols == 1 && g.rows == 3 && g.count == 3 && g.w == 177 && g.h == 3 * 177 && g.stride == 177);
    cp_frame_geom_origin(&g, 2, &x, &y);
    CHECK(x == 0 && y == 354);
    CHECK(cp_frame_geom_calc(&g, 15, 2, 4, 2, 1) && g.cols == 2 && g.rows == 1 && g.stride == 2 * (77 + 8));
    CHECK(!cp_frame_geom_calc(&g, 0, 1, 4, 0, 1) && !cp_frame_geom_calc(&g, 41, 1, 4, 0, 1));
    CHECK(!cp_frame_geom_calc(&g, 15, 0, 4, 1, 1) && !cp_frame_geom_calc(&g, 15, CP_MAX_CODES + 1, 4, 1, 1));
}

static void test_mask_policy(void)
{
    CHECK(!cp_fixed_mask(10, 1) && !cp_fixed_mask(30, 2) && !cp_fixed_mask(60, 1) && !cp_fixed_mask(10, 6));
    CHECK(cp_fixed_mask(45, 2) && cp_fixed_mask(30, 3) && cp_fixed_mask(60, 2) && cp_fixed_mask(15, 6));
    CHECK(cp_fixed_mask(90, 1) && !cp_fixed_mask(89, 1));
}

/* same session id + payload => identical packet stream, so a twin tx is the reference */
static cp_tx *twin_d3(const cp_payload *p, size_t cap)
{
    cp_tx_opts o;
    memset(&o, 0, sizeof o);
    o.session_id = 0x1234;
    return cp_tx_new(CP_PROTO_D3, p, cap, &o);
}

static void test_tx_render(void)
{
    const char *text = "tx_render: shared QR rasteriser, mask policy and board layout";
    cp_payload *p = cp_payload_text(text, strlen(text));
    int ver = 12, ecc = CP_ECC_M, mask;
    size_t cap = cp_qr_capacity(ver, ecc);
    for (mask = 0; mask < 2; mask++) {
        cp_tx *a = twin_d3(p, cap), *b = twin_d3(p, cap);
        cp_frame_geom g;
        uint8_t *pix, buf[qrcodegen_BUFFER_LEN_MAX], qr[qrcodegen_BUFFER_LEN_MAX];
        int i, bad = 0;
        CHECK(a && b && cp_frame_geom_calc(&g, ver, 3, 2, 0, 4));
        pix = (uint8_t *)malloc((size_t)g.stride * g.h);
        memset(pix, 7, (size_t)g.stride * g.h);                  /* must be fully overwritten */
        CHECK(cp_tx_render_frame(a, &g, ecc, mask, pix));
        for (i = 0; i < g.count; i++) {
            size_t n = cp_tx_next(b, buf, sizeof buf);
            int x0, y0, x, y;
            CHECK(n > 0 && qrcodegen_encodeBinary(buf, n, qr, (enum qrcodegen_Ecc)ecc, ver, ver,
                                                  mask ? qrcodegen_Mask_0 : qrcodegen_Mask_AUTO, false));
            cp_frame_geom_origin(&g, i, &x0, &y0);
            for (y = 0; y < g.modules; y++)
                for (x = 0; x < g.modules; x++)
                    bad += pix[(size_t)(y0 + y) * g.stride + x0 + x] != (qrcodegen_getModule(qr, x, y) ? 1 : 0);
        }
        CHECK(bad == 0);
        /* quiet zones and row padding stay light; only 0 / 1 anywhere */
        for (i = 0, bad = 0; i < g.stride * g.h; i++)
            bad += pix[i] > 1;
        CHECK(bad == 0);
        CHECK(pix[0] == 0 && pix[g.stride - 1] == 0 && pix[(size_t)(g.h - 1) * g.stride + g.stride - 1] == 0);
        free(pix);
        cp_tx_free(a);
        cp_tx_free(b);
    }
    /* the two mask policies really differ on at least one of the first frames */
    {
        static uint8_t rnd_data[6000];
        cp_payload *rp;
        cp_tx *a, *b;
        cp_frame_geom g;
        uint8_t *x1, *x2;
        int diff = 0, i;
        fill_random(rnd_data, sizeof rnd_data);
        rp = cp_payload_files(1, (const char *const[]){ "m.bin" }, (const uint8_t *const[]){ rnd_data },
                              (const uint64_t[]){ sizeof rnd_data });
        a = twin_d3(rp, cap);
        b = twin_d3(rp, cap);
        cp_payload_free(rp);
        CHECK(a && b && cp_frame_geom_calc(&g, ver, 1, 0, 1, 1));
        x1 = (uint8_t *)malloc((size_t)g.stride * g.h);
        x2 = (uint8_t *)malloc((size_t)g.stride * g.h);
        for (i = 0; i < 8 && !diff; i++) {
            CHECK(cp_tx_render_frame(a, &g, ecc, 0, x1) && cp_tx_render_frame(b, &g, ecc, 1, x2));
            diff = memcmp(x1, x2, (size_t)g.stride * g.h) != 0;
        }
        CHECK(diff);
        free(x1);
        free(x2);
        cp_tx_free(a);
        cp_tx_free(b);
    }
    cp_payload_free(p);

    {   /* bad arguments */
        cp_frame_geom g;
        uint8_t pix[16];
        CHECK(cp_frame_geom_calc(&g, 10, 1, 4, 0, 1));
        CHECK(!cp_tx_render_frame(NULL, &g, 0, 0, pix));
    }
}

static void test_tx_render_boards(void)
{
    static uint8_t data[20000];
    int profile, mode;
    fill_random(data, sizeof data);
    for (profile = CP_OB_BANDS; profile <= CP_OB_TILES; profile++) {
        cp_payload *p = cp_payload_files(1, (const char *const[]){ "b.bin" }, (const uint8_t *const[]){ data },
                                         (const uint64_t[]){ sizeof data });
        for (mode = 0; mode < 2; mode++) {
            /* mode 0: stacked, packed (Android); mode 1: 2x1 grid, quiet 4, DWORD rows (Windows) */
            cp_tx_opts o;
            cp_tx *a, *b;
            cp_tx_info info;
            cp_frame_geom g;
            uint8_t *pix, *ref;
            int i, bad = 0;
            memset(&o, 0, sizeof o);
            o.ob_profile = profile;
            a = cp_tx_new(CP_PROTO_OB, p, 0, &o);
            b = cp_tx_new(CP_PROTO_OB, p, 0, &o);
            CHECK(a && b);
            cp_tx_info_get(a, &info);
            CHECK(info.proto == CP_PROTO_OB && info.ob_profile == profile);
            CHECK(mode ? cp_frame_geom_calc(&g, 40, 2, 4, 0, 4) : cp_frame_geom_calc(&g, 40, 2, 0, 1, 1));
            pix = (uint8_t *)malloc((size_t)g.stride * g.h);
            ref = (uint8_t *)malloc(2 * (size_t)CP_OB_SIDE * CP_OB_SIDE);
            memset(pix, 9, (size_t)g.stride * g.h);
            CHECK(cp_tx_render_frame(a, &g, CP_ECC_L, 0, pix));
            CHECK(cp_tx_next_board(b, 2, ref));
            for (i = 0; i < 2; i++) {
                int x0, y0, x, y;
                cp_frame_geom_origin(&g, i, &x0, &y0);
                for (y = 0; y < CP_OB_SIDE; y++)
                    for (x = 0; x < CP_OB_SIDE; x++)
                        bad += pix[(size_t)(y0 + y) * g.stride + x0 + x] !=
                               ref[(size_t)i * CP_OB_SIDE * CP_OB_SIDE + (size_t)y * CP_OB_SIDE + x];
            }
            CHECK(bad == 0);
            for (i = 0, bad = 0; i < g.stride * g.h; i++)
                bad += pix[i] > 1;
            CHECK(bad == 0);
            /* a QR-sized geometry is not a board geometry */
            CHECK(cp_frame_geom_calc(&g, 39, 1, 0, 1, 1) && !cp_tx_render_frame(a, &g, CP_ECC_L, 0, pix));
            free(pix);
            free(ref);
            cp_tx_free(a);
            cp_tx_free(b);
        }
        cp_payload_free(p);
    }
}

static int near_(double a, double b) { return a > b - 1e-9 && a < b + 1e-9; }

static void test_units_and_eta(void)
{
    cp_tx_info qr, rq, bands, tiles;
    memset(&qr, 0, sizeof qr);
    qr.proto = CP_PROTO_D3;
    qr.total_source_symbols = 300;
    rq = qr;
    rq.proto = CP_PROTO_RQ;
    rq.Z = 3;
    bands = qr;
    bands.proto = CP_PROTO_OB;
    bands.ob_profile = CP_OB_BANDS;
    tiles = bands;
    tiles.ob_profile = CP_OB_TILES;

    CHECK(near_(cp_tx_units_per_frame(&qr, 1), 1) && near_(cp_tx_units_per_frame(&qr, 2), 2));
    CHECK(near_(cp_tx_units_per_frame(&bands, 1), 6 - 1.0 / 16) && near_(cp_tx_units_per_frame(&bands, 2), 12 - 1.0 / 16));
    CHECK(near_(cp_tx_units_per_frame(&tiles, 1), 24 - 1.0 / 8) && near_(cp_tx_units_per_frame(&tiles, 2), 48 - 1.0 / 8));
    CHECK(cp_tx_units_per_frame(&qr, 0) == 0 && cp_tx_units_per_frame(NULL, 1) == 0);

    CHECK(near_(cp_tx_eta_seconds(&qr, 2, 10), 15.0));                   /* 300 / (2 x 10)          */
    CHECK(near_(cp_tx_eta_seconds(&rq, 2, 10), 306.0 / 20));             /* + 2 per source block    */
    CHECK(near_(cp_tx_eta_seconds(&bands, 1, 10), 300.0 / ((6 - 1.0 / 16) * 10)));
    CHECK(near_(cp_tx_eta_seconds(&tiles, 2, 7.5), 300.0 / ((48 - 1.0 / 8) * 7.5)));
    CHECK(cp_tx_eta_seconds(&qr, 2, 0) == 0 && cp_tx_eta_seconds(&qr, 0, 10) == 0 && cp_tx_eta_seconds(NULL, 1, 1) == 0);
}

static void test_pacing(void)
{
    CHECK(cp_pace_vblanks(60, 60) == 1 && cp_pace_vblanks(30, 60) == 2 && cp_pace_vblanks(20, 60) == 3);
    CHECK(cp_pace_vblanks(45, 60) == 1);                  /* 1.33 -> 1: really 60 fps       */
    CHECK(cp_pace_vblanks(24, 60) == 3);                  /* 2.5 rounds up: 20 fps          */
    CHECK(cp_pace_vblanks(10, 60) == 6 && cp_pace_vblanks(6, 60) == 10);
    CHECK(cp_pace_vblanks(60, 30) == 1);                  /* faster than the panel: every refresh */
    CHECK(cp_pace_vblanks(30, 144) == 5 && cp_pace_vblanks(60, 144) == 2);
    CHECK(cp_pace_vblanks(0, 60) == 60 && cp_pace_vblanks(-5, 60) == 60);
    CHECK(cp_pace_vblanks(30, 0) == 1 && cp_pace_vblanks(30, -1) == 1);
    CHECK(near_(cp_pace_effective_fps(60, 60), 60) && near_(cp_pace_effective_fps(30, 60), 30));
    CHECK(near_(cp_pace_effective_fps(45, 60), 60) && near_(cp_pace_effective_fps(24, 60), 20));
    CHECK(near_(cp_pace_effective_fps(30, 59.94), 29.97) && near_(cp_pace_effective_fps(60, 144), 72));
    CHECK(near_(cp_pace_effective_fps(30, 0), 30) && near_(cp_pace_effective_fps(0, 0), 1));
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
    test_text_and_names();
    test_image_sniff();
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
    note("optical board: codes\n");
    test_ob_codes();
    note("optical board: loopback\n");
    test_ob_loopback();
    note("optical board: synthetic camera frames\n");
    test_ob_camera();
    note("adaptive compression\n");
    test_compression();
    note("rx diagnostics\n");
    test_rx_stats();
    note("camera scanner: partial frames\n");
    test_scan_partial();
    note("camera scanner: preview outline tracker\n");
    test_quad_tracker();
    test_quad_tracker_view();
    note("camera scanner: confirmed regions and session\n");
    test_confirmed_regions();
    test_scan_session();
    note("sender helpers: capacity, geometry, render, mask, ETA, pacing\n");
    test_qr_capacity_table();
    test_frame_geom();
    test_mask_policy();
    test_tx_render();
    test_tx_render_boards();
    test_units_and_eta();
    test_pacing();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
