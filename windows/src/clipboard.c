/* clipboard.c - see clipboard.h */
#include "clipboard.h"
#include "util.h"
#include "dbglog.h"
#include <stdlib.h>
#include <string.h>

typedef BOOL (WINAPI *PFN_AddClipboardFormatListener)(HWND);

static UINT cf_png, cf_excl, cf_ignore, cf_history;

static void ensure_formats(void)
{
    if (cf_png)
        return;
    cf_png = RegisterClipboardFormatW(L"PNG");
    cf_excl = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    cf_ignore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    cf_history = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
}

BOOL clip_listen(HWND hwnd)
{
    ensure_formats();
    return AddClipboardFormatListener(hwnd);   /* Vista+ */
}

void clip_unlisten(HWND hwnd)
{
    RemoveClipboardFormatListener(hwnd);
}

static BOOL open_retry(HWND hwnd)
{
    for (int i = 0; i < 10; i++) {
        if (OpenClipboard(hwnd))
            return TRUE;
        Sleep(20);
    }
    return FALSE;
}

/* copies a global memory clipboard handle */
static uint8_t *copy_global(HANDLE h, size_t *n)
{
    void *p;
    SIZE_T sz;
    uint8_t *out;
    *n = 0;
    if (!h)
        return NULL;
    sz = GlobalSize(h);
    p = GlobalLock(h);
    if (!p || sz == 0) {
        if (p)
            GlobalUnlock(h);
        return NULL;
    }
    out = (uint8_t *)malloc(sz);
    if (out) {
        memcpy(out, p, sz);
        *n = sz;
    }
    GlobalUnlock(h);
    return out;
}

static BOOL marked_private(void)
{
    if (IsClipboardFormatAvailable(cf_excl) || IsClipboardFormatAvailable(cf_ignore))
        return TRUE;
    /* KeePass & co. also set CanIncludeInClipboardHistory = 0 */
    if (IsClipboardFormatAvailable(cf_history)) {
        HANDLE h = GetClipboardData(cf_history);
        DWORD *v = h ? (DWORD *)GlobalLock(h) : NULL;
        BOOL priv = FALSE;
        if (v) {
            priv = GlobalSize(h) >= sizeof(DWORD) && *v == 0;
            GlobalUnlock(h);
        }
        return priv;
    }
    return FALSE;
}

static int read_text(content **out)
{
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const wchar_t *w;
    size_t cap, n = 0;
    char *u8;
    size_t u8n;
    if (!h)
        return CLIP_ERROR;
    w = (const wchar_t *)GlobalLock(h);
    if (!w)
        return CLIP_ERROR;
    cap = GlobalSize(h) / sizeof(wchar_t);
    while (n < cap && w[n])          /* stop at the first NUL */
        n++;
    if (n == 0) {
        GlobalUnlock(h);
        return CLIP_EMPTY;
    }
    u8 = utf16_to_utf8(w, n, &u8n);
    GlobalUnlock(h);
    if (!u8)
        return CLIP_ERROR;
    *out = content_new_text(u8, u8n);
    free(u8);
    return *out ? CLIP_OK : CLIP_ERROR;
}

int clip_read(HWND hwnd, BOOL skip_marked, content **out)
{
    int r = CLIP_NOTHING;
    *out = NULL;
    ensure_formats();
    if (!open_retry(hwnd))
        return CLIP_BUSY;

    if (skip_marked && marked_private()) {
        r = CLIP_SKIPPED;
    } else if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        r = read_text(out);
    } else if (cf_png && IsClipboardFormatAvailable(cf_png)) {
        /* lossless original, keeps transparency; re-encoded only for JPEG */
        size_t n;
        uint8_t *b = copy_global(GetClipboardData(cf_png), &n);
        if (b && n >= 8 && !memcmp(b, "\x89PNG", 4)) {
            *out = content_new_image(CONTENT_IMAGE_PNG, b, n);
            r = *out ? CLIP_OK : CLIP_ERROR;
        } else {
            free(b);
            r = CLIP_ERROR;
        }
    }
    if (r != CLIP_OK && r != CLIP_SKIPPED && r != CLIP_EMPTY &&
        (IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB))) {
        UINT fmt = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB;
        size_t n;
        uint8_t *b = copy_global(GetClipboardData(fmt), &n);
        if (!b && fmt == CF_DIBV5)
            b = copy_global(GetClipboardData(CF_DIB), &n);
        if (b) {
            *out = content_new_image(CONTENT_IMAGE_DIB, b, n);
            r = *out ? CLIP_OK : CLIP_ERROR;
        } else {
            r = CLIP_ERROR;
        }
    }
    CloseClipboard();
    DBG("clip_read -> %d", r);
    return r;
}

static HGLOBAL global_from(const void *p, size_t n)
{
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    void *d;
    if (!g)
        return NULL;
    d = GlobalLock(g);
    if (!d) {
        GlobalFree(g);
        return NULL;
    }
    memcpy(d, p, n);
    GlobalUnlock(g);
    return g;
}

BOOL clip_write_image(HWND hwnd, const uint8_t *dib, size_t dib_len, const uint8_t *png, size_t png_len)
{
    HGLOBAL gd = NULL, gp = NULL;
    BOOL ok = FALSE;
    ensure_formats();
    if (!open_retry(hwnd))
        return FALSE;
    EmptyClipboard();
    if (dib && (gd = global_from(dib, dib_len)) != NULL) {
        if (SetClipboardData(CF_DIB, gd))
            ok = TRUE;
        else
            GlobalFree(gd);
    }
    if (png && cf_png && (gp = global_from(png, png_len)) != NULL) {
        if (SetClipboardData(cf_png, gp))
            ok = TRUE;
        else
            GlobalFree(gp);
    }
    CloseClipboard();
    return ok;
}
