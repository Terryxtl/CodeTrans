/* image_enc.c - see image_enc.h */
#include "image_enc.h"
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

/* ---- stb_image_write with miniz deflate (level 9) ---------------------- */

static unsigned char *cpw_zlib_compress(unsigned char *data, int data_len, int *out_len, int quality)
{
    mz_ulong cap = mz_compressBound((mz_ulong)data_len);
    unsigned char *out = (unsigned char *)malloc(cap ? cap : 1);
    (void)quality;
    *out_len = 0;
    if (!out)
        return NULL;
    if (mz_compress2(out, &cap, data, (mz_ulong)data_len, MZ_BEST_COMPRESSION) != MZ_OK) {
        free(out);
        return NULL;
    }
    *out_len = (int)cap;
    return out;
}

#define STBIW_ZLIB_COMPRESS cpw_zlib_compress
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image_write.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#define STBI_ONLY_BMP
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* ---- growable output buffer for the stb *_to_func writers ------------- */

typedef struct membuf {
    uint8_t *p;
    size_t n, cap;
    int failed;
} membuf;

static void membuf_write(void *ctx, void *data, int size)
{
    membuf *m = (membuf *)ctx;
    if (m->failed || size <= 0)
        return;
    if (m->n + (size_t)size > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 65536;
        while (nc < m->n + (size_t)size)
            nc *= 2;
        uint8_t *np = (uint8_t *)realloc(m->p, nc);
        if (!np) {
            m->failed = 1;
            return;
        }
        m->p = np;
        m->cap = nc;
    }
    memcpy(m->p + m->n, data, (size_t)size);
    m->n += (size_t)size;
}

static uint8_t *membuf_finish(membuf *m, int ok, size_t *out_len)
{
    if (!ok || m->failed || m->n == 0) {
        free(m->p);
        *out_len = 0;
        return NULL;
    }
    *out_len = m->n;
    return m->p;
}

/* ---- DIB -> BGRA ------------------------------------------------------- */

static int popcount32(DWORD v)
{
    int c = 0;
    for (; v; v &= v - 1)
        c++;
    return c;
}

static int ctz32(DWORD v)
{
    int c = 0;
    if (!v)
        return 0;
    while (!(v & 1)) {
        v >>= 1;
        c++;
    }
    return c;
}

/* channel value scaled to 8 bits */
static uint8_t take(DWORD px, DWORD mask)
{
    int bits, shift;
    DWORD v;
    if (!mask)
        return 0;
    shift = ctz32(mask);
    bits = popcount32(mask);
    v = (px & mask) >> shift;
    if (bits >= 8)
        return (uint8_t)(v >> (bits - 8));
    return (uint8_t)((v * 255u) / ((1u << bits) - 1u));
}

