/* config.c - see config.h */
#include "config.h"
#include "util.h"
#include <shlobj.h>
#include <stdlib.h>
#include <wchar.h>

#define SEC L"CodeTrans"
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"CodeTrans"

static wchar_t s_ini[MAX_PATH * 2];

int cfg_layout_version(const app_settings *s)
{
    return CFG_PROTO_IS_OB(s->protocol) ? CFG_OB_VERSION : s->version;
}

int cfg_layout_ecc(const app_settings *s)
{
    return CFG_PROTO_IS_OB(s->protocol) ? CFG_ECC_L : s->ecc;
}

size_t cfg_code_bytes(const app_settings *s)
{
    if (s->protocol == CFG_PROTO_OB_BANDS)
        return cp_ob_board_bytes(CP_OB_BANDS);
    if (s->protocol == CFG_PROTO_OB_TILES)
        return cp_ob_board_bytes(CP_OB_TILES);
    return cp_qr_capacity(s->version, s->ecc);
}

void config_defaults(app_settings *s)
{
    ZeroMemory(s, sizeof *s);
    s->protocol = CFG_PROTO_DECIMEN3;   /* Decimen v3 · RS96 */
    s->version = 40;                    /* the Decimen family default (see settings_bar.c) */
    s->ecc = CFG_ECC_L;
    s->module_px = 4;
    s->code_count = 2;
    s->fps = 30;
    s->image_format = CFG_IMG_AUTO;
    s->topmost = TRUE;
    s->auto_popup = TRUE;
    s->pause_monitor = FALSE;
    s->force_builtin_snip = FALSE;
    s->jpeg_quality = 85;
    s->skip_password_managers = TRUE;
    s->quiet_zone = 4;
    s->confirm_mb = 20;
    s->snip_mods = MOD_WIN | MOD_SHIFT;
    s->snip_vk = 'S';
    s->fallback_mods = MOD_CONTROL | MOD_ALT;
    s->fallback_vk = 'S';
    s->recv_auto_open = TRUE;
    s->camera_index = 0;
    s->settings_expanded = FALSE;
    s->has_window_pos = FALSE;
}

void config_validate(app_settings *s)
{
    if (s->protocol < 0 || s->protocol >= CFG_PROTO_COUNT)
        s->protocol = CFG_PROTO_CODETRANS;
    s->version = CPW_CLAMP(s->version, CFG_VERSION_MIN, CFG_VERSION_MAX);
    s->ecc = CPW_CLAMP(s->ecc, CFG_ECC_L, CFG_ECC_H);
    s->module_px = CPW_CLAMP(s->module_px, CFG_MODULE_MIN, CFG_MODULE_MAX);
    if (!cp_code_count_ok(s->code_count))
        s->code_count = 2;
    s->fps = CPW_CLAMP(s->fps, CFG_FPS_MIN, CFG_FPS_MAX);
    if (s->image_format != CFG_IMG_PNG && s->image_format != CFG_IMG_JPEG && s->image_format != CFG_IMG_AUTO)
        s->image_format = CFG_IMG_AUTO;
    s->jpeg_quality = CPW_CLAMP(s->jpeg_quality, CFG_JPEGQ_MIN, CFG_JPEGQ_MAX);
    s->quiet_zone = CPW_CLAMP(s->quiet_zone, CFG_QUIET_MIN, CFG_QUIET_MAX);
    s->confirm_mb = CPW_CLAMP(s->confirm_mb, 1, 256);
    s->topmost = !!s->topmost;
    s->auto_popup = !!s->auto_popup;
    s->pause_monitor = !!s->pause_monitor;
    s->force_builtin_snip = !!s->force_builtin_snip;
    s->skip_password_managers = !!s->skip_password_managers;
    s->settings_expanded = !!s->settings_expanded;
    s->recv_auto_open = !!s->recv_auto_open;
    s->camera_index = CPW_CLAMP(s->camera_index, 0, 63);
    if (s->snip_vk == 0 || s->snip_vk > 0xFE) {
        s->snip_mods = MOD_WIN | MOD_SHIFT;
        s->snip_vk = 'S';
    }
    if (s->fallback_vk == 0 || s->fallback_vk > 0xFE) {
        s->fallback_mods = MOD_CONTROL | MOD_ALT;
        s->fallback_vk = 'S';
    }
    s->snip_mods &= MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
    s->fallback_mods &= MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
}

/* ------------------------------------------------------------------ path */

/* Opens (creating if needed) the file for writing. A newly created file gets
 * a UTF-16LE BOM so that the profile API keeps it in Unicode. */
