/*
 * Generates the frame sequences used by the instrumented test
 * (app/src/androidTest/.../NativeCoreTest.kt, PLAN §14 A1 / §15):
 *
 *   text_NNN.png   CodeTrans TEXT transfer, 2 codes per 1280x720 frame, every 4th
 *                  code dropped so RaptorQ repair symbols are needed
 *   files_NNN.png  CodeTrans FILES transfer (Unicode names), same scheme
 *   plain.png      one ordinary QR code
 *   d3rs_NNN.png   Decimen v3 Cauchy RS-96 text snippet (same text as text_), V27-L,
 *                  2 codes per frame, every 4th code dropped
 *   d3lt_NNN.png   Decimen v3 LT carousel, one file (= file1.bin), same scheme
 *   d3hard.png     one Decimen frame, blurred + moire stripes: quirc alone fails,
 *                  the scanner's contrast retry reads it (d3hard.txt)
 *   text.txt, plain.txt, file0.bin, file1.bin, files.txt   expected results
 *
 * Every generated sequence is verified here first with the real receiver
 * (core/scan/scanner.c + core) so a test failure on the device
 * points at the Android side, not at the data.
 *
 * Build (any C11 host compiler), see README.md next to this file.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cp.h"
#include "qrcodegen.h"
#include "scanner.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define W 1280
#define H 720
#define VER 25
#define SCALE 3
#define QR_CAP 1273   /* byte capacity of version 25-L */
#define D3_VER 27
#define D3_CAP 1465   /* byte capacity of version 27-L */

static const char *g_out, *g_verify;
static uint32_t g_seed = 20261120u;

static uint32_t rnd(void)
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return g_seed >> 8;
}

static int draw_v(uint8_t *img, const uint8_t *data, size_t n, int ox, int oy, int ver)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    int size, x, y, dx, dy;
    if (n > sizeof tmp)
        return 0;
    memcpy(tmp, data, n);
    if (!qrcodegen_encodeBinary(tmp, n, qr, qrcodegen_Ecc_LOW, 1, ver, qrcodegen_Mask_AUTO, false))
        return 0;
    size = qrcodegen_getSize(qr);
    for (y = -4; y < size + 4; y++)
        for (x = -4; x < size + 4; x++) {
            uint8_t v = qrcodegen_getModule(qr, x, y) ? 24 : 232;
            for (dy = 0; dy < SCALE; dy++)
                for (dx = 0; dx < SCALE; dx++) {
                    int px = ox + (x + 4) * SCALE + dx, py = oy + (y + 4) * SCALE + dy;
                    if (px >= 0 && px < W && py >= 0 && py < H)
                        img[py * W + px] = v;
                }
        }
    return 1;
}

static int draw(uint8_t *img, const uint8_t *data, size_t n, int ox, int oy)
{
    return draw_v(img, data, n, ox, oy, VER);
}

static int write_bytes(const char *name, const void *d, size_t n)
{
    char path[1024];
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", g_out, name);
    f = fopen(path, "wb");
    if (!f || fwrite(d, 1, n, f) != n) {
        fprintf(stderr, "cannot write %s\n", path);
        return 0;
    }
    fclose(f);
    return 1;
}

static int write_png(const char *name, const uint8_t *img)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_out, name);
    if (!stbi_write_png(path, W, H, 1, img, W)) {
        fprintf(stderr, "cannot write %s\n", path);
        return 0;
    }
    return 1;
}

/* Renders the transfer and checks it decodes; returns the number of frames.
 * proto CP_PROTO_D3 uses version 27-L and the given Decimen outer code. */
