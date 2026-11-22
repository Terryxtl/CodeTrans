#include "tracked_grid.h"
#include <string.h>

typedef struct {
    double ax, bx, cx, ay, by, cy, g, h;
} projection;

/* Project the module square onto its camera quadrilateral, including perspective. */
static int projection_for(const cp_grid *grid, int width, int height, projection *p)
{
    const struct quirc_point *q = grid->corners;
    int n = grid->size;
    if (n < 21 || n > QUIRC_MAX_GRID_SIZE || (n - 21) % 4)
        return 0;
    for (int i = 0; i < 4; i++)
        if ((unsigned)q[i].x >= (unsigned)width || (unsigned)q[i].y >= (unsigned)height)
            return 0;
    double dx1 = q[1].x - q[2].x, dx2 = q[3].x - q[2].x;
    double dy1 = q[1].y - q[2].y, dy2 = q[3].y - q[2].y;
    double sx = q[0].x - q[1].x + q[2].x - q[3].x;
    double sy = q[0].y - q[1].y + q[2].y - q[3].y;
    double det = dx1 * dy2 - dx2 * dy1;
    if (det == 0) return 0;
    double g = (sx * dy2 - dx2 * sy) / det;
    double h = (dx1 * sy - sx * dy1) / det;
    if (1 + g <= 0 || 1 + h <= 0 || 1 + g + h <= 0) return 0;
    *p = (projection){
        (q[1].x * (1 + g) - q[0].x) / n, (q[3].x * (1 + h) - q[0].x) / n, q[0].x,
        (q[1].y * (1 + g) - q[0].y) / n, (q[3].y * (1 + h) - q[0].y) / n, q[0].y,
        g / n, h / n
    };
    return 1;
}

static struct quirc_point centre(const projection *p, int col, int row)
{
    double x = col + 0.5, y = row + 0.5;
    double scale = 1.0 / (1 + p->g * x + p->h * y);
    return (struct quirc_point){(int)((p->ax*x + p->bx*y + p->cx)*scale + 0.5),
                                (int)((p->ay*x + p->by*y + p->cy)*scale + 0.5)};
}

typedef struct {
    int pixel;
    int dark;
} finder_sample;

/* Use the unchanging three finder templates to estimate the black/white levels
 * and to score small translations. Positions have a two-pixel search margin. */
static int finder_score(const finder_sample samples[147], const uint8_t *plane, int shift, int *threshold)
{
    int black = 0, white = 0, score = 0;
    for (int i = 0; i < 147; i++) {
        int value = plane[samples[i].pixel + shift];
        if (samples[i].dark) black += value;
        else white += value;
    }
    black /= 99;
    white /= 48;
    if (white - black < 24) return 0;
    *threshold = (white + black) / 2;
    for (int i = 0; i < 147; i++)
        score += (plane[samples[i].pixel + shift] <= *threshold) == samples[i].dark;
    return score;
}

int cp_sample_grid(const cp_grid *grid, const uint8_t *y, int w, int h, struct quirc_code *out)
{
    projection p;
    if (!projection_for(grid, w, h, &p)) return 0;
    int n = grid->size;
    finder_sample samples[147];
    int count = 0;
    for (int f = 0; f < 3; f++) {
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 7; col++) {
                struct quirc_point pt = centre(&p, col + (f == 1 ? n-7 : 0), row + (f == 2 ? n-7 : 0));
                if (pt.x < 2 || pt.y < 2 || pt.x >= w-2 || pt.y >= h-2) return 0;
                int dark = !col || !row || col == 6 || row == 6 ||
                           (col >= 2 && col <= 4 && row >= 2 && row <= 4);
                samples[count++] = (finder_sample){pt.y*w + pt.x, dark};
            }
        }
    }
    int dx = 0, dy = 0, threshold = 0;
    int best = finder_score(samples, y, 0, &threshold);
    for (int v = -2; best < 147 && v <= 2; v++) {
        for (int u = -2; u <= 2; u++) {
            int t = 0, score = finder_score(samples, y, v*w + u, &t);
            if (score > best || (score == best && u*u + v*v < dx*dx + dy*dy)) {
                best = score;
                dx = u;
                dy = v;
                threshold = t;
            }
        }
    }
    if (best < 125) return 0;
    memset(out, 0, sizeof *out);
    out->size = n;
    for (int i = 0; i < 4; i++)
        out->corners[i] = (struct quirc_point){grid->corners[i].x + dx, grid->corners[i].y + dy};

    /* Use local luminance levels for unevenly lit screens. In a flat tile
     * the finder contrast supplies the threshold, including all-white tiles. */
    for (int top = 0; top < n; top += 8) {
        for (int left = 0; left < n; left += 8) {
            uint8_t tile[8][8];
            int lo = 255, hi = 0;
            int rows = n-top < 8 ? n-top : 8, cols = n-left < 8 ? n-left : 8;
            for (int row = 0; row < rows; row++)
                for (int col = 0; col < cols; col++) {
                    struct quirc_point pt = centre(&p, left + col, top + row);
                    int px = pt.x + dx, py = pt.y + dy;
                    if ((unsigned)px >= (unsigned)w || (unsigned)py >= (unsigned)h) return 0;
                    int luma = y[py*w + px];
                    tile[row][col] = (uint8_t)luma;
                    if (luma < lo) lo = luma;
                    if (luma > hi) hi = luma;
                }
            int t = hi-lo >= 24 ? (hi+lo)/2 : threshold;
            for (int row = 0; row < rows; row++)
                for (int col = 0; col < cols; col++) {
                    int bit = (top+row)*n + left+col;
                    if (tile[row][col] <= t) out->cell_bitmap[bit >> 3] |= 1u << (bit & 7);
                }
        }
    }
    return 1;
}
