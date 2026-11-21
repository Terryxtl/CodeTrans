/* Sender scheduling. See core/PROTOCOL.md §4.
 *
 * CodeTrans v1 (CP_PROTO_RQ):
 * - T = floor((qr_capacity - 28) / Al) * Al, Al = 4, capped at CP_MAX_T.
 * - Z = ceil(Kt / CP_MAX_K) so every source block has K <= 4096.
 * - Frames go round-robin over SBN 0..Z-1; per block the ESI starts at 0
 *   (K source symbols) and then continues with repair symbols forever,
 *   strictly increasing, never repeated.
 *
 * Decimen v3 (CP_PROTO_D3, see d3.c):
 * - The CPO1 payload is re-packed as a DCF2 container.
 * - blockLen = qr_capacity - 22 ("fixed geometry": every frame fills the QR
 *   version at the chosen ECC level completely).
 * - seq counts up from 0; the outer code (Cauchy RS-96 by default, or the
 *   stock LT carousel) maps it to a block. A new session starts when the u32
 *   seq space runs out.
 * A cp_tx is not thread-safe; use one per producer thread. */
#include "cp_internal.h"

#define CP_TX_AL 4u

struct cp_tx {
    int       proto;
    cp_oti    oti;
    uint32_t  session_id;
    uint8_t  *data;          /* private copy of the container bytes */
    fec_enc  *enc;
    uint8_t   next_sbn;
    uint32_t  next_esi[256];
    uint8_t  *sym;           /* T bytes scratch */
    /* Decimen */
    d3_enc   *d3;
    d3_header d3h;
    uint64_t  d3_seq;
    int       d3_gzip;
};

static cp_tx *tx_new_d3(const cp_payload *p, size_t qr_capacity, const cp_tx_opts *opts)
{
    const uint8_t *bytes;
    size_t n = 0, dn = 0;
    uint8_t *dcf = NULL;
    int gz = 0;
    uint32_t sid;
    cp_tx *tx;

    bytes = cp_payload_bytes(p, &n);
    if (!bytes || n == 0 || qr_capacity <= D3_HDR_LEN || qr_capacity - D3_HDR_LEN > 0xFFFFu)
        return NULL;
    if (!d3_from_cpo(bytes, n, &dcf, &dn, &gz))
        return NULL;
    tx = (cp_tx *)calloc(1, sizeof *tx);
    if (!tx) {
        free(dcf);
        return NULL;
    }
    tx->proto = CP_PROTO_D3;
    tx->d3_gzip = gz;
    sid = opts && opts->session_id ? (opts->session_id & 0xFFFFu) : 0;
    while (sid == 0)
        sid = cp_random_u32() & 0xFFFFu;
    tx->session_id = sid;
    tx->d3h.flags = (opts && opts->d3_fec == CP_D3_FEC_LT) ? 0 : D3_FLAG_CAUCHY96;
    tx->d3h.session_id = (uint16_t)sid;
    tx->d3h.block_len = (uint16_t)(qr_capacity - D3_HDR_LEN);
    tx->d3h.total_len = (uint32_t)dn;
    tx->d3h.fnv = d3_fnv1a(dcf, dn);
    tx->d3 = d3_enc_new(dcf, dn, tx->d3h.block_len, tx->d3h.session_id, tx->d3h.flags & D3_FLAG_CAUCHY96);
    tx->data = dcf;
    tx->sym = (uint8_t *)malloc(tx->d3h.block_len);
    if (!tx->d3 || !tx->sym) {
        cp_tx_free(tx);
        return NULL;
    }
    tx->d3h.k = (uint16_t)d3_enc_k(tx->d3);
    tx->oti.F = dn;
    tx->oti.T = tx->d3h.block_len;
    return tx;
}

