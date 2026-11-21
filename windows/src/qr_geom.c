/* qr_geom.c - see qr_geom.h
 *
 * The two tables below are ISO/IEC 18004 Table 9 in the layout used by
 * Project Nayuki's QR Code generator (MIT), core/third_party/qrcodegen. */
#include "qr_geom.h"

static const signed char ECC_CODEWORDS_PER_BLOCK[4][41] = {
    {-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},
};

static const signed char NUM_ERROR_CORRECTION_BLOCKS[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4,  4,  4,  4,  4,  6,  6,  6,  6,  7,  8,  8,  9,  9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5,  5,  8,  9,  9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8,  8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},
};

int qr_modules(int ver)
{
    return ver * 4 + 17;
}

static int raw_data_modules(int ver)
{
    int r = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int na = ver / 7 + 2;
        r -= (25 * na - 10) * na - 55;
        if (ver >= 7)
            r -= 36;
    }
    return r;
}

size_t qr_byte_capacity(int ver, int ecc)
{
    int data_cw, bits;
    if (ver < 1 || ver > 40 || ecc < 0 || ecc > 3)
        return 0;
    data_cw = raw_data_modules(ver) / 8 - ECC_CODEWORDS_PER_BLOCK[ecc][ver] * NUM_ERROR_CORRECTION_BLOCKS[ecc][ver];
    /* 4-bit mode indicator + character count (8 bits for v1-9, 16 for v10+) */
    bits = data_cw * 8 - 4 - (ver < 10 ? 8 : 16);
    return bits > 0 ? (size_t)(bits / 8) : 0;
}

int qr_ecc_to_qrcodegen(int ecc)
{
    /* qrcodegen_Ecc_LOW..HIGH = 0..3, same order as CFG_ECC_L..H */
    return ecc < 0 ? 0 : ecc > 3 ? 3 : ecc;
}

void qr_grid(int count, int *cols, int *rows)
{
    switch (count) {
    case 2: *cols = 2; *rows = 1; break;
    case 3: *cols = 3; *rows = 1; break;
    case 4: *cols = 2; *rows = 2; break;
    case 6: *cols = 3; *rows = 2; break;
    default: *cols = 1; *rows = 1; break;
    }
}

void frame_geom_calc(frame_geom *g, int ver, int count, int quiet)
{
    g->ver = ver;
    g->quiet = quiet;
    qr_grid(count, &g->cols, &g->rows);
    g->count = g->cols * g->rows;
    g->modules = qr_modules(ver);
    g->cell = g->modules + 2 * quiet;
    g->w = g->cols * g->cell;
    g->h = g->rows * g->cell;
    g->stride = (g->w + 3) & ~3;
}

void frame_geom_origin(const frame_geom *g, int i, int *x, int *y)
{
    *x = (i % g->cols) * g->cell + g->quiet;
    *y = (i / g->cols) * g->cell + g->quiet;
}
