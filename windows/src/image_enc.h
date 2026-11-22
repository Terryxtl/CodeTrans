/* image_enc.h - clipboard images -> PNG / JPEG bytes (PLAN.md §6.4).
 *
 * Pixel buffers are 32 bpp, top-down, B G R A byte order (Windows DIB order),
 * rows of w * 4 bytes. All returned buffers are malloc'ed (free()).
 * Safe to call from any thread (GDI memory DCs only). */
#ifndef CPW_IMAGE_ENC_H
#define CPW_IMAGE_ENC_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#define IMG_MAX_PIXELS (16384u * 16384u)

/* Packed DIB (CF_DIB / CF_DIBV5 contents) -> BGRA. *has_alpha is set only for
 * a 32 bpp BITMAPV5HEADER with a non-zero alpha mask whose alpha channel is
 * not uniformly 0 or 255; otherwise alpha is forced to 255. */
uint8_t *dib_to_bgra(const void *dib, size_t len, int *w, int *h, BOOL *has_alpha);

/* PNG / JPEG / GIF / BMP bytes -> BGRA (stb_image). */
uint8_t *image_decode_bgra(const uint8_t *data, size_t len, int *w, int *h, BOOL *has_alpha);

/* PNG: RGB when !alpha, else RGBA. zlib level 9 via miniz. */
uint8_t *image_encode_png(const uint8_t *bgra, int w, int h, BOOL alpha, size_t *out_len);
/* Baseline JPEG, quality 1..100. Transparent pixels are composited on white. */
uint8_t *image_encode_jpeg(const uint8_t *bgra, int w, int h, BOOL alpha, int quality, size_t *out_len);

/* BGRA -> packed CF_DIB (BITMAPINFOHEADER, 32 bpp BI_RGB, bottom-up). */
uint8_t *image_make_dib(const uint8_t *bgra, int w, int h, size_t *out_len);

/* ---- adaptive image format (CFG_IMG_AUTO) ------------------------------- */

/* Distinct colours among up to 4096 pixels sampled on a regular grid
 * (saturates at IMG_AUTO_PHOTO_COLORS * 2). Screenshots, diagrams and text sit
 * far below IMG_AUTO_PHOTO_COLORS; photos and gradients above it. */
#define IMG_AUTO_PHOTO_COLORS 512
int  image_sample_colors(const uint8_t *bgra, int w, int h);
/* TRUE: send the JPEG. Only for photo-like, opaque pictures, and only when the
 * JPEG is at most half the PNG: the lossy format must buy a clearly shorter
 * transfer, and text or line art always stays lossless. */
BOOL image_auto_prefers_jpeg(size_t png_len, size_t jpeg_len, int colors, BOOL alpha);

#endif