uint8_t *dib_to_bgra(const void *dib, size_t len, int *w, int *h, BOOL *has_alpha)
{
    const BITMAPINFOHEADER *bi = (const BITMAPINFOHEADER *)dib;
    size_t hdr, colors = 0, masks = 0, bits_off, row, need;
    int width, height, top_down;
    uint8_t *out = NULL;

    *w = *h = 0;
    *has_alpha = FALSE;
    if (!dib || len < sizeof(BITMAPINFOHEADER) || bi->biSize < sizeof(BITMAPINFOHEADER) || bi->biSize > len)
        return NULL;
    hdr = bi->biSize;
    width = bi->biWidth;
    height = bi->biHeight < 0 ? -bi->biHeight : bi->biHeight;
    top_down = bi->biHeight < 0;
    if (width <= 0 || height <= 0 || (uint64_t)width * (uint64_t)height > IMG_MAX_PIXELS)
        return NULL;
    if (bi->biCompression == BI_BITFIELDS && hdr == sizeof(BITMAPINFOHEADER))
        masks = 12;
    if (bi->biBitCount <= 8)
        colors = bi->biClrUsed ? bi->biClrUsed : (1u << bi->biBitCount);
    else
        colors = bi->biClrUsed;
    bits_off = hdr + masks + colors * 4;
    if (bits_off > len)
        return NULL;
    if (bi->biCompression == BI_RGB || bi->biCompression == BI_BITFIELDS) {
        row = (((size_t)width * bi->biBitCount + 31) / 32) * 4;
        need = row * (size_t)height;
        if (bits_off + need > len) {
            /* some writers omit the colour table count; try without it */
            if (colors && hdr + masks + need <= len && bi->biBitCount > 8)
                bits_off = hdr + masks;
            else
                return NULL;
        }
    }

    out = (uint8_t *)malloc((size_t)width * height * 4);
    if (!out)
        return NULL;

    /* 32 bpp with an alpha mask (BITMAPV5HEADER): read pixels directly. */
    if (bi->biBitCount == 32 && hdr >= sizeof(BITMAPV5HEADER) &&
        (bi->biCompression == BI_BITFIELDS || bi->biCompression == BI_RGB) &&
        ((const BITMAPV5HEADER *)dib)->bV5AlphaMask != 0) {
        const BITMAPV5HEADER *v5 = (const BITMAPV5HEADER *)dib;
        DWORD rm = v5->bV5RedMask, gm = v5->bV5GreenMask, bm = v5->bV5BlueMask, am = v5->bV5AlphaMask;
        const uint8_t *src = (const uint8_t *)dib + bits_off;
        int any_partial = 0, all_zero = 1;
        if (bi->biCompression == BI_RGB) {
            rm = 0x00FF0000;
            gm = 0x0000FF00;
            bm = 0x000000FF;
        }
        for (int y = 0; y < height; y++) {
            const uint8_t *s = src + (size_t)(top_down ? y : height - 1 - y) * width * 4;
            uint8_t *d = out + (size_t)y * width * 4;
            for (int x = 0; x < width; x++, s += 4, d += 4) {
                DWORD px = (DWORD)s[0] | ((DWORD)s[1] << 8) | ((DWORD)s[2] << 16) | ((DWORD)s[3] << 24);
                uint8_t a = take(px, am);
                d[0] = take(px, bm);
                d[1] = take(px, gm);
                d[2] = take(px, rm);
                d[3] = a;
                if (a != 0)
                    all_zero = 0;
                if (a != 255)
                    any_partial = 1;
            }
        }
        if (all_zero || !any_partial) {
            /* alpha unused (many apps write 0): treat as opaque */
            for (size_t i = 3; i < (size_t)width * height * 4; i += 4)
                out[i] = 255;
        } else {
            *has_alpha = TRUE;
        }
        *w = width;
        *h = height;
        return out;
    }

    /* Anything else: let GDI convert (palettes, 16/24 bpp, RLE, JPEG/PNG-in-DIB excluded). */
    {
        BITMAPINFO dst;
        void *bits = NULL;
        HDC dc = CreateCompatibleDC(NULL);
        HBITMAP bmp, old;
        int ok = 0;
        memset(&dst, 0, sizeof dst);
        dst.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        dst.bmiHeader.biWidth = width;
        dst.bmiHeader.biHeight = -height;
        dst.bmiHeader.biPlanes = 1;
        dst.bmiHeader.biBitCount = 32;
        dst.bmiHeader.biCompression = BI_RGB;
        bmp = dc ? CreateDIBSection(dc, &dst, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
        if (bmp && bits) {
            old = (HBITMAP)SelectObject(dc, bmp);
            ok = SetDIBitsToDevice(dc, 0, 0, (DWORD)width, (DWORD)height, 0, 0, 0, (UINT)height,
                                   (const uint8_t *)dib + bits_off, (const BITMAPINFO *)dib,
                                   DIB_RGB_COLORS) > 0;
            GdiFlush();
            if (ok) {
                memcpy(out, bits, (size_t)width * height * 4);
                for (size_t i = 3; i < (size_t)width * height * 4; i += 4)
                    out[i] = 255;
            }
            SelectObject(dc, old);
        }
        if (bmp)
            DeleteObject(bmp);
        if (dc)
            DeleteDC(dc);
        if (!ok) {
            free(out);
            return NULL;
        }
    }
    *w = width;
    *h = height;
    return out;
}

uint8_t *image_decode_bgra(const uint8_t *data, size_t len, int *w, int *h, BOOL *has_alpha)
{
    int x, y, comp;
    uint8_t *rgba;
    *w = *h = 0;
    *has_alpha = FALSE;
    if (!data || len == 0 || len > 0x7FFFFFFF)
        return NULL;
    rgba = stbi_load_from_memory(data, (int)len, &x, &y, &comp, 4);
    if (!rgba)
        return NULL;
    for (size_t i = 0; i < (size_t)x * y * 4; i += 4) {
        uint8_t t = rgba[i];
        rgba[i] = rgba[i + 2];
        rgba[i + 2] = t;
        if (rgba[i + 3] != 255)
            *has_alpha = TRUE;
    }
    *w = x;
    *h = y;
    return rgba;   /* stb uses malloc/free */
}

/* BGRA -> RGB or RGBA (straight alpha); composite on white when flatten */
static uint8_t *to_rgb(const uint8_t *bgra, int w, int h, int channels, int flatten)
{
    size_t n = (size_t)w * h;
    uint8_t *o = (uint8_t *)malloc(n * channels);
    if (!o)
        return NULL;
    for (size_t i = 0; i < n; i++) {
        const uint8_t *s = bgra + i * 4;
        uint8_t *d = o + i * channels;
        if (flatten && s[3] != 255) {
            unsigned a = s[3];
            d[0] = (uint8_t)((s[2] * a + 255 * (255 - a)) / 255);
            d[1] = (uint8_t)((s[1] * a + 255 * (255 - a)) / 255);
            d[2] = (uint8_t)((s[0] * a + 255 * (255 - a)) / 255);
        } else {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
        }
        if (channels == 4)
            d[3] = s[3];
    }
    return o;
}

uint8_t *image_encode_png(const uint8_t *bgra, int w, int h, BOOL alpha, size_t *out_len)
{
    int ch = alpha ? 4 : 3;
    uint8_t *rgb = to_rgb(bgra, w, h, ch, 0);
    membuf m = { 0 };
    int ok;
    *out_len = 0;
    if (!rgb)
        return NULL;
    ok = stbi_write_png_to_func(membuf_write, &m, w, h, ch, rgb, w * ch);
    free(rgb);
    return membuf_finish(&m, ok, out_len);
}

uint8_t *image_encode_jpeg(const uint8_t *bgra, int w, int h, BOOL alpha, int quality, size_t *out_len)
{
    uint8_t *rgb = to_rgb(bgra, w, h, 3, alpha);
    membuf m = { 0 };
    int ok;
    *out_len = 0;
    if (!rgb)
        return NULL;
    ok = stbi_write_jpg_to_func(membuf_write, &m, w, h, 3, rgb, quality);
    free(rgb);
    return membuf_finish(&m, ok, out_len);
}

uint8_t *image_make_dib(const uint8_t *bgra, int w, int h, size_t *out_len)
{
    size_t row = (size_t)w * 4, len = sizeof(BITMAPINFOHEADER) + row * h;
    uint8_t *p = (uint8_t *)malloc(len);
    BITMAPINFOHEADER *bi = (BITMAPINFOHEADER *)p;
    *out_len = 0;
    if (!p)
        return NULL;
    memset(bi, 0, sizeof *bi);
    bi->biSize = sizeof *bi;
    bi->biWidth = w;
    bi->biHeight = h;                 /* bottom-up: the most compatible form */
    bi->biPlanes = 1;
    bi->biBitCount = 32;
    bi->biCompression = BI_RGB;
    bi->biSizeImage = (DWORD)(row * h);
    for (int y = 0; y < h; y++)
        memcpy(p + sizeof *bi + (size_t)(h - 1 - y) * row, bgra + (size_t)y * row, row);
    *out_len = len;
    return p;
}
