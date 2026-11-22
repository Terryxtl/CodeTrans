/* CodeTrans optical board: inner and outer codes. See cp_internal.h ("OB").
 *
 * Port of CodeTrans-Optical-Reference/tile.c (same arithmetic, same wire bytes):
 * - CRC-32C, reflected polynomial 0x82F63B78, init / final XOR all ones.
 * - Shortened systematic RS over GF(256) / 0x11d, alpha = 2, consecutive roots
 *   starting at alpha^0. tiles: RS(140,124) over a 120-byte body + CRC.
 *   bands: 524-byte body + CRC split round-robin into four RS(140,132) lanes.
 *   Decoding: raw CRC first, then hard Berlekamp-Massey, then one retry with
 *   the caller's low-confidence bytes as erasures (2e + s <= parity).
 * - Outer code: for source column i and repair row r (32..255) the coefficient
 *   is (32 ^ i) / (r ^ i): a Cauchy matrix with non-zero column scaling, so any
 *   k distinct valid rows recover the group; row 32 is the plain XOR. */
#include "cp_internal.h"

#include <stdatomic.h>

enum { N = OB_N, K = 124, P = N - K, BODY = (int)OB_TILE_BODY };
enum { LANES = 4, BAND_P = 8, BAND_K = N - BAND_P, BAND_BODY = LANES * BAND_K - 4 };

static uint8_t    exp_table[510], log_table[256], generator[P + 1], band_generator[BAND_P + 1];
static uint32_t   crc_table[256];
static atomic_int fec_ready;

static uint8_t mul(uint8_t a, uint8_t b) { return a && b ? exp_table[log_table[a] + log_table[b]] : 0; }
static uint8_t inv(uint8_t a) { return exp_table[255 - log_table[a]]; }

static void fec_init(void)
{
    unsigned v = 1, i;
    int j, k;
    if (atomic_load_explicit(&fec_ready, memory_order_acquire))
        return;
    /* Concurrent first calls write identical bytes; the flag publishes them. */
    for (i = 0; i < 255; ++i) {
        exp_table[i] = exp_table[i + 255] = (uint8_t)v;
        log_table[v] = (uint8_t)i;
        v <<= 1;
        if (v & 256)
            v ^= 0x11d;
    }
    memset(generator, 0, sizeof generator);
    generator[0] = 1;
    for (j = 0; j < P; ++j)
        for (k = j + 1; k > 0; --k)
            generator[k] ^= mul(generator[k - 1], exp_table[j]);
    memset(band_generator, 0, sizeof band_generator);
    band_generator[0] = 1;
    for (j = 0; j < BAND_P; ++j)
        for (k = j + 1; k > 0; --k)
            band_generator[k] ^= mul(band_generator[k - 1], exp_table[j]);
    for (i = 0; i < 256; ++i) {
        uint32_t crc = i;
        for (j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78u : 0);
        crc_table[i] = crc;
    }
    atomic_store_explicit(&fec_ready, 1, memory_order_release);
}

