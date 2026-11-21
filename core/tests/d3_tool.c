/*
 * Decimen interoperability tool (frames files = repeated [u16 LE length][frame]).
 *
 *   cp_d3_tool decode <in.frames> <out-dir>
 *       feeds every frame to cp_rx (Decimen enabled) and prints the result
 *   cp_d3_tool encode rs|lt <qr-capacity> text|image|file|files <out.frames>
 *                     <frames> <drop-every> <input...>
 *       builds a CodeTrans payload, sends it with CP_PROTO_D3 and writes the
 *       frames (every drop-every-th one left out, 0 = none)
 *
 * Used to cross-check this implementation against the Decimen reference
 * (TypeScript) in both directions, and to produce golden vectors.
 */
#include "cp_internal.h"

static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = cp_fopen_utf8(path, "rb");
    uint8_t *b = NULL;
    long sz;
    *n = 0;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
    if (b && sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b)
        *n = (size_t)sz;
    return b;
}

static const char *base(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

static int do_decode(const char *in, const char *out_dir)
{
    size_t n, off = 0, i;
    uint8_t *buf = slurp(in, &n);
    cp_rx_config cfg;
    cp_rx *rx;
    int frames = 0, ev_all = 0;
    if (!buf) {
        fprintf(stderr, "cannot read %s\n", in);
        return 2;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = out_dir;
    cfg.enable_d3 = 1;
    rx = cp_rx_new(&cfg);
    while (off + 2 <= n) {
        size_t len = (size_t)buf[off] | ((size_t)buf[off + 1] << 8);
        int ev;
        if (off + 2 + len > n)
            break;
        ev = cp_rx_push(rx, buf + off + 2, len);
        ev_all |= ev;
        frames++;
        off += 2 + len;
        if (ev & CP_EV_COMPLETE) {
            cp_result *r = cp_rx_take_result(rx);
            printf("{\"ok\":true,\"frames\":%d,\"kind\":%d,\"mime\":\"%s\",\"count\":%u", frames, r->kind, r->mime,
                   (unsigned)r->count);
            for (i = 0; i < r->count; i++)
                printf(",\"name%u\":\"%s\",\"size%u\":%llu,\"path%u\":\"%s\"", (unsigned)i, r->names[i],
                       (unsigned)i, (unsigned long long)r->sizes[i], (unsigned)i, r->paths[i]);
            printf("}\n");
            cp_result_free(r);
            cp_rx_free(rx);
            free(buf);
            return 0;
        }
    }
    {
        cp_progress p;
        cp_rx_progress(rx, &p);
        printf("{\"ok\":false,\"frames\":%d,\"events\":%d,\"proto\":%d,\"have\":%llu,\"need\":%llu}\n", frames, ev_all,
               p.proto, (unsigned long long)p.have, (unsigned long long)p.need);
    }
    cp_rx_free(rx);
    free(buf);
    return 1;
}

static int do_encode(int argc, char **argv)
{
    const char *fec = argv[0], *kind = argv[2], *out = argv[3];
    size_t cap = (size_t)strtoul(argv[1], NULL, 10);
    long frames = strtol(argv[4], NULL, 10), drop = strtol(argv[5], NULL, 10), i, written = 0;
    int nin = argc - 6, k;
    cp_payload *p = NULL;
    cp_tx_opts o;
    cp_tx *tx;
    cp_tx_info ti;
    uint8_t **datas = (uint8_t **)calloc((size_t)(nin > 0 ? nin : 1), sizeof(uint8_t *));
    uint64_t *sizes = (uint64_t *)calloc((size_t)(nin > 0 ? nin : 1), sizeof(uint64_t));
    const char **names = (const char **)calloc((size_t)(nin > 0 ? nin : 1), sizeof(char *));
    uint8_t frame[4096];
    FILE *f;

    for (k = 0; k < nin; k++) {
        size_t n;
        datas[k] = slurp(argv[6 + k], &n);
        sizes[k] = n;
        names[k] = base(argv[6 + k]);
        if (!datas[k]) {
            fprintf(stderr, "cannot read %s\n", argv[6 + k]);
            return 2;
        }
    }
    if (!strcmp(kind, "text") && nin == 1)
        p = cp_payload_text((const char *)datas[0], (size_t)sizes[0]);
    else if (!strcmp(kind, "image") && nin == 1)
        p = cp_payload_image(datas[0], (size_t)sizes[0], NULL);
    else if (nin >= 1)
        p = cp_payload_files((size_t)nin, names, (const uint8_t *const *)datas, sizes);
    memset(&o, 0, sizeof o);
    o.session_id = 0x1234;
    o.d3_fec = !strcmp(fec, "lt") ? CP_D3_FEC_LT : CP_D3_FEC_RS96;
    tx = p ? cp_tx_new(CP_PROTO_D3, p, cap, &o) : NULL;
    if (!tx) {
        fprintf(stderr, "cannot create the sender\n");
        return 1;
    }
    cp_tx_info_get(tx, &ti);
    f = cp_fopen_utf8(out, "wb");
    if (!f)
        return 2;
    for (i = 0; written < frames; i++) {
        size_t n = cp_tx_next(tx, frame, sizeof frame);
        uint8_t len[2];
        if (!n)
            return 1;
        if (drop > 0 && i % drop == drop - 1)
            continue;
        len[0] = (uint8_t)n;
        len[1] = (uint8_t)(n >> 8);
        fwrite(len, 1, 2, f);
        fwrite(frame, 1, n, f);
        written++;
    }
    fclose(f);
    printf("{\"k\":%u,\"blockLen\":%u,\"container\":%llu,\"gzip\":%d}\n", ti.total_source_symbols, ti.T,
           (unsigned long long)ti.F, ti.d3_gzip);
    cp_tx_free(tx);
    cp_payload_free(p);
    for (k = 0; k < nin; k++)
        free(datas[k]);
    free(datas);
    free(sizes);
    free((void *)names);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "decode"))
        return do_decode(argv[2], argv[3]);
    if (argc >= 9 && !strcmp(argv[1], "encode"))
        return do_encode(argc - 2, argv + 2);
    fprintf(stderr, "usage: cp_d3_tool decode <in.frames> <out-dir>\n"
                    "       cp_d3_tool encode rs|lt <qr-capacity> text|image|file|files <out.frames> <frames>"
                    " <drop-every> <input...>\n");
    return 2;
}
