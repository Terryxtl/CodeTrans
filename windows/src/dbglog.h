/* dbglog.h - optional debug log (configure with -DCP_DEBUG_LOG=ON).
 * Writes UTF-8 lines to codetrans-debug.log next to the exe (falls back to
 * %TEMP%). Thread-safe. Compiles to nothing in normal builds. */
#ifndef CPW_DBGLOG_H
#define CPW_DBGLOG_H

#include <wchar.h>

#ifdef CP_DEBUG_LOG
void dbg_log(const char *fmt, ...);                      /* printf-style */
void dbg_logw(const char *prefix, const wchar_t *text);  /* prefix + UTF-16 text */
#define DBG(...) dbg_log(__VA_ARGS__)
#define DBGW(prefix, text) dbg_logw((prefix), (text))
#else
#define DBG(...) ((void)0)
#define DBGW(prefix, text) ((void)0)
#endif

#endif
