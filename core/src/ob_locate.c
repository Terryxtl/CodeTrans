/* CodeTrans optical board: finding boards in camera luminance frames.
 * Port of CodeTrans-Optical-Reference/acquire.py (find_boards / refine).
 *
 * Cold search (cp_ob_locate), on the half-resolution frame:
 *   dark = pixel < 21x21 local mean - 30; dilate 5x5; close 3x3; 4-connected
 *   components that are large (short side >= 130 half-res pixels), squarish
 *   (aspect >= 0.55) and dense (>= 38 % of the bounding box). The outermost
 *   dark full-resolution pixels give four corners; a homography samples the
 *   module grid, the pilot correlation picks one of four rotations (> 0.50),
 *   the corners are refined against the known pilot edges and the board is
 *   accepted when the refined correlation exceeds 0.68.
 * Refinement (also cp_ob_track): at known black/white pilot edges, compare
 * the luminance half a module on either side with the value on the edge; the
 * edge offsets along the normal give a least-squares update of the 4 corners
 * (bilinearly weighted), up to 4 iterations. */
#include "cp_internal.h"

#include <math.h>
#include <stdatomic.h>

#define SIDE OB_SIDE
#define AREA (SIDE * SIDE)
#define MAX_EDGES 4096

static float      edge_x[MAX_EDGES], edge_y[MAX_EDGES], edge_sign[MAX_EDGES];
static uint8_t    edge_dx[MAX_EDGES];      /* 1 = horizontal neighbour, 0 = vertical */
static int        nedge;
static atomic_int edges_ready;

static void edges_init(void)
{
    int pass, x, y, n = 0;
    if (atomic_load_explicit(&edges_ready, memory_order_acquire))
        return;
    ob_layout_init();
    for (pass = 0; pass < 2; pass++) {
        int dx = pass == 0, dy = pass == 1, seen = 0;
        for (y = 0; y < SIDE - dy; y++)
            for (x = 0; x < SIDE - dx; x++) {
                int a = y * SIDE + x, b = (y + dy) * SIDE + x + dx;
                if (!ob_pilot_score[a] || !ob_pilot_score[b] || ob_pilots[a] == ob_pilots[b])
                    continue;
                if (seen++ % 3 == 0 && n < MAX_EDGES) {   /* every third edge */
                    edge_x[n] = x + 0.5f + dx * 0.5f;
                    edge_y[n] = y + 0.5f + dy * 0.5f;
                    edge_dx[n] = (uint8_t)dx;
                    edge_sign[n] = (float)ob_pilots[b] - (float)ob_pilots[a];
                    n++;
                }
            }
    }
    nedge = n;
    atomic_store_explicit(&edges_ready, 1, memory_order_release);
}

/* ---- geometry ------------------------------------------------------------ */

/* Gaussian elimination with partial pivoting, a is n x (n+1). 1 = solved. */
static int solve(double *a, int n, double *x)
{
    int i, j, k;
    for (i = 0; i < n; i++) {
        int p = i;
        double best = fabs(a[i * (n + 1) + i]);
        for (k = i + 1; k < n; k++)
            if (fabs(a[k * (n + 1) + i]) > best) {
                best = fabs(a[k * (n + 1) + i]);
                p = k;
            }
        if (best < 1e-12)
            return 0;
        if (p != i)
            for (j = 0; j <= n; j++) {
                double t = a[i * (n + 1) + j];
                a[i * (n + 1) + j] = a[p * (n + 1) + j];
                a[p * (n + 1) + j] = t;
            }
        for (k = 0; k < n; k++) {
            double f;
            if (k == i)
                continue;
            f = a[k * (n + 1) + i] / a[i * (n + 1) + i];
            if (f != 0)
                for (j = i; j <= n; j++)
                    a[k * (n + 1) + j] -= f * a[i * (n + 1) + j];
        }
    }
    for (i = 0; i < n; i++)
        x[i] = a[i * (n + 1) + n] / a[i * (n + 1) + i];
    return 1;
}

