/* cpwin_tests - unit tests for the Windows helpers that don't need a window
 * (PLAN.md §9): QR capacity / geometry, hotkey strings, INI round trip,
 * DIB / PNG / JPEG conversion, content building and a full multi-code frame
 * render loopback (cp_tx_render_frame -> scaled image -> quirc -> cp_rx). */
#include "compat.h"
#include "config.h"
#include "content.h"
#include "image_enc.h"
#include "camera.h"
#include "camera_scan.h"
#include "receiver.h"
#include "recv_open.h"
#include "util.h"
#include "qrcodegen.h"
#include "quirc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_geom(void)
{
    /* capacity table / grids are tested in cp_core_tests; here: the DIB row alignment the app uses */
    cp_frame_geom g;
    int c, r;
    CHECK(cp_frame_geom_calc(&g, 25, 4, 4, 0, 4));
    CHECK(g.cols == 2 && g.rows == 2 && g.cell == 125 && g.w == 250 && g.h == 250 && g.stride == 252);
    cp_frame_geom_origin(&g, 3, &c, &r);
    CHECK(c == 129 && r == 129);
    CHECK(cp_qr_capacity(40, CFG_ECC_L) == 2953 && CFG_VERSION_MAX == 40);
}

static void test_strings(void)
{
    UINT m, k;
    wchar_t b[64];
    CHECK(hotkey_parse(L"Win+Shift+S", &m, &k) && m == (MOD_WIN | MOD_SHIFT) && k == 'S');
    CHECK(hotkey_parse(L" ctrl + alt + f12 ", &m, &k) && m == (MOD_CONTROL | MOD_ALT) && k == VK_F12);
    CHECK(hotkey_parse(L"PrintScreen", &m, &k) && m == 0 && k == VK_SNAPSHOT);
    CHECK(!hotkey_parse(L"Win+", &m, &k));
    CHECK(!hotkey_parse(L"S+Win", &m, &k));
    CHECK(!hotkey_parse(L"Hyper+S", &m, &k));
    hotkey_format(MOD_WIN | MOD_SHIFT, 'S', b, 64);
    CHECK(!wcscmp(b, L"Win+Shift+S"));
    fmt_size(512, b, 64);
    CHECK(!wcscmp(b, L"512B"));
    fmt_size(12595, b, 64);
    CHECK(!wcscmp(b, L"12.3KB"));
    fmt_size(286u * 1024, b, 64);
    CHECK(!wcscmp(b, L"286KB"));
    fmt_size(45u << 20, b, 64);
    CHECK(!wcscmp(b, L"45.0MB"));
    CHECK(!wcscmp(path_basename(L"C:\\a\\b.txt"), L"b.txt"));
}

static void test_config(void)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    app_settings a, b;
    GetTempPathW(MAX_PATH, dir);
    wfmt(path, MAX_PATH, L"%lscpwin_test_%lu.ini", dir, GetCurrentProcessId());
    DeleteFileW(path);
    config_set_path(path);
    config_load(&a);
    config_defaults(&b);
    CHECK(!memcmp(&a, &b, sizeof a));
    a.protocol = CFG_PROTO_DECIMEN3;
    a.version = 33;
    a.ecc = CFG_ECC_Q;
    a.module_px = 7;
    a.code_count = 6;
    a.fps = 24;
    a.image_format = CFG_IMG_JPEG;
    a.topmost = FALSE;
    a.snip_mods = MOD_CONTROL | MOD_SHIFT;
    a.snip_vk = VK_F9;
    a.has_window_pos = TRUE;
    a.window_x = -1200;
    a.window_y = 40;
    CHECK(config_save(&a));
    config_load(&b);
    CHECK(!memcmp(&a, &b, sizeof a));
    WritePrivateProfileStringW(L"CodeTrans", L"Codes", L"5", path);     /* invalid -> default */
    WritePrivateProfileStringW(L"CodeTrans", L"FPS", L"500", path);     /* clamped */
    config_load(&b);
    CHECK(b.code_count == 2 && b.fps == 60);
    a.protocol = CFG_PROTO_DECIMEN3_LT;
    CHECK(config_save(&a));
    config_load(&b);
    CHECK(b.protocol == CFG_PROTO_DECIMEN3_LT);
    a.protocol = CFG_PROTO_OB_BANDS;
    CHECK(config_save(&a));
    config_load(&b);
    CHECK(b.protocol == CFG_PROTO_OB_BANDS && b.version == 33);   /* QR version kept for later */
    a.protocol = CFG_PROTO_OB_TILES;
    CHECK(config_save(&a));
    config_load(&b);
    CHECK(b.protocol == CFG_PROTO_OB_TILES && cfg_layout_version(&b) == 40 && cfg_layout_ecc(&b) == CFG_ECC_L);
    WritePrivateProfileStringW(L"CodeTrans", L"Protocol", L"DecimenV9", path);   /* unknown -> default */
    config_load(&b);
    CHECK(b.protocol == CFG_PROTO_DECIMEN3);
    WritePrivateProfileStringW(L"CodeTrans", L"Protocol", L"CodeTrans", path);
    config_load(&b);
    CHECK(b.protocol == CFG_PROTO_CODETRANS);
    config_defaults(&b);
    CHECK(b.protocol == CFG_PROTO_DECIMEN3 && b.version == 40 && b.fps == 30);
    DeleteFileW(path);
}

