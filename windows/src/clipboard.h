/* clipboard.h - clipboard listener and reader (PLAN.md §6.3). */
#ifndef CPW_CLIPBOARD_H
#define CPW_CLIPBOARD_H

#include <windows.h>
#include "content.h"

enum {
    CLIP_OK = 0,        /* *out holds new content                        */
    CLIP_NOTHING,       /* only CF_HDROP / unsupported formats: ignore   */
    CLIP_EMPTY,         /* empty text                                    */
    CLIP_SKIPPED,       /* marked by a password manager                  */
    CLIP_BUSY,          /* OpenClipboard kept failing                    */
    CLIP_ERROR          /* out of memory / unreadable data               */
};

BOOL clip_listen(HWND hwnd);
void clip_unlisten(HWND hwnd);
/* Reads the clipboard with the priority of §6.3 (retries OpenClipboard
 * 10 x 20 ms). skip_marked: honour password-manager exclusion formats. */
int  clip_read(HWND hwnd, BOOL skip_marked, content **out);
/* Writes a picture (as CF_DIB + "PNG") to the clipboard (snip result). */
BOOL clip_write_image(HWND hwnd, const uint8_t *dib, size_t dib_len, const uint8_t *png, size_t png_len);

#endif