/* Maps the board square (0,0)-(177,177) onto corners. */
static int homography(const double c[4][2], double h[9])
{
    static const double sq[4][2] = { { 0, 0 }, { SIDE, 0 }, { SIDE, SIDE }, { 0, SIDE } };
    double a[8 * 9], x[8];
    int i;
    for (i = 0; i < 4; i++) {
        double sx = sq[i][0], sy = sq[i][1], u = c[i][0], v = c[i][1];
        double *r0 = a + (2 * i) * 9, *r1 = a + (2 * i + 1) * 9;
        r0[0] = sx; r0[1] = sy; r0[2] = 1; r0[3] = 0; r0[4] = 0; r0[5] = 0; r0[6] = -u * sx; r0[7] = -u * sy; r0[8] = u;
        r1[0] = 0; r1[1] = 0; r1[2] = 0; r1[3] = sx; r1[4] = sy; r1[5] = 1; r1[6] = -v * sx; r1[7] = -v * sy; r1[8] = v;
    }
    if (!solve(a, 8, x))
        return 0;
    for (i = 0; i < 8; i++)
        h[i] = x[i];
    h[8] = 1;
    return 1;
}

static void project(const double h[9], double x, double y, double *u, double *v)
{
    double den = h[6] * x + h[7] * y + h[8];
    *u = (h[0] * x + h[1] * y + h[2]) / den;
    *v = (h[3] * x + h[4] * y + h[5]) / den;
}

/* scipy map_coordinates(order=1, mode="nearest"): pixel centres at integers */
static float bilinear(const uint8_t *f, int w, int h, double x, double y)
{
    int x0, y0, x1, y1;
    double fx, fy, top, bot;
    if (!(x >= 0)) x = 0;            /* also catches NaN */
    if (!(y >= 0)) y = 0;
    if (x > w - 1) x = w - 1;
    if (y > h - 1) y = h - 1;
    x0 = (int)x;
    y0 = (int)y;
    x1 = x0 + 1 < w ? x0 + 1 : x0;
    y1 = y0 + 1 < h ? y0 + 1 : y0;
    fx = x - x0;
    fy = y - y0;
    top = f[(size_t)y0 * w + x0] + fx * (f[(size_t)y0 * w + x1] - f[(size_t)y0 * w + x0]);
    bot = f[(size_t)y1 * w + x0] + fx * (f[(size_t)y1 * w + x1] - f[(size_t)y1 * w + x0]);
    return (float)(top + fy * (bot - top));
}

static int sample_grid(const uint8_t *f, int w, int h, const double c[4][2], float *luma)
{
    double hm[9], u, v;
    int gx, gy;
    if (!homography(c, hm))
        return 0;
    for (gy = 0; gy < SIDE; gy++)
        for (gx = 0; gx < SIDE; gx++) {
            project(hm, gx + 0.5, gy + 0.5, &u, &v);
            luma[gy * SIDE + gx] = bilinear(f, w, h, u, v);
        }
    return 1;
}

/* Pilot correlation of the board seen in rotation k (numpy.rot90(luma, k)). */
static float pilot_score(const float *luma, int k)
{
    double sum = 0, sq = 0, cross = 0, sgn = 0, mean, var;
    int i, n = 0, last = SIDE - 1;
    for (i = 0; i < AREA; i++) {
        int r, c, src;
        float s, sign;
        if (!ob_pilot_score[i])
            continue;
        r = i / SIDE;
        c = i % SIDE;
        switch (k & 3) {
        case 0: src = r * SIDE + c; break;
        case 1: src = c * SIDE + (last - r); break;
        case 2: src = (last - r) * SIDE + (last - c); break;
        default: src = (last - c) * SIDE + r; break;
        }
        s = luma[src];
        sign = ob_pilots[i] ? 1.0f : -1.0f;
        sum += s;
        sq += (double)s * s;
        cross += s * sign;
        sgn += sign;
        n++;
    }
    if (!n)
        return 0;
    mean = sum / n;
    var = sq / n - mean * mean;
    return (float)((cross / n - mean * sgn / n) / (sqrt(var > 0 ? var : 0) + 1));
}

