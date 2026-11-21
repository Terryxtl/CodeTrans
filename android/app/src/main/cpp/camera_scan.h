#ifndef CP_CAMERA_SCAN_H
#define CP_CAMERA_SCAN_H
#include "scanner.h"
#include "tracked_grid.h"

/* seeds contains confirmed grids from this camera session. Replace it with the
 * observations from this frame. A negative output count requests full discovery
 * when more symbols were decoded than fit in the cache. */
int cp_scan_seeded(cp_scanner *s, cp_rx *rx, const uint8_t *y, int w, int h,
                   cp_grid seeds[CP_SCAN_MAX_QUADS], int *count);
#endif
