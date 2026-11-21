/*
 * Thin RaptorQ (RFC 6330) wrapper over nanorq. Swapping the FEC library only
 * touches this file.
 *
 * - Encoder: one nanorq encoder for the whole object (all Z blocks).
 * - Decoder: one nanorq decoder object per source block, used only for that
 *   block. Different blocks can therefore be decoded concurrently from
 *   different threads; each decoder writes only into its own block buffer.
 *
 * nanorq's OTI packing stores T-1 / Z-1 / N-1; the CodeTrans wire format uses
 * the plain RFC 6330 values, the conversion happens here.
 */
#include "cp_internal.h"

#include "nanorq.h"

/* ---- memory ioctx -------------------------------------------------------- */

typedef struct mem_io {
    struct ioctx io;
    const uint8_t *rbuf;  /* read side (encoder), whole transfer          */
    uint8_t *wbuf;        /* write side (decoder), one block              */
    uint64_t base;        /* transfer offset of buf[0]                    */
    uint64_t len;         /* bytes in buf                                 */
    uint64_t pos;         /* current absolute transfer offset             */
} mem_io;

static size_t mio_read(struct ioctx *io, uint8_t *dst, size_t n)
{
    mem_io *m = (mem_io *)io;
    uint64_t rel;
    if (!m->rbuf || m->pos < m->base)
        return 0;
    rel = m->pos - m->base;
    if (rel > m->len || n > m->len - rel)
        return 0;
    memcpy(dst, m->rbuf + rel, n);
    m->pos += n;
    return n;
}

static size_t mio_write(struct ioctx *io, const uint8_t *src, size_t n)
{
    mem_io *m = (mem_io *)io;
    uint64_t rel;
    if (!m->wbuf || m->pos < m->base)
        return 0;
    rel = m->pos - m->base;
    if (rel > m->len || n > m->len - rel)
        return 0;
    memmove(m->wbuf + rel, src, n);
    m->pos += n;
    return n;
}

static bool mio_seek(struct ioctx *io, const size_t off)
{
    mem_io *m = (mem_io *)io;
    m->pos = off;
    return true;
}

static size_t mio_size(struct ioctx *io)
{
    mem_io *m = (mem_io *)io;
    return (size_t)(m->base + m->len);
}

static long mio_tell(struct ioctx *io)
{
    mem_io *m = (mem_io *)io;
    return (long)m->pos;
}

static void mio_destroy(struct ioctx *io) { (void)io; }

static void mio_init(mem_io *m, const uint8_t *rbuf, uint8_t *wbuf, uint64_t base, uint64_t len)
{
    memset(m, 0, sizeof *m);
    m->io.read = mio_read;
    m->io.write = mio_write;
    m->io.seek = mio_seek;
    m->io.size = mio_size;
    m->io.tell = mio_tell;
    m->io.destroy = mio_destroy;
    m->io.seekable = true;
    m->io.writable = wbuf != NULL;
    m->rbuf = rbuf;
    m->wbuf = wbuf;
    m->base = base;
    m->len = len;
}

static uint64_t nrq_common(const cp_oti *o) { return (o->F << 24) | (uint64_t)(o->T - 1u); }
static uint32_t nrq_specific(const cp_oti *o)
{
    return ((uint32_t)(o->Z - 1u) << 24) | ((uint32_t)(o->N - 1u) << 8) | o->Al;
}

/* ---- encoder ------------------------------------------------------------- */

struct fec_enc {
    nanorq *rq;
    cp_oti oti;
    mem_io io;
    int precalc_tried;
};

fec_enc *fec_enc_new(const uint8_t *data, const cp_oti *o)
{
    fec_enc *e;
    unsigned sbn;
    if (!data || !o || !cp_oti_valid(o) || o->F > (uint64_t)SIZE_MAX)
        return NULL;
    e = (fec_enc *)calloc(1, sizeof *e);
    if (!e)
        return NULL;
    e->oti = *o;
    e->rq = nanorq_encoder_new_ex((size_t)o->F, o->T, 0, o->Z, o->Al);
    if (!e->rq)
        goto fail;
    /* nanorq must have chosen exactly our partition. */
    if (nanorq_symbol_size(e->rq) != o->T || nanorq_blocks(e->rq) != o->Z ||
        nanorq_oti_common(e->rq) != nrq_common(o) || nanorq_oti_scheme_specific(e->rq) != nrq_specific(o))
        goto fail;
    for (sbn = 0; sbn < o->Z; sbn++)
        if (nanorq_block_symbols(e->rq, (uint8_t)sbn) != cp_oti_block_K(o, (uint8_t)sbn))
            goto fail;
    mio_init(&e->io, data, NULL, 0, o->F);
    return e;
fail:
    if (e->rq)
        nanorq_free(e->rq);
    free(e);
    return NULL;
}

