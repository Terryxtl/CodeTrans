/* hotkey.c - see hotkey.h */
#include "hotkey.h"
#include "app.h"
#include "compat.h"
#include "util.h"

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

static BOOL s_registered;

BOOL hotkey_builtin_wanted(const app_settings *s)
{
    DWORD major = compat_os_major(), build = compat_os_build();
    BOOL old_os = major < 10 || (major == 10 && build < WIN10_1703_BUILD);
    return old_os || s->force_builtin_snip;
}

void hotkey_release(HWND hwnd)
{
    if (s_registered) {
        UnregisterHotKey(hwnd, HOTKEY_ID_SNIP);
        s_registered = FALSE;
    }
}

void hotkey_apply(HWND hwnd, const app_settings *s, wchar_t *active, size_t acap, wchar_t *note, size_t ncap)
{
    wchar_t want[48], alt[48];
    active[0] = 0;
    note[0] = 0;
    hotkey_release(hwnd);
    if (!hotkey_builtin_wanted(s))
        return;
    hotkey_format(s->snip_mods, s->snip_vk, want, CPW_COUNTOF(want));
    if (RegisterHotKey(hwnd, HOTKEY_ID_SNIP, s->snip_mods | MOD_NOREPEAT, s->snip_vk)) {
        s_registered = TRUE;
        lstrcpynW(active, want, (int)acap);
        return;
    }
    /* taken (e.g. OneNote 2010/2013 owns Win+Shift+S): use the fallback */
    hotkey_format(s->fallback_mods, s->fallback_vk, alt, CPW_COUNTOF(alt));
    if (RegisterHotKey(hwnd, HOTKEY_ID_SNIP, s->fallback_mods | MOD_NOREPEAT, s->fallback_vk)) {
        s_registered = TRUE;
        lstrcpynW(active, alt, (int)acap);
        wfmt(note, ncap, L"%ls 已被占用,截图改用 %ls", want, alt);
    } else {
        wfmt(note, ncap, L"截图热键 %ls / %ls 都被占用,请用托盘菜单截图", want, alt);
    }
}
