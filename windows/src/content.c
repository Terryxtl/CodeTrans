/* content.c - see content.h */
#include "content.h"
#include "config.h"
#include "image_enc.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

/* ---- hash: 64-bit multiply-xorshift over 8-byte words ------------------- */

uint64_t cpw_hash64(uint64_t seed, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint64_t m = 0x9E3779B97F4A7C15ull;
    uint64_t h = seed ^ (n * m);
    while (n >= 8) {
        uint64_t v;
        memcpy(&v, p, 8);
        h = (h ^ (v * m)) * 0xBF58476D1CE4E5B9ull;
        h ^= h >> 31;
        p += 8;
        n -= 8;
    }
    if (n) {
        uint64_t v = 0;
        memcpy(&v, p, n);
        h = (h ^ (v * m)) * 0xBF58476D1CE4E5B9ull;
    }
    h ^= h >> 29;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 32;
    return h;
}

/* ---- construction ------------------------------------------------------ */

content *content_new_text(const char *utf8, size_t n)
{
    content *c = (content *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->type = CONTENT_TEXT;
    c->data = (uint8_t *)malloc(n ? n : 1);
    if (!c->data) {
        free(c);
        return NULL;
    }
    memcpy(c->data, utf8, n);
    c->len = n;
    c->raw_size = n;
    c->hash = cpw_hash64(CONTENT_TEXT, c->data, n);
    return c;
}

content *content_new_image(int type, uint8_t *owned, size_t n)
{
    content *c = (content *)calloc(1, sizeof *c);
    if (!c) {
        free(owned);
        return NULL;
    }
    c->type = type;
    c->data = owned;
    c->len = n;
    /* a DIB is re-encoded; PNG is usually 3-10x smaller than raw pixels */
    c->raw_size = type == CONTENT_IMAGE_DIB ? n / 4 : n;
    /* PNG and DIB of the same picture hash differently; that is fine */
    c->hash = cpw_hash64((uint64_t)type, owned, n);
    return c;
}

content *content_new_files(size_t n, const wchar_t *const *paths, const wchar_t **bad)
{
    content *c;
    uint64_t h = CONTENT_FILES;
    if (bad)
        *bad = NULL;
    if (n == 0)
        return NULL;
    c = (content *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->type = CONTENT_FILES;
    c->paths = (wchar_t **)calloc(n, sizeof *c->paths);
    c->sizes = (uint64_t *)calloc(n, sizeof *c->sizes);
    if (!c->paths || !c->sizes)
        goto fail;
    for (size_t i = 0; i < n; i++) {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(paths[i], GetFileExInfoStandard, &fa) ||
            (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (bad)
                *bad = paths[i];
            goto fail;
        }
        c->paths[i] = _wcsdup(paths[i]);
        if (!c->paths[i])
            goto fail;
        c->nfiles++;
        c->sizes[i] = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        c->raw_size += c->sizes[i];
        h = cpw_hash64(h, paths[i], wcslen(paths[i]) * sizeof(wchar_t));
        h = cpw_hash64(h, &c->sizes[i], sizeof c->sizes[i]);
        h = cpw_hash64(h, &fa.ftLastWriteTime, sizeof fa.ftLastWriteTime);
    }
    c->hash = h;
    return c;
fail:
    content_free(c);
    return NULL;
}

void content_free(content *c)
{
    if (!c)
        return;
    free(c->data);
    for (size_t i = 0; i < c->nfiles; i++)
        free(c->paths[i]);
    free(c->paths);
    free(c->sizes);
    free(c);
}

int content_same(const content *a, const content *b)
{
    if (!a || !b)
        return 0;
    return a->type == b->type && a->len == b->len && a->nfiles == b->nfiles && a->hash == b->hash;
}

int content_kind(const content *c)
{
    if (!c)
        return CP_KIND_NONE;
    switch (c->type) {
    case CONTENT_TEXT: return CP_KIND_TEXT;
    case CONTENT_FILES: return CP_KIND_FILES;
    default: return CP_KIND_IMAGE;
    }
}

/* ---- build (producer thread) ------------------------------------------ */

static int build_image(const content *c, const build_opts *o, build_result *r)
{
    uint8_t *bgra = NULL, *enc = NULL;
    size_t enc_len = 0;
    int w = 0, h = 0;
    BOOL alpha = FALSE;

    if (c->type == CONTENT_IMAGE_PNG && o->image_format == CFG_IMG_PNG) {
        /* lossless, keeps transparency: use the bytes as they are */
        r->payload = cp_payload_image(c->data, c->len, "image/png");
        lstrcpyA(r->mime, "image/png");
        if (!r->payload)
            wfmt(r->err, CPW_COUNTOF(r->err), L"图片打包失败");
        return r->payload != NULL;
    }
    if (c->type == CONTENT_IMAGE_PNG)
        bgra = image_decode_bgra(c->data, c->len, &w, &h, &alpha);
    else
        bgra = dib_to_bgra(c->data, c->len, &w, &h, &alpha);
    if (!bgra) {
        wfmt(r->err, CPW_COUNTOF(r->err), L"无法读取剪切板图片");
        return 0;
    }
    r->img_w = w;
    r->img_h = h;
    if (o->image_format == CFG_IMG_AUTO) {
        /* lossless unless the picture is a photo and JPEG halves the transfer */
        int colors = image_sample_colors(bgra, w, h);
        uint8_t *jpg = NULL;
        size_t jpg_len = 0;
        const uint8_t *png = NULL;
        size_t png_len = 0;
        if (c->type == CONTENT_IMAGE_PNG) {   /* the PNG that was copied: no re-encode */
            png = c->data;
            png_len = c->len;
        } else {
            enc = image_encode_png(bgra, w, h, alpha, &enc_len);
            png = enc;
            png_len = enc_len;
        }
        if (!alpha && colors >= IMG_AUTO_PHOTO_COLORS)
            jpg = image_encode_jpeg(bgra, w, h, FALSE, o->jpeg_quality, &jpg_len);
        free(bgra);
        if (jpg && image_auto_prefers_jpeg(png ? png_len : (size_t)-1, jpg_len, colors, alpha)) {
            free(enc);
            enc = jpg;
            enc_len = jpg_len;
            lstrcpyA(r->mime, "image/jpeg");
        } else if (png) {
            free(jpg);
            if (!enc) {                        /* the original PNG bytes */
                r->payload = cp_payload_image(c->data, c->len, "image/png");
                lstrcpyA(r->mime, "image/png");
                if (!r->payload)
                    wfmt(r->err, CPW_COUNTOF(r->err), L"图片打包失败");
                return r->payload != NULL;
            }
            lstrcpyA(r->mime, "image/png");
        } else {
            free(jpg);
        }
    } else if (o->image_format == CFG_IMG_JPEG) {
        enc = image_encode_jpeg(bgra, w, h, alpha, o->jpeg_quality, &enc_len);
        lstrcpyA(r->mime, "image/jpeg");
        free(bgra);
    } else {
        enc = image_encode_png(bgra, w, h, alpha, &enc_len);
        lstrcpyA(r->mime, "image/png");
        free(bgra);
    }
    if (!enc) {
        wfmt(r->err, CPW_COUNTOF(r->err), L"图片编码失败(内存不足?)");
        return 0;
    }
    if (enc_len > o->max_bytes) {
        free(enc);
        wfmt(r->err, CPW_COUNTOF(r->err), L"图片编码后超过大小上限");
        return 0;
    }
    r->payload = cp_payload_image(enc, enc_len, r->mime);
    free(enc);
    if (!r->payload)
        wfmt(r->err, CPW_COUNTOF(r->err), L"图片打包失败");
    return r->payload != NULL;
}

static uint8_t *read_whole_file(const wchar_t *path, uint64_t max, uint64_t *size, wchar_t *err, size_t cap)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    LARGE_INTEGER sz;
    uint8_t *buf;
    uint64_t done = 0;
    *size = 0;
    if (f == INVALID_HANDLE_VALUE) {
        wfmt(err, cap, L"无法打开 %ls", path_basename(path));
        return NULL;
    }
    if (!GetFileSizeEx(f, &sz) || (uint64_t)sz.QuadPart > max) {
        CloseHandle(f);
        wfmt(err, cap, L"%ls 超过大小上限", path_basename(path));
        return NULL;
    }
    buf = (uint8_t *)malloc(sz.QuadPart ? (size_t)sz.QuadPart : 1);
    if (!buf) {
        CloseHandle(f);
        wfmt(err, cap, L"内存不足");
        return NULL;
    }
    while (done < (uint64_t)sz.QuadPart) {
        DWORD want = (DWORD)CPW_MIN((uint64_t)sz.QuadPart - done, (uint64_t)(16u << 20)), got = 0;
        if (!ReadFile(f, buf + done, want, &got, NULL) || got == 0)
            break;
        done += got;
    }
    CloseHandle(f);
    if (done != (uint64_t)sz.QuadPart) {
        free(buf);
        wfmt(err, cap, L"读取 %ls 失败", path_basename(path));
        return NULL;
    }
    *size = done;
    return buf;
}

static int build_files(const content *c, const build_opts *o, build_result *r)
{
    size_t n = c->nfiles;
    char **names = (char **)calloc(n, sizeof *names);
    uint8_t **datas = (uint8_t **)calloc(n, sizeof *datas);
    uint64_t *sizes = (uint64_t *)calloc(n, sizeof *sizes);
    uint64_t total = 0;
    int ok = 0;
    if (!names || !datas || !sizes) {
        wfmt(r->err, CPW_COUNTOF(r->err), L"内存不足");
        goto done;
    }
    for (size_t i = 0; i < n; i++) {
        const wchar_t *bn = path_basename(c->paths[i]);
        names[i] = utf16_to_utf8(bn, wcslen(bn), NULL);
        if (!names[i]) {
            wfmt(r->err, CPW_COUNTOF(r->err), L"文件名无效");
            goto done;
        }
        datas[i] = read_whole_file(c->paths[i], o->max_bytes - total, &sizes[i], r->err, CPW_COUNTOF(r->err));
        if (!datas[i])
            goto done;
        total += sizes[i];
    }
    r->payload = cp_payload_files(n, (const char *const *)names, (const uint8_t *const *)datas, sizes);
    if (!r->payload)
        wfmt(r->err, CPW_COUNTOF(r->err), L"文件打包失败(超过上限或内存不足)");
    ok = r->payload != NULL;
done:
    for (size_t i = 0; names && datas && i < n; i++) {
        free(names[i]);
        free(datas[i]);
    }
    free(names);
    free(datas);
    free(sizes);
    return ok;
}

int content_build(const content *c, const build_opts *o, build_result *r)
{
    memset(r, 0, sizeof *r);
    if (!c)
        return 0;
    if (c->raw_size > o->max_bytes && c->type != CONTENT_IMAGE_DIB) {
        wchar_t a[32], b[32];
        fmt_size(c->raw_size, a, CPW_COUNTOF(a));
        fmt_size(o->max_bytes, b, CPW_COUNTOF(b));
        wfmt(r->err, CPW_COUNTOF(r->err), L"内容 %ls 超过 %ls 上限", a, b);
        return 0;
    }
    switch (c->type) {
    case CONTENT_TEXT:
        r->payload = cp_payload_text((const char *)c->data, c->len);
        lstrcpyA(r->mime, "text/plain");
        if (!r->payload)
            wfmt(r->err, CPW_COUNTOF(r->err), L"文本打包失败");
        return r->payload != NULL;
    case CONTENT_IMAGE_PNG:
    case CONTENT_IMAGE_DIB:
        return build_image(c, o, r);
    case CONTENT_FILES:
        return build_files(c, o, r);
    }
    return 0;
}
