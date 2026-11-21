/* dbglog.c - see dbglog.h */
#include "dbglog.h"

#ifdef CP_DEBUG_LOG
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"

static CRITICAL_SECTION s_cs;
static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
static HANDLE s_file = INVALID_HANDLE_VALUE;

static BOOL CALLBACK init_once(PINIT_ONCE o, PVOID p, PVOID *ctx)
{
    InitializeCriticalSection(&s_cs);
    wchar_t path[MAX_PATH * 2];
    wfmt(path, CPW_COUNTOF(path), L"%lscodetrans-debug.log", exe_dir());
    s_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (s_file == INVALID_HANDLE_VALUE) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wfmt(path, CPW_COUNTOF(path), L"%lscodetrans-debug.log", tmp);
        s_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    return TRUE;
}

static void write_line(const char *s)
{
    InitOnceExecuteOnce(&s_once, init_once, NULL, NULL);
    if (s_file == INVALID_HANDLE_VALUE)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[64];
    int hn = _snprintf(head, sizeof head - 1, "%02u:%02u:%02u.%03u [%5lu] ", st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds, (unsigned long)GetCurrentThreadId());
    if (hn < 0)
        hn = 0;
    EnterCriticalSection(&s_cs);
    DWORD w;
    WriteFile(s_file, head, (DWORD)hn, &w, NULL);
    WriteFile(s_file, s, (DWORD)strlen(s), &w, NULL);
    WriteFile(s_file, "\r\n", 2, &w, NULL);
    LeaveCriticalSection(&s_cs);
}

void dbg_log(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    if (n >= 0 && (size_t)n < sizeof buf)
        buf[n] = 0;
    write_line(buf);
}

void dbg_logw(const char *prefix, const wchar_t *text)
{
    size_t n = 0;
    char *u = text ? utf16_to_utf8(text, wcslen(text), &n) : NULL;
    size_t pl = strlen(prefix);
    char *line = (char *)malloc(pl + n + 1);
    if (line) {
        memcpy(line, prefix, pl);
        if (u)
            memcpy(line + pl, u, n);
        line[pl + n] = 0;
        write_line(line);
        free(line);
    }
    free(u);
}
#else
typedef int cpw_dbglog_unused;
#endif
