/* frame_render.c - see frame_render.h */
#include "frame_render.h"
#include "qrcodegen.h"
#include <string.h>

void frame_draw_qr(const uint8_t *qrcode, uint8_t *pix, int stride, int x0, int y0)
{
    int size = qrcodegen_getSize(qrcode);
    for (int y = 0; y < size; y++) {
        uint8_t *row = pix + (size_t)(y0 + y) * stride + x0;
        for (int x = 0; x < size; x++)
            row[x] = qrcodegen_getModule(qrcode, x, y) ? 1 : 0;
    }
}

int frame_render(cp_tx *tx, const frame_geom *g, int ecc, int fixed_mask, uint8_t *pix)
{
    uint8_t buf[qrcodegen_BUFFER_LEN_MAX];
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    enum qrcodegen_Mask mask = fixed_mask ? qrcodegen_Mask_0 : qrcodegen_Mask_AUTO;

    memset(pix, 0, (size_t)g->stride * g->h);
    for (int i = 0; i < g->count; i++) {
        size_t n = cp_tx_next(tx, buf, sizeof buf);
        int x, y;
        if (n == 0)
            return 0;
        if (!qrcodegen_encodeBinary(buf, n, qr, (enum qrcodegen_Ecc)qr_ecc_to_qrcodegen(ecc), g->ver, g->ver,
                                    mask, false))
            return 0;
        frame_geom_origin(g, i, &x, &y);
        frame_draw_qr(qr, pix, g->stride, x, y);
    }
    return 1;
}
