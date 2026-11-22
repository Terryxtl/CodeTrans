/* tx_render.c - sender helpers shared by the Windows and Android clients:
 * QR capacity table, frame geometry, QR / board rasterisation, mask policy,
 * units per frame / ETA and frame pacing. Declarations: cp.h.
 *
 * The two ECC tables are ISO/IEC 18004 Table 9 in the layout used by Project
 * Nayuki's QR Code generator (MIT), core/third_party/qrcodegen. */
#include "cp.h"
#include "cp_internal.h"
#include "qrcodegen.h"
#include <stdlib.h>
#include <string.h>

/* the public optical board constants must match the codec's */
typedef char cp_assert_bands_units[(CP_OB_BANDS_UNITS == OB_ROWS) ? 1 : -1];
typedef char cp_assert_bands_bytes[(CP_OB_BANDS_UNIT_BYTES == OB_BAND_DATA) ? 1 : -1];
typedef char cp_assert_tiles_units[(CP_OB_TILES_UNITS == OB_SLOTS) ? 1 : -1];
typedef char cp_assert_tiles_bytes[(CP_OB_TILES_UNIT_BYTES == OB_TILE_DATA) ? 1 : -1];
typedef char cp_assert_side[(CP_OB_SIDE == OB_SIDE) ? 1 : -1];

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

/* ---- constants ------------------------------------------------------------ */

int cp_ob_units_per_board(int profile)
{
    return profile == CP_OB_BANDS ? CP_OB_BANDS_UNITS : profile == CP_OB_TILES ? CP_OB_TILES_UNITS : 0;
}

size_t cp_ob_unit_bytes(int profile)
{
    return profile == CP_OB_BANDS ? CP_OB_BANDS_UNIT_BYTES : profile == CP_OB_TILES ? CP_OB_TILES_UNIT_BYTES : 0;
}

size_t cp_ob_board_bytes(int profile)
{
    return (size_t)cp_ob_units_per_board(profile) * cp_ob_unit_bytes(profile);
}

/* ---- QR capacity ---------------------------------------------------------- */

