/*
 * CodeTrans shared core — public API (C11, no platform deps).
 *
 * CONTRACT FILE: written before implementation so core / Android JNI / Windows
 * can be developed in parallel. Do not change existing signatures or numeric
 * values without updating every consumer. Additive changes are fine.
 *
 * Wire formats: see core/PROTOCOL.md (CodeTrans v1 frame, CPO1 container).
 */
#ifndef CODETRANS_CP_H
#define CODETRANS_CP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- constants -------------------------------------------------------- */

enum {
    CP_PROTO_NONE  = 0,
    CP_PROTO_RQ    = 1,   /* CodeTrans v1, RaptorQ (RFC 6330)          */
    CP_PROTO_D3    = 2,   /* Decimen v3: LT carousel or Cauchy RS-96   */
    CP_PROTO_PLAIN = 3,   /* ordinary QR code                          */
    CP_PROTO_OB    = 4    /* [additive] CodeTrans optical board (experimental,
                             not a QR code; needs a CodeTrans receiver)  */
};

/* [additive] Optical board profiles (cp_tx_opts.ob_profile). One board is
 * CP_OB_SIDE x CP_OB_SIDE modules, the footprint of a version 40 QR code.
 *   bands: 6 horizontal units x 512 B (3072 B per board), clear, frontal links
 *   tiles: 24 rectangular units x 108 B (2592 B per board), tolerant of tilted
 *          rolling-shutter seams that cut across several units */
enum {
    CP_OB_BANDS = 0,
    CP_OB_TILES = 1
};
#define CP_OB_SIDE       177
#define CP_OB_MAX_BYTES  (64ull << 20)   /* container cap, optical board */

enum {
    CP_KIND_NONE  = 0,
    CP_KIND_TEXT  = 1,
    CP_KIND_IMAGE = 2,
    CP_KIND_FILES = 3
};

/* Event bits returned by cp_rx_push() (OR-ed). */
enum {
    CP_EV_PROGRESS    = 1 << 0,  /* a new, unique symbol was accepted        */
    CP_EV_COMPLETE    = 1 << 1,  /* a result is ready: cp_rx_take_result()   */
    CP_EV_PLAIN       = 1 << 2,  /* plain QR text ready: cp_rx_take_plain()  */
    CP_EV_FAILED      = 1 << 3,  /* session failed (SHA-256 / container); restarted */
    CP_EV_UNSUPPORTED = 1 << 4   /* recognised but unusable: Decimen v1/v2/v4+, an
                                    unknown must-understand flag, or too large */
};

#define CP_FRAME_OVERHEAD      28u              /* CodeTrans v1 header + crc32 */
#define CP_MAX_TOTAL_BYTES     (256ull << 20)   /* hard cap, CodeTrans        */
#define CP_MAX_TOTAL_BYTES_D3  (64ull << 20)    /* hard cap, Decimen         */
#define CP_MAX_T               4096u
#define CP_MAX_Z               255u
#define CP_MAX_K               4096u            /* sender block size cap     */

/* All heap memory handed out by this library is released with cp_free(). */
void cp_free(void *p);

/* ---- payload (sender side) -------------------------------------------- */

typedef struct cp_payload cp_payload;

/* ---- text and file names (shared by both clients) ---------------------- */

/* [additive] 1 if s[0..n) is strictly valid UTF-8 (no overlongs, surrogates, > U+10FFFF). */
int cp_utf8_valid(const uint8_t *s, size_t n);
/* [additive] 3 if the text starts with a UTF-8 BOM (EF BB BF), else 0. */
size_t cp_text_bom_len(const uint8_t *s, size_t n);

#define CP_NAME_MAX 120u   /* max sanitized file name length in bytes */

/* [additive] Sanitizes an untrusted UTF-8 file name into out (cap >= CP_NAME_MAX + 1):
 * control / bidi chars dropped, reserved chars and ".." -> "_", edge spaces and dots
 * trimmed, Windows device names (CON, NUL, COM1, ...) prefixed with "_", empty -> "file",
 * length-capped on a UTF-8 boundary keeping a short extension. */
