/* frame_render.h - one display frame = N QR codes laid out on a module grid.
 *
 * The frame buffer holds one byte per module (0 = white, 1 = black), rows of
 * g->stride bytes, top-down. The pacer blits it with a two-colour palette and
 * an integer stretch factor (the module size in physical pixels), so the
 * module size can change without re-rendering. */
#ifndef CPW_FRAME_RENDER_H
#define CPW_FRAME_RENDER_H

#include <stdint.h>
#include "cp.h"
#include "qr_geom.h"

/* Pulls g->count packets from tx, encodes each as a QR code of version
 * g->ver and error-correction level ecc (CFG_ECC_*) and draws them into pix.
 * fixed_mask = 1 uses mask 0 instead of the (≈4x slower) automatic choice.
 * Returns 1 on success, 0 on error (tx exhausted / packet too large). */
int frame_render(cp_tx *tx, const frame_geom *g, int ecc, int fixed_mask, uint8_t *pix);

/* Draws one already encoded qrcodegen symbol at module (x0, y0). */
void frame_draw_qr(const uint8_t *qrcode, uint8_t *pix, int stride, int x0, int y0);

#endif
