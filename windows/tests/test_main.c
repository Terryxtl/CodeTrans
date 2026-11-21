/* cpwin_tests - unit tests for the Windows helpers that don't need a window
 * (PLAN.md §9): QR capacity / geometry, hotkey strings, INI round trip,
 * DIB / PNG / JPEG conversion, content building and a full multi-code frame
 * render loopback (frame_render -> scaled image -> quirc -> cp_rx). */
#include "compat.h"
#include "config.h"
#include "content.h"
#include "frame_render.h"
#include "image_enc.h"
#include "qr_geom.h"
#include "util.h"
#include "quirc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(c) do { if (c) g_pass++; else { g_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_geom(void)
{
    frame_geom g;
    int c, r;
    CHECK(qr_byte_capacity(1, CFG_ECC_L) == 17);
    CHECK(qr_byte_capacity(10, CFG_ECC_L) == 271);
    CHECK(qr_byte_capacity(15, CFG_ECC_L) == 520);
    CHECK(qr_byte_capacity(20, CFG_ECC_L) == 858);
    CHECK(qr_byte_capacity(25, CFG_ECC_L) == 1273);
    CHECK(qr_byte_capacity(30, CFG_ECC_L) == 1732);
    CHECK(qr_byte_capacity(40, CFG_ECC_L) == 2953);
    CHECK(qr_byte_capacity(40, CFG_ECC_M) == 2331);
    CHECK(qr_byte_capacity(40, CFG_ECC_Q) == 1663);
    CHECK(qr_byte_capacity(40, CFG_ECC_H) == 1273);
    CHECK(qr_byte_capacity(41, CFG_ECC_L) == 0);
    CHECK(qr_modules(25) == 117);
    qr_grid(6, &c, &r);
    CHECK(c == 3 && r == 2);
    qr_grid(5, &c, &r);
    CHECK(c == 1 && r == 1);
    frame_geom_calc(&g, 25, 4, 4);
    CHECK(g.cols == 2 && g.rows == 2 && g.cell == 125 && g.w == 250 && g.h == 250 && g.stride == 252);
    frame_geom_origin(&g, 3, &c, &r);
    CHECK(c == 129 && r == 129);
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

/* frame_render -> nearest-neighbour scale -> quirc -> cp_rx, `count` codes / frame */
static void frame_loopback(int proto, int d3_fec, int ver, int ecc, int count)
{
    const int px = 3, quiet = 4;
    frame_geom g;
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
    frame_geom_calc(&g, ver, count, quiet);
    frame = (uint8_t *)malloc((size_t)g.stride * g.h);
    p = cp_payload_text(text, n);
    memset(&opts, 0, sizeof opts);
    opts.d3_fec = d3_fec;
    tx = cp_tx_new(proto, p, qr_byte_capacity(ver, ecc), &opts);
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
        if (!frame_render(tx, &g, ecc, frames & 1, frame)) {
            CHECK(!"frame_render");
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

int wmain(void)
{
    compat_init();
    test_geom();
    test_strings();
    test_config();
    test_images();
    test_content();
    frame_loopback(CP_PROTO_RQ, 0, 10, CFG_ECC_L, 4);
    frame_loopback(CP_PROTO_D3, CP_D3_FEC_RS96, 27, CFG_ECC_L, 2);
    frame_loopback(CP_PROTO_D3, CP_D3_FEC_LT, 20, CFG_ECC_M, 3);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
