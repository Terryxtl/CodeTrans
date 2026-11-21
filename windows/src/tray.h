/* tray.h - notification area icon and menu (PLAN.md §6.8). */
#ifndef CPW_TRAY_H
#define CPW_TRAY_H

#include <windows.h>
#include "config.h"

BOOL tray_add(HWND hwnd, HICON icon, const wchar_t *tip);
void tray_remove(HWND hwnd);
void tray_set_tip(HWND hwnd, const wchar_t *tip);
/* Shows the context menu at the cursor; commands arrive as WM_COMMAND. */
void tray_menu(HWND hwnd, const app_settings *s, BOOL window_visible, BOOL snip_available);

#endif