void cp_sanitize_name(const uint8_t *in, size_t n, char *out, size_t cap);
/* [additive] Makes name unique among names[0..count) by inserting " (1)", " (2)", ...
 * before the extension. The comparison is case-insensitive (ASCII, Latin-1, Latin
 * Extended-A, Greek, Cyrillic). In place, cap >= CP_NAME_MAX + 1. */
void cp_unique_name(char *name, size_t cap, char *const *names, size_t count);

/* Builds the CPO1 container (meta, optional raw deflate, SHA-256). NULL on error. */
cp_payload *cp_payload_text(const char *utf8, size_t n);
cp_payload *cp_payload_image(const uint8_t *data, size_t n, const char *mime);
cp_payload *cp_payload_files(size_t count,
                             const char *const *names,      /* UTF-8; sanitized and de-duplicated here */
                             const uint8_t *const *datas,
                             const uint64_t *sizes);
/* Serialized container bytes (valid until cp_payload_free). */
const uint8_t *cp_payload_bytes(const cp_payload *p, size_t *n);
void cp_payload_free(cp_payload *p);

/* [additive] How a body was compressed. The sender picks, per payload, the
 * deflate configuration that makes it smallest (sampled from its start, middle
 * and end), or stores it when nothing saves 5 %. Every choice is plain raw
 * deflate on the wire, so receivers need not know which one was used. */
enum {
    CP_COMP_STORE    = 0,   /* not compressed                                */
    CP_COMP_HUFFMAN  = 1,   /* Huffman coding only (noise-like, skewed bytes) */
    CP_COMP_RLE      = 2,   /* run-length matches only (sparse / flat data)   */
    CP_COMP_FAST     = 3,   /* level 1                                       */
    CP_COMP_DEFAULT  = 4,   /* level 6                                       */
    CP_COMP_FILTERED = 5,   /* level 9, short matches filtered (tables)      */
    CP_COMP_MAX      = 6    /* level 9 / exhaustive (small inputs)           */
};
const char *cp_comp_name(int method);   /* "store", "deflate-9", ... */

/* [additive] Summary of a built payload, e.g. for a "12.3KB -> 4.1KB" status line. */
typedef struct cp_payload_info {
    int      kind;           /* CP_KIND_*                                  */
    uint64_t raw_len;        /* uncompressed body length                   */
    uint64_t container_len;  /* serialized CPO1 container length (= F)     */
    int      compressed;     /* 1 if the body is raw-deflate compressed    */
    int      method;         /* [additive] CP_COMP_*                       */
} cp_payload_info;
void cp_payload_info_get(const cp_payload *p, cp_payload_info *out);

/* ---- tx (sender) ------------------------------------------------------ */

typedef struct cp_tx cp_tx;

/* Decimen v3 outer code (cp_tx_opts.d3_fec). */
enum {
    CP_D3_FEC_RS96 = 0,    /* interleaved Cauchy RS-96 (0.5.3-fixed, flag 0x02) */
    CP_D3_FEC_LT   = 1     /* stock v0.5.3 LT carousel                        */
};

typedef struct cp_tx_opts {
    uint32_t session_id;   /* 0 = random (Decimen: low 16 bits, never 0)       */
    int      d3_fec;       /* [additive] CP_D3_FEC_*, CP_PROTO_D3 only         */
    int      ob_profile;   /* [additive] CP_OB_*, CP_PROTO_OB only             */
} cp_tx_opts;

typedef struct cp_tx_info {
    uint32_t session_id;
    uint64_t F;            /* transfer length (container bytes) */
    uint16_t T;            /* symbol size */
    uint8_t  Z;            /* source blocks */
    uint32_t total_source_symbols;  /* sum of K_i */
    /* [additive] */
    int      proto;        /* CP_PROTO_*                                       */
    int      d3_gzip;      /* Decimen: DCF2 body is gzip-compressed            */
    int      ob_profile;   /* [additive] CP_OB_*, CP_PROTO_OB only             */
} cp_tx_info;