static BOOL ensure_writable(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        static const BYTE bom[2] = { 0xFF, 0xFE };
        DWORD w;
        WriteFile(h, bom, sizeof bom, &w, NULL);
    }
    CloseHandle(h);
    return TRUE;
}

BOOL config_init(void)
{
    wchar_t appdata[MAX_PATH], legacy[MAX_PATH * 2];
    wfmt(legacy, CPW_COUNTOF(legacy), L"%lscodetrans.ini", exe_dir());

    /* The user's profile: %APPDATA%\CodeTrans\codetrans.ini */
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appdata)) ||
        GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) > 0) {
        wchar_t dir[MAX_PATH * 2];
        wfmt(dir, CPW_COUNTOF(dir), L"%ls\\CodeTrans", appdata);
        CreateDirectoryW(dir, NULL);
        wfmt(s_ini, CPW_COUNTOF(s_ini), L"%ls\\codetrans.ini", dir);
        /* earlier versions kept the INI next to the exe: take its settings over once */
        if (GetFileAttributesW(s_ini) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(legacy) != INVALID_FILE_ATTRIBUTES)
            CopyFileW(legacy, s_ini, TRUE);
        if (ensure_writable(s_ini))
            return TRUE;
    }

    /* no usable profile folder: next to the exe */
    lstrcpynW(s_ini, legacy, (int)CPW_COUNTOF(s_ini));
    return ensure_writable(s_ini);   /* FALSE: reads still work, writes fail */
}

void config_set_path(const wchar_t *path)
{
    lstrcpynW(s_ini, path, (int)CPW_COUNTOF(s_ini));
}

const wchar_t *config_path(void)
{
    return s_ini;
}

/* ------------------------------------------------------------------ hotkeys */

static const struct { const wchar_t *name; UINT vk; } s_keys[] = {
    { L"PrintScreen", VK_SNAPSHOT }, { L"PrtSc", VK_SNAPSHOT }, { L"Pause", VK_PAUSE },
    { L"Insert", VK_INSERT }, { L"Delete", VK_DELETE }, { L"Home", VK_HOME }, { L"End", VK_END },
    { L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT }, { L"Space", VK_SPACE },
};

BOOL hotkey_parse(const wchar_t *s, UINT *mods, UINT *vk)
{
    UINT m = 0, k = 0;
    wchar_t tok[32];
    const wchar_t *p = s;
    while (*p) {
        while (*p == L' ' || *p == L'\t')
            p++;
        size_t n = 0;
        while (*p && *p != L'+' && n + 1 < CPW_COUNTOF(tok))
            tok[n++] = *p++;
        while (n > 0 && (tok[n - 1] == L' ' || tok[n - 1] == L'\t'))
            n--;
        tok[n] = 0;
        if (*p == L'+')
            p++;
        if (n == 0)
            return FALSE;
        if (k != 0)
            return FALSE;   /* the key must be the last token */
        if (!_wcsicmp(tok, L"Win")) m |= MOD_WIN;
        else if (!_wcsicmp(tok, L"Ctrl") || !_wcsicmp(tok, L"Control")) m |= MOD_CONTROL;
        else if (!_wcsicmp(tok, L"Shift")) m |= MOD_SHIFT;
        else if (!_wcsicmp(tok, L"Alt")) m |= MOD_ALT;
        else if (n == 1 && ((tok[0] >= L'A' && tok[0] <= L'Z') || (tok[0] >= L'0' && tok[0] <= L'9')))
            k = tok[0];
        else if (n == 1 && tok[0] >= L'a' && tok[0] <= L'z')
            k = (UINT)(tok[0] - L'a' + L'A');
        else if ((tok[0] == L'F' || tok[0] == L'f') && n >= 2 && n <= 3) {
            int f = _wtoi(tok + 1);
            if (f < 1 || f > 24)
                return FALSE;
            k = VK_F1 + (UINT)(f - 1);
        } else {
            size_t i;
            for (i = 0; i < CPW_COUNTOF(s_keys); i++)
                if (!_wcsicmp(tok, s_keys[i].name)) {
                    k = s_keys[i].vk;
                    break;
                }
            if (i == CPW_COUNTOF(s_keys))
                return FALSE;
        }
    }
    if (k == 0)
        return FALSE;
    *mods = m;
    *vk = k;
    return TRUE;
}