static void refine(const uint8_t *f, int w, int h, double c[4][2])
{
    int iter, i;
    edges_init();
    for (iter = 0; iter < 4; iter++) {
        double hm[9], ata[8 * 9], step[8], maxstep = 0;
        int keep = 0, r, s;
        if (!homography(c, hm))
            return;
        memset(ata, 0, sizeof ata);
        for (i = 0; i < nedge; i++) {
            double ex = edge_x[i], ey = edge_y[i], nx = edge_dx[i], ny = 1 - edge_dx[i];
            double cx, cy, ax, ay, bx, by, a, b, mid, sig, dx, dy, len, dist, uu, vv, wt[4], row[8];
            project(hm, ex, ey, &cx, &cy);
            project(hm, ex - .42 * nx, ey - .42 * ny, &ax, &ay);
            project(hm, ex + .42 * nx, ey + .42 * ny, &bx, &by);
            mid = bilinear(f, w, h, cx, cy);
            a = bilinear(f, w, h, ax, ay);
            b = bilinear(f, w, h, bx, by);
            sig = b - a;
            if (!(edge_sign[i] * sig > 36))
                continue;
            dx = bx - ax;
            dy = by - ay;
            len = hypot(dx, dy);
            if (len <= 0)
                continue;
            dist = len * ((a + b) * .5 - mid) / sig;
            dist = dist < -1.5 ? -1.5 : dist > 1.5 ? 1.5 : dist;
            uu = ex / SIDE;
            vv = ey / SIDE;
            wt[0] = (1 - uu) * (1 - vv);
            wt[1] = uu * (1 - vv);
            wt[2] = uu * vv;
            wt[3] = (1 - uu) * vv;
            for (r = 0; r < 4; r++) {
                row[2 * r] = wt[r] * dx / len;
                row[2 * r + 1] = wt[r] * dy / len;
            }
            for (r = 0; r < 8; r++) {
                for (s = 0; s < 8; s++)
                    ata[r * 9 + s] += row[r] * row[s];
                ata[r * 9 + 8] += row[r] * dist;
            }
            keep++;
        }
        if (keep < 32 || !solve(ata, 8, step))
            return;
        for (r = 0; r < 4; r++)
            for (s = 0; s < 2; s++) {
                double d = step[2 * r + s];
                if (fabs(d) > maxstep)
                    maxstep = fabs(d);
                c[r][s] += d < -2.5 ? -2.5 : d > 2.5 ? 2.5 : d;
            }
        if (maxstep < .04)
            return;
    }
}

/* ---- cold search --------------------------------------------------------- */

static int reflect(int i, int n)
{
    while (i < 0 || i >= n)
        i = i < 0 ? -i - 1 : 2 * n - 1 - i;
    return i;
}

/* Accepts one component; returns 1 and fills q when it is a board. */
static int try_component(const uint8_t *f, int w, int h, const int *labels, int hw, int lab, int bx0, int by0,
                         int bw, int bh, float *luma, cp_ob_quad *q)
{
    int x0 = 2 * bx0, y0 = 2 * by0, x1 = x0 + 2 * bw, y1 = y0 + 2 * bh, x, y, count = 0, k, best = 0;
    long ext[4];
    int px[4] = { 0 }, py[4] = { 0 };
    double c[4][2], cx = 0, cy = 0, rc[4][2], bestscore = -1e9;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    ext[0] = ext[3] = 0x7fffffffL;
    ext[1] = ext[2] = -0x7fffffffL;
    for (y = y0; y < y1; y++)
        for (x = x0; x < x1; x++) {
            if (f[(size_t)y * w + x] >= 112 || labels[(size_t)(y >> 1) * hw + (x >> 1)] != lab)
                continue;
            count++;
            if (x + y < ext[0]) { ext[0] = x + y; px[0] = x; py[0] = y; }   /* top-left     */
            if (x - y > ext[1]) { ext[1] = x - y; px[1] = x; py[1] = y; }   /* top-right    */
            if (x + y > ext[2]) { ext[2] = x + y; px[2] = x; py[2] = y; }   /* bottom-right */
            if (x - y < ext[3]) { ext[3] = x - y; px[3] = x; py[3] = y; }   /* bottom-left  */
        }
    if (count < 1000)
        return 0;
    for (k = 0; k < 4; k++) {
        c[k][0] = px[k];
        c[k][1] = py[k];
        cx += px[k] / 4.0;
        cy += py[k] / 4.0;
    }
    /* the extreme dark pixel is a pixel centre inside the outer cell edges */
    for (k = 0; k < 4; k++) {
        double r = hypot(c[k][0] - cx, c[k][1] - cy), g = 0.75 / (r > 1 ? r : 1);
        c[k][0] += (c[k][0] - cx) * g;
        c[k][1] += (c[k][1] - cy) * g;
    }
    if (!sample_grid(f, w, h, c, luma))
        return 0;
    for (k = 0; k < 4; k++) {
        float s = pilot_score(luma, k);
        if (s > bestscore) {
            bestscore = s;
            best = k;
        }
    }
    if (!(bestscore > 0.50))
        return 0;
    for (k = 0; k < 4; k++) {          /* np.roll(corners, -orientation) */
        rc[k][0] = c[(k + best) & 3][0];
        rc[k][1] = c[(k + best) & 3][1];
    }
    refine(f, w, h, rc);
    if (!sample_grid(f, w, h, rc, luma))
        return 0;
    q->score = pilot_score(luma, 0);
    if (!(q->score > OB_TRACK_MIN_SCORE))
        return 0;
    for (k = 0; k < 4; k++) {
        q->x[k] = (float)rc[k][0];
        q->y[k] = (float)rc[k][1];
    }
    return 1;
}