/* qr_capacity = byte-mode capacity of one QR code. opts may be NULL.
 * CP_PROTO_D3: every frame fills the whole capacity (blockLen = capacity - 22);
 * the CPO1 payload is re-packed as a Decimen DCF2 container (text -> snippet,
 * several files -> stored zip), content limit 64 MiB, k <= 65535. F / T /
 * total_source_symbols report the DCF2 length / blockLen / k, Z = 0. */
cp_tx *cp_tx_new(int proto, const cp_payload *p, size_t qr_capacity, const cp_tx_opts *opts);
/* Writes the next frame (one QR code's binary content). Returns its length, 0 on error.
 * Not used for CP_PROTO_OB (returns 0): use cp_tx_next_board(). */
size_t cp_tx_next(cp_tx *tx, uint8_t *buf, size_t cap);
/* [additive] CP_PROTO_OB (qr_capacity is ignored by cp_tx_new): renders the next
 * display refresh as `boards` (1..16) boards of CP_OB_SIDE^2 modules, one byte
 * per module, 1 = dark, 0 = light, rows top-down; board b starts at
 * out + b * CP_OB_SIDE * CP_OB_SIDE. Each board needs its own quiet zone of
 * light modules around it. F / T / total_source_symbols report the container
 * length / unit payload size / source rows, Z = 0. Returns 1, 0 on error. */
int    cp_tx_next_board(cp_tx *tx, int boards, uint8_t *out);
void cp_tx_info_get(const cp_tx *tx, cp_tx_info *out);
void cp_tx_free(cp_tx *tx);

/* ---- rx (receiver) ---------------------------------------------------- */

typedef struct cp_rx cp_rx;

typedef struct cp_rx_config {
    const char *out_dir;    /* results go to <out_dir>/<session_id as 8 hex>/  */
    uint64_t    max_bytes;  /* 0 = CP_MAX_TOTAL_BYTES; clamped to the hard caps */
    int         enable_d3;  /* receive Decimen v3 frames (LT and Cauchy RS-96) */
    int         enable_plain; /* report ordinary QR codes                      */
} cp_rx_config;

cp_rx *cp_rx_new(const cp_rx_config *cfg);
void   cp_rx_free(cp_rx *rx);
void   cp_rx_reset(cp_rx *rx);          /* drop all sessions, keep config */

/* [additive] Thread-safe. Changes enable_d3 / enable_plain immediately without
 * dropping sessions or progress (settings change while receiving). */
void   cp_rx_set_options(cp_rx *rx, int enable_d3, int enable_plain);

/* Thread-safe. data = raw bytes of one decoded QR code. Returns CP_EV_* bits. */
int    cp_rx_push(cp_rx *rx, const uint8_t *data, size_t n);

/* Scanner reports how many QR codes it found in the last camera frame (thread-safe). */
void   cp_rx_note_frame(cp_rx *rx, int codes_in_frame);

typedef struct cp_progress {
    int      proto;          /* CP_PROTO_* of the most recently active session, NONE if idle */
    int      kind;           /* CP_KIND_* once known (after container header decoded), else NONE */
    uint64_t have;           /* sum min(received_i, K_i)                         */
    uint64_t need;           /* sum K_i                                           */
    uint64_t total_bytes;    /* F (container size)                                */
    uint64_t bytes_per_sec;  /* unique symbol bytes over the last ~2 s            */
    int      codes_in_frame; /* last value from cp_rx_note_frame                  */
    uint32_t session_id;
} cp_progress;

void cp_rx_progress(cp_rx *rx, cp_progress *out);

/* [additive] Diagnostics (developer builds). Counters run from cp_rx_new and
 * survive cp_rx_reset. Thread-safe. */
