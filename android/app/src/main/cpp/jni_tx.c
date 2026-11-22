/*
 * JNI sender for com.codetrans.NativeCore: the phone -> PC direction.
 *
 * A handle owns the CPO1 payload and the cp_tx built from it. Frames come out
 * as Decimen QR or optical board module matrices that SendActivity draws
 * with a quiet zone; the PC's camera receiver reads them with the same
 * scanner + rx as the phone does.
 */
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <android/log.h>

#include "cp.h"

#define TAG "CodeTrans"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define FN(name) Java_com_codetrans_NativeCore_##name
#define H2P(type, h) ((type *)(intptr_t)(h))
#define P2H(p) ((jlong)(intptr_t)(p))

typedef struct jtx {
    cp_payload *payload;
    cp_tx      *tx;
    int         proto;
    int         version, ecc;     /* CP_ECC_* */
    size_t      capacity;         /* bytes per code  */
} jtx;

/* byte[] (standard UTF-8, no NUL) -> malloc'd C string; NULL if invalid */
static char *dup_bytes(JNIEnv *env, jbyteArray a)
{
    jsize n;
    char *d;
    if (!a)
        return NULL;
    n = (*env)->GetArrayLength(env, a);
    d = (char *)malloc((size_t)n + 1);
    if (!d)
        return NULL;
    (*env)->GetByteArrayRegion(env, a, 0, n, (jbyte *)d);
    d[n] = 0;
    if (strlen(d) != (size_t)n) {   /* embedded NUL */
        free(d);
        return NULL;
    }
    return d;
}

/* ASCII strings only (MIME types) */
static char *dup_utf(JNIEnv *env, jstring s)
{
    const char *c;
    char *d;
    size_t n;
    if (!s)
        return NULL;
    c = (*env)->GetStringUTFChars(env, s, NULL);
    if (!c)
        return NULL;
    n = strlen(c);
    d = (char *)malloc(n + 1);
    if (d)
        memcpy(d, c, n + 1);
    (*env)->ReleaseStringUTFChars(env, s, c);
    return d;
}

/*
 * kind: CP_KIND_TEXT (datas[0] = UTF-8 text), CP_KIND_IMAGE (datas[0], mime),
 * CP_KIND_FILES (names[i] = UTF-8 bytes of the file name / datas[i]). proto:
 * CP_PROTO_D3 (Cauchy RS-96) or CP_PROTO_OB (bands / tiles).
 * Version and ECC apply only to QR. Returns 0 on error.
 *
 * Names are byte[] rather than String: GetStringUTFChars yields modified
 * UTF-8, which mangles supplementary characters (emoji) in file names.
 */
