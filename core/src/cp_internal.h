/*
 * CodeTrans core — internal declarations shared between the core/src sources and the
 * desktop tests. Not part of the public API (see include/cp.h).
 */
#ifndef CODETRANS_CP_INTERNAL_H
#define CODETRANS_CP_INTERNAL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- byte order helpers (all CodeTrans wire fields are big-endian) ------ */

static inline void cp_be16w(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void cp_be32w(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void cp_be64w(uint8_t *p, uint64_t v)
{
    cp_be32w(p, (uint32_t)(v >> 32));
    cp_be32w(p + 4, (uint32_t)v);
}
static inline uint16_t cp_be16r(const uint8_t *p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static inline uint32_t cp_be32r(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t cp_be64r(const uint8_t *p) { return ((uint64_t)cp_be32r(p) << 32) | cp_be32r(p + 4); }

/* ---- crc32 / sha256 ---------------------------------------------------- */

uint32_t cp_crc32_update(uint32_t crc, const uint8_t *data, size_t n);

typedef struct cp_sha256_ctx {
    uint32_t h[8];
    uint64_t len;      /* bytes processed */
    uint8_t  buf[64];
    size_t   buf_len;
} cp_sha256_ctx;

void cp_sha256_init(cp_sha256_ctx *c);
void cp_sha256_update(cp_sha256_ctx *c, const uint8_t *data, size_t n);
void cp_sha256_final(cp_sha256_ctx *c, uint8_t out[32]);

/* ---- CodeTrans v1 frame (frame_cp.c) ----------------------------------- */

#define CP_MAGIC0         0xC0u
#define CP_MAGIC1         0xDEu
#define CP_VERSION        1u
#define CP_FLAGS_MUST     0x0Fu      /* low nibble: must-understand bits   */
#define CP_HDR_LEN        24u
#define CP_MAX_ESI        0x00FFFFFFu

#define D3_MAGIC0         0xD1u
#define D3_MAGIC1         0xC3u
#define D3_VERSION        3u
#define D3_HDR_LEN        22u
#define D3_FLAGS_MUST     0x0Fu
#define D3_FLAG_ENCRYPTED 0x01u      /* never supported: refuse loudly      */
#define D3_FLAG_CAUCHY96  0x02u      /* interleaved Cauchy RS outer code    */
#define D3_FLAGS_KNOWN    D3_FLAG_CAUCHY96
#define D3_MAX_BLOCK_RS   (2953u - D3_HDR_LEN)  /* one V40-L QR minus header */
#define D3_FILE_HDR_LEN   49u        /* DCF2 container header               */
#define D3_MAX_K          0xFFFFu
/* largest container a stream may declare: file + header + u16 name + u16 type */
#define D3_MAX_TOTAL      (CP_MAX_TOTAL_BYTES_D3 + D3_FILE_HDR_LEN + 2u * 0xFFFFu)

/* Validated FEC object transmission information. */
typedef struct cp_oti {
    uint64_t F;     /* transfer length, 1 .. 2^40-1 (further capped by rx)   */
    uint16_t T;     /* symbol size, 1 .. CP_MAX_T, multiple of Al             */
    uint8_t  Z;     /* number of source blocks, 1 .. CP_MAX_Z                 */
    uint16_t N;     /* number of sub-blocks, v1: always 1                     */
    uint8_t  Al;    /* symbol alignment, one of 1, 2, 4, 8                    */
} cp_oti;

typedef struct cp_frame {
    uint8_t  flags;
    uint32_t session_id;
    cp_oti   oti;
    uint8_t  sbn;
    uint32_t esi;
    const uint8_t *symbol;   /* points into the parsed buffer, T bytes */
} cp_frame;

enum {
    CP_FRAME_OK = 0,
    CP_FRAME_ERR_SHORT,      /* < 28 bytes                                  */
    CP_FRAME_ERR_MAGIC,
    CP_FRAME_ERR_VERSION,
    CP_FRAME_ERR_FLAGS,      /* unknown must-understand flag bits          */
    CP_FRAME_ERR_LENGTH,     /* length != 28 + T                           */
    CP_FRAME_ERR_CRC,
    CP_FRAME_ERR_OTI,        /* OTI outside protocol limits                */
    CP_FRAME_ERR_TOO_BIG,    /* F > caller's max_F (frame otherwise valid) */
    CP_FRAME_ERR_PAYLOAD_ID  /* SBN >= Z                                   */
};

/* Number of source symbols of the whole object: ceil(F / T). */
static inline uint64_t cp_oti_Kt(const cp_oti *o) { return (o->F + o->T - 1) / o->T; }

/* RFC 6330 §4.4.1.2 Partition[Kt, Z]: the first ZL blocks have KL symbols,
 * the remaining ZS blocks KS symbols. */
void     cp_oti_partition(const cp_oti *o, uint32_t *KL, uint32_t *KS, uint32_t *ZL);
uint32_t cp_oti_block_K(const cp_oti *o, uint8_t sbn);
uint64_t cp_oti_block_offset(const cp_oti *o, uint8_t sbn);  /* byte offset in transfer */
uint64_t cp_oti_block_bytes(const cp_oti *o, uint8_t sbn);   /* bytes of transfer in block */

/* Protocol-level OTI validation (independent of rx max_bytes). Returns 1 if OK. */
int cp_oti_valid(const cp_oti *o);

/* Parses + validates one frame. max_F = 0 means CP_MAX_TOTAL_BYTES. */
int cp_frame_parse(const uint8_t *d, size_t n, uint64_t max_F, cp_frame *out);

/* Writes header + symbol + crc. Returns bytes written (28 + T) or 0. */
size_t cp_frame_write(uint8_t *buf, size_t cap, uint32_t session_id, const cp_oti *o,
                      uint8_t sbn, uint32_t esi, const uint8_t *symbol);

/* ---- CPO1 container (container.c) ------------------------------------- */

#define CPO_HDR_LEN        48u
#define CPO_FLAG_DEFLATE   0x01u
#define CPO_FLAGS_KNOWN    0x01u
#define CPO_MAX_FILES      4096u
#define CPO_MAX_NAME       4096u    /* longest entry name accepted on rx */

typedef struct cp_container_hdr {
    int      kind;
    uint8_t  flags;
    uint16_t meta_len;
    uint64_t raw_len;
    uint8_t  sha256[32];
    const uint8_t *meta;     /* meta_len bytes                 */
    const uint8_t *body;     /* (possibly compressed) body     */
    size_t   body_len;
} cp_container_hdr;

/* Header-level validation of a complete container. max_raw = cap for raw_len. */
int cp_container_parse(const uint8_t *d, size_t n, uint64_t max_raw, cp_container_hdr *h);

/* Full decode: parse, bounded inflate, length + SHA-256 check. Returns 1 on
 * success: *raw points at h->raw_len body bytes; *owned is the malloc'd
 * buffer behind it (NULL when *raw is a view into d, i.e. not compressed). */
int cp_container_decode(const uint8_t *d, size_t n, uint64_t max_raw,
                        cp_container_hdr *h, uint8_t **owned, const uint8_t **raw);

/* Looks up "key=value" in meta; copies value (NUL-terminated, truncated). */
int cp_meta_get(const uint8_t *meta, size_t len, const char *key, char *out, size_t cap);

typedef struct cp_file_entry {
    const uint8_t *name;
    size_t         name_len;
    const uint8_t *data;
    uint64_t       size;
} cp_file_entry;

/* Iterates FILES body entries. Returns 1 = entry produced, 0 = clean end,
 * -1 = malformed. *off starts at 0. */
int cp_files_next(const uint8_t *body, size_t len, size_t *off, cp_file_entry *e);

/* Detects already-compressed content by magic bytes. */
int cp_is_compressed_magic(const uint8_t *d, size_t n);
/* Detects already-compressed content by file extension (png, zip, mp4, ...). */
int cp_is_compressed_name(const char *name);
/* Adaptive raw deflate (container.c): the smallest of several deflate
 * configurations, tried on a sample. NULL (and *method = CP_COMP_STORE) when
 * storing is better or memory runs out; free the result with mz_free. */
uint8_t *cp_deflate_auto(const uint8_t *src, size_t n, size_t *out_n, int *method);

/* ---- filenames (output.c) --------------------------------------------- */

/* cp_sanitize_name / cp_unique_name / CP_NAME_MAX are public (cp.h). */

/* Decodes one strict UTF-8 code point (no overlong, surrogates or > U+10FFFF);
 * returns its length, or 0 if invalid / truncated (container.c). */
size_t cp_utf8_decode(const uint8_t *s, size_t n, uint32_t *cp);
/* MIME of a strictly recognised image signature (PNG, JPEG, GIF, WebP, BMP), or NULL. */
const char *cp_image_mime_sniff(const uint8_t *d, size_t n);

/* ---- RaptorQ wrapper (fec_rq.c) --------------------------------------- */

typedef struct fec_enc fec_enc;
/* data must stay valid for the encoder's lifetime. */
fec_enc *fec_enc_new(const uint8_t *data, const cp_oti *o);
/* Writes encoding symbol (sbn, esi) into out (T bytes). 1 = ok, 0 = error. */
int      fec_enc_symbol(fec_enc *e, uint8_t sbn, uint32_t esi, uint8_t *out);
void     fec_enc_free(fec_enc *e);

typedef struct fec_dec fec_dec;
/* Per-block decoder; recovered source bytes are written to block_buf
 * (cp_oti_block_bytes(o, sbn) bytes). */
fec_dec *fec_dec_new(const cp_oti *o, uint8_t sbn, uint8_t *block_buf);
/* 1 = added, 0 = duplicate / ignored, -1 = error. */
int      fec_dec_add(fec_dec *d, uint32_t esi, const uint8_t *symbol);
/* 1 = block recovered into block_buf, 0 = not yet decodable, -1 = error. */
int      fec_dec_try(fec_dec *d);
void     fec_dec_free(fec_dec *d);

/* ---- Decimen v3 (d3.c) -------------------------------------------------
 * Wire format of Decimen Optical Transfer v0.5.3 and its "fixed geometry"
 * 0.5.3-fixed.1 variant: 22-byte little-endian header, either the systematic
 * LT carousel (flags & 0x02 == 0) or the interleaved Cauchy RS-96 outer code
 * (flags & 0x02), DCF2 container with optional gzip and SHA-256. */

enum {
    D3_OK = 0,
    D3_FOREIGN,              /* not a Decimen frame at all (silent)         */
    D3_OLDER_SENDER,         /* v1/v2 or a lower version byte               */
    D3_NEWER_SENDER,
    D3_UNSUPPORTED_FLAGS,    /* unknown must-understand bit (e.g. encrypted) */
    D3_MALFORMED             /* ours, but inconsistent (silent)             */
};

typedef struct d3_header {
    uint8_t  flags;
    uint16_t session_id;
    uint32_t seq;
    uint16_t k;
    uint16_t block_len;
    uint32_t total_len;
    uint32_t fnv;
} d3_header;

/* Classifies one QR payload; fills *h (if not NULL) when D3_OK. The block is
 * d + D3_HDR_LEN, h->block_len bytes. */
int      d3_classify(const uint8_t *d, size_t n, d3_header *h);
size_t   d3_frame_write(uint8_t *buf, size_t cap, const d3_header *h, const uint8_t *block);
uint32_t d3_fnv1a(const uint8_t *d, size_t n);

typedef struct d3_enc d3_enc;
/* payload is copied. block_len 1..65535; k = ceil(n / block_len) must fit u16
 * (and block_len <= D3_MAX_BLOCK_RS for Cauchy). NULL on error. */
d3_enc  *d3_enc_new(const uint8_t *payload, size_t n, uint16_t block_len, uint16_t session_id, int cauchy);
uint32_t d3_enc_k(const d3_enc *e);
void     d3_enc_block(d3_enc *e, uint32_t seq, uint8_t *out);   /* block_len bytes */
void     d3_enc_free(d3_enc *e);

typedef struct d3_dec d3_dec;
typedef struct d3_dec_stats {
    uint32_t k;
    uint32_t solved;         /* source blocks known                          */
    uint32_t useful;         /* distinct frames that carried new information */
    uint32_t frames_new;     /* distinct seqs (LT) / distinct rows (Cauchy)  */
} d3_dec_stats;
d3_dec  *d3_dec_new(const d3_header *h);        /* h must have passed d3_classify */
/* 1 = new information, 0 = duplicate / redundant, -1 = out of memory */
int      d3_dec_add(d3_dec *d, uint32_t seq, const uint8_t *block);
int      d3_dec_complete(const d3_dec *d);
void     d3_dec_stats_get(const d3_dec *d, d3_dec_stats *out);
/* Source block 0 once known (block_len bytes), else NULL. */
const uint8_t *d3_dec_block0(const d3_dec *d);
/* malloc'd total_len bytes once complete, else NULL. */
uint8_t *d3_dec_assemble(const d3_dec *d);
void     d3_dec_free(d3_dec *d);

/* DCF2 container. pack: try_gzip -> gzip is used only when it saves > 64 bytes. */
int d3_container_pack(const char *name, const char *type, const uint8_t *data, size_t n, int try_gzip,
                      uint8_t **out, size_t *out_n, int *gzipped);

typedef struct d3_file {
    const uint8_t *name;  size_t name_len;   /* untrusted UTF-8              */
    const uint8_t *type;  size_t type_len;
    const uint8_t *data;  size_t len;        /* verified file bytes          */
    int            gzipped;
} d3_file;
/* Validates, inflates (bounded) and checks SHA-256. 1 on success; *owned is the
 * inflate buffer behind f->data (NULL when f->data points into c). */
int d3_container_unpack(const uint8_t *c, size_t n, d3_file *f, uint8_t **owned);

#define D3_SNIPPET_TYPE "application/vnd.decimen.snippet"
#define D3_SNIPPET_NAME "snippet.txt"

/* Sender: converts a CPO1 container into a DCF2 one (text -> snippet,
 * image -> image.<ext>, one file -> as is, several -> stored zip). */
int d3_from_cpo(const uint8_t *cpo, size_t n, uint8_t **out, size_t *out_n, int *gzipped);

/* ---- CodeTrans optical board, "OB" (ob_fec.c, ob.c, ob_locate.c) --------
 * Experimental non-QR screen-to-camera format, port of the
 * CodeTrans-Optical-Reference package (protocol.py / tile.c / acquire.py).
 * One board = 177 x 177 black / white modules: four oriented 7x7 corner marks,
 * known pilot modules, and 6 rows x 4 columns of 40 x 28 data rectangles.
 * Each rectangle carries 140 octets (one octet = a 4x2 module patch, upper
 * row first, MSB first), XOR-ed with a fixed per-rectangle mask.
 *   tiles: every rectangle is one RS(140,124) unit  -> 24 x 108 B per board
 *   bands: one row of 4 rectangles is one unit of four interleaved
 *          RS(140,132) lanes                       ->  6 x 512 B per board
 * Unit body = 12-byte header (8 B session tag, u16 group, u8 row, u8 type;
 * little-endian) + fixed-length payload, protected by CRC-32C (Castagnoli).
 * Outer code: systematic Cauchy-style erasure code over GF(256) / 0x11d in
 * groups of <= 32 source rows; repair rows 32..255. */

#define OB_SIDE           177
#define OB_ROWS           6
#define OB_COLS           4
#define OB_SLOTS          (OB_ROWS * OB_COLS)
#define OB_N              140      /* octets per rectangle                     */
#define OB_HDR_LEN        12u
#define OB_TILE_BODY      120u     /* header + 108-byte payload                */
#define OB_BAND_BODY      524u     /* header + 512-byte payload                */
#define OB_TILE_DATA      108u
#define OB_BAND_DATA      512u
#define OB_GROUP          32
#define OB_MANIFEST_GROUP 0xFFFFu
#define OB_TYPE_TILE      0x10u    /* | 1 = session manifest                   */
#define OB_TYPE_BAND      0x20u
#define OB_MAX_FILE       (64u << 20)
#define OB_PENDING_MAX    768      /* units buffered before their manifest     */
#define OB_TRACK_MIN_SCORE 0.68f

/* inner / outer codes (ob_fec.c); tables are built on first use, thread-safe */
uint32_t ob_crc32c(const uint8_t *s, size_t n);
void     ob_tile_encode(const uint8_t body[OB_TILE_BODY], uint8_t wire[OB_N]);
/* 1 = raw CRC pass, 2 = hard RS, 3 = RS with erasures, 0 = discard */
int      ob_tile_decode(const uint8_t wire[OB_N], const uint8_t *erasures, int count, uint8_t body[OB_TILE_BODY]);
void     ob_band_encode(const uint8_t body[OB_BAND_BODY], uint8_t wire[4 * OB_N]);
/* erasures: four lists of up to 8 lane-byte positions (4 x 8), counts[4] <= 8 */
int      ob_band_decode(const uint8_t wire[4 * OB_N], const uint8_t *erasures, const uint8_t *counts,
                        uint8_t body[OB_BAND_BODY]);
/* source: k adjacent rows of `bytes` octets. row < k (copy) or 32..255. 1 = ok */
int      ob_outer_symbol(const uint8_t *source, int k, int bytes, int row, uint8_t *out);
/* exactly k distinct valid rows; out (k * bytes) must not overlap the input. 1 = ok */
int      ob_outer_recover(const uint8_t *ids, const uint8_t *symbols, int k, int bytes, uint8_t *out);

/* board layout, rendering and registered read (ob.c) */
void     ob_layout_init(void);                  /* idempotent, thread-safe          */
extern uint8_t ob_pilots[OB_SIDE * OB_SIDE];      /* 1 = white (tiles profile)       */
extern uint8_t ob_pilot_score[OB_SIDE * OB_SIDE]; /* known pilots used for scoring    */
/* profile CP_OB_*; bodies: OB_SLOTS (tiles) or OB_ROWS (bands) unit bodies of
 * OB_TILE_BODY / OB_BAND_BODY bytes back to back. Writes 1 = white modules. */
void     ob_draw(int profile, const uint8_t *bodies, uint8_t white[OB_SIDE * OB_SIDE]);

typedef struct ob_unit {
    int      index;              /* tile 0..23 or band 0..5            */
    int      status;             /* ob_*_decode result (1..3)          */
    size_t   len;                /* OB_TILE_BODY / OB_BAND_BODY        */
    uint8_t  body[OB_BAND_BODY];
} ob_unit;
/* luma: 177 x 177 samples at module centres. Regular pass, then one spatially
 * equalized pass for the units that failed when fewer than 85 % passed.
 * Returns the number of units written to out (room for OB_SLOTS). */
int      ob_read_board(const float *luma, ob_unit *out);

/* sender carousel (ob.c) */
typedef struct ob_enc ob_enc;
ob_enc  *ob_enc_new(const uint8_t *data, size_t n, int profile);
/* Next display refresh: `boards` boards into white (boards * 177 * 177, 1 = white). */
int      ob_enc_boards(ob_enc *e, int boards, uint8_t *white);
uint32_t ob_enc_rows(const ob_enc *e);
uint32_t ob_enc_width(const ob_enc *e);
void     ob_enc_tag(const ob_enc *e, uint8_t tag[8]);
void     ob_enc_free(ob_enc *e);

/* receiver entry for one CRC-verified unit body (rx.c). Returns CP_EV_* bits. */
int      ob_rx_feed(cp_rx *rx, const uint8_t *body, size_t n);

/* ---- platform shim (platform.c) --------------------------------------- */

#if defined(_WIN32)
typedef struct cp_mutex { void *srw; } cp_mutex;   /* SRWLOCK */
#else
#include <pthread.h>
typedef struct cp_mutex { pthread_mutex_t m; } cp_mutex;
#endif

void     cp_mutex_init(cp_mutex *m);
void     cp_mutex_destroy(cp_mutex *m);
void     cp_mutex_lock(cp_mutex *m);
void     cp_mutex_unlock(cp_mutex *m);
uint64_t cp_mono_ms(void);
uint32_t cp_random_u32(void);
FILE    *cp_fopen_utf8(const char *path, const char *mode);
int      cp_mkdir_utf8(const char *path);   /* 0 = created or already exists */

/* ---- misc ------------------------------------------------------------- */

/* Length of the longest prefix of s[0..n) that does not split a UTF-8 sequence. */
size_t cp_utf8_cut(const uint8_t *s, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* CODETRANS_CP_INTERNAL_H */