static int gen_transfer(const char *prefix, cp_payload *p, int proto, int d3_fec)
{
    cp_tx_opts opts;
    cp_tx *tx;
    cp_rx_config cfg;
    cp_rx *rx;
    cp_scanner *sc = cp_scanner_new();
    uint8_t *img = (uint8_t *)malloc(W * H), frame[4096];
    char rxdir[1024], name[64];
    cp_tx_info info;
    int frames = 0, code_no = 0, done = 0, ver = proto == CP_PROTO_D3 ? D3_VER : VER;

    memset(&opts, 0, sizeof opts);
    opts.d3_fec = d3_fec;
    tx = cp_tx_new(proto, p, proto == CP_PROTO_D3 ? D3_CAP : QR_CAP, &opts);
    snprintf(rxdir, sizeof rxdir, "%s", g_verify);
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = rxdir;
    cfg.enable_plain = 1;
    cfg.enable_d3 = 1;
    rx = cp_rx_new(&cfg);
    if (!tx || !rx || !sc || !img)
        return -1;
    cp_tx_info_get(tx, &info);

    while (!done && frames < 500) {
        int k = 0, codes = 0, ev;
        memset(img, 200, W * H);
        while (k < 2) {
            size_t n = cp_tx_next(tx, frame, sizeof frame);
            if (!n)
                return -1;
            if (code_no++ % 4 == 3)
                continue;   /* "missed by the camera" */
            if (!draw_v(img, frame, n, 40 + k * 620, 150 - (ver - VER) * 6, ver))
                return -1;
            k++;
        }
        snprintf(name, sizeof name, "%s_%03d.png", prefix, frames);
        if (!write_png(name, img))
            return -1;
        frames++;
        ev = cp_scanner_scan(sc, rx, img, W, H, &codes);
        if (codes != 2 || (ev & CP_EV_FAILED))
            return -1;
        if (ev & CP_EV_COMPLETE)
            done = 1;
    }
    printf("%s: F=%llu T=%u Z=%u K=%u -> %d frames\n", prefix, (unsigned long long)info.F, info.T, info.Z,
           info.total_source_symbols, frames);
    cp_result_free(cp_rx_take_result(rx));
    cp_scanner_free(sc);
    cp_rx_free(rx);
    cp_tx_free(tx);
    free(img);
    return done ? frames : -1;
}

/* separable Gaussian blur (sigma in px) + sinusoidal moire stripes */
static void degrade(const uint8_t *src, uint8_t *dst, double sigma, double amp, double period)
{
    static float tmp[W * H];
    float k[64];
    int r = (int)(sigma * 3 + 0.5), i, x, y;
    double ks = 0;
    if (r > 31)
        r = 31;
    for (i = -r; i <= r; i++)
        ks += (k[i + r] = (float)exp(-(double)(i * i) / (2 * sigma * sigma)));
    for (i = 0; i <= 2 * r; i++)
        k[i] = (float)(k[i] / ks);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            float a = 0;
            for (i = -r; i <= r; i++) {
                int xx = x + i < 0 ? 0 : x + i >= W ? W - 1 : x + i;
                a += k[i + r] * src[y * W + xx];
            }
            tmp[y * W + x] = a;
        }
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            float a = 0;
            double v;
            for (i = -r; i <= r; i++) {
                int yy = y + i < 0 ? 0 : y + i >= H ? H - 1 : y + i;
                a += k[i + r] * tmp[yy * W + x];
            }
            v = a + amp * sin(2 * 3.14159265358979 * (x + 0.35 * y) / period);
            dst[y * W + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5);
        }
}

static int scan_codes(const uint8_t *img, int retry_ms, int *complete)
{
    cp_rx_config cfg;
    cp_rx *rx;
    cp_scanner *sc = cp_scanner_new();
    int codes = 0, ev;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = g_verify;
    cfg.enable_d3 = 1;
    rx = cp_rx_new(&cfg);
    cp_scanner_set_retry_interval(sc, retry_ms);
    ev = cp_scanner_scan(sc, rx, img, W, H, &codes);
    if (complete)
        *complete = (ev & CP_EV_COMPLETE) != 0;
    cp_result_free(cp_rx_take_result(rx));
    cp_scanner_free(sc);
    cp_rx_free(rx);
    return codes;
}

/* One Decimen frame that only the contrast retry can read. 1 = written. */
static int gen_hard(void)
{
    static const double sigmas[] = { 1.0, 1.2, 1.4, 1.6, 1.8, 2.0, 2.2, 2.4, 2.6, 2.8, 3.0, 3.2, 3.4, 3.6 };
    static const double amps[] = { 0, 10, 20, 30, 45, 60 };
    static const double periods[] = { 5.0, 6.5, 8.0, 12.0 };
    const int diag = getenv("GEN_DIAG") != NULL;
    const char *msg = "Decimen RS96 · 对比度重试 · contrast retry rescued this frame";
    cp_payload *p = cp_payload_text(msg, strlen(msg));
    cp_tx *tx = cp_tx_new(CP_PROTO_D3, p, D3_CAP, NULL);
    uint8_t *clean = (uint8_t *)malloc(W * H), *img = (uint8_t *)malloc(W * H), frame[4096];
    size_t n = tx ? cp_tx_next(tx, frame, sizeof frame) : 0;
    size_t si, ai, pi;
    int found = 0, complete = 0;
    memset(clean, 200, W * H);
    if (!n || !draw_v(clean, frame, n, 440, 150 - (D3_VER - VER) * 6, D3_VER))
        return 0;
    for (pi = 0; pi < sizeof periods / sizeof periods[0] && !found; pi++)
        for (si = 0; si < sizeof sigmas / sizeof sigmas[0] && !found; si++)
            for (ai = 0; ai < sizeof amps / sizeof amps[0] && !found; ai++) {
                int plain, retried;
                degrade(clean, img, sigmas[si], amps[ai], periods[pi]);
                plain = scan_codes(img, -1, NULL);
                retried = plain ? plain : scan_codes(img, 0, &complete);
                if (diag)
                    printf("  sigma %.1f amp %2.0f period %4.1f: quirc %d, with retry %d\n", sigmas[si], amps[ai],
                           periods[pi], plain, retried);
                if (plain == 0 && retried == 1 && complete) {
                    found = 1;
                    printf("d3hard: sigma %.1f px, stripes %.0f / period %.1f px\n", sigmas[si], amps[ai],
                           periods[pi]);
                }
            }
    if (found)
        found = write_png("d3hard.png", img) && write_bytes("d3hard.txt", msg, strlen(msg));
    else
        printf("d3hard: no degradation found that only the retry can read (skipped)\n");
    cp_tx_free(tx);
    cp_payload_free(p);
    free(clean);
    free(img);
    return found;
}

