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
    CP_PROTO_PLAIN = 3    /* ordinary QR code                          */
};

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

/* Builds the CPO1 container (meta, optional raw deflate, SHA-256). NULL on error. */
cp_payload *cp_payload_text(const char *utf8, size_t n);
cp_payload *cp_payload_image(const uint8_t *data, size_t n, const char *mime);
cp_payload *cp_payload_files(size_t count,
                             const char *const *names,      /* UTF-8 basenames */
                             const uint8_t *const *datas,
                             const uint64_t *sizes);
/* Serialized container bytes (valid until cp_payload_free). */
const uint8_t *cp_payload_bytes(const cp_payload *p, size_t *n);
void cp_payload_free(cp_payload *p);

/* [additive] Summary of a built payload, e.g. for a "12.3KB -> 4.1KB" status line. */
typedef struct cp_payload_info {
    int      kind;           /* CP_KIND_*                                  */
    uint64_t raw_len;        /* uncompressed body length                   */
    uint64_t container_len;  /* serialized CPO1 container length (= F)     */
    int      compressed;     /* 1 if the body is raw-deflate compressed    */
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
} cp_tx_info;

/* qr_capacity = byte-mode capacity of one QR code. opts may be NULL.
 * CP_PROTO_D3: every frame fills the whole capacity (blockLen = capacity - 22);
 * the CPO1 payload is re-packed as a Decimen DCF2 container (text -> snippet,
 * several files -> stored zip), content limit 64 MiB, k <= 65535. F / T /
 * total_source_symbols report the DCF2 length / blockLen / k, Z = 0. */
cp_tx *cp_tx_new(int proto, const cp_payload *p, size_t qr_capacity, const cp_tx_opts *opts);
/* Writes the next frame (one QR code's binary content). Returns its length, 0 on error. */
size_t cp_tx_next(cp_tx *tx, uint8_t *buf, size_t cap);
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

/* ---- utilities exposed for tests / platform code ---------------------- */

uint32_t cp_crc32(const uint8_t *data, size_t n);                 /* IEEE 802.3 */
void     cp_sha256(const uint8_t *data, size_t n, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif /* CODETRANS_CP_H */