int cp_ob_locate(const uint8_t *f, int w, int h, cp_ob_quad *out, int max)
{
    int hw, hh, x, y, d, nfound = 0, lab = 0;
    size_t hn, i;
    uint8_t *half = NULL, *a = NULL, *b = NULL;
    int32_t *sums = NULL, *labels = NULL, *stack = NULL;
    float *luma = NULL;

    if (!f || !out || max <= 0 || w < 64 || h < 64 || w > 16384 || h > 16384)
        return 0;
    edges_init();
    hw = (w + 1) / 2;
    hh = (h + 1) / 2;
    hn = (size_t)hw * hh;
    half = (uint8_t *)malloc(hn);
    a = (uint8_t *)malloc(hn);
    b = (uint8_t *)malloc(hn);
    sums = (int32_t *)malloc(hn * sizeof *sums);
    labels = (int32_t *)malloc(hn * sizeof *labels);
    stack = (int32_t *)malloc(hn * sizeof *stack);
    luma = (float *)malloc(sizeof(float) * AREA);
    if (!half || !a || !b || !sums || !labels || !stack || !luma || hw < 11 || hh < 11)
        goto out;
    for (y = 0; y < hh; y++)
        for (x = 0; x < hw; x++)
            half[(size_t)y * hw + x] = f[(size_t)(2 * y) * w + 2 * x];

    /* dark = half < uniform_filter(half, 21, mode="reflect") - 30 (exact, integer) */
    for (y = 0; y < hh; y++) {
        const uint8_t *row = half + (size_t)y * hw;
        int32_t s = 0;
        for (d = -10; d <= 10; d++)
            s += row[reflect(d, hw)];
        for (x = 0; x < hw; x++) {
            sums[(size_t)y * hw + x] = s;
            s += row[reflect(x + 11, hw)] - row[reflect(x - 10, hw)];
        }
    }
    for (x = 0; x < hw; x++) {
        int32_t s = 0;
        for (d = -10; d <= 10; d++)
            s += sums[(size_t)reflect(d, hh) * hw + x];
        for (y = 0; y < hh; y++) {
            a[(size_t)y * hw + x] = 441 * half[(size_t)y * hw + x] < s - 30 * 441;
            s += sums[(size_t)reflect(y + 11, hh) * hw + x] - sums[(size_t)reflect(y - 10, hh) * hw + x];
        }
    }
    /* maximum_filter 5x5 (window clipped to the image), separable: a -> a */
    for (y = 0; y < hh; y++)
        for (x = 0; x < hw; x++) {
            uint8_t m = 0;
            for (d = -2; d <= 2 && !m; d++)
                if (x + d >= 0 && x + d < hw)
                    m = a[(size_t)y * hw + x + d];
            b[(size_t)y * hw + x] = m;
        }
    for (y = 0; y < hh; y++)
        for (x = 0; x < hw; x++) {
            uint8_t m = 0;
            for (d = -2; d <= 2 && !m; d++)
                if (y + d >= 0 && y + d < hh)
                    m = b[(size_t)(y + d) * hw + x];
            a[(size_t)y * hw + x] = m;
        }
    /* binary_closing 3x3: dilation (outside = 0) then erosion (outside = 0) */
    for (y = 0; y < hh; y++)
        for (x = 0; x < hw; x++) {
            uint8_t m = 0;
            int dy, dx;
            for (dy = -1; dy <= 1 && !m; dy++)
                for (dx = -1; dx <= 1 && !m; dx++)
                    if (x + dx >= 0 && x + dx < hw && y + dy >= 0 && y + dy < hh)
                        m = a[(size_t)(y + dy) * hw + x + dx];
            b[(size_t)y * hw + x] = m;
        }
    for (y = 0; y < hh; y++)
        for (x = 0; x < hw; x++) {
            uint8_t m = 1;
            int dy, dx;
            for (dy = -1; dy <= 1 && m; dy++)
                for (dx = -1; dx <= 1 && m; dx++)
                    m = (x + dx >= 0 && x + dx < hw && y + dy >= 0 && y + dy < hh) ? b[(size_t)(y + dy) * hw + x + dx] : 0;
            a[(size_t)y * hw + x] = m;
        }

    /* 4-connected labelling in raster order (scipy.ndimage.label) */
    memset(labels, 0, hn * sizeof *labels);
    for (i = 0; i < hn && nfound < max; i++) {
        int top = 0, bx0, by0, bx1, by1;
        long count = 0;
        if (!a[i] || labels[i])
            continue;
        lab++;
        labels[i] = lab;
        stack[top++] = (int32_t)i;
        bx0 = bx1 = (int)(i % hw);
        by0 = by1 = (int)(i / hw);
        while (top) {
            int p = stack[--top], px = p % hw, py = p / hw, n4[4], k;
            count++;
            if (px < bx0) bx0 = px;
            if (px > bx1) bx1 = px;
            if (py < by0) by0 = py;
            if (py > by1) by1 = py;
            n4[0] = px > 0 ? p - 1 : -1;
            n4[1] = px + 1 < hw ? p + 1 : -1;
            n4[2] = py > 0 ? p - hw : -1;
            n4[3] = py + 1 < hh ? p + hw : -1;
            for (k = 0; k < 4; k++)
                if (n4[k] >= 0 && a[n4[k]] && !labels[n4[k]]) {
                    labels[n4[k]] = lab;
                    stack[top++] = n4[k];
                }
        }
        {
            int bw = bx1 - bx0 + 1, bh = by1 - by0 + 1, lo = bw < bh ? bw : bh, hi = bw < bh ? bh : bw;
            if (lo < 130 || lo < 0.55 * hi || count < 0.38 * bw * bh)
                continue;
            if (try_component(f, w, h, labels, hw, lab, bx0, by0, bw, bh, luma, &out[nfound]))
                nfound++;
        }
    }
out:
    free(half);
    free(a);
    free(b);
    free(sums);
    free(labels);
    free(stack);
    free(luma);
    return nfound;
}

