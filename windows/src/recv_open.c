/* recv_open.c - see recv_open.h */
#include "recv_open.h"
#include "util.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

BOOL recv_is_image_path(const wchar_t *path)
{
    static const wchar_t *const exts[] = { L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", NULL };
    const wchar_t *dot = path ? wcsrchr(path, L'.') : NULL;
    if (!dot || wcschr(dot, L'\\') || wcschr(dot, L'/'))
        return FALSE;
    for (int i = 0; exts[i]; i++)
        if (!_wcsicmp(dot, exts[i]))
            return TRUE;
    return FALSE;
}

static BOOL utf8_to_w(const char *s, wchar_t *out, size_t cap)
{
    return s && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, (int)cap) > 0;
}

int recv_classify(const cp_result *r, wchar_t *path, size_t cap)
{
    if (!r || r->count == 0 || !r->paths || !r->paths[0] || !utf8_to_w(r->paths[0], path, cap))
        return RECV_OPEN_NONE;
    switch (r->kind) {
    case CP_KIND_TEXT: {
        /* the receiver always writes the text as text.txt */
        const wchar_t *base = path_basename(path);
        return !_wcsicmp(base, L"text.txt") ? RECV_OPEN_DEFAULT_APP : RECV_OPEN_SHOW_IN_FOLDER;
    }
    case CP_KIND_IMAGE:
        return r->count == 1 && recv_is_image_path(path) ? RECV_OPEN_DEFAULT_APP : RECV_OPEN_SHOW_IN_FOLDER;
    default:
        return RECV_OPEN_SHOW_IN_FOLDER;
    }
}

uint8_t *recv_plain_to_utf8(const char *text, size_t n, size_t *out_n)
{
    static const uint8_t bom[3] = { 0xEF, 0xBB, 0xBF };
    const uint8_t *s = (const uint8_t *)text;
    uint8_t *o = NULL;
    size_t skip, body = 0;
    if (!out_n || (!text && n))
        return NULL;
    skip = cp_text_bom_len(s, n);
    s += skip;
    n -= skip;
    if (cp_utf8_valid(s, n)) {
        o = (uint8_t *)malloc(3 + n + 1);
        if (o)
            memcpy(o + 3, s, n), body = n;
    } else if (n <= (size_t)0x3FFFFFFF) {
        /* not UTF-8: Chinese QR generators commonly emit GB18030 / GBK */
        int wn = MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS, (const char *)s, (int)n, NULL, 0);
        wchar_t *w = wn > 0 ? (wchar_t *)malloc((size_t)wn * sizeof *w) : NULL;
        if (w && MultiByteToWideChar(54936, MB_ERR_INVALID_CHARS, (const char *)s, (int)n, w, wn) == wn) {
            int un = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
            o = un > 0 ? (uint8_t *)malloc(3 + (size_t)un + 1) : NULL;
            if (o && WideCharToMultiByte(CP_UTF8, 0, w, wn, (char *)o + 3, un, NULL, NULL) == un)
                body = (size_t)un;
            else {
                free(o);
                o = NULL;
            }
        }
        free(w);
        if (!o) {   /* undecodable: keep the raw bytes, without a BOM that would mislabel them */
            o = (uint8_t *)malloc(n + 1);
            if (!o)
                return NULL;
            memcpy(o, s, n);
            *out_n = n;
            return o;
        }
    }
    if (!o)
        return NULL;
    memcpy(o, bom, 3);
    *out_n = 3 + body;
    return o;
}

BOOL recv_save_plain(const char *dir, const char *text, size_t n, wchar_t *path, size_t cap)
{
    wchar_t wdir[MAX_PATH];
    SYSTEMTIME t;
    HANDLE h;
    DWORD wr;
    BOOL ok;
    size_t len = 0;
    uint8_t *buf;
    if (!utf8_to_w(dir, wdir, MAX_PATH))
        return FALSE;
    buf = recv_plain_to_utf8(text, n, &len);
    if (!buf || len > 0x7FFFFFFFu) {
        free(buf);
        return FALSE;
    }
    CreateDirectoryW(wdir, NULL);
    GetLocalTime(&t);
    _snwprintf(path, cap - 1, L"%ls\\qr-%04u%02u%02u-%02u%02u%02u.txt", wdir, t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond);
    path[cap - 1] = 0;
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(buf);
        return FALSE;
    }
    ok = WriteFile(h, buf, (DWORD)len, &wr, NULL) && wr == (DWORD)len;
    CloseHandle(h);
    free(buf);
    return ok;
}

BOOL recv_perform(int action, const wchar_t *path)
{
    if (action == RECV_OPEN_DEFAULT_APP)
        return (INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL) > 32;
    if (action == RECV_OPEN_SHOW_IN_FOLDER) {
        wchar_t args[MAX_PATH + 16];
        _snwprintf(args, MAX_PATH + 15, L"/select,\"%ls\"", path);
        args[MAX_PATH + 15] = 0;
        return (INT_PTR)ShellExecuteW(NULL, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL) > 32;
    }
    return FALSE;
}
