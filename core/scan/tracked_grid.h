#ifndef CODETRANS_TRACKED_GRID_H
#define CODETRANS_TRACKED_GRID_H

#include "quirc.h"

/* A successful decode supplies the orientation, module count and four outer corners. */
typedef struct {
    struct quirc_point corners[4];
    int size;
    int drift; /* displacement estimate, pixels per observation */
} cp_grid;

/* Sample the current Y plane at a previously decoded grid. The caller must still
 * validate the sampled bits with quirc_decode before adopting the new corners. */
int cp_sample_grid(const cp_grid *grid, const uint8_t *y, int w, int h, struct quirc_code *out);

#endif