int cp_qr_modules(int ver)
{
    return ver >= 1 && ver <= 40 ? ver * 4 + 17 : 0;
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

size_t cp_qr_capacity(int ver, int ecc)
{
    int data_cw, bits;
    if (ver < 1 || ver > 40 || ecc < 0 || ecc > 3)
        return 0;
    data_cw = raw_data_modules(ver) / 8 - ECC_CODEWORDS_PER_BLOCK[ecc][ver] * NUM_ERROR_CORRECTION_BLOCKS[ecc][ver];
    /* 4-bit mode indicator + character count (8 bits for v1-9, 16 for v10+) */
    bits = data_cw * 8 - 4 - (ver < 10 ? 8 : 16);
    return bits > 0 ? (size_t)(bits / 8) : 0;
}

int cp_code_count_ok(int count)
{
    return count == 1 || count == 2 || count == 3 || count == 4 || count == 6;
}

/* ---- geometry ------------------------------------------------------------- */

int cp_frame_geom_calc(cp_frame_geom *g, int ver, int count, int quiet, int cols, int stride_align)
{
    if (!g || ver < 1 || ver > 40 || quiet < 0 || quiet > 16)
        return 0;
    if (stride_align < 1)
        stride_align = 1;
    if (cols <= 0) {
        switch (count) {
        case 2: g->cols = 2; g->rows = 1; break;
        case 3: g->cols = 3; g->rows = 1; break;
        case 4: g->cols = 2; g->rows = 2; break;
        case 6: g->cols = 3; g->rows = 2; break;
        default: g->cols = 1; g->rows = 1; break;
        }
        g->count = g->cols * g->rows;
    } else {
        if (count < 1 || count > CP_MAX_CODES)
            return 0;
        g->cols = cols < count ? cols : count;
        g->rows = (count + g->cols - 1) / g->cols;
        g->count = count;
    }
    g->ver = ver;
    g->quiet = quiet;
    g->modules = cp_qr_modules(ver);
    g->cell = g->modules + 2 * quiet;
    g->w = g->cols * g->cell;
    g->h = g->rows * g->cell;
    g->stride = (g->w + stride_align - 1) / stride_align * stride_align;
    return 1;
}

void cp_frame_geom_origin(const cp_frame_geom *g, int i, int *x, int *y)
{
    *x = (i % g->cols) * g->cell + g->quiet;
    *y = (i / g->cols) * g->cell + g->quiet;
}

/* ---- rendering ------------------------------------------------------------ */

int cp_fixed_mask(int fps, int count)
{
    return fps * count >= 90;
}

static void draw_qr(const uint8_t *qrcode, uint8_t *pix, int stride, int x0, int y0)
{
    int size = qrcodegen_getSize(qrcode);
    for (int y = 0; y < size; y++) {
        uint8_t *row = pix + (size_t)(y0 + y) * stride + x0;
        for (int x = 0; x < size; x++)
            row[x] = qrcodegen_getModule(qrcode, x, y) ? 1 : 0;
    }
}

/* One refresh of optical boards; same frame buffer convention (1 = black). */
static int render_boards(cp_tx *tx, const cp_frame_geom *g, uint8_t *pix)
{
    const size_t area = (size_t)CP_OB_SIDE * CP_OB_SIDE;
    uint8_t *boards;
    int ok;
    if (g->modules != CP_OB_SIDE || g->count < 1 || g->count > 16)
        return 0;
    if (g->cols == 1 && g->quiet == 0 && g->stride == CP_OB_SIDE)   /* boards stacked: render in place */
        return cp_tx_next_board(tx, g->count, pix);
    boards = (uint8_t *)malloc(area * (size_t)g->count);
    if (!boards)
        return 0;
    ok = cp_tx_next_board(tx, g->count, boards);
    for (int i = 0; ok && i < g->count; i++) {
        int x0, y0;
        cp_frame_geom_origin(g, i, &x0, &y0);
        for (int y = 0; y < CP_OB_SIDE; y++)
            memcpy(pix + (size_t)(y0 + y) * g->stride + x0, boards + area * i + (size_t)y * CP_OB_SIDE, CP_OB_SIDE);
    }
    free(boards);
    return ok;
}

int cp_tx_render_frame(cp_tx *tx, const cp_frame_geom *g, int ecc, int fixed_mask, uint8_t *pix)
{
    uint8_t buf[qrcodegen_BUFFER_LEN_MAX];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    enum qrcodegen_Mask mask = fixed_mask ? qrcodegen_Mask_0 : qrcodegen_Mask_AUTO;
    cp_tx_info info;

    if (!tx || !g || !pix || ecc < 0 || ecc > 3)
        return 0;
    memset(pix, 0, (size_t)g->stride * g->h);
    cp_tx_info_get(tx, &info);
    if (info.proto == CP_PROTO_OB)
        return render_boards(tx, g, pix);
    for (int i = 0; i < g->count; i++) {
        size_t n = cp_tx_next(tx, buf, sizeof buf);
        int x, y;
        if (n == 0)
            return 0;
        if (!qrcodegen_encodeBinary(buf, n, qr, (enum qrcodegen_Ecc)ecc, g->ver, g->ver, mask, false))
            return 0;
        cp_frame_geom_origin(g, i, &x, &y);
        draw_qr(qr, pix, g->stride, x, y);
    }
    return 1;
}

/* ---- units per frame and ETA ---------------------------------------------- */

double cp_tx_units_per_frame(const cp_tx_info *info, int count)
{
    if (!info || count < 1)
        return 0.0;
    if (info->proto == CP_PROTO_OB) {
        int per = cp_ob_units_per_board(info->ob_profile);
        if (!per)
            return 0.0;
        /* one unit slot of a whole refresh carries the manifest every N refreshes */
        return (double)count * per - 1.0 / (info->ob_profile == CP_OB_TILES ? CP_OB_TILES_BEACON_EVERY
                                                                           : CP_OB_BANDS_BEACON_EVERY);
    }
    return (double)count;
}

double cp_tx_eta_seconds(const cp_tx_info *info, int count, double fps)
{
    double units = cp_tx_units_per_frame(info, count);
    double need;
    if (!info || units <= 0.0 || !(fps > 0.0))
        return 0.0;
    need = (double)info->total_source_symbols + 2.0 * info->Z;   /* Z = 0 except RaptorQ */
    return need / (units * fps);
}

/* ---- pacing --------------------------------------------------------------- */

int cp_pace_vblanks(int fps, double refresh_hz)
{
    int n;
    if (fps < 1)
        fps = 1;
    if (!(refresh_hz > 0.0))
        return 1;
    n = (int)(refresh_hz / fps + 0.5);
    return n < 1 ? 1 : n;
}

double cp_pace_effective_fps(int fps, double refresh_hz)
{
    if (!(refresh_hz > 0.0))
        return fps < 1 ? 1.0 : (double)fps;
    return refresh_hz / cp_pace_vblanks(fps, refresh_hz);
}