/* 3 x 2, 24 bpp bottom-up DIB with row padding */
static void test_images(void)
{
    uint8_t dib[sizeof(BITMAPINFOHEADER) + 2 * 12];
    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)dib;
    uint8_t *px = dib + sizeof *bi, *bgra, *png, *jpg, *back, *mk;
    int w, h;
    BOOL alpha;
    size_t n, jn, mn;

    memset(dib, 0, sizeof dib);
    bi->biSize = sizeof *bi;
    bi->biWidth = 3;
    bi->biHeight = 2;
    bi->biPlanes = 1;
    bi->biBitCount = 24;
    /* bottom row (y=1): blue green red ; top row (y=0): white black grey */
    memcpy(px, "\xFF\x00\x00" "\x00\xFF\x00" "\x00\x00\xFF" "\x00\x00\x00", 12);
    memcpy(px + 12, "\xFF\xFF\xFF" "\x00\x00\x00" "\x80\x80\x80" "\x00\x00\x00", 12);
    bgra = dib_to_bgra(dib, sizeof dib, &w, &h, &alpha);
    CHECK(bgra && w == 3 && h == 2 && !alpha);
    if (!bgra)
        return;
    CHECK(!memcmp(bgra, "\xFF\xFF\xFF\xFF" "\x00\x00\x00\xFF" "\x80\x80\x80\xFF", 12));
    CHECK(!memcmp(bgra + 12, "\xFF\x00\x00\xFF" "\x00\xFF\x00\xFF" "\x00\x00\xFF\xFF", 12));

    png = image_encode_png(bgra, w, h, FALSE, &n);
    CHECK(png && n > 8 && !memcmp(png, "\x89PNG", 4));
    back = image_decode_bgra(png, n, &w, &h, &alpha);
    CHECK(back && w == 3 && h == 2 && !alpha && !memcmp(back, bgra, 24));
    free(back);
    free(png);

    jpg = image_encode_jpeg(bgra, 3, 2, FALSE, 90, &jn);
    CHECK(jpg && jn > 100 && jpg[0] == 0xFF && jpg[1] == 0xD8);
    back = image_decode_bgra(jpg, jn, &w, &h, &alpha);
    CHECK(back && w == 3 && h == 2);
    free(back);
    free(jpg);

    mk = image_make_dib(bgra, 3, 2, &mn);
    back = dib_to_bgra(mk, mn, &w, &h, &alpha);
    CHECK(back && w == 3 && h == 2 && !memcmp(back, bgra, 24));
    free(back);
    free(mk);
    free(bgra);

    /* 32 bpp BITMAPV5HEADER with real alpha, top-down */
    {
        uint8_t v5buf[sizeof(BITMAPV5HEADER) + 8];
        BITMAPV5HEADER *v5 = (BITMAPV5HEADER *)v5buf;
        memset(v5buf, 0, sizeof v5buf);
        v5->bV5Size = sizeof *v5;
        v5->bV5Width = 2;
        v5->bV5Height = -1;
        v5->bV5Planes = 1;
        v5->bV5BitCount = 32;
        v5->bV5Compression = BI_BITFIELDS;
        v5->bV5RedMask = 0x00FF0000;
        v5->bV5GreenMask = 0x0000FF00;
        v5->bV5BlueMask = 0x000000FF;
        v5->bV5AlphaMask = 0xFF000000;
        memcpy(v5buf + sizeof *v5, "\x10\x20\x30\x80" "\x01\x02\x03\xFF", 8);
        bgra = dib_to_bgra(v5buf, sizeof v5buf, &w, &h, &alpha);
        CHECK(bgra && w == 2 && h == 1 && alpha && !memcmp(bgra, "\x10\x20\x30\x80\x01\x02\x03\xFF", 8));
        free(bgra);
        /* alpha all zero -> opaque */
        memcpy(v5buf + sizeof *v5, "\x10\x20\x30\x00" "\x01\x02\x03\x00", 8);
        bgra = dib_to_bgra(v5buf, sizeof v5buf, &w, &h, &alpha);
        CHECK(bgra && !alpha && bgra[3] == 255 && bgra[7] == 255);
        free(bgra);
    }
    /* truncated input */
    CHECK(dib_to_bgra(dib, 30, &w, &h, &alpha) == NULL);
}