JNIEXPORT jlong JNICALL FN(txNew)(JNIEnv *env, jclass cls, jint kind, jobjectArray names, jobjectArray datas,
                                  jstring mime, jint proto, jint profile, jint version, jint ecc)
{
    jsize count = datas ? (*env)->GetArrayLength(env, datas) : 0, i;
    jbyteArray *arrs = NULL;
    jbyte **bytes = NULL;
    char **nm = NULL, *mime_c = NULL;
    uint64_t *sizes = NULL;
    jtx *t = NULL;
    cp_tx_opts opts;
    (void)cls;

    if (count < 1 || (kind == CP_KIND_FILES && (!names || (*env)->GetArrayLength(env, names) != count)))
        return 0;
    if (proto == CP_PROTO_D3) {
        if (version < CP_QR_VERSION_MIN || version > CP_QR_VERSION_MAX || ecc < CP_ECC_L || ecc > CP_ECC_H)
            return 0;
    } else if (proto != CP_PROTO_OB || (profile != CP_OB_BANDS && profile != CP_OB_TILES))
        return 0;
    arrs = (jbyteArray *)calloc((size_t)count, sizeof *arrs);
    bytes = (jbyte **)calloc((size_t)count, sizeof *bytes);
    nm = (char **)calloc((size_t)count, sizeof *nm);
    sizes = (uint64_t *)calloc((size_t)count, sizeof *sizes);
    t = (jtx *)calloc(1, sizeof *t);
    if (!arrs || !bytes || !nm || !sizes || !t)
        goto fail;
    for (i = 0; i < count; i++) {
        arrs[i] = (jbyteArray)(*env)->GetObjectArrayElement(env, datas, i);
        if (!arrs[i])
            goto fail;
        sizes[i] = (uint64_t)(*env)->GetArrayLength(env, arrs[i]);
        /* critical access would block the GC for the whole container build */
        bytes[i] = (*env)->GetByteArrayElements(env, arrs[i], NULL);
        if (!bytes[i])
            goto fail;
        if (kind == CP_KIND_FILES) {
            jbyteArray s = (jbyteArray)(*env)->GetObjectArrayElement(env, names, i);
            nm[i] = dup_bytes(env, s);
            if (s)
                (*env)->DeleteLocalRef(env, s);
            if (!nm[i])
                goto fail;
        }
    }
    switch (kind) {
    case CP_KIND_TEXT:
        t->payload = cp_payload_text((const char *)bytes[0], (size_t)sizes[0]);
        break;
    case CP_KIND_IMAGE:
        mime_c = dup_utf(env, mime);
        t->payload = cp_payload_image((const uint8_t *)bytes[0], (size_t)sizes[0],
                                      mime_c ? mime_c : "application/octet-stream");
        break;
    case CP_KIND_FILES:
        t->payload = cp_payload_files((size_t)count, (const char *const *)nm, (const uint8_t *const *)bytes, sizes);
        break;
    default:
        break;
    }
    if (!t->payload)
        goto fail;
    t->proto = proto;
    t->version = version;
    t->ecc = ecc;
    t->capacity = proto == CP_PROTO_D3 ? cp_qr_capacity(version, ecc) : 0;
    memset(&opts, 0, sizeof opts);
    opts.d3_fec = CP_D3_FEC_RS96;
    opts.ob_profile = profile;
    t->tx = cp_tx_new(proto, t->payload, t->capacity, &opts);
    if (!t->tx)
        goto fail;
    goto done;
fail:
    LOGE("txNew failed (kind %d, %d items)", (int)kind, (int)count);
    if (t) {
        cp_payload_free(t->payload);
        free(t);
        t = NULL;
    }
done:
    for (i = 0; arrs && i < count; i++) {
        if (bytes && bytes[i])
            (*env)->ReleaseByteArrayElements(env, arrs[i], bytes[i], JNI_ABORT);
        if (arrs[i])
            (*env)->DeleteLocalRef(env, arrs[i]);
        if (nm)
            free(nm[i]);
    }
    free(arrs);
    free(bytes);
    free(nm);
    free(sizes);
    free(mime_c);
    return P2H(t);
}

JNIEXPORT void JNICALL FN(txFree)(JNIEnv *env, jclass cls, jlong h)
{
    jtx *t = H2P(jtx, h);
    (void)env; (void)cls;
    if (!t)
        return;
    cp_tx_free(t->tx);
    cp_payload_free(t->payload);
    free(t);
}

/*
 * One display refresh: `count` codes / boards as contiguous side*side matrices
 * (1 = dark, no quiet zone; QrPlayerView adds that). fps (requested) only picks
 * the mask policy, see cp_fixed_mask(). Returns side, or 0 on error.
 */
JNIEXPORT jint JNICALL FN(txNextFrame)(JNIEnv *env, jclass cls, jlong h, jint count, jint fps, jbyteArray out)
{
    jtx *t = H2P(jtx, h);
    cp_frame_geom g;
    jbyte *o;
    int ok;
    (void)cls;
    if (!t || !out || count < 1 || count > 2)
        return 0;
    /* stacked, packed rows: code i occupies rows [i * side, (i + 1) * side) */
    if (!cp_frame_geom_calc(&g, t->proto == CP_PROTO_OB ? 40 : t->version, count, 0, 1, 1))
        return 0;
    if ((*env)->GetArrayLength(env, out) < g.stride * g.h)
        return 0;
    o = (*env)->GetByteArrayElements(env, out, NULL);
    if (!o)
        return 0;
    ok = cp_tx_render_frame(t->tx, &g, t->ecc, cp_fixed_mask(fps, count), (uint8_t *)o);
    (*env)->ReleaseByteArrayElements(env, out, o, ok ? 0 : JNI_ABORT);
    return ok ? g.modules : 0;
}

