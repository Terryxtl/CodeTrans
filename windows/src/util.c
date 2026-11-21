/* util.c - see util.h */
#include "util.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int wfmt(wchar_t *buf, size_t cap, const wchar_t *fmt, ...)
{
    if (!buf || cap == 0)
        return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnwprintf(buf, cap - 1, fmt, ap);
    va_end(ap);
    buf[cap - 1] = 0;
    if (n < 0 || (size_t)n >= cap)
        n = (int)wcslen(buf);
    else
        buf[n] = 0;
    return n;
}

void fmt_size(uint64_t bytes, wchar_t *buf, size_t cap)
{
    static const wchar_t *units[] = { L"KB", L"MB", L"GB", L"TB" };
    if (bytes < 1024) {
        wfmt(buf, cap, L"%uB", (unsigned)bytes);
        return;
    }
    double v = (double)bytes / 1024.0;
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        u++;
    }
    if (v < 100.0)
        wfmt(buf, cap, L"%.1f%ls", v, units[u]);
    else
        wfmt(buf, cap, L"%.0f%ls", v, units[u]);
}

char *utf16_to_utf8(const wchar_t *w, size_t wlen, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (wlen > 0x3FFFFFFF)
        return NULL;
    int n = 0;
    if (wlen > 0) {
        n = WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, NULL, 0, NULL, NULL);
        if (n <= 0)
            return NULL;
    }
    char *s = (char *)malloc((size_t)n + 1);
    if (!s)
        return NULL;
    if (n > 0 && WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, s, n, NULL, NULL) != n) {
        free(s);
        return NULL;
    }
    s[n] = 0;
    if (out_len)
        *out_len = (size_t)n;
    return s;
}

const wchar_t *path_basename(const wchar_t *path)
{
    const wchar_t *b = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/' || *p == L':')
            b = p + 1;
    return b;
}

const wchar_t *exe_dir(void)
{
    static wchar_t dir[MAX_PATH * 2];
    if (!dir[0]) {
        DWORD n = GetModuleFileNameW(NULL, dir, (DWORD)CPW_COUNTOF(dir));
        if (n == 0 || n >= CPW_COUNTOF(dir)) {
            dir[0] = L'.';
            dir[1] = L'\\';
            dir[2] = 0;
        } else {
            wchar_t *b = (wchar_t *)path_basename(dir);
            *b = 0;
        }
    }
    return dir;
}
