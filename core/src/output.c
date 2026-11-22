/* Untrusted file name sanitization + unique naming. See PROTOCOL.md §6. */
#include "cp_internal.h"

/* Simple 1:1 case folding for name comparison: ASCII, Latin-1, Latin Extended-A,
 * Greek and basic Cyrillic. NTFS and Android shared storage compare names
 * case-insensitively beyond ASCII. */
static uint32_t fold(uint32_t c)
{
    if (c < 0x80)
        return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE)                              /* Latin-1 */
        return c == 0xD7 ? c : c + 32;
    if (c >= 0x100 && c <= 0x17F) {                          /* Latin Extended-A */
        if (c == 0x178)
            return 0xFF;
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
            return (c & 1) ? c + 1 : c;                      /* upper case is odd */
        if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F)
            return c;                                        /* no simple pair */
        return (c & 1) ? c : c + 1;                          /* upper case is even */
    }
    if (c >= 0x386 && c <= 0x3AB) {                          /* Greek */
        if (c == 0x386) return 0x3AC;
        if (c >= 0x388 && c <= 0x38A) return c + 37;
        if (c == 0x38C) return 0x3CC;
        if (c == 0x38E || c == 0x38F) return c + 63;
        if ((c >= 0x391 && c <= 0x3A1) || (c >= 0x3A3 && c <= 0x3AB)) return c + 32;
        return c;
    }
    if (c == 0x3C2)
        return 0x3C3;                                        /* final sigma */
    if (c >= 0x400 && c <= 0x40F)                            /* Cyrillic */
        return c + 80;
    if (c >= 0x410 && c <= 0x42F)
        return c + 32;
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F))
        return (c & 1) ? c : c + 1;
    return c;
}

/* case-insensitive equality of two UTF-8 strings (invalid bytes compare as-is) */
static int ieq(const char *a, const char *b)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    size_t nx = strlen(a), ny = strlen(b);
    while (nx && ny) {
        uint32_t cx = 0, cy = 0;
        size_t lx = cp_utf8_decode(x, nx, &cx), ly = cp_utf8_decode(y, ny, &cy);
        if (!lx) { cx = 0x110000u + *x; lx = 1; }
        if (!ly) { cy = 0x110000u + *y; ly = 1; }
        if (fold(cx) != fold(cy))
            return 0;
        x += lx; nx -= lx;
        y += ly; ny -= ly;
    }
    return nx == 0 && ny == 0;
}

static int is_dropped(uint32_t c)
{
    if (c < 0x20 || c == 0x7F || (c >= 0x80 && c <= 0x9F))
        return 1;                                  /* C0 / C1 controls */
    if (c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069))
        return 1;                                  /* bidi overrides (name spoofing) */
    if (c == 0xFEFF)
        return 1;
    return 0;
}

static int is_reserved_char(uint32_t c)
{
    return c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
           c == '|';
}

static int is_reserved_device(const char *s, size_t stem_len)
{
    static const char *const dev[] = { "con", "prn", "aux", "nul", NULL };
    char stem[8];
    int i;
    if (stem_len < 3 || stem_len > 4)
        return 0;
    memcpy(stem, s, stem_len);
    stem[stem_len] = 0;
    for (i = 0; dev[i]; i++)
        if (ieq(stem, dev[i]))
            return 1;
    if (stem_len == 4 && stem[3] >= '1' && stem[3] <= '9') {
        stem[3] = 0;
        if (ieq(stem, "com") || ieq(stem, "lpt"))
            return 1;
    }
    return 0;
}

/* finds an extension worth preserving: index of the last '.', or len */
static size_t ext_pos(const char *s, size_t len)
{
    size_t i = len;
    while (i > 0) {
        i--;
        if (s[i] == '.')
            return (i > 0 && len - i <= 16) ? i : len;
    }
    return len;
}

