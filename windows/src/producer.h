/* producer.h - producer thread + frame ring (PLAN.md §6.5).
 *
 * The UI thread hands over content and parameters; the producer builds the
 * CPO1 payload (image encoding, file reading, deflate, SHA-256), creates the
 * RaptorQ sender and keeps a ring of ready frames ~2 s ahead of the pacer.
 *
 * Session rules (§5.3): new content, protocol, QR version, ECC level or image
 * format -> new session (new session_id). Code count / quiet zone -> same
 * session, ring flushed and re-laid out. fps -> only the ring depth. */
#ifndef CPW_PRODUCER_H
#define CPW_PRODUCER_H

#include <windows.h>
#include "content.h"
#include "cp.h"

typedef struct job_params {
    int proto;          /* CFG_PROTO_*        session  */
    int version;        /* 5..40              session  */
    int ecc;            /* CFG_ECC_*          session  */
    int image_format;   /* CFG_IMG_*          session  */
    int jpeg_quality;   /*                    session  */
    int count;          /* codes per frame    layout   */
    int quiet;          /* quiet zone modules layout   */
    int fps;            /* ring depth / mask choice    */
} job_params;

enum { JOB_IDLE = 0, JOB_PREPARING, JOB_PLAYING, JOB_ERROR };

typedef struct job_status {
    int             state;
    int             kind;          /* CP_KIND_*                       */
    char            mime[32];
    size_t          nfiles;
    int             img_w, img_h;
    uint64_t        estimate;      /* content raw size estimate (PREPARING) */
    cp_payload_info pinfo;         /* PLAYING                         */
    cp_tx_info      tinfo;         /* PLAYING                         */
    wchar_t         err[160];      /* ERROR                           */
} job_status;

/* Status updates are posted to `notify` as WM_APP_JOB (lParam: malloc'ed job_status). */
BOOL producer_start(HWND notify, const job_params *p);
void producer_stop(void);

/* Takes ownership of c (NULL = stop sending). Always starts a new session. */
void producer_submit(content *c);
void producer_set_params(const job_params *p);

/* Pacer side, non-blocking. Copies the oldest ready frame into *dst (grown
 * with realloc as needed) and its geometry into *g. 1 = got a frame. */
int  producer_pop(uint8_t **dst, size_t *cap, cp_frame_geom *g);
/* TRUE while a session is being prepared or played. */
BOOL producer_busy(void);

#endif
