/* config.h - all user settings (PLAN.md §6.2) and INI persistence (§6.8).
 *
 * The INI lives next to the exe (codetrans.ini, portable). If that directory
 * is not writable we fall back to %APPDATA%\CodeTrans\codetrans.ini.
 * "开机自启" is not stored in the INI: the HKCU Run value is the source of
 * truth (autostart_is_enabled / autostart_set). */
#ifndef CPW_CONFIG_H
#define CPW_CONFIG_H

#include <windows.h>

/* Decimen v3 comes in two outer codes: Cauchy RS-96 (0.5.3-fixed, faster under
 * loss, needs a receiver that knows flag 0x02) and the stock v0.5.3 LT carousel
 * (any v3 receiver, e.g. an unmodified decimen.app). */
enum { CFG_PROTO_CODETRANS = 0, CFG_PROTO_DECIMEN3 = 1, CFG_PROTO_DECIMEN3_LT = 2 };
#define CFG_PROTO_IS_DECIMEN(p) ((p) == CFG_PROTO_DECIMEN3 || (p) == CFG_PROTO_DECIMEN3_LT)
enum { CFG_ECC_L = 0, CFG_ECC_M = 1, CFG_ECC_Q = 2, CFG_ECC_H = 3 };
enum { CFG_IMG_PNG = 0, CFG_IMG_JPEG = 1 };

#define CFG_VERSION_MIN   5
#define CFG_VERSION_MAX   40
#define CFG_MODULE_MIN    1
#define CFG_MODULE_MAX    16
#define CFG_FPS_MIN       1
#define CFG_FPS_MAX       60
#define CFG_QUIET_MIN     1
#define CFG_QUIET_MAX     4
#define CFG_JPEGQ_MIN     50
#define CFG_JPEGQ_MAX     95
#define CFG_HARD_CAP_BYTES (256ull * 1024 * 1024)   /* §6.4 total size hard cap */

typedef struct app_settings {
    /* --- settings bar --- */
    int  protocol;          /* CFG_PROTO_*            default Decimen RS96 -> new session */
    int  version;           /* QR version 5..40       default 40         -> new session */
    int  ecc;               /* CFG_ECC_*              default L          -> new session */
    int  module_px;         /* 1..16 physical px      default 4          -> keep session */
    int  code_count;        /* 1/2/3/4/6              default 2          -> keep session */
    int  fps;               /* 1..60                  default 30         -> keep session */
    int  image_format;      /* CFG_IMG_*              default PNG        -> new session */
    BOOL topmost;           /* default on */
    BOOL auto_popup;        /* default on */
    BOOL pause_monitor;     /* default off (also in tray) */
    /* --- tray --- */
    BOOL force_builtin_snip;/* default off: take over Win+Shift+S even on 1703+ */
    /* --- INI only --- */
    int  jpeg_quality;      /* 50..95, default 85     -> new session */
    BOOL skip_password_managers; /* default on */
    int  quiet_zone;        /* 1..4 modules, default 4 -> keep session */
    int  confirm_mb;        /* large-content confirm threshold, MB, default 20 */
    UINT snip_mods, snip_vk;          /* MOD_* (no MOD_NOREPEAT) + VK; default Win+Shift+S */
    UINT fallback_mods, fallback_vk;  /* used when the snip hotkey is taken; default Ctrl+Alt+S */
    /* --- UI state --- */
    BOOL settings_expanded; /* settings bar open */
    BOOL has_window_pos;
    int  window_x, window_y;
} app_settings;

void config_defaults(app_settings *s);
void config_validate(app_settings *s);   /* clamp everything into range */

/* Resolve the INI path (exe dir, else %APPDATA%\CodeTrans). */
BOOL config_init(void);
void config_set_path(const wchar_t *path);   /* tests */
const wchar_t *config_path(void);

void config_load(app_settings *s);       /* defaults for missing keys, validated */
BOOL config_save(const app_settings *s);

/* "Win+Shift+S" <-> (MOD_WIN|MOD_SHIFT, 'S'). Modifier names: Win, Ctrl
 * (Control), Shift, Alt. Keys: A-Z, 0-9, F1-F24, PrintScreen, Pause, Insert,
 * Delete, Home, End, PageUp, PageDown, Space. */
BOOL hotkey_parse(const wchar_t *s, UINT *mods, UINT *vk);
void hotkey_format(UINT mods, UINT vk, wchar_t *buf, size_t cap);

/* HKCU\Software\Microsoft\Windows\CurrentVersion\Run\CodeTrans */
BOOL autostart_is_enabled(void);
BOOL autostart_set(BOOL enable);

#endif