void cp_sanitize_name(const uint8_t *in, size_t n, char *out, size_t cap)
{
    char *buf, *tmp;
    size_t bl = 0, i = 0, start, end, ep, el;
    size_t tl = 0;

    if (cap == 0)
        return;
    /* filtering never grows the name, +8 covers "file" and the device prefix */
    buf = (char *)malloc(n + 8);
    tmp = (char *)malloc(n + 8);
    if (!buf || !tmp) {
        free(buf);
        free(tmp);
        snprintf(out, cap, "file");
        return;
    }
    /* 1. code point filter */
    while (i < n) {
        uint32_t c = 0;
        size_t l = cp_utf8_decode(in + i, n - i, &c);
        if (l == 0) {
            buf[bl++] = '_';
            i++;
            continue;
        }
        if (is_dropped(c)) {
            i += l;
            continue;
        }
        if (is_reserved_char(c))
            buf[bl++] = '_';
        else {
            memcpy(buf + bl, in + i, l);
            bl += l;
        }
        i += l;
    }
    /* 2. ".." -> "_" */
    for (i = 0; i < bl; i++) {
        if (buf[i] == '.' && i + 1 < bl && buf[i + 1] == '.') {
            tmp[tl++] = '_';
            i++;
        } else {
            tmp[tl++] = buf[i];
        }
    }
    /* 3. trim leading spaces, trailing spaces and dots */
    start = 0;
    end = tl;
    while (start < end && tmp[start] == ' ')
        start++;
    while (end > start && (tmp[end - 1] == ' ' || tmp[end - 1] == '.'))
        end--;
    bl = end - start;
    memmove(buf, tmp + start, bl);
    if (bl == 0) {
        memcpy(buf, "file", 4);
        bl = 4;
    }
    /* 4. Windows device names */
    {
        size_t stem = 0;
        while (stem < bl && buf[stem] != '.')
            stem++;
        if (is_reserved_device(buf, stem)) {
            memmove(buf + 1, buf, bl);
            buf[0] = '_';
            bl++;
        }
    }
    /* 5. length cap on a UTF-8 boundary, preserving a short extension */
    if (bl > CP_NAME_MAX) {
        ep = ext_pos(buf, bl);
        el = bl - ep;
        if (el < CP_NAME_MAX / 2) {
            size_t sl = cp_utf8_cut((const uint8_t *)buf, CP_NAME_MAX - el);
            memmove(buf + sl, buf + ep, el);
            bl = sl + el;
        } else {
            bl = cp_utf8_cut((const uint8_t *)buf, CP_NAME_MAX);
        }
    }
    if (bl > cap - 1)
        bl = cp_utf8_cut((const uint8_t *)buf, cap - 1);
    memcpy(out, buf, bl);
    out[bl] = 0;
    free(buf);
    free(tmp);
}

static int name_taken(const char *name, char *const *names, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++)
        if (names[i] && ieq(name, names[i]))
            return 1;
    return 0;
}

void cp_unique_name(char *name, size_t cap, char *const *names, size_t count)
{
    char stem[CP_NAME_MAX + 1], ext[CP_NAME_MAX + 1], cand[CP_NAME_MAX + 32];
    size_t len = strlen(name), ep;
    unsigned k;
    if (!name_taken(name, names, count))
        return;
    ep = ext_pos(name, len);
    memcpy(stem, name, ep);
    stem[ep] = 0;
    memcpy(ext, name + ep, len - ep);
    ext[len - ep] = 0;
    for (k = 1; k < 100000u; k++) {
        char suffix[16];
        size_t sfx, room, sl;
        snprintf(suffix, sizeof suffix, " (%u)", k);
        sfx = strlen(suffix);
        room = CP_NAME_MAX > sfx + strlen(ext) ? CP_NAME_MAX - sfx - strlen(ext) : 0;
        sl = strlen(stem);
        if (sl > room)
            sl = cp_utf8_cut((const uint8_t *)stem, room);
        memcpy(cand, stem, sl);
        snprintf(cand + sl, sizeof cand - sl, "%s%s", suffix, ext);
        if (!name_taken(cand, names, count)) {
            size_t cl = strlen(cand);
            if (cl > cap - 1)
                cl = cap - 1;
            memcpy(name, cand, cl);
            name[cl] = 0;
            return;
        }
    }
}