void hotkey_format(UINT mods, UINT vk, wchar_t *buf, size_t cap)
{
    wchar_t key[16];
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
        wfmt(key, CPW_COUNTOF(key), L"%lc", (wchar_t)vk);
    else if (vk >= VK_F1 && vk <= VK_F24)
        wfmt(key, CPW_COUNTOF(key), L"F%u", vk - VK_F1 + 1);
    else {
        key[0] = 0;
        for (size_t i = 0; i < CPW_COUNTOF(s_keys); i++)
            if (s_keys[i].vk == vk) {
                lstrcpynW(key, s_keys[i].name, (int)CPW_COUNTOF(key));
                break;
            }
        if (!key[0])
            wfmt(key, CPW_COUNTOF(key), L"0x%02X", vk);
    }
    wfmt(buf, cap, L"%ls%ls%ls%ls%ls", (mods & MOD_WIN) ? L"Win+" : L"",
         (mods & MOD_CONTROL) ? L"Ctrl+" : L"", (mods & MOD_SHIFT) ? L"Shift+" : L"",
         (mods & MOD_ALT) ? L"Alt+" : L"", key);
}

/* ------------------------------------------------------------------ INI */

static int get_int(const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(SEC, key, def, s_ini);
}

static void get_str(const wchar_t *key, const wchar_t *def, wchar_t *buf, DWORD cap)
{
    GetPrivateProfileStringW(SEC, key, def, buf, cap, s_ini);
}

static BOOL put_int(const wchar_t *key, int v)
{
    wchar_t b[24];
    wfmt(b, CPW_COUNTOF(b), L"%d", v);
    return WritePrivateProfileStringW(SEC, key, b, s_ini);
}

static BOOL put_str(const wchar_t *key, const wchar_t *v)
{
    return WritePrivateProfileStringW(SEC, key, v, s_ini);
}

void config_load(app_settings *s)
{
    wchar_t b[64];
    config_defaults(s);

    get_str(L"Protocol", L"", b, CPW_COUNTOF(b));
    s->protocol = !_wcsicmp(b, L"DecimenV3")      ? CFG_PROTO_DECIMEN3
                : !_wcsicmp(b, L"DecimenV3-LT")   ? CFG_PROTO_DECIMEN3_LT
                : !_wcsicmp(b, L"CodeTransOptical-Bands") ? CFG_PROTO_OB_BANDS
                : !_wcsicmp(b, L"CodeTransOptical-Tiles") ? CFG_PROTO_OB_TILES
                : !_wcsicmp(b, L"CodeTrans")      ? CFG_PROTO_CODETRANS
                : !_wcsicmp(b, L"CodePass")       ? CFG_PROTO_CODETRANS   /* pre-rename name */
                                                  : s->protocol;   /* missing / unknown: default */
    s->version = get_int(L"Version", s->version);
    get_str(L"ECC", L"L", b, CPW_COUNTOF(b));
    switch (b[0]) {
    case L'M': case L'm': s->ecc = CFG_ECC_M; break;
    case L'Q': case L'q': s->ecc = CFG_ECC_Q; break;
    case L'H': case L'h': s->ecc = CFG_ECC_H; break;
    default: s->ecc = CFG_ECC_L; break;
    }
    s->module_px = get_int(L"ModulePx", s->module_px);
    s->code_count = get_int(L"Codes", s->code_count);
    s->fps = get_int(L"FPS", s->fps);
    get_str(L"ImageFormat", L"", b, CPW_COUNTOF(b));
    s->image_format = (!_wcsicmp(b, L"JPEG") || !_wcsicmp(b, L"JPG")) ? CFG_IMG_JPEG
                    : !_wcsicmp(b, L"PNG")                            ? CFG_IMG_PNG
                    : !_wcsicmp(b, L"Auto")                           ? CFG_IMG_AUTO
                                                                      : s->image_format;   /* missing: default */
    s->jpeg_quality = get_int(L"JpegQuality", s->jpeg_quality);
    s->topmost = get_int(L"Topmost", s->topmost);
    s->auto_popup = get_int(L"AutoPopup", s->auto_popup);
    s->pause_monitor = get_int(L"PauseMonitor", s->pause_monitor);
    s->force_builtin_snip = get_int(L"ForceBuiltinSnip", s->force_builtin_snip);
    s->skip_password_managers = get_int(L"SkipPasswordManagers", s->skip_password_managers);
    s->quiet_zone = get_int(L"QuietZone", s->quiet_zone);
    s->confirm_mb = get_int(L"ConfirmMB", s->confirm_mb);

    get_str(L"SnipHotkey", L"Win+Shift+S", b, CPW_COUNTOF(b));
    if (!hotkey_parse(b, &s->snip_mods, &s->snip_vk)) {
        s->snip_mods = MOD_WIN | MOD_SHIFT;
        s->snip_vk = 'S';
    }
    get_str(L"FallbackHotkey", L"Ctrl+Alt+S", b, CPW_COUNTOF(b));
    if (!hotkey_parse(b, &s->fallback_mods, &s->fallback_vk)) {
        s->fallback_mods = MOD_CONTROL | MOD_ALT;
        s->fallback_vk = 'S';
    }

    s->recv_auto_open = get_int(L"RecvAutoOpen", s->recv_auto_open);
    s->camera_index = get_int(L"CameraIndex", s->camera_index);
    s->settings_expanded = get_int(L"SettingsExpanded", s->settings_expanded);
    get_str(L"WindowX", L"", b, CPW_COUNTOF(b));
    wchar_t by[64];
    get_str(L"WindowY", L"", by, CPW_COUNTOF(by));
    if (b[0] && by[0]) {
        s->has_window_pos = TRUE;
        s->window_x = _wtoi(b);
        s->window_y = _wtoi(by);
    }
    config_validate(s);
}

