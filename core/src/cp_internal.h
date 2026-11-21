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

/* ---- filenames (output.c) --------------------------------------------- */

#define CP_NAME_MAX 120u

/* Sanitizes an untrusted UTF-8 file name into out (cap >= CP_NAME_MAX + 1). */
void cp_sanitize_name(const uint8_t *in, size_t n, char *out, size_t cap);
/* Makes name unique among names[0..count) (ASCII case-insensitive) by
 * inserting " (1)", " (2)", ... before the extension. In place, cap >= 121. */
void cp_unique_name(char *name, size_t cap, char *const *names, size_t count);

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
