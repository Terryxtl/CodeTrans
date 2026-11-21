/* snip.h - built-in region screenshot for systems without Win+Shift+S
 * (PLAN.md §6.7). The result goes to the clipboard as CF_DIB + "PNG"; the
 * clipboard listener then sends it like any other copied picture. */
#ifndef CPW_SNIP_H
#define CPW_SNIP_H

#include <windows.h>

BOOL snip_register(HINSTANCE hi);
/* Captures the virtual screen and opens the selection overlay. The caller
 * hides its own window first. WM_APP_SNIP_DONE is posted to owner when the
 * overlay closes (wParam = 1 if an image was put on the clipboard). */
BOOL snip_start(HWND owner);
BOOL snip_active(void);

#endif