BOOL config_save(const app_settings *s)
{
    static const wchar_t *ecc[] = { L"L", L"M", L"Q", L"H" };
    wchar_t b[64];
    BOOL ok = TRUE;
    ok &= put_str(L"Protocol", s->protocol == CFG_PROTO_DECIMEN3      ? L"DecimenV3"
                             : s->protocol == CFG_PROTO_DECIMEN3_LT   ? L"DecimenV3-LT"
                             : s->protocol == CFG_PROTO_OB_BANDS      ? L"CodeTransOptical-Bands"
                             : s->protocol == CFG_PROTO_OB_TILES      ? L"CodeTransOptical-Tiles"
                                                                      : L"CodeTrans");
    ok &= put_int(L"Version", s->version);
    ok &= put_str(L"ECC", ecc[CPW_CLAMP(s->ecc, 0, 3)]);
    ok &= put_int(L"ModulePx", s->module_px);
    ok &= put_int(L"Codes", s->code_count);
    ok &= put_int(L"FPS", s->fps);
    ok &= put_str(L"ImageFormat", s->image_format == CFG_IMG_JPEG ? L"JPEG"
                                : s->image_format == CFG_IMG_AUTO ? L"Auto" : L"PNG");
    ok &= put_int(L"JpegQuality", s->jpeg_quality);
    ok &= put_int(L"Topmost", s->topmost);
    ok &= put_int(L"AutoPopup", s->auto_popup);
    ok &= put_int(L"PauseMonitor", s->pause_monitor);
    ok &= put_int(L"ForceBuiltinSnip", s->force_builtin_snip);
    ok &= put_int(L"SkipPasswordManagers", s->skip_password_managers);
    ok &= put_int(L"QuietZone", s->quiet_zone);
    ok &= put_int(L"ConfirmMB", s->confirm_mb);
    hotkey_format(s->snip_mods, s->snip_vk, b, CPW_COUNTOF(b));
    ok &= put_str(L"SnipHotkey", b);
    hotkey_format(s->fallback_mods, s->fallback_vk, b, CPW_COUNTOF(b));
    ok &= put_str(L"FallbackHotkey", b);
    ok &= put_int(L"RecvAutoOpen", s->recv_auto_open);
    ok &= put_int(L"CameraIndex", s->camera_index);
    ok &= put_int(L"SettingsExpanded", s->settings_expanded);
    if (s->has_window_pos) {
        ok &= put_int(L"WindowX", s->window_x);
        ok &= put_int(L"WindowY", s->window_y);
    }
    return ok;
}

/* ------------------------------------------------------------------ autostart */

static void run_command(wchar_t *buf, size_t cap)
{
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, exe, (DWORD)CPW_COUNTOF(exe));
    if (n == 0 || n >= CPW_COUNTOF(exe))
        exe[0] = 0;
    wfmt(buf, cap, L"\"%ls\" --tray", exe);
}

BOOL autostart_is_enabled(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return FALSE;
    wchar_t val[MAX_PATH * 2 + 16];
    DWORD type = 0, cb = sizeof val - sizeof(wchar_t);
    LONG r = RegQueryValueExW(k, RUN_VALUE, NULL, &type, (BYTE *)val, &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return FALSE;
    val[cb / sizeof(wchar_t)] = 0;
    wchar_t want[MAX_PATH * 2 + 16];
    run_command(want, CPW_COUNTOF(want));
    /* Enabled only if it points at *this* exe (a moved portable exe = off). */
    return _wcsicmp(val, want) == 0;
}

BOOL autostart_set(BOOL enable)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) !=
        ERROR_SUCCESS)
        return FALSE;
    LONG r;
    if (enable) {
        wchar_t cmd[MAX_PATH * 2 + 16];
        run_command(cmd, CPW_COUNTOF(cmd));
        r = RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd,
                           (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(k, RUN_VALUE);
        if (r == ERROR_FILE_NOT_FOUND)
            r = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
