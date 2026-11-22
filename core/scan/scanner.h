/*
 * quirc wrapper: one 8-bit luminance frame -> every QR code in it -> cp_rx.
 * Pure C (no JNI / Android headers) so desktop tests can compile it too.
 *
 * A scanner is NOT thread-safe: use one per decoder thread (PLAN §3).
 * The cp_rx it pushes into is thread-safe and may be shared.
 */
#ifndef CODETRANS_SCANNER_H
#define CODETRANS_SCANNER_H

#include <stddef.h>
#include <stdint.h>

#include "cp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CP_SCAN_MAX_DIM 8192   /* larger frames are rejected */

typedef struct cp_scanner cp_scanner;

cp_scanner *cp_scanner_new(void);
void        cp_scanner_free(cp_scanner *s);

/* y: tightly packed w*h luminance bytes (row stride == w).
 * Detects + decodes all QR codes, pushes each payload into rx (when rx != NULL)
 * and reports the decoded count via cp_rx_note_frame(). Also finds, tracks and
 * decodes CodeTrans optical boards (cp_rx_push_board); they count towards
 * cp_rx_note_frame() and appear in cp_scanner_last_quads(), not in *codes.
 * Returns the OR of CP_EV_* bits from cp_rx_push; *codes (optional) = codes decoded.
 * Returns 0 with *codes = -1 on invalid arguments / out of memory. */
int cp_scanner_scan(cp_scanner *s, cp_rx *rx, const uint8_t *y, int w, int h, int *codes);

/* Number of QR symbols quirc located in the last frame (decoded or not).
 * detected > decoded means codes are seen but unreadable (too small, blurry,
 * over-exposed); detected == 0 means nothing code-like is in view. */
int cp_scanner_last_detected(const cp_scanner *s);

/* Where the codes of the last frame are, for drawing boxes over the preview:
 * corners in frame pixels (quirc order: top-left, top-right, bottom-right,
 * bottom-left of the symbol), decoded = 1 when its payload was read. Copies
 * up to max entries into out and returns how many. */
#define CP_SCAN_MAX_QUADS 16
typedef struct cp_scan_quad {
    int x[4], y[4];
    int decoded;
} cp_scan_quad;
int cp_scanner_last_quads(const cp_scanner *s, cp_scan_quad *out, int max);

/* Optional per-code callback (tests / debugging); called before cp_rx_push. */
typedef void (*cp_scan_cb)(void *user, const uint8_t *payload, size_t n);
void cp_scanner_set_callback(cp_scanner *s, cp_scan_cb cb, void *user);

/* Contrast retry (Otsu + high-boost re-binarisation when a frame decodes
 * nothing, or fewer codes than it located): at most once per `ms` per scanner
 * (default 100), 0 = every frame, < 0 = off. cp_scanner_retry_hits counts
 * frames it rescued (fully or partially). */
void     cp_scanner_set_retry_interval(cp_scanner *s, int ms);
unsigned cp_scanner_retry_hits(const cp_scanner *s);

#ifdef __cplusplus
}
#endif
#endif /* CODETRANS_SCANNER_H */
