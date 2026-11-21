/* content.h - what is being sent, before it becomes a CPO1 payload.
 *
 * The UI thread only copies raw bytes out of the clipboard / collects dropped
 * paths (content_new_*). Image encoding, file reading, deflate and SHA-256
 * happen later on the producer thread (content_build), so a large drop or a
 * 4K screenshot never blocks the message loop (PLAN.md §6.4). */
#ifndef CPW_CONTENT_H
#define CPW_CONTENT_H

#include <windows.h>
#include <stdint.h>
#include "cp.h"

enum {
    CONTENT_TEXT = 1,       /* UTF-8, no NUL                                  */
    CONTENT_IMAGE_PNG,      /* PNG bytes from the "PNG" clipboard format      */
    CONTENT_IMAGE_DIB,      /* packed DIB from CF_DIBV5 / CF_DIB              */
    CONTENT_FILES           /* dropped files (paths, read on the producer)    */
};

typedef struct content {
    int       type;
    uint8_t  *data;         /* TEXT / IMAGE_*: owned bytes                    */
    size_t    len;
    size_t    nfiles;       /* FILES                                          */
    wchar_t **paths;
    uint64_t *sizes;        /* sizes seen at drop time                        */
    uint64_t  raw_size;     /* estimate of the payload body, for the UI       */
    uint64_t  hash;         /* identity for de-duplication                    */
} content;

content *content_new_text(const char *utf8, size_t n);                /* copies   */
content *content_new_image(int type, uint8_t *owned, size_t n);       /* takes it */
/* Regular files only; *bad receives the first path that could not be opened. */
content *content_new_files(size_t n, const wchar_t *const *paths, const wchar_t **bad);
void     content_free(content *c);
int      content_same(const content *a, const content *b);
int      content_kind(const content *c);                              /* CP_KIND_* */

typedef struct build_opts {
    int      image_format;  /* CFG_IMG_PNG / CFG_IMG_JPEG */
    int      jpeg_quality;
    uint64_t max_bytes;     /* hard cap for the body      */
} build_opts;

typedef struct build_result {
    cp_payload *payload;
    char        mime[32];   /* images: "image/png" / "image/jpeg" */
    int         img_w, img_h;
    wchar_t     err[160];
} build_result;

/* Producer thread. Returns 1 and r->payload on success, else 0 and r->err. */
int content_build(const content *c, const build_opts *o, build_result *r);

/* 64-bit non-cryptographic hash (de-dup only). */
uint64_t cpw_hash64(uint64_t seed, const void *data, size_t n);

#endif
