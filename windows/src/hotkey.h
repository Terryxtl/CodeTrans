/* hotkey.h - when and how to take over the snip hotkey (PLAN.md §6.7). */
#ifndef CPW_HOTKEY_H
#define CPW_HOTKEY_H

#include <windows.h>
#include "config.h"

#define WIN10_1703_BUILD 15063

/* TRUE when the built-in snip should own the hotkey: OS older than Win10
 * 1703 (no system Win+Shift+S) or "强制内置截图" is on. */
BOOL hotkey_builtin_wanted(const app_settings *s);

/* (Re-)registers HOTKEY_ID_SNIP on hwnd. active receives the hotkey that is
 * now live ("" if none); note receives a warning for the status line. */
void hotkey_apply(HWND hwnd, const app_settings *s, wchar_t *active, size_t acap, wchar_t *note, size_t ncap);
void hotkey_release(HWND hwnd);

#endif