int main(int argc, char **argv)
{
    static const char *words[] = { "CodePass", "二维码", "传输", "RaptorQ", "😀", "fountain", "相机",
                                   "symbol", "🚀", "接收端", "quirc", "Ünïcödé", "block", "校验" };
    char *text;
    size_t tl = 0, i;
    uint8_t *bin0, *bin1;
    char list[512];
    cp_payload *p;
    int n_text, n_files, n_d3rs, n_d3lt;

    if (argc < 3) {
        fprintf(stderr, "usage: gen_frames <out-dir> <scratch-dir for the verification receiver>\n");
        return 2;
    }
    g_out = argv[1];
    g_verify = argv[2];

    /* ---- TEXT: mixed CJK / emoji (4-byte UTF-8) / ASCII, weakly compressible */
    text = (char *)malloc(64 * 1024);
    while (tl < 40000) {
        const char *w = words[rnd() % (sizeof words / sizeof words[0])];
        size_t wl = strlen(w);
        memcpy(text + tl, w, wl);
        tl += wl;
        tl += (size_t)sprintf(text + tl, "%u%c", rnd() % 1000, (rnd() % 9) ? ' ' : '\n');
    }
    if (!write_bytes("text.txt", text, tl))
        return 1;
    p = cp_payload_text(text, tl);
    n_text = p ? gen_transfer("text", p, CP_PROTO_RQ, 0) : -1;
    n_d3rs = p ? gen_transfer("d3rs", p, CP_PROTO_D3, CP_D3_FEC_RS96) : -1;
    cp_payload_free(p);

    /* ---- FILES: Unicode names, one incompressible binary */
    {
        static const char *names[2] = { "报告 😀.txt", "data.bin" };
        const uint8_t *datas[2];
        uint64_t sizes[2];
        size_t n0 = 3000, n1 = 20000;
        bin0 = (uint8_t *)malloc(n0);
        bin1 = (uint8_t *)malloc(n1);
        for (i = 0; i < n0; i++)
            bin0[i] = (uint8_t)("hello CodePass\n"[i % 15]);
        for (i = 0; i < n1; i++)
            bin1[i] = (uint8_t)rnd();
        datas[0] = bin0;
        datas[1] = bin1;
        sizes[0] = n0;
        sizes[1] = n1;
        if (!write_bytes("file0.bin", bin0, n0) || !write_bytes("file1.bin", bin1, n1))
            return 1;
        snprintf(list, sizeof list, "%s\n%s\n", names[0], names[1]);
        if (!write_bytes("files.txt", list, strlen(list)))
            return 1;
        p = cp_payload_files(2, names, datas, sizes);
        n_files = p ? gen_transfer("files", p, CP_PROTO_RQ, 0) : -1;
        cp_payload_free(p);
        p = cp_payload_files(1, &names[1], &datas[1], &sizes[1]);
        n_d3lt = p ? gen_transfer("d3lt", p, CP_PROTO_D3, CP_D3_FEC_LT) : -1;
        cp_payload_free(p);
    }

    /* ---- plain QR */
    {
        const char *plain = "https://example.com/扫码?q=😀";
        uint8_t *img = (uint8_t *)malloc(W * H);
        memset(img, 200, W * H);
        if (!draw(img, (const uint8_t *)plain, strlen(plain), 400, 150) || !write_png("plain.png", img) ||
            !write_bytes("plain.txt", plain, strlen(plain)))
            return 1;
        free(img);
    }

    gen_hard();
    if (n_text < 0 || n_files < 0 || n_d3rs < 0 || n_d3lt < 0) {
        fprintf(stderr, "generation / verification failed (text=%d files=%d d3rs=%d d3lt=%d)\n", n_text, n_files,
                n_d3rs, n_d3lt);
        return 1;
    }
    printf("OK\n");
    return 0;
}
