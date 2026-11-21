/* cpwin_qrscan - development tool: decodes screenshots of the CodeTrans window
 * through the receiver core (quirc -> cp_rx), like the phone would.
 *
 *   cpwin_qrscan shot1.png [shot2.png ...]
 *
 * Prints per image how many codes were found / decoded, and the result when
 * the transfer completes (text is printed, files are written to %TEMP%). */
#include "image_enc.h"
#include "quirc.h"
#include "cp.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t *read_all(const wchar_t *path, size_t *n)
{
    FILE *f = _wfopen(path, L"rb");
    uint8_t *b;
    long sz;
    *n = 0;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
    if (b && fread(b, 1, (size_t)sz, f) == (size_t)sz)
        *n = (size_t)sz;
    fclose(f);
    return b;
}

int wmain(int argc, wchar_t **argv)
{
    struct quirc *q = quirc_new();
    cp_rx_config cfg = { 0 };
    char out[MAX_PATH];
    cp_rx *rx;
    int complete = 0;
    GetTempPathA(MAX_PATH, out);
    lstrcatA(out, "cpwin_qrscan");
    cfg.out_dir = out;
    rx = cp_rx_new(&cfg);
    for (int a = 1; a < argc && !complete; a++) {
        size_t n;
        uint8_t *file = read_all(argv[a], &n), *bgra, *img;
        int w, h, qw, qh, found, ok = 0;
        BOOL alpha;
        bgra = file ? image_decode_bgra(file, n, &w, &h, &alpha) : NULL;
        free(file);
        if (!bgra || quirc_resize(q, w, h) < 0) {
            wprintf(L"%ls: cannot load\n", argv[a]);
            free(bgra);
            continue;
        }
        img = quirc_begin(q, &qw, &qh);
        for (size_t i = 0; i < (size_t)w * h; i++)
            img[i] = (uint8_t)((bgra[i * 4] * 29u + bgra[i * 4 + 1] * 150u + bgra[i * 4 + 2] * 77u) >> 8);
        quirc_end(q);
        free(bgra);
        found = quirc_count(q);
        for (int i = 0; i < found; i++) {
            struct quirc_code code;
            static struct quirc_data d;
            int ev;
            quirc_extract(q, i, &code);
            if (quirc_decode(&code, &d) != QUIRC_SUCCESS)
                continue;
            ok++;
            ev = cp_rx_push(rx, d.payload, (size_t)d.payload_len);
            if (ev & CP_EV_COMPLETE) {
                cp_result *r = cp_rx_take_result(rx);
                complete = 1;
                if (r) {
                    printf("COMPLETE kind=%d mime=%s session=%08x files=%u\n", r->kind, r->mime,
                           (unsigned)r->session_id, (unsigned)r->count);
                    if (r->text)
                        printf("TEXT: %s\n", r->text);
                    for (size_t k = 0; k < r->count; k++)
                        printf("  %s (%llu B)\n", r->paths[k], (unsigned long long)r->sizes[k]);
                    cp_result_free(r);
                }
            }
        }
        {
            cp_progress pr;
            cp_rx_progress(rx, &pr);
            wprintf(L"%ls: %dx%d, %d candidates, %d decoded, progress %llu/%llu\n", argv[a], w, h, found, ok,
                    (unsigned long long)pr.have, (unsigned long long)pr.need);
        }
    }
    cp_rx_free(rx);
    quirc_destroy(q);
    return complete ? 0 : 1;
}
