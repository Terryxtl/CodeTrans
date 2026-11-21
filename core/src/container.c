/* CPO1 payload container: build (sender) and validate / decode (receiver).
 * See core/PROTOCOL.md §3. */
#include "cp_internal.h"

#include "miniz.h"

struct cp_payload {
    uint8_t *bytes;
    size_t   n;
    int      kind;
    uint64_t raw_len;
    int      compressed;
};

#define CPO_SAMPLE_BYTES (64u * 1024u)
#define CPO_LEVEL        6

/* ---- helpers ------------------------------------------------------------ */

size_t cp_utf8_cut(const uint8_t *s, size_t n)
{
    size_t i = n, back = 0, need;
    uint8_t lead;
    /* walk back over trailing continuation bytes (at most 3) to the lead byte */
    while (i > 0 && back < 3 && (s[i - 1] & 0xC0u) == 0x80u) {
        i--;
        back++;
    }
    if (i == 0)
        return n;                          /* only stray continuation bytes */
    lead = s[i - 1];
    if (lead < 0x80u)
        return n;
    if ((lead & 0xE0u) == 0xC0u)
        need = 1;
    else if ((lead & 0xF0u) == 0xE0u)
        need = 2;
    else if ((lead & 0xF8u) == 0xF0u)
        need = 3;
    else
        return n;                          /* invalid lead: nothing to protect */
    return back >= need ? n : i - 1;       /* incomplete sequence: cut before lead */
}