typedef struct cp_rx_stats {
    uint64_t frames;       /* CodeTrans / Decimen frames and optical board units pushed */
    uint64_t unique;       /* ... that carried new information                         */
    uint64_t duplicates;   /* ... already received: the same code scanned again, incl.
                              frames of a transfer that already completed              */
    uint64_t redundant;    /* Decimen: new frame / row that added nothing (linear
                              combination of what was known)                           */
    /* Decimen Cauchy RS-96 stream most recently fed (all 0 when none): */
    uint32_t rs96_k;       /* source rows                                              */
    uint32_t rs96_rows;    /* distinct coded rows received                             */
    uint32_t rs96_useful;  /* rows that carried new information (rank so far)          */
    uint32_t rs96_solved;  /* source rows recovered                                    */
    /* Optical board stream most recently fed (all 0 when none): */
    uint32_t ob_mode;      /* CP_OB_BANDS + 1 or CP_OB_TILES + 1                        */
    uint32_t ob_k;         /* source rows                                              */
    uint32_t ob_rows;      /* distinct rows received (each one was new information)    */
    uint32_t ob_groups;    /* groups of the outer code the rows are split into         */
    uint32_t ob_solved;    /* groups recovered                                         */
} cp_rx_stats;
void cp_rx_stats_get(cp_rx *rx, cp_rx_stats *out);

/* Plain QR: returns malloc'd raw bytes (free with cp_free), NULL if none pending.
 * Identical consecutive content is reported only once. */
uint8_t *cp_rx_take_plain(cp_rx *rx, size_t *n);

typedef struct cp_result {
    int       kind;           /* CP_KIND_*                                     */
    char      mime[128];      /* e.g. "text/plain", "image/png", "application/octet-stream" */
    uint32_t  session_id;
    char     *text;           /* TEXT only: UTF-8, NUL-terminated; full text if <= 1 MiB,
                                 otherwise the first 64 KiB cut at a UTF-8 boundary */
    size_t    text_len;
    int       text_truncated;
    size_t    count;          /* number of files written (TEXT: 1 = text.txt, IMAGE: 1) */
    char    **paths;          /* absolute paths inside <out_dir>/<session>/     */
    char    **names;          /* sanitized display names                        */
    uint64_t *sizes;
} cp_result;

/* Returns the pending result (ownership to caller) or NULL. */
cp_result *cp_rx_take_result(cp_rx *rx);
void cp_result_free(cp_result *r);

/* ---- optical board receive path [additive] ------------------------------
 * Camera frames are 8-bit luminance planes, row stride == w. A board quad
 * holds the frame-pixel positions of the board's corners in board order:
 * module grid (0,0), (177,0), (177,177), (0,177) = top-left, top-right,
 * bottom-right, bottom-left of the board as drawn (any rotation in the frame). */
typedef struct cp_ob_quad {
    float x[4], y[4];
    float score;             /* pilot correlation, >= 0.68 when accepted */
} cp_ob_quad;

/* Cold search of a whole frame for boards with a visible light quiet zone.
 * Returns the number of boards written to out (<= max). */
int  cp_ob_locate(const uint8_t *y, int w, int h, cp_ob_quad *out, int max);
/* Re-fits a quad found in an earlier frame to this frame (small motion).
 * Updates *q and returns 1 while the board is still locked, else 0. */
int  cp_ob_track(const uint8_t *y, int w, int h, cp_ob_quad *q);
/* Bilinear samples at the CP_OB_SIDE^2 module centres of q, row-major. */
void cp_ob_sample(const uint8_t *y, int w, int h, const cp_ob_quad *q, float *luma);
/* Decodes every data unit of one sampled board (cp_ob_sample) that passes its
 * checks and feeds it to rx. Thread-safe; heavy work runs outside rx's lock.
 * Returns CP_EV_* bits; *units (optional) = units accepted. */
int  cp_rx_push_board(cp_rx *rx, const float *luma, int *units);

/* ---- sender rendering, geometry, pacing and ETA [additive] ---------------
 * Shared by the Windows and Android senders (core/src/tx_render.c). */

/* QR codes: error-correction levels and the version range the senders offer.
 * The capacity table covers every version 1..40. */
enum { CP_ECC_L = 0, CP_ECC_M = 1, CP_ECC_Q = 2, CP_ECC_H = 3 };
#define CP_QR_VERSION_MIN  5
#define CP_QR_VERSION_MAX  40
#define CP_MAX_CODES       16       /* codes / boards per display frame, upper bound */