int cp_ob_track(const uint8_t *f, int w, int h, cp_ob_quad *q)
{
    double c[4][2];
    float *luma;
    int k, ok = 0;
    if (!f || !q || w <= 0 || h <= 0)
        return 0;
    luma = (float *)malloc(sizeof(float) * AREA);
    if (!luma)
        return 0;
    for (k = 0; k < 4; k++) {
        c[k][0] = q->x[k];
        c[k][1] = q->y[k];
    }
    refine(f, w, h, c);
    if (sample_grid(f, w, h, c, luma)) {
        float s = pilot_score(luma, 0);
        if (s > OB_TRACK_MIN_SCORE) {
            for (k = 0; k < 4; k++) {
                q->x[k] = (float)c[k][0];
                q->y[k] = (float)c[k][1];
            }
            q->score = s;
            ok = 1;
        }
    }
    free(luma);
    return ok;
}

void cp_ob_sample(const uint8_t *f, int w, int h, const cp_ob_quad *q, float *luma)
{
    double c[4][2];
    int k;
    if (!f || !q || !luma)
        return;
    for (k = 0; k < 4; k++) {
        c[k][0] = q->x[k];
        c[k][1] = q->y[k];
    }
    if (!sample_grid(f, w, h, c, luma))
        memset(luma, 0, sizeof(float) * AREA);
}