int cp_is_compressed_magic(const uint8_t *d, size_t n)
{
    if (!d || n < 4)
        return 0;
    if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return 1;        /* PNG   */
    if (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return 1;                     /* JPEG  */
    if (d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8') return 1;         /* GIF   */
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return 1;    /* WEBP  */
    if (d[0] == 'P' && d[1] == 'K' && (d[2] == 3 || d[2] == 5 || d[2] == 7)) return 1; /* zip/docx/apk */
    if (n >= 6 && !memcmp(d, "7z\xBC\xAF\x27\x1C", 6)) return 1;                    /* 7z    */
    if (d[0] == 0x1F && d[1] == 0x8B) return 1;                                     /* gzip  */
    if (d[0] == 'B' && d[1] == 'Z' && d[2] == 'h') return 1;                        /* bzip2 */
    if (n >= 6 && !memcmp(d, "\xFD" "7zXZ\x00", 6)) return 1;                       /* xz    */
    if (d[0] == 0x28 && d[1] == 0xB5 && d[2] == 0x2F && d[3] == 0xFD) return 1;     /* zstd  */
    if (!memcmp(d, "Rar!", 4)) return 1;                                            /* rar   */
    if (n >= 8 && !memcmp(d + 4, "ftyp", 4)) return 1;                              /* mp4/mov/heic/avif */
    if (d[0] == 0x1A && d[1] == 0x45 && d[2] == 0xDF && d[3] == 0xA3) return 1;     /* mkv/webm */
    if (!memcmp(d, "OggS", 4) || !memcmp(d, "fLaC", 4)) return 1;                   /* ogg/flac */
    if (d[0] == 'I' && d[1] == 'D' && d[2] == '3') return 1;                        /* mp3   */
    if (d[0] == 0x04 && d[1] == 0x22 && d[2] == 0x4D && d[3] == 0x18) return 1;     /* lz4   */
    return 0;
}

static int ascii_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int compressed_ext(const char *name)
{
    static const char *const exts[] = {
        "png", "jpg", "jpeg", "gif", "webp", "heic", "heif", "avif", "mp4", "m4v", "mov", "mkv",
        "webm", "avi", "mp3", "m4a", "aac", "ogg", "opus", "flac", "zip", "7z", "rar", "gz", "tgz",
        "bz2", "xz", "zst", "lz4", "apk", "jar", "docx", "xlsx", "pptx", "odt", "epub", NULL };
    const char *dot = name ? strrchr(name, '.') : NULL;
    int i;
    if (!dot || !dot[1])
        return 0;
    for (i = 0; exts[i]; i++)
        if (ascii_ieq(dot + 1, exts[i]))
            return 1;
    return 0;
}

int cp_is_compressed_name(const char *name) { return compressed_ext(name); }

static int compressed_mime(const char *mime)
{
    static const char *const m[] = { "image/png", "image/jpeg", "image/gif", "image/webp", "image/heic",
                                     "image/heif", "image/avif", "application/zip", NULL };
    int i;
    if (!mime)
        return 0;
    if (!strncmp(mime, "video/", 6) || !strncmp(mime, "audio/", 6))
        return 1;
    for (i = 0; m[i]; i++)
        if (ascii_ieq(mime, m[i]))
            return 1;
    return 0;
}

/* raw deflate level 6; NULL if allocation fails */
static uint8_t *deflate_raw(const uint8_t *src, size_t n, size_t *out_n)
{
    int flags = (int)tdefl_create_comp_flags_from_zip_params(CPO_LEVEL, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY);
    *out_n = 0;
    return (uint8_t *)tdefl_compress_mem_to_heap(src, n, out_n, flags);
}

static int ratio_ok(size_t out, size_t in) { return (uint64_t)out * 100u <= (uint64_t)in * 95u; }

/* Assembles header + meta + body (compressing if worthwhile). */
static cp_payload *build(int kind, const char *meta, const uint8_t *body, size_t body_len, int try_compress)
{
    cp_payload *p;
    size_t meta_len = strlen(meta);
    uint8_t *comp = NULL;
    size_t comp_len = 0;
    const uint8_t *use = body;
    size_t use_len = body_len;
    int compressed = 0;

    if (meta_len > 0xFFFFu || (uint64_t)body_len > CP_MAX_TOTAL_BYTES)
        return NULL;
    if (try_compress && body_len >= 64) {
        int go = 1;
        if (body_len > CPO_SAMPLE_BYTES) {
            size_t sl = 0;
            uint8_t *s = deflate_raw(body, CPO_SAMPLE_BYTES, &sl);
            if (!s || !ratio_ok(sl, CPO_SAMPLE_BYTES))
                go = 0;
            mz_free(s);
        }
        if (go) {
            comp = deflate_raw(body, body_len, &comp_len);
            if (comp && ratio_ok(comp_len, body_len)) {
                use = comp;
                use_len = comp_len;
                compressed = 1;
            }
        }
    }
    if ((uint64_t)CPO_HDR_LEN + meta_len + use_len > CP_MAX_TOTAL_BYTES) {
        mz_free(comp);
        return NULL;
    }
    p = (cp_payload *)calloc(1, sizeof *p);
    if (!p) {
        mz_free(comp);
        return NULL;
    }
    p->n = CPO_HDR_LEN + meta_len + use_len;
    p->bytes = (uint8_t *)malloc(p->n);
    if (!p->bytes) {
        mz_free(comp);
        free(p);
        return NULL;
    }
    memcpy(p->bytes, "CPO1", 4);
    p->bytes[4] = (uint8_t)kind;
    p->bytes[5] = compressed ? CPO_FLAG_DEFLATE : 0;
    cp_be16w(p->bytes + 6, (uint16_t)meta_len);
    cp_be64w(p->bytes + 8, (uint64_t)body_len);
    cp_sha256(body, body_len, p->bytes + 16);
    memcpy(p->bytes + CPO_HDR_LEN, meta, meta_len);
    if (use_len)
        memcpy(p->bytes + CPO_HDR_LEN + meta_len, use, use_len);
    mz_free(comp);
    p->kind = kind;
    p->raw_len = body_len;
    p->compressed = compressed;
    return p;
}

/* ---- public sender API ------------------------------------------------- */

cp_payload *cp_payload_text(const char *utf8, size_t n)
{
    const uint8_t *s = (const uint8_t *)utf8;
    if (!s && n)
        return NULL;
    if (n >= 3 && s[0] == 0xEF && s[1] == 0xBB && s[2] == 0xBF) {   /* no BOM on the wire */
        s += 3;
        n -= 3;
    }
    return build(CP_KIND_TEXT, "mime=text/plain\n", s ? s : (const uint8_t *)"", n, 1);
}

static const char *sniff_image_mime(const uint8_t *d, size_t n)
{
    if (n >= 4 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "image/png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
    if (n >= 4 && !memcmp(d, "GIF8", 4)) return "image/gif";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return "image/webp";
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return "image/bmp";
    return "application/octet-stream";
}

static int mime_ok(const char *m)
{
    size_t i, len = strlen(m);
    if (len == 0 || len > 127)
        return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)m[i];
        if (c <= 0x20 || c >= 0x7F)
            return 0;
    }
    return 1;
}

cp_payload *cp_payload_image(const uint8_t *data, size_t n, const char *mime)
{
    char meta[160];
    int skip;
    if (!data || n == 0)
        return NULL;
    if (!mime || !*mime)
        mime = sniff_image_mime(data, n);
    if (!mime_ok(mime))
        return NULL;
    snprintf(meta, sizeof meta, "mime=%s\n", mime);
    skip = compressed_mime(mime) || cp_is_compressed_magic(data, n);
    return build(CP_KIND_IMAGE, meta, data, n, !skip);
}

static const char *base_name(const char *s)
{
    const char *b = s;
    for (; *s; s++)
        if (*s == '/' || *s == '\\')
            b = s + 1;
    return b;
}

cp_payload *cp_payload_files(size_t count, const char *const *names, const uint8_t *const *datas,
                             const uint64_t *sizes)
{
    uint64_t total = 0;
    size_t i, off = 0;
    uint8_t *body;
    int all_compressed = 1;
    char meta[64];
    cp_payload *p;

    if (count == 0 || count > CPO_MAX_FILES || !names || !sizes || !datas)
        return NULL;
    for (i = 0; i < count; i++) {
        size_t nl;
        if (!names[i] || (sizes[i] && !datas[i]))
            return NULL;
        nl = strlen(base_name(names[i]));
        if (nl > 0xFFFFu)
            return NULL;
        total += 2u + nl + 8u + sizes[i];
        if (sizes[i] > CP_MAX_TOTAL_BYTES || total > CP_MAX_TOTAL_BYTES)
            return NULL;
        if (!(compressed_ext(names[i]) || cp_is_compressed_magic(datas[i], (size_t)sizes[i])))
            all_compressed = 0;
    }
    body = (uint8_t *)malloc(total ? (size_t)total : 1);
    if (!body)
        return NULL;
    for (i = 0; i < count; i++) {
        const char *bn = base_name(names[i]);
        size_t nl = strlen(bn);
        cp_be16w(body + off, (uint16_t)nl);
        off += 2;
        memcpy(body + off, bn, nl);
        off += nl;
        cp_be64w(body + off, sizes[i]);
        off += 8;
        if (sizes[i])
            memcpy(body + off, datas[i], (size_t)sizes[i]);
        off += (size_t)sizes[i];
    }
    snprintf(meta, sizeof meta, "count=%u\n", (unsigned)count);
    p = build(CP_KIND_FILES, meta, body, off, !all_compressed);
    free(body);
    return p;
}

const uint8_t *cp_payload_bytes(const cp_payload *p, size_t *n)
{
    if (!p) {
        if (n)
            *n = 0;
        return NULL;
    }
    if (n)
        *n = p->n;
    return p->bytes;
}

void cp_payload_info_get(const cp_payload *p, cp_payload_info *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!p)
        return;
    out->kind = p->kind;
    out->raw_len = p->raw_len;
    out->container_len = p->n;
    out->compressed = p->compressed;
}