int fec_enc_symbol(fec_enc *e, uint8_t sbn, uint32_t esi, uint8_t *out)
{
    uint32_t K;
    if (!e || !out || sbn >= e->oti.Z || esi > CP_MAX_ESI)
        return 0;
    K = cp_oti_block_K(&e->oti, sbn);
    if (esi < K) {
        /* source symbol: straight copy, zero padded at the end of the object */
        uint64_t off = cp_oti_block_offset(&e->oti, sbn) + (uint64_t)esi * e->oti.T;
        size_t n = e->oti.T;
        if (off >= e->oti.F)
            n = 0;
        else if (e->oti.F - off < n)
            n = (size_t)(e->oti.F - off);
        if (n)
            memcpy(out, e->io.rbuf + off, n);
        if (n < e->oti.T)
            memset(out + n, 0, e->oti.T - n);
        return 1;
    }
    if (!e->precalc_tried && e->oti.Z > 1) {
        /* one shared inversion schedule for all blocks of size KL */
        e->precalc_tried = 1;
        (void)nanorq_precalculate(e->rq);
    }
    return nanorq_encode(e->rq, out, esi, sbn, &e->io.io) == e->oti.T;
}

void fec_enc_free(fec_enc *e)
{
    if (!e)
        return;
    nanorq_free(e->rq);
    free(e);
}

/* ---- per-block decoder --------------------------------------------------- */

struct fec_dec {
    nanorq *rq;
    cp_oti oti;
    uint8_t sbn;
    uint32_t K;
    mem_io io;
    int done;
};

fec_dec *fec_dec_new(const cp_oti *o, uint8_t sbn, uint8_t *block_buf)
{
    fec_dec *d;
    if (!o || !block_buf || !cp_oti_valid(o) || sbn >= o->Z || o->F > (uint64_t)SIZE_MAX)
        return NULL;
    d = (fec_dec *)calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->oti = *o;
    d->sbn = sbn;
    d->K = cp_oti_block_K(o, sbn);
    d->rq = nanorq_decoder_new(nrq_common(o), nrq_specific(o));
    if (!d->rq || nanorq_blocks(d->rq) != o->Z || nanorq_block_symbols(d->rq, sbn) != d->K) {
        if (d->rq)
            nanorq_free(d->rq);
        free(d);
        return NULL;
    }
    mio_init(&d->io, NULL, block_buf, cp_oti_block_offset(o, sbn), cp_oti_block_bytes(o, sbn));
    return d;
}

int fec_dec_add(fec_dec *d, uint32_t esi, const uint8_t *symbol)
{
    int r;
    if (!d || !symbol || esi > CP_MAX_ESI)
        return -1;
    if (d->done)
        return 0;
    /* nanorq takes a non-const pointer but only reads from it */
    r = nanorq_decoder_add_symbol(d->rq, (void *)(uintptr_t)symbol, nanorq_tag(d->sbn, esi), &d->io.io);
    if (r == NANORQ_SYM_ADDED)
        return 1;
    if (r == NANORQ_SYM_DUP || r == NANORQ_SYM_IGN)
        return 0;
    return -1;
}

int fec_dec_try(fec_dec *d)
{
    size_t missing, repair;
    if (!d)
        return -1;
    if (d->done)
        return 1;
    missing = nanorq_num_missing(d->rq, d->sbn);
    repair = nanorq_num_repair(d->rq, d->sbn);
    if (missing == 0) {
        d->done = 1;
        return 1;
    }
    if (repair < missing)
        return 0;
    if (nanorq_repair_block(d->rq, &d->io.io, d->sbn)) {
        d->done = 1;
        nanorq_encoder_cleanup(d->rq, d->sbn);   /* release matrices early */
        return 1;
    }
    return 0;
}

void fec_dec_free(fec_dec *d)
{
    if (!d)
        return;
    nanorq_free(d->rq);
    free(d);
}
