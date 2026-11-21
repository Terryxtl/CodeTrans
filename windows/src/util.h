/* util.h - small string / size helpers shared by the app and the pipeline. */
#ifndef CPW_UTIL_H
#define CPW_UTIL_H

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#define CPW_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CPW_MAX(a, b) ((a) > (b) ? (a) : (b))
#define CPW_CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
#define CPW_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* vsnwprintf into buf, always NUL-terminated (truncates). Uses msvcrt
 * semantics: use %ls for wchar_t*, %hs for char*. Returns chars written. */
int wfmt(wchar_t *buf, size_t cap, const wchar_t *fmt, ...);

/* "512B", "12.3KB", "286KB", "1.2MB", "45MB", "1.1GB" */
void fmt_size(uint64_t bytes, wchar_t *buf, size_t cap);

/* UTF-16 -> UTF-8, malloc'ed and NUL-terminated. wlen = number of wchar_t
 * (not counting a terminator). *out_len excludes the NUL. NULL on failure. */
char *utf16_to_utf8(const wchar_t *w, size_t wlen, size_t *out_len);

/* File name part of a path ("C:\a\b.txt" -> "b.txt"). */
const wchar_t *path_basename(const wchar_t *path);

/* Directory of the running exe, with trailing backslash. */
const wchar_t *exe_dir(void);

#endif