void cp_payload_free(cp_payload *p)
{
    if (!p)
        return;
    free(p->bytes);
    free(p);
}

void cp_free(void *p) { free(p); }

/* ---- receiver side ------------------------------------------------------ */

int cp_container_parse(const uint8_t *d, size_t n, uint64_t max_raw, cp_container_hdr *h)
{
    cp_container_hdr x;
    if (!d || n < CPO_HDR_LEN || memcmp(d, "CPO1", 4) != 0)
        return 0;
    memset(&x, 0, sizeof x);
    x.kind = d[4];
    x.flags = d[5];
    x.meta_len = cp_be16r(d + 6);
    x.raw_len = cp_be64r(d + 8);
    memcpy(x.sha256, d + 16, 32);
    if (x.kind < CP_KIND_TEXT || x.kind > CP_KIND_FILES)
        return 0;
    if (x.flags & ~CPO_FLAGS_KNOWN)
        return 0;
    if (x.meta_len > n - CPO_HDR_LEN)
        return 0;
    if (max_raw == 0 || max_raw > CP_MAX_TOTAL_BYTES)
        max_raw = CP_MAX_TOTAL_BYTES;
    if (x.raw_len > max_raw || x.raw_len > (uint64_t)SIZE_MAX)
        return 0;
    x.meta = d + CPO_HDR_LEN;
    x.body = x.meta + x.meta_len;
    x.body_len = n - CPO_HDR_LEN - x.meta_len;
    if (!(x.flags & CPO_FLAG_DEFLATE) && (uint64_t)x.body_len != x.raw_len)
        return 0;
    if (x.kind == CP_KIND_IMAGE && x.raw_len == 0)
        return 0;
    if (h)
        *h = x;
    return 1;
}

