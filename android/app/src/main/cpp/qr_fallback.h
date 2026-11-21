#ifndef CP_QR_FALLBACK_H
#define CP_QR_FALLBACK_H

#include "tracked_grid.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*cp_qr_found)(void *, const cp_grid *, const uint8_t *, size_t);
void cp_find_qr(const uint8_t *y, int width, int height, int stride,
                cp_qr_found found, void *user);
#ifdef __cplusplus
}
#endif
#endif