cp_tx *cp_tx_new(int proto, const cp_payload *p, size_t qr_capacity, const cp_tx_opts *opts)
{
    cp_tx *tx;
    const uint8_t *bytes;
    size_t n = 0;
    uint64_t T, Kt, Z;

    if (proto == CP_PROTO_D3 && p)
        return tx_new_d3(p, qr_capacity, opts);
    if (proto != CP_PROTO_RQ || !p)
        return NULL;
    bytes = cp_payload_bytes(p, &n);
    if (!bytes || n == 0 || (uint64_t)n > CP_MAX_TOTAL_BYTES)
        return NULL;
    if (qr_capacity < CP_FRAME_OVERHEAD + CP_TX_AL)
        return NULL;
    T = (uint64_t)(qr_capacity - CP_FRAME_OVERHEAD) / CP_TX_AL * CP_TX_AL;
    if (T > CP_MAX_T)
        T = CP_MAX_T;
    Kt = ((uint64_t)n + T - 1) / T;
    Z = (Kt + CP_MAX_K - 1) / CP_MAX_K;
    if (Z == 0 || Z > CP_MAX_Z)
        return NULL;

    tx = (cp_tx *)calloc(1, sizeof *tx);
    if (!tx)
        return NULL;
    tx->proto = CP_PROTO_RQ;
    tx->oti.F = n;
    tx->oti.T = (uint16_t)T;
    tx->oti.Z = (uint8_t)Z;
    tx->oti.N = 1;
    tx->oti.Al = (uint8_t)CP_TX_AL;
    if (!cp_oti_valid(&tx->oti))
        goto fail;
    tx->session_id = opts && opts->session_id ? opts->session_id : 0;
    while (tx->session_id == 0)
        tx->session_id = cp_random_u32();
    tx->data = (uint8_t *)malloc(n);
    tx->sym = (uint8_t *)malloc((size_t)T);
    if (!tx->data || !tx->sym)
        goto fail;
    memcpy(tx->data, bytes, n);
    tx->enc = fec_enc_new(tx->data, &tx->oti);
    if (!tx->enc)
        goto fail;
    return tx;
fail:
    cp_tx_free(tx);
    return NULL;
}

size_t cp_tx_next(cp_tx *tx, uint8_t *buf, size_t cap)
{
    uint8_t sbn;
    uint32_t esi;
    if (tx && tx->proto == CP_PROTO_D3) {
        if (!buf || cap < (size_t)D3_HDR_LEN + tx->d3h.block_len || tx->d3_seq > 0xFFFFFFFFull)
            return 0;                      /* seq space exhausted: start a new session */
        tx->d3h.seq = (uint32_t)tx->d3_seq++;
        d3_enc_block(tx->d3, tx->d3h.seq, tx->sym);
        return d3_frame_write(buf, cap, &tx->d3h, tx->sym);
    }
    if (!tx || !buf || cap < (size_t)CP_FRAME_OVERHEAD + tx->oti.T)
        return 0;
    sbn = tx->next_sbn;
    esi = tx->next_esi[sbn];
    if (esi > CP_MAX_ESI)                  /* ESI space exhausted: start a new session */
        return 0;
    if (!fec_enc_symbol(tx->enc, sbn, esi, tx->sym))
        return 0;
    tx->next_esi[sbn] = esi + 1;
    tx->next_sbn = (uint8_t)((sbn + 1u) % tx->oti.Z);
    return cp_frame_write(buf, cap, tx->session_id, &tx->oti, sbn, esi, tx->sym);
}

void cp_tx_info_get(const cp_tx *tx, cp_tx_info *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof *out);
    if (!tx)
        return;
    out->proto = tx->proto;
    out->session_id = tx->session_id;
    out->F = tx->oti.F;
    out->T = tx->oti.T;
    out->Z = tx->oti.Z;
    if (tx->proto == CP_PROTO_D3) {
        out->total_source_symbols = tx->d3h.k;
        out->d3_gzip = tx->d3_gzip;
    } else {
        out->total_source_symbols = (uint32_t)cp_oti_Kt(&tx->oti);
    }
}

void cp_tx_free(cp_tx *tx)
{
    if (!tx)
        return;
    fec_enc_free(tx->enc);
    d3_enc_free(tx->d3);
    free(tx->data);
    free(tx->sym);
    free(tx);
}