static int inflate_exact(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len)
{
    tinfl_decompressor *r = tinfl_decompressor_alloc();
    size_t in_sz = in_len, out_sz = out_len;
    tinfl_status st;
    uint8_t dummy = 0;
    if (!r)
        return 0;
    st = tinfl_decompress(r, in, &in_sz, out_len ? out : &dummy, out_len ? out : &dummy, &out_sz,
                          TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    tinfl_decompressor_free(r);
    /* must reach end of stream, use every input byte and fill exactly raw_len */
    return st == TINFL_STATUS_DONE && in_sz == in_len && out_sz == out_len;
}

int cp_container_decode(const uint8_t *d, size_t n, uint64_t max_raw, cp_container_hdr *h,
                        uint8_t **owned, const uint8_t **raw)
{
    cp_container_hdr x;
    uint8_t digest[32];
    uint8_t *buf = NULL;
    const uint8_t *body;
    *owned = NULL;
    *raw = NULL;
    if (!cp_container_parse(d, n, max_raw, &x))
        return 0;
    if (x.flags & CPO_FLAG_DEFLATE) {
        buf = (uint8_t *)malloc(x.raw_len ? (size_t)x.raw_len : 1);
        if (!buf)
            return 0;
        if (!inflate_exact(x.body, x.body_len, buf, (size_t)x.raw_len)) {
            free(buf);
            return 0;
        }
        body = buf;
    } else {
        body = x.body;
    }
    cp_sha256(body, (size_t)x.raw_len, digest);
    if (memcmp(digest, x.sha256, 32) != 0) {
        free(buf);
        return 0;
    }
    if (h)
        *h = x;
    *owned = buf;
    *raw = body;
    return 1;
}

int cp_meta_get(const uint8_t *meta, size_t len, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key), i = 0;
    if (cap)
        out[0] = 0;
    while (i < len) {
        size_t e = i, vl;
        while (e < len && meta[e] != '\n')
            e++;
        vl = e - i;
        if (vl > kl && !memcmp(meta + i, key, kl) && meta[i + kl] == '=') {
            size_t vs = i + kl + 1, ve = e;
            if (ve > vs && meta[ve - 1] == '\r')
                ve--;
            if (cap) {
                size_t c = ve - vs;
                if (c > cap - 1)
                    c = cap - 1;
                memcpy(out, meta + vs, c);
                out[c] = 0;
            }
            return 1;
        }
        i = e + 1;
    }
    return 0;
}

int cp_files_next(const uint8_t *body, size_t len, size_t *off, cp_file_entry *e)
{
    size_t o = *off, nl;
    uint64_t sz;
    if (o == len)
        return 0;
    if (o > len || len - o < 2)
        return -1;
    nl = cp_be16r(body + o);
    o += 2;
    if (nl > CPO_MAX_NAME || len - o < nl)
        return -1;
    e->name = body + o;
    e->name_len = nl;
    o += nl;
    if (len - o < 8)
        return -1;
    sz = cp_be64r(body + o);
    o += 8;
    if (sz > (uint64_t)(len - o))
        return -1;
    e->data = body + o;
    e->size = sz;
    o += (size_t)sz;
    *off = o;
    return 1;
}