static void test_content(void)
{
    content *a = content_new_text("hello", 5), *b = content_new_text("hello", 5), *c = content_new_text("hellO", 5);
    build_opts o = { CFG_IMG_PNG, 85, CFG_HARD_CAP_BYTES };
    build_result r;
    cp_payload_info pi;
    CHECK(a && b && c && content_same(a, b) && !content_same(a, c));
    CHECK(content_build(a, &o, &r) && r.payload);
    cp_payload_info_get(r.payload, &pi);
    CHECK(pi.kind == CP_KIND_TEXT && pi.raw_len == 5);
    cp_payload_free(r.payload);
    content_free(a);
    content_free(b);
    content_free(c);

    /* files: two temp files */
    {
        wchar_t dir[MAX_PATH], p1[MAX_PATH], p2[MAX_PATH];
        const wchar_t *paths[2] = { p1, p2 }, *bad = NULL;
        HANDLE f;
        DWORD wr;
        GetTempPathW(MAX_PATH, dir);
        wfmt(p1, MAX_PATH, L"%lscpwin_测试_1.txt", dir);
        wfmt(p2, MAX_PATH, L"%lscpwin_test_2.bin", dir);
        f = CreateFileW(p1, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        WriteFile(f, "first file", 10, &wr, NULL);
        CloseHandle(f);
        f = CreateFileW(p2, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        CloseHandle(f);
        a = content_new_files(2, paths, &bad);
        CHECK(a && a->nfiles == 2 && a->raw_size == 10 && !bad);
        CHECK(a && content_build(a, &o, &r) && r.payload);
        if (r.payload) {
            cp_payload_info_get(r.payload, &pi);
            CHECK(pi.kind == CP_KIND_FILES && pi.raw_len == 2 + strlen("cpwin_测试_1.txt") + 8 + 10 +
                                                           2 + strlen("cpwin_test_2.bin") + 8);
            cp_payload_free(r.payload);
        }
        content_free(a);
        DeleteFileW(p1);
        CHECK(content_new_files(2, paths, &bad) == NULL && bad == p1);
        DeleteFileW(p2);
    }
}

/* cp_tx_render_frame -> nearest-neighbour scale -> quirc -> cp_rx, `count` codes / frame */
static void frame_loopback(int proto, int d3_fec, int ver, int ecc, int count)
{
    const int px = 3, quiet = 4;
    cp_frame_geom g;
    cp_tx_opts opts;
    size_t n = 30000;
    char *text = (char *)malloc(n + 1);
    cp_payload *p;
    cp_tx *tx;
    cp_rx *rx;
    cp_rx_config cfg;
    cp_result *res = NULL;
    struct quirc *q = quirc_new();
    uint8_t *frame;
    int frames = 0, decoded = 0;
    uint32_t seed = 12345;
    char outdir[MAX_PATH];

    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        text[i] = (char)('a' + (seed >> 16) % 26);
    }
    text[n] = 0;
    cp_frame_geom_calc(&g, ver, count, quiet, 0, 4);
    frame = (uint8_t *)malloc((size_t)g.stride * g.h);
    p = cp_payload_text(text, n);
    memset(&opts, 0, sizeof opts);
    opts.d3_fec = d3_fec;
    tx = cp_tx_new(proto, p, cp_qr_capacity(ver, ecc), &opts);
    CHECK(tx != NULL);
    GetTempPathA(MAX_PATH, outdir);
    lstrcatA(outdir, "cpwin_rx");
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = outdir;
    cfg.enable_d3 = 1;
    rx = cp_rx_new(&cfg);
    quirc_resize(q, g.w * px, g.h * px);
    while (tx && !res && frames < 200) {
        int w, h;
        uint8_t *img;
        if (!cp_tx_render_frame(tx, &g, ecc, frames & 1, frame)) {
            CHECK(!"cp_tx_render_frame");
            break;
        }
        frames++;
        img = quirc_begin(q, &w, &h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                img[(size_t)y * w + x] = frame[(size_t)(y / px) * g.stride + x / px] ? 0 : 255;
        quirc_end(q);
        CHECK(quirc_count(q) >= count);    /* may include undecodable false candidates */
        for (int i = 0; i < quirc_count(q); i++) {
            struct quirc_code code;
            static struct quirc_data qd;
            quirc_extract(q, i, &code);
            if (quirc_decode(&code, &qd) == QUIRC_SUCCESS) {
                decoded++;
                if (cp_rx_push(rx, qd.payload, (size_t)qd.payload_len) & CP_EV_COMPLETE)
                    res = cp_rx_take_result(rx);
            }
        }
    }
    printf("  frame loopback %s v%d: %d frames x %d codes, %d decoded, %s\n",
           proto == CP_PROTO_RQ ? "CodeTrans" : d3_fec == CP_D3_FEC_LT ? "Decimen LT" : "Decimen RS96", ver, frames,
           count, decoded, res ? "ok" : "FAILED");
    CHECK(decoded == frames * count);
    CHECK(res && res->kind == CP_KIND_TEXT && res->text_len == n && !memcmp(res->text, text, n));
    cp_result_free(res);
    cp_rx_free(rx);
    cp_tx_free(tx);
    cp_payload_free(p);
    quirc_destroy(q);
    free(frame);
    free(text);
}

/* Optical boards through the real frame layout: cp_tx_render_frame -> scaled image
 * -> cp_ob_locate (every board, side by side) -> cp_ob_sample -> cp_rx. */
static void frame_loopback_ob(int cfg_proto, int count, int px)
{
    app_settings s;
    cp_frame_geom g;
    cp_tx_opts opts;
    size_t n = 60000;
    uint8_t *data = (uint8_t *)malloc(n), *frame, *img;
    float *luma = (float *)malloc(sizeof(float) * CP_OB_SIDE * CP_OB_SIDE);
    const char *name = "ob.bin";
    const uint8_t *datas[1];
    uint64_t sizes[1] = { 60000 };
    cp_payload *p;
    cp_tx *tx;
    cp_rx *rx;
    cp_rx_config cfg;
    cp_result *res = NULL;
    int frames = 0, located = 0, w, h, units = 0;
    uint32_t seed = 777;
    char outdir[MAX_PATH];

    for (size_t i = 0; i < n; i++) {
        seed = seed * 1103515245u + 12345u;
        data[i] = (uint8_t)(seed >> 16);
    }
    datas[0] = data;
    config_defaults(&s);
    s.protocol = cfg_proto;
    s.version = 12;                           /* ignored for boards */
    CHECK(cfg_layout_version(&s) == 40);
    CHECK(cfg_code_bytes(&s) == (cfg_proto == CFG_PROTO_OB_TILES ? 2592u : 3072u));
    cp_frame_geom_calc(&g, cfg_layout_version(&s), count, 4, 0, 4);
    w = g.w * px;
    h = g.h * px;
    frame = (uint8_t *)malloc((size_t)g.stride * g.h);
    img = (uint8_t *)malloc((size_t)w * h);
    p = cp_payload_files(1, &name, datas, sizes);
    memset(&opts, 0, sizeof opts);
    opts.ob_profile = cfg_proto == CFG_PROTO_OB_TILES ? CP_OB_TILES : CP_OB_BANDS;
    tx = cp_tx_new(CP_PROTO_OB, p, 0, &opts);
    CHECK(tx != NULL);
    GetTempPathA(MAX_PATH, outdir);
    lstrcatA(outdir, "cpwin_rx");
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = outdir;
    rx = cp_rx_new(&cfg);
    while (tx && !res && frames < 200) {
        cp_ob_quad q[8];
        int nb;
        if (!cp_tx_render_frame(tx, &g, CFG_ECC_L, 0, frame)) {
            CHECK(!"cp_tx_render_frame");
            break;
        }
        frames++;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                img[(size_t)y * w + x] = frame[(size_t)(y / px) * g.stride + x / px] ? 16 : 235;
        nb = cp_ob_locate(img, w, h, q, 8);
        located += nb;
        for (int i = 0; i < nb; i++) {
            int u = 0;
            cp_ob_sample(img, w, h, &q[i], luma);
            if (cp_rx_push_board(rx, luma, &u) & CP_EV_COMPLETE)
                res = cp_rx_take_result(rx);
            units += u;
        }
    }
    printf("  frame loopback %s x%d: %d frames, %d boards located, %d units, %s\n",
           cfg_proto == CFG_PROTO_OB_TILES ? "board tiles" : "board bands", count, frames, located, units,
           res ? "ok" : "FAILED");
    CHECK(located == frames * count);
    CHECK(units == located * (cfg_proto == CFG_PROTO_OB_TILES ? 24 : 6));
    CHECK(res && res->kind == CP_KIND_FILES && res->count == 1 && res->sizes[0] == n);
    cp_result_free(res);
    cp_rx_free(rx);
    cp_tx_free(tx);
    cp_payload_free(p);
    free(frame);
    free(img);
    free(luma);
    free(data);
}

/* ---- adaptive image format ---------------------------------------------- */

/* packed 24 bpp DIB of w x h, filled by `px(x, y, bgr)` */
static content *dib_content(int w, int h, void (*px)(int, int, uint8_t *))
{
    size_t row = ((size_t)w * 3 + 3) & ~(size_t)3, len = sizeof(BITMAPINFOHEADER) + row * h;
    uint8_t *d = (uint8_t *)calloc(1, len);
    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)d;
    bi->biSize = sizeof *bi;
    bi->biWidth = w;
    bi->biHeight = -h;
    bi->biPlanes = 1;
    bi->biBitCount = 24;
    bi->biCompression = BI_RGB;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px(x, y, d + sizeof *bi + (size_t)y * row + (size_t)x * 3);
    return content_new_image(CONTENT_IMAGE_DIB, d, len);
}

static void px_screenshot(int x, int y, uint8_t *p)   /* window chrome + "text" lines */
{
    int ink = (y % 18) < 10 && (x / 3 + y) % 7 < 3 && x > 20;
    p[0] = p[1] = p[2] = ink ? 30 : (y < 40 ? 225 : 250);
}

static void px_photo(int x, int y, uint8_t *p)        /* smooth gradients + sensor noise */
{
    uint32_t s = (uint32_t)(x * 73856093u ^ y * 19349663u);
    s ^= s >> 13;
    s *= 0x5bd1e995u;
    s ^= s >> 15;
    p[0] = (uint8_t)(60 + x / 5 + (s & 15));
    p[1] = (uint8_t)(90 + y / 4 + ((s >> 4) & 15));
    p[2] = (uint8_t)(120 + (x + y) / 9 + ((s >> 8) & 15));
}

static void test_image_auto(void)
{
    build_opts o = { CFG_IMG_AUTO, 85, CFG_HARD_CAP_BYTES };
    build_result r;
    content *c;
    CHECK(!image_auto_prefers_jpeg(1000, 400, 4000, TRUE));    /* transparency stays PNG */
    CHECK(!image_auto_prefers_jpeg(1000, 400, 100, FALSE));    /* line art stays PNG     */
    CHECK(!image_auto_prefers_jpeg(1000, 600, 4000, FALSE));   /* not worth the loss     */
    CHECK(image_auto_prefers_jpeg(1000, 500, 4000, FALSE));

    c = dib_content(640, 400, px_screenshot);
    memset(&r, 0, sizeof r);
    CHECK(c && content_build(c, &o, &r) && !strcmp(r.mime, "image/png"));
    cp_payload_free(r.payload);
    content_free(c);

    c = dib_content(640, 400, px_photo);
    memset(&r, 0, sizeof r);
    CHECK(c && content_build(c, &o, &r) && !strcmp(r.mime, "image/jpeg"));
    {
        cp_payload_info pi;
        cp_payload_info_get(r.payload, &pi);
        printf("  image auto: photo 640x400 -> %s %llu bytes\n", r.mime, (unsigned long long)pi.raw_len);
    }
    cp_payload_free(r.payload);
    content_free(c);
}

/* ---- camera receiver (phone -> PC) ---------------------------------------- */

static void check_qr_bytes(void *context, const uint8_t *payload, size_t n)
{
    const struct quirc_data *expected = context;
    CHECK(n == (size_t)expected->payload_len && !memcmp(payload, expected->payload, n));
}

/* Same dense symbol and radial warp as Android NativeCoreTest. A fresh scanner
 * must recover the exact payload without a previous frame's geometry. */
static void test_distorted_qr(void)
{
    enum { SIDE = 512 };
    FILE *file = _wfopen(CP_DENSE_FRAME, L"rb");
    CHECK(file != NULL);
    if (!file) return;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    int w, h;
    BOOL alpha;
    uint8_t *png = size > 0 ? malloc((size_t)size) : NULL;
    uint8_t *pixels = png && fread(png, 1, size, file) == (size_t)size
        ? image_decode_bgra(png, size, &w, &h, &alpha) : NULL;
    fclose(file);
    free(png);
    uint8_t *warped = malloc(SIDE * SIDE);
    struct quirc *q = quirc_new();
    cp_scanner *scanner = cp_scanner_new();
    int ready = pixels && w >= SIDE && h >= SIDE + 70 && warped && scanner && q &&
                quirc_resize(q, SIDE, SIDE) == 0;
    CHECK(ready);
    if (ready) {
        uint8_t *clean = quirc_begin(q, NULL, NULL);
        for (int y = 0; y < SIDE; ++y)
            for (int x = 0; x < SIDE; ++x)
                clean[y * SIDE + x] = pixels[((y + 70) * w + x) * 4];
        for (int y = 0; y < SIDE; ++y) for (int x = 0; x < SIDE; ++x) {
            double dx = x - 256, dy = y - 256;
            double scale = 1 + 0.04 * (dx * dx + dy * dy) / (256 * 256);
            double px = CPW_CLAMP(256 + dx * scale, 0, 510.999);
            double py = CPW_CLAMP(256 + dy * scale, 0, 510.999);
            int i = (int)py * SIDE + (int)px;
            double fx = px - (int)px, fy = py - (int)py;
            double top = clean[i] * (1-fx) + clean[i+1] * fx;
            double bottom = clean[i+SIDE] * (1-fx) + clean[i+SIDE+1] * fx;
            warped[y * SIDE + x] = (uint8_t)(top * (1-fy) + bottom * fy);
        }
        quirc_end(q);
        struct quirc_code code;
        struct quirc_data expected;
        int clean_ok = quirc_count(q) == 1;
        if (clean_ok) {
            quirc_extract(q, 0, &code);
            clean_ok = quirc_decode(&code, &expected) == QUIRC_SUCCESS;
        }
        CHECK(clean_ok);
        cp_grid grids[CP_SCAN_MAX_QUADS];
        int count = 0, decoded;
        if (clean_ok) cp_scanner_set_callback(scanner, check_qr_bytes, &expected);
        cp_scan_seeded(scanner, NULL, warped, SIDE, SIDE, grids, &count, &decoded);
        CHECK(count == 1 && decoded == 1);
    }
    cp_scanner_free(scanner);
    if (q) quirc_destroy(q);
    free(warped);
    free(pixels);
}

static void test_camera_luma(void)
{
    camera_mode target = {1920, 1080, 60, 0};
    CHECK(camera_prefer_mode(target, (camera_mode){1280, 720, 60, 1}));
    CHECK(camera_prefer_mode(target, (camera_mode){1920, 1080, 30, 1}));
    CHECK(camera_prefer_mode(target, (camera_mode){1920, 1080, 120, 1}));
    CHECK(camera_prefer_mode((camera_mode){1280,720,30,1}, (camera_mode){640,480,60,1}));
    /* 4x2 frame, every format carries luma 10,20,30,40 / 50,60,70,80 */
    static const uint8_t want[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    uint8_t nv12[2 * 6], yuy2[2 * 8], rgb[2 * 16], y[8];
    memset(nv12, 0xEE, sizeof nv12);   /* pitch 6: 2 bytes of padding per row */
    for (int r = 0; r < 2; r++)
        for (int x = 0; x < 4; x++) {
            uint8_t v = want[r * 4 + x];
            nv12[r * 6 + x] = v;
            yuy2[r * 8 + 2 * x] = v;
            yuy2[r * 8 + 2 * x + 1] = 128;
            /* bottom-up RGB32: row 0 of the picture is stored last */
            rgb[(1 - r) * 16 + 4 * x + 0] = v;
            rgb[(1 - r) * 16 + 4 * x + 1] = v;
            rgb[(1 - r) * 16 + 4 * x + 2] = v;
            rgb[(1 - r) * 16 + 4 * x + 3] = 0;
        }
    camera_extract_luma(CAM_FMT_NV12, nv12, 6, 4, 2, y);
    CHECK(!memcmp(y, want, 8));
    camera_extract_luma(CAM_FMT_YUY2, yuy2, 8, 4, 2, y);
    CHECK(!memcmp(y, want, 8));
    camera_extract_luma(CAM_FMT_RGB32, rgb + 16, -16, 4, 2, y);
    {
        int ok = 1;
        for (int i = 0; i < 8; i++)      /* grey in, grey out (±1 for the integer weights) */
            ok &= y[i] >= want[i] - 1 && y[i] <= want[i];
        CHECK(ok);
    }
}

static void test_recv_open(void)
{
    char p0[] = "C:\\out\\0000abcd\\text.txt", p1[] = "C:\\out\\0000abcd\\photo.JPG",
         p2[] = "C:\\out\\0000abcd\\image.exe", p3[] = "C:\\out\\0000abcd\\a.txt";
    char *paths[1];
    uint64_t sizes[1] = { 5 };
    cp_result r;
    wchar_t w[MAX_PATH];
    memset(&r, 0, sizeof r);
    r.count = 1;
    r.paths = paths;
    r.sizes = sizes;
    CHECK(recv_is_image_path(L"C:\\x\\a.png") && recv_is_image_path(L"b.WEBP") && recv_is_image_path(L"c.jpeg"));
    CHECK(!recv_is_image_path(L"a.png.exe") && !recv_is_image_path(L"C:\\x.png\\a") && !recv_is_image_path(L"a"));
    r.kind = CP_KIND_TEXT;
    paths[0] = p0;
    CHECK(recv_classify(&r, w, MAX_PATH) == RECV_OPEN_DEFAULT_APP && !wcscmp(w, L"C:\\out\\0000abcd\\text.txt"));
    r.kind = CP_KIND_IMAGE;
    paths[0] = p1;
    CHECK(recv_classify(&r, w, MAX_PATH) == RECV_OPEN_DEFAULT_APP);
    paths[0] = p2;                                   /* "image" saved under a non-image name */
    CHECK(recv_classify(&r, w, MAX_PATH) == RECV_OPEN_SHOW_IN_FOLDER);
    r.kind = CP_KIND_FILES;
    paths[0] = p3;                                   /* files are never started, even .txt */
    CHECK(recv_classify(&r, w, MAX_PATH) == RECV_OPEN_SHOW_IN_FOLDER);
    r.count = 0;
    CHECK(recv_classify(&r, w, MAX_PATH) == RECV_OPEN_NONE);
}

static void test_recv_plain_text(void)
{
    size_t n = 0;
    uint8_t *o;
    /* valid UTF-8: one BOM added */
    o = recv_plain_to_utf8("h\xC3\xA9", 3, &n);
    CHECK(o && n == 6 && !memcmp(o, "\xEF\xBB\xBFh\xC3\xA9", 6));
    free(o);
    /* UTF-8 that already has a BOM: still exactly one */
    o = recv_plain_to_utf8("\xEF\xBB\xBF" "hi", 5, &n);
    CHECK(o && n == 5 && !memcmp(o, "\xEF\xBB\xBF" "hi", 5));
    free(o);
    /* GBK bytes for U+4E2D U+6587 ("zhong wen") -> UTF-8 */
    o = recv_plain_to_utf8("\xD6\xD0\xCE\xC4", 4, &n);
    CHECK(o && n == 3 + 6 && !memcmp(o, "\xEF\xBB\xBF\xE4\xB8\xAD\xE6\x96\x87", 9));
    free(o);
    /* empty: just the BOM */
    o = recv_plain_to_utf8("", 0, &n);
    CHECK(o && n == 3);
    free(o);
    /* neither UTF-8 nor GB18030 (lone lead byte): raw bytes kept, no BOM */
    o = recv_plain_to_utf8("a\xFF", 2, &n);
    CHECK(o && n == 2 && !memcmp(o, "a\xFF", 2));
    free(o);
}

static cp_result *g_recv_result;
static int g_recv_frames;

static LRESULT CALLBACK recv_test_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_APP + 77) {
        if (wp == RECV_MSG_RESULT && !g_recv_result)
            g_recv_result = (cp_result *)lp;
        else if (wp == RECV_MSG_RESULT)
            cp_result_free((cp_result *)lp);
        else if (wp == RECV_MSG_PLAIN)
            free((void *)lp);
        else if (wp == RECV_MSG_FRAME)
            g_recv_frames++;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* What the phone shows (2 codes, v15-L, 3 px per module) as a 1280x720 camera
 * frame -> receiver (decode thread) -> RECV_MSG_RESULT on a window. */
static void test_receiver_loopback(void)
{
    enum { W = 1280, H = 720, VER = 15, PX = 3 };
    WNDCLASSW wc;
    HWND hwnd;
    receiver *r;
    size_t n = 7000, cap = cp_qr_capacity(VER, CFG_ECC_L);
    uint8_t *data = (uint8_t *)malloc(n), *img = (uint8_t *)malloc(W * H), frame[4096];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    const char *name = "phone.bin";
    const uint8_t *dp = data;
    uint64_t size = n;
    cp_payload *p;
    cp_tx *tx;
    char dir[MAX_PATH];
    int frames = 0;
    {
        uint32_t s = 2463534242u;  /* xorshift: incompressible, so ~16 symbols */
        for (size_t i = 0; i < n; i++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            data[i] = (uint8_t)s;
        }
    }
    p = cp_payload_files(1, &name, &dp, &size);
    tx = cp_tx_new(CP_PROTO_RQ, p, cap, NULL);
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = recv_test_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"cpwin_tests.recv";
    RegisterClassW(&wc);
    hwnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    GetTempPathA(MAX_PATH - 40, dir);
    strcat(dir, "cpwin_recv_test");
    CreateDirectoryA(dir, NULL);
    r = receiver_start(hwnd, WM_APP + 77, -1, dir);
    CHECK(tx && hwnd && r);
    while (tx && r && !g_recv_result && frames < 300) {
        MSG m;
        memset(img, 190, W * H);   /* a phone screen is never pure white to a webcam */
        for (int k = 0; k < 2; k++) {
            size_t len = cp_tx_next(tx, frame, sizeof frame);
            int side, x0 = 160 + k * 520, y0 = 120;
            if (!len || !qrcodegen_encodeBinary(frame, len, qr, qrcodegen_Ecc_LOW, VER, VER, qrcodegen_Mask_AUTO,
                                                false))
                break;
            side = qrcodegen_getSize(qr);
            for (int y = -4; y < side + 4; y++)
                for (int x = -4; x < side + 4; x++)
                    for (int d = 0; d < PX * PX; d++)
                        img[(y0 + (y + 4) * PX + d / PX) * W + x0 + (x + 4) * PX + d % PX] =
                            qrcodegen_getModule(qr, x, y) ? 30 : 225;
        }
        receiver_feed(r, img, W, H);
        frames++;
        Sleep(33);                 /* ~30 fps camera */
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
            DispatchMessageW(&m);
        receiver_frame_seen(r);
    }
    {
        recv_status st;
        if (r)
            receiver_status(r, &st);
        else
            memset(&st, 0, sizeof st);
        printf("  camera receiver loopback: %d frames fed, %llu scanned (%.1f ms each), %s\n", frames,
               (unsigned long long)st.scanned, st.scan_ms, g_recv_result ? "ok" : "FAILED");
        CHECK(st.frames == (uint64_t)frames && st.scanned > 0);
    }
    CHECK(g_recv_frames > 0);
    CHECK(g_recv_result && g_recv_result->kind == CP_KIND_FILES && g_recv_result->count == 1 &&
          g_recv_result->sizes[0] == n && !strcmp(g_recv_result->names[0], "phone.bin"));
    receiver_stop(r);
    cp_result_free(g_recv_result);
    g_recv_result = NULL;
    DestroyWindow(hwnd);
    cp_tx_free(tx);
    cp_payload_free(p);
    free(img);
    free(data);
}

int wmain(void)
{
    compat_init();
    test_image_auto();
    test_camera_luma();
    test_distorted_qr();
    test_recv_open();
    test_recv_plain_text();
    test_receiver_loopback();
    test_geom();
    test_strings();
    test_config();
    test_images();
    test_content();
    frame_loopback(CP_PROTO_RQ, 0, 10, CFG_ECC_L, 4);
    frame_loopback(CP_PROTO_D3, CP_D3_FEC_RS96, 27, CFG_ECC_L, 2);
    frame_loopback(CP_PROTO_D3, CP_D3_FEC_LT, 20, CFG_ECC_M, 3);
    frame_loopback_ob(CFG_PROTO_OB_BANDS, 2, 3);
    frame_loopback_ob(CFG_PROTO_OB_TILES, 3, 3);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