/* Optical boards: units per board, payload bytes per unit and refreshes per
 * manifest beacon (one unit slot of the whole refresh carries the manifest). */
#define CP_OB_BANDS_UNITS         6
#define CP_OB_BANDS_UNIT_BYTES    512
#define CP_OB_BANDS_BEACON_EVERY  16
#define CP_OB_TILES_UNITS         24
#define CP_OB_TILES_UNIT_BYTES    108
#define CP_OB_TILES_BEACON_EVERY  8
int    cp_ob_units_per_board(int profile);   /* 0 for an unknown profile */
size_t cp_ob_unit_bytes(int profile);
size_t cp_ob_board_bytes(int profile);       /* units x unit bytes       */

/* Byte-mode payload capacity of one QR code (0 for an invalid version / ecc). */
size_t cp_qr_capacity(int ver, int ecc);
/* Modules per side of a version: 4 * ver + 17 (0 for an invalid version). */
int    cp_qr_modules(int ver);
/* 1 for the code counts a frame may hold: 1, 2, 3, 4 and 6. */
int    cp_code_count_ok(int count);

/* One display frame: `count` codes on a grid, one byte per module, rows of
 * `stride` bytes (1 = dark, 0 = light). Quiet zones and padding stay 0. */
typedef struct cp_frame_geom {
    int ver, count, quiet;
    int cols, rows;
    int modules;        /* modules per code side                       */
    int cell;           /* modules + 2 * quiet                         */
    int w, h;           /* whole frame, in modules                     */
    int stride;         /* bytes per row, w rounded up to stride_align */
} cp_frame_geom;

/* cols == 0: the standard grid for count 1/2/3/4/6 (1x1, 2x1, 3x1, 2x2, 3x2);
 * any other count falls back to 1. cols >= 1: count (1..CP_MAX_CODES) codes in
 * that many columns. stride_align: 4 for Windows DIB rows, 1 for packed rows.
 * Optical boards use ver 40 (CP_OB_SIDE = 4 * 40 + 17). Returns 0 on bad input. */
int  cp_frame_geom_calc(cp_frame_geom *g, int ver, int count, int quiet, int cols, int stride_align);
/* Top-left module of code i inside the frame. */
void cp_frame_geom_origin(const cp_frame_geom *g, int i, int *x, int *y);

/* Mask policy: at fps x count >= 90 codes per second the automatic mask search
 * (~4x slower) is skipped for mask 0. Returns 1 for a fixed mask. */
int  cp_fixed_mask(int fps, int count);

/* Pulls g->count packets (QR) or one refresh of g->count boards (CP_PROTO_OB,
 * needs g->modules == CP_OB_SIDE) from tx and draws them into pix
 * (g->stride * g->h bytes, fully overwritten). ecc: CP_ECC_*. fixed_mask:
 * cp_fixed_mask(). Returns 1, or 0 on error (tx exhausted / code too small). */
int  cp_tx_render_frame(cp_tx *tx, const cp_frame_geom *g, int ecc, int fixed_mask, uint8_t *pix);

/* Source symbols one display refresh of `count` codes / boards delivers:
 * count for QR, count * units - 1/beacon-interval for boards. */
double cp_tx_units_per_frame(const cp_tx_info *info, int count);
/* Seconds to show enough symbols for the receiver (K, plus 2 per source block
 * for RaptorQ) at `fps` refreshes per second. 0 when it cannot be computed. */
double cp_tx_eta_seconds(const cp_tx_info *info, int count, double fps);

/* Frame pacing on a display refreshing at refresh_hz: a frame lasts a whole
 * number of vblanks, round(refresh_hz / fps) but at least 1. The platform
 * timing loops stay per-platform; both use these numbers. */
int    cp_pace_vblanks(int fps, double refresh_hz);
double cp_pace_effective_fps(int fps, double refresh_hz);   /* refresh_hz / vblanks */

/* ---- utilities exposed for tests / platform code ---------------------- */

uint32_t cp_crc32(const uint8_t *data, size_t n);                 /* IEEE 802.3 */
void     cp_sha256(const uint8_t *data, size_t n, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif /* CODETRANS_CP_H */
