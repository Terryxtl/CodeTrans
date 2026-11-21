/* qr_geom.h - QR capacity table and code-area geometry (PLAN.md §5.5, §6.1).
 * Pure computation, no Win32 calls; shared by the app and the tests. */
#ifndef CPW_QR_GEOM_H
#define CPW_QR_GEOM_H

#include <stddef.h>

/* Modules per side: 4 * ver + 17. */
int    qr_modules(int ver);
/* Byte-mode payload capacity of one code. ecc = CFG_ECC_* (L/M/Q/H = 0..3). */
size_t qr_byte_capacity(int ver, int ecc);
/* qrcodegen_Ecc value for CFG_ECC_* (they differ only in name today). */
int    qr_ecc_to_qrcodegen(int ecc);

/* 1 -> 1x1, 2 -> 2x1, 3 -> 3x1, 4 -> 2x2, 6 -> 3x2 */
void   qr_grid(int count, int *cols, int *rows);

/* Frame bitmap geometry in modules (one byte per module in the frame buffer;
 * the pacer scales it by the module size at blit time). */
typedef struct frame_geom {
    int ver, count, quiet;
    int cols, rows;
    int modules;        /* modules per code side                      */
    int cell;           /* modules + 2 * quiet                        */
    int w, h;           /* whole frame, in modules                    */
    int stride;         /* bytes per row, DWORD aligned               */
} frame_geom;

void   frame_geom_calc(frame_geom *g, int ver, int count, int quiet);
/* Top-left module of code i inside the frame. */
void   frame_geom_origin(const frame_geom *g, int i, int *x, int *y);

#endif
