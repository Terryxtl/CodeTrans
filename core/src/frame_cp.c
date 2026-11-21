/* CodeTrans v1 frame: header encode / parse + CRC32. See core/PROTOCOL.md §2. */
#include "cp_internal.h"

void cp_oti_partition(const cp_oti *o, uint32_t *KL, uint32_t *KS, uint32_t *ZL)
{
    uint64_t Kt = cp_oti_Kt(o);
    uint64_t Z = o->Z;
    uint64_t kl = (Kt + Z - 1) / Z;
    uint64_t ks = Kt / Z;
    *KL = (uint32_t)kl;
    *KS = (uint32_t)ks;
    *ZL = (uint32_t)(Kt - ks * Z);
}

uint32_t cp_oti_block_K(const cp_oti *o, uint8_t sbn)
{
    uint32_t KL, KS, ZL;
    if (sbn >= o->Z)
        return 0;
    cp_oti_partition(o, &KL, &KS, &ZL);
    return sbn < ZL ? KL : KS;
}

uint64_t cp_oti_block_offset(const cp_oti *o, uint8_t sbn)
{
    uint32_t KL, KS, ZL;
    uint64_t first;
    cp_oti_partition(o, &KL, &KS, &ZL);
    if (sbn < ZL)
        first = (uint64_t)sbn * KL;
    else
        first = (uint64_t)ZL * KL + (uint64_t)(sbn - ZL) * KS;
    return first * o->T;
}

uint64_t cp_oti_block_bytes(const cp_oti *o, uint8_t sbn)
{
    uint64_t off = cp_oti_block_offset(o, sbn);
    uint64_t len = (uint64_t)cp_oti_block_K(o, sbn) * o->T;
    if (off >= o->F)
        return 0;
    if (len > o->F - off)
        len = o->F - off;
    return len;
}

int cp_oti_valid(const cp_oti *o)
{
    uint64_t Kt, KL;
    if (o->F == 0 || o->F > 0xFFFFFFFFFFull)
        return 0;
    if (o->Al != 1 && o->Al != 2 && o->Al != 4 && o->Al != 8)
        return 0;
    if (o->T == 0 || o->T > CP_MAX_T || (o->T % o->Al) != 0)
        return 0;
    if (o->N != 1)                       /* v1: no sub-blocking */
        return 0;
    if (o->Z == 0 || o->Z > CP_MAX_Z)
        return 0;
    Kt = cp_oti_Kt(o);
    if (o->Z > Kt)                       /* every block needs >= 1 symbol */
        return 0;
    KL = (Kt + o->Z - 1) / o->Z;
    if (KL > CP_MAX_K)
        return 0;
    return 1;
}

int cp_frame_parse(const uint8_t *d, size_t n, uint64_t max_F, cp_frame *out)
{
    uint16_t T;
    uint64_t common;
    uint32_t specific, pid;
    cp_frame f;

    if (!d || n < CP_FRAME_OVERHEAD)
        return CP_FRAME_ERR_SHORT;
    if (d[0] != CP_MAGIC0 || d[1] != CP_MAGIC1)
        return CP_FRAME_ERR_MAGIC;
    if (d[2] != CP_VERSION)
        return CP_FRAME_ERR_VERSION;
    if (d[3] & CP_FLAGS_MUST)
        return CP_FRAME_ERR_FLAGS;
    T = cp_be16r(d + 14);
    if (n != (size_t)CP_FRAME_OVERHEAD + T)
        return CP_FRAME_ERR_LENGTH;
    if (cp_crc32(d, CP_HDR_LEN + (size_t)T) != cp_be32r(d + CP_HDR_LEN + T))
        return CP_FRAME_ERR_CRC;

    memset(&f, 0, sizeof f);
    f.flags = d[3];
    f.session_id = cp_be32r(d + 4);
    common = cp_be64r(d + 8);
    specific = cp_be32r(d + 16);
    pid = cp_be32r(d + 20);
    if ((common >> 16) & 0xFFu)          /* reserved octet must be zero */
        return CP_FRAME_ERR_OTI;
    f.oti.F = common >> 24;
    f.oti.T = T;
    f.oti.Z = (uint8_t)(specific >> 24);
    f.oti.N = (uint16_t)(specific >> 8);
    f.oti.Al = (uint8_t)specific;
    if (!cp_oti_valid(&f.oti))
        return CP_FRAME_ERR_OTI;
    if (max_F == 0 || max_F > CP_MAX_TOTAL_BYTES)
        max_F = CP_MAX_TOTAL_BYTES;
    if (f.oti.F > max_F)
        return CP_FRAME_ERR_TOO_BIG;
    f.sbn = (uint8_t)(pid >> 24);
    f.esi = pid & CP_MAX_ESI;
    if (f.sbn >= f.oti.Z)
        return CP_FRAME_ERR_PAYLOAD_ID;
    f.symbol = d + CP_HDR_LEN;
    if (out)
        *out = f;
    return CP_FRAME_OK;
}

size_t cp_frame_write(uint8_t *buf, size_t cap, uint32_t session_id, const cp_oti *o,
                      uint8_t sbn, uint32_t esi, const uint8_t *symbol)
{
    size_t len;
    if (!buf || !o || !symbol || !cp_oti_valid(o) || sbn >= o->Z || esi > CP_MAX_ESI)
        return 0;
    len = (size_t)CP_FRAME_OVERHEAD + o->T;
    if (cap < len)
        return 0;
    buf[0] = CP_MAGIC0;
    buf[1] = CP_MAGIC1;
    buf[2] = CP_VERSION;
    buf[3] = 0;
    cp_be32w(buf + 4, session_id);
    cp_be64w(buf + 8, (o->F << 24) | o->T);
    cp_be32w(buf + 16, ((uint32_t)o->Z << 24) | ((uint32_t)o->N << 8) | o->Al);
    cp_be32w(buf + 20, ((uint32_t)sbn << 24) | esi);
    if (symbol != buf + CP_HDR_LEN)
        memmove(buf + CP_HDR_LEN, symbol, o->T);
    cp_be32w(buf + CP_HDR_LEN + o->T, cp_crc32(buf, CP_HDR_LEN + (size_t)o->T));
    return len;
}