uint32_t ob_crc32c(const uint8_t *s, size_t n)
{
    uint32_t crc = UINT32_MAX;
    size_t i;
    fec_init();
    for (i = 0; i < n; ++i)
        crc = crc_table[(crc ^ s[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

static int crc_valid(const uint8_t *word, int length)
{
    uint32_t crc = ob_crc32c(word, (size_t)length);
    int j;
    for (j = 0; j < 4; ++j)
        if (word[length + j] != (uint8_t)(crc >> (8 * j)))
            return 0;
    return 1;
}

static void encode_rs(uint8_t out[N], int parity, const uint8_t *gen)
{
    int data_len = N - parity, i, j;
    uint8_t work[N];
    memset(out + data_len, 0, (size_t)parity);
    memcpy(work, out, N);
    for (i = 0; i < data_len; ++i) {
        uint8_t lead = work[i];
        if (lead)
            for (j = 1; j <= parity; ++j)
                work[i + j] ^= mul(lead, gen[j]);
    }
    memcpy(out + data_len, work + data_len, (size_t)parity);
}

void ob_tile_encode(const uint8_t body[OB_TILE_BODY], uint8_t out[OB_N])
{
    uint32_t crc;
    int j;
    fec_init();
    memcpy(out, body, BODY);
    crc = ob_crc32c(body, BODY);
    for (j = 0; j < 4; ++j)
        out[BODY + j] = (uint8_t)(crc >> (8 * j));
    encode_rs(out, P, generator);
}

static void syndromes(const uint8_t *word, uint8_t *s, int parity)
{
    int i, j;
    for (j = 0; j < parity; ++j) {
        uint8_t accum = 0;
        for (i = 0; i < N; ++i)
            accum = mul(accum, exp_table[j]) ^ word[i];
        s[j] = accum;
    }
}

/* Ascending-order connection polynomial for the syndrome sequence. */
static int berlekamp(const uint8_t *s, int count, uint8_t *poly)
{
    uint8_t previous[P + 1] = { 1 }, saved[P + 1], last = 1;
    int degree = 0, shift = 1, n, i, j;
    memset(poly, 0, P + 1);
    poly[0] = 1;
    for (n = 0; n < count; ++n) {
        uint8_t delta = s[n], factor;
        for (i = 1; i <= degree; ++i)
            delta ^= mul(poly[i], s[n - i]);
        if (!delta) {
            ++shift;
            continue;
        }
        memcpy(saved, poly, P + 1);
        factor = mul(delta, inv(last));
        for (j = 0; j + shift <= P; ++j)
            poly[j + shift] ^= mul(factor, previous[j]);
        if (2 * degree <= n) {
            degree = n + 1 - degree;
            memcpy(previous, saved, P + 1);
            last = delta;
            shift = 1;
        } else {
            ++shift;
        }
    }
    return degree;
}

/* Recover the magnitudes from the first m original syndromes. */
static int correct_values(uint8_t *word, const uint8_t *synd, const uint8_t *pos, int m)
{
    uint8_t a[P][P + 1];
    int i, j, r;
    memset(a, 0, sizeof a);
    for (j = 0; j < m; ++j) {
        uint8_t x = exp_table[N - 1 - pos[j]], power = 1;
        for (i = 0; i < m; ++i) {
            a[i][j] = power;
            power = mul(power, x);
        }
    }
    for (i = 0; i < m; ++i)
        a[i][m] = synd[i];
    for (i = 0; i < m; ++i) {
        int pivot = i;
        uint8_t scale;
        while (pivot < m && !a[pivot][i])
            ++pivot;
        if (pivot == m)
            return 0;
        if (pivot != i)
            for (j = i; j <= m; ++j) {
                uint8_t t = a[i][j];
                a[i][j] = a[pivot][j];
                a[pivot][j] = t;
            }
        scale = inv(a[i][i]);
        for (j = i; j <= m; ++j)
            a[i][j] = mul(a[i][j], scale);
        for (r = 0; r < m; ++r) {
            uint8_t f;
            if (r == i || !a[r][i])
                continue;
            f = a[r][i];
            for (j = i; j <= m; ++j)
                a[r][j] ^= mul(f, a[i][j]);
        }
    }
    for (i = 0; i < m; ++i)
        word[pos[i]] ^= a[i][m];
    return 1;
}

static int try_rs(const uint8_t *in, const uint8_t *synd, const uint8_t *erasures, int nerasures, int parity,
                  uint8_t *word)
{
    uint8_t s[P], poly[P + 1], pos[P], used[N];
    int length = parity, degree, m, i, j;
    memset(used, 0, sizeof used);
    memcpy(s, synd, (size_t)parity);
    for (j = 0; j < nerasures; ++j) {
        int p = erasures[j];
        uint8_t x;
        if (p >= N || used[p])
            return 0;
        pos[j] = (uint8_t)p;
        used[p] = 1;
        x = exp_table[N - 1 - p];
        --length;
        for (i = 0; i < length; ++i)
            s[i] = s[i + 1] ^ mul(x, s[i]);
    }
    degree = berlekamp(s, length, poly);
    m = nerasures;
    if (2 * degree > length)
        return 0;
    for (i = 0; i < N && degree; ++i) {
        uint8_t z = inv(exp_table[N - 1 - i]), value = poly[degree];
        for (j = degree - 1; j >= 0; --j)
            value = mul(value, z) ^ poly[j];
        if (!value) {
            if (used[i] || m == P)
                return 0;
            pos[m++] = (uint8_t)i;
        }
    }
    if (m != degree + nerasures || !m)
        return 0;
    memcpy(word, in, N);
    if (!correct_values(word, synd, pos, m))
        return 0;
    syndromes(word, s, parity);
    for (j = 0; j < parity; ++j)
        if (s[j])
            return 0;
    return 1;
}

int ob_tile_decode(const uint8_t in[OB_N], const uint8_t *erasures, int nerasures, uint8_t out[OB_TILE_BODY])
{
    uint8_t synd[P], word[N];
    fec_init();
    if (nerasures < 0 || nerasures > P)
        return 0;
    if (crc_valid(in, BODY)) {
        memcpy(out, in, BODY);
        return 1;
    }
    syndromes(in, synd, P);
    if (try_rs(in, synd, NULL, 0, P, word) && crc_valid(word, BODY)) {
        memcpy(out, word, BODY);
        return 2;
    }
    if (nerasures && try_rs(in, synd, erasures, nerasures, P, word) && crc_valid(word, BODY)) {
        memcpy(out, word, BODY);
        return 3;
    }
    return 0;
}

void ob_band_encode(const uint8_t body[OB_BAND_BODY], uint8_t *wire)
{
    uint8_t message[BAND_BODY + 4], lane[N];
    uint32_t crc;
    int i, l;
    fec_init();
    memcpy(message, body, BAND_BODY);
    crc = ob_crc32c(body, BAND_BODY);
    for (i = 0; i < 4; ++i)
        message[BAND_BODY + i] = (uint8_t)(crc >> (8 * i));
    for (l = 0; l < LANES; ++l) {
        for (i = 0; i < BAND_K; ++i)
            lane[i] = message[LANES * i + l];
        encode_rs(lane, BAND_P, band_generator);
        for (i = 0; i < N; ++i)
            wire[LANES * i + l] = lane[i];
    }
}

int ob_band_decode(const uint8_t *wire, const uint8_t *erasures, const uint8_t *counts, uint8_t *out)
{
    uint8_t lanes[LANES][N], syn[LANES][P], clean[LANES];
    int l, i, trial;
    fec_init();
    if (crc_valid(wire, BAND_BODY)) {
        memcpy(out, wire, BAND_BODY);
        return 1;
    }
    memset(syn, 0, sizeof syn);
    for (l = 0; l < LANES; ++l) {
        if (counts[l] > BAND_P)
            return 0;
        for (i = 0; i < N; ++i)
            lanes[l][i] = wire[LANES * i + l];
        syndromes(lanes[l], syn[l], BAND_P);
        clean[l] = 1;
        for (i = 0; i < BAND_P; ++i)
            if (syn[l][i])
                clean[l] = 0;
    }
    for (trial = 0; trial < 2; ++trial) {
        uint8_t message[BAND_BODY + 4], corrected[N];
        int ok = 1;
        for (l = 0; l < LANES; ++l) {
            const uint8_t *word = lanes[l];
            if (!clean[l]) {
                if (!try_rs(word, syn[l], erasures + l * BAND_P, trial ? counts[l] : 0, BAND_P, corrected)) {
                    ok = 0;
                    break;
                }
                word = corrected;
            }
            for (i = 0; i < BAND_K; ++i)
                message[LANES * i + l] = word[i];
        }
        if (ok && crc_valid(message, BAND_BODY)) {
            memcpy(out, message, BAND_BODY);
            return 2 + trial;
        }
    }
    return 0;
}

/* ---- outer erasure code ------------------------------------------------- */

enum { GROUP = OB_GROUP, MAX_SYMBOL = (int)OB_BAND_DATA };

static uint8_t outer_coeff(int row, int col)
{
    return row == GROUP ? 1 : mul((uint8_t)(GROUP ^ col), inv((uint8_t)(row ^ col)));
}

static void add_scaled(uint8_t *dst, const uint8_t *src, int n, uint8_t factor)
{
    int i;
    if (factor == 1) {
        for (i = 0; i < n; ++i)
            dst[i] ^= src[i];
    } else if (factor) {
        for (i = 0; i < n; ++i)
            dst[i] ^= mul(factor, src[i]);
    }
}

int ob_outer_symbol(const uint8_t *source, int k, int bytes, int row, uint8_t *out)
{
    int col;
    fec_init();
    if (k < 1 || k > GROUP || bytes < 1 || bytes > MAX_SYMBOL || row < 0 || row > 255 || (row >= k && row < GROUP))
        return 0;
    if (row < k) {
        memcpy(out, source + (size_t)row * bytes, (size_t)bytes);
    } else {
        memset(out, 0, (size_t)bytes);
        for (col = 0; col < k; ++col)
            add_scaled(out, source + (size_t)col * bytes, bytes, outer_coeff(row, col));
    }
    return 1;
}

int ob_outer_recover(const uint8_t *ids, const uint8_t *symbols, int k, int bytes, uint8_t *out)
{
    uint8_t seen[256], missing[GROUP], repair[GROUP], matrix[GROUP][GROUP];
    int m = 0, received_repairs = 0, i, j, r, c;
    fec_init();
    if (k < 1 || k > GROUP || bytes < 1 || bytes > MAX_SYMBOL)
        return 0;
    memset(seen, 0, sizeof seen);
    for (i = 0; i < k; ++i) {
        int id = ids[i];
        if (seen[id] || (id >= k && id < GROUP))
            return 0;
        seen[id] = 1;
        if (id < k)
            memcpy(out + (size_t)id * bytes, symbols + (size_t)i * bytes, (size_t)bytes);
        else
            repair[received_repairs++] = (uint8_t)i;
    }
    if (!received_repairs)
        return 1;
    for (j = 0; j < k; ++j)
        if (!seen[j])
            missing[m++] = (uint8_t)j;
    /* Use output positions of the missing sources as working buffers. */
    for (r = 0; r < m; ++r) {
        int id = ids[repair[r]];
        uint8_t *dest = out + (size_t)missing[r] * bytes;
        memcpy(dest, symbols + (size_t)repair[r] * bytes, (size_t)bytes);
        for (j = 0; j < k; ++j)
            if (seen[j])
                add_scaled(dest, out + (size_t)j * bytes, bytes, outer_coeff(id, j));
        for (c = 0; c < m; ++c)
            matrix[r][c] = outer_coeff(id, missing[c]);
    }
    if (m == 1 && ids[repair[0]] == GROUP)
        return 1;
    for (r = 0; r < m; ++r) {
        uint8_t scale, *pivot;
        if (!matrix[r][r])
            return 0;
        scale = inv(matrix[r][r]);
        pivot = out + (size_t)missing[r] * bytes;
        if (scale != 1) {
            for (i = 0; i < bytes; ++i)
                pivot[i] = mul(scale, pivot[i]);
            for (j = r; j < m; ++j)
                matrix[r][j] = mul(scale, matrix[r][j]);
        }
        for (i = 0; i < m; ++i)
            if (i != r) {
                uint8_t f = matrix[i][r];
                add_scaled(out + (size_t)missing[i] * bytes, pivot, bytes, f);
                for (j = r; j < m; ++j)
                    matrix[i][j] ^= mul(f, matrix[r][j]);
            }
    }
    return 1;
}
