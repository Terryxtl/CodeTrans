/* recv_open.h - what to do with a transfer the camera receiver completed.
 *
 * Text and images open in their default program (ShellExecute "open").
 * Anything else is only shown in Explorer: a received .exe / .lnk / .js must
 * never start by itself. An "image" whose saved name does not end in a known
 * image extension is treated like any other file. */
#ifndef CPW_RECV_OPEN_H
#define CPW_RECV_OPEN_H

#include <windows.h>
#include "cp.h"

enum { RECV_OPEN_NONE = 0, RECV_OPEN_DEFAULT_APP = 1, RECV_OPEN_SHOW_IN_FOLDER = 2 };

/* Image extensions safe to hand to the default viewer (case-insensitive). */
BOOL recv_is_image_path(const wchar_t *path);

/* Decides for result r; path receives the file to open / select (wide). */
int  recv_classify(const cp_result *r, wchar_t *path, size_t cap);

/* Converts a plain QR's bytes to UTF-8 with exactly one BOM: valid UTF-8 is kept
 * (any BOM of its own dropped), otherwise decoded as GB18030, otherwise raw bytes
 * are kept. Returns a malloc'd buffer (free()) and its length, or NULL. */
uint8_t *recv_plain_to_utf8(const char *text, size_t n, size_t *out_n);

/* Saves a plain QR's text as <dir>\qr-<YYYYMMDD-HHMMSS>.txt (see
 * recv_plain_to_utf8; the BOM lets Notepad on Windows 7 detect UTF-8). dir is UTF-8. Returns TRUE and the path. */
BOOL recv_save_plain(const char *dir, const char *text, size_t n, wchar_t *path, size_t cap);

/* Performs the action (UI thread). Returns FALSE if the shell refused. */
BOOL recv_perform(int action, const wchar_t *path);

#endif