/* Seconds for the receiver to see enough symbols at `fps` refreshes per second. */
JNIEXPORT jdouble JNICALL FN(txEtaSeconds)(JNIEnv *env, jclass cls, jlong h, jint count, jdouble fps)
{
    jtx *t = H2P(jtx, h);
    cp_tx_info ti;
    (void)env; (void)cls;
    if (!t)
        return 0.0;
    cp_tx_info_get(t->tx, &ti);
    return cp_tx_eta_seconds(&ti, count, fps);
}

JNIEXPORT jint JNICALL FN(paceVblanks)(JNIEnv *env, jclass cls, jint fps, jdouble hz)
{
    (void)env; (void)cls;
    return cp_pace_vblanks(fps, hz);
}

JNIEXPORT jdouble JNICALL FN(paceEffectiveFps)(JNIEnv *env, jclass cls, jint fps, jdouble hz)
{
    (void)env; (void)cls;
    return cp_pace_effective_fps(fps, hz);
}

/* Constants of cp.h that Kotlin mirrors; NativeCoreTest checks both agree.
 * out (size 9): [qrVersionMin, qrVersionMax, obSide, bandsUnits, bandsUnitBytes, tilesUnits,
 *                tilesUnitBytes, maxCodes, qrCapacity(40, L)] */
JNIEXPORT void JNICALL FN(constants)(JNIEnv *env, jclass cls, jintArray out)
{
    jint v[9] = { CP_QR_VERSION_MIN, CP_QR_VERSION_MAX, CP_OB_SIDE, cp_ob_units_per_board(CP_OB_BANDS),
                  (jint)cp_ob_unit_bytes(CP_OB_BANDS), cp_ob_units_per_board(CP_OB_TILES),
                  (jint)cp_ob_unit_bytes(CP_OB_TILES), CP_MAX_CODES, (jint)cp_qr_capacity(40, CP_ECC_L) };
    jsize n;
    (void)cls;
    if (!out)
        return;
    n = (*env)->GetArrayLength(env, out);
    (*env)->SetIntArrayRegion(env, out, 0, n < 9 ? n : 9, v);
}

JNIEXPORT jint JNICALL FN(qrCapacity)(JNIEnv *env, jclass cls, jint version, jint ecc)
{
    (void)env; (void)cls;
    return (jint)cp_qr_capacity(version, ecc);
}

/* out (size 6): [rawBytes, containerBytes, compressed, symbolBytes, sourceSymbols, codeBytes] */
JNIEXPORT void JNICALL FN(txInfo)(JNIEnv *env, jclass cls, jlong h, jlongArray out)
{
    jtx *t = H2P(jtx, h);
    cp_payload_info pi;
    cp_tx_info ti;
    jlong v[6];
    jsize n;
    (void)cls;
    if (!t || !out)
        return;
    cp_payload_info_get(t->payload, &pi);
    cp_tx_info_get(t->tx, &ti);
    v[0] = (jlong)pi.raw_len;
    v[1] = (jlong)(ti.proto == CP_PROTO_D3 ? ti.F : pi.container_len);
    v[2] = ti.proto == CP_PROTO_D3 ? ti.d3_gzip : pi.compressed;
    v[3] = ti.T;
    v[4] = ti.total_source_symbols;
    v[5] = (jlong)t->capacity;
    n = (*env)->GetArrayLength(env, out);
    (*env)->SetLongArrayRegion(env, out, 0, n < 6 ? n : 6, v);
}
