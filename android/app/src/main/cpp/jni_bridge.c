/*
 * JNI entry points for com.codetrans.NativeCore (PLAN §6.2).
 *
 * - Handles (rx, scanner) are native pointers passed as jlong; 0 is tolerated
 *   everywhere and treated as "nothing to do".
 * - Image data never touches the Java heap: frames are direct ByteBuffers.
 * - Strings going to Java are built from UTF-16 (NewString), not NewStringUTF:
 *   received text / file names are standard UTF-8 and may contain 4-byte
 *   sequences that are not valid "modified UTF-8" (CheckJNI would abort).
 */
#include <jni.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <android/log.h>

#include "cp.h"
#include "scanner.h"
#include "camera_scan.h"

#define TAG "CodeTrans"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define FN(name) Java_com_codetrans_NativeCore_##name
#define H2P(type, h) ((type *)(intptr_t)(h))
#define P2H(p) ((jlong)(intptr_t)(p))

#define PROGRESS_SIZE 8

static jclass    g_string_cls;   /* java/lang/String          */
static jclass    g_result_cls;   /* com/codetrans/NativeResult */
static jmethodID g_result_ctor;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    JNIEnv *env;
    jclass c;
    (void)reserved;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        return JNI_ERR;

    c = (*env)->FindClass(env, "java/lang/String");
    if (!c)
        return JNI_ERR;
    g_string_cls = (jclass)(*env)->NewGlobalRef(env, c);
    (*env)->DeleteLocalRef(env, c);

    c = (*env)->FindClass(env, "com/codetrans/NativeResult");
    if (!c) {
        LOGE("NativeResult class not found (R8 keep rule missing?)");
        return JNI_ERR;
    }
    g_result_cls = (jclass)(*env)->NewGlobalRef(env, c);
    (*env)->DeleteLocalRef(env, c);
    /* NativeResult(kind, mime, text, textTruncated, paths, names, sizes) */
    g_result_ctor = (*env)->GetMethodID(env, g_result_cls, "<init>",
        "(ILjava/lang/String;Ljava/lang/String;Z[Ljava/lang/String;[Ljava/lang/String;[J)V");
    if (!g_string_cls || !g_result_cls || !g_result_ctor)
        return JNI_ERR;
    return JNI_VERSION_1_6;
}

/* ---- UTF-8 -> java.lang.String ------------------------------------------ */

/* Decodes standard UTF-8 into UTF-16; invalid sequences become U+FFFD. */
static jstring new_string_utf8(JNIEnv *env, const char *s, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    jchar *u, stackbuf[256];
    size_t i = 0, j = 0;
    jstring r;

    if (!s)
        return NULL;
    /* every byte yields at most one UTF-16 unit, 4-byte sequences yield 2 */
    u = n + 1 <= sizeof stackbuf / sizeof stackbuf[0] ? stackbuf : (jchar *)malloc((n + 1) * sizeof(jchar));
    if (!u)
        return NULL;
    while (i < n) {
        uint8_t c = p[i];
        uint32_t cp;
        size_t len, k;
        if (c < 0x80) {
            u[j++] = c;
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
        else { u[j++] = 0xFFFD; i++; continue; }
        if (i + len > n) { u[j++] = 0xFFFD; i++; continue; }
        for (k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80)
                break;
            cp = (cp << 6) | (p[i + k] & 0x3Fu);
        }
        if (k != len || (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
            (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
            u[j++] = 0xFFFD;
            i++;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u[j++] = (jchar)(0xD800 + (cp >> 10));
            u[j++] = (jchar)(0xDC00 + (cp & 0x3FF));
        } else {
            u[j++] = (jchar)cp;
        }
        i += len;
    }
    r = (*env)->NewString(env, u, (jsize)j);
    if (u != stackbuf)
        free(u);
    return r;
}

static jobjectArray new_string_array(JNIEnv *env, char **items, size_t count)
{
    jobjectArray a = (*env)->NewObjectArray(env, (jsize)count, g_string_cls, NULL);
    size_t i;
    if (!a)
        return NULL;
    for (i = 0; i < count; i++) {
        const char *s = items && items[i] ? items[i] : "";
        jstring js = new_string_utf8(env, s, strlen(s));
        if (!js)
            return NULL;   /* OutOfMemoryError pending */
        (*env)->SetObjectArrayElement(env, a, (jsize)i, js);
        (*env)->DeleteLocalRef(env, js);
    }
    return a;
}

/* ---- rx -------------------------------------------------------------------- */

JNIEXPORT jlong JNICALL FN(rxNew)(JNIEnv *env, jclass cls, jstring outDir, jlong maxBytes,
                                  jboolean enableD3, jboolean enablePlain)
{
    cp_rx_config cfg;
    const char *dir;
    cp_rx *rx;
    (void)cls;
    if (!outDir)
        return 0;
    dir = (*env)->GetStringUTFChars(env, outDir, NULL);   /* app cache path: ASCII */
    if (!dir)
        return 0;
    memset(&cfg, 0, sizeof cfg);
    cfg.out_dir = dir;
    cfg.max_bytes = maxBytes > 0 ? (uint64_t)maxBytes : 0;
    cfg.enable_d3 = enableD3 ? 1 : 0;
    cfg.enable_plain = enablePlain ? 1 : 0;
    rx = cp_rx_new(&cfg);
    (*env)->ReleaseStringUTFChars(env, outDir, dir);
    if (!rx)
        LOGE("cp_rx_new failed");
    return P2H(rx);
}

JNIEXPORT void JNICALL FN(rxFree)(JNIEnv *env, jclass cls, jlong rx)
{
    (void)env; (void)cls;
    cp_rx_free(H2P(cp_rx, rx));
}

JNIEXPORT void JNICALL FN(rxReset)(JNIEnv *env, jclass cls, jlong rx)
{
    (void)env; (void)cls;
    cp_rx_reset(H2P(cp_rx, rx));
}

JNIEXPORT void JNICALL FN(rxSetOptions)(JNIEnv *env, jclass cls, jlong rx, jboolean enableD3,
                                        jboolean enablePlain)
{
    (void)env; (void)cls;
    cp_rx_set_options(H2P(cp_rx, rx), enableD3 ? 1 : 0, enablePlain ? 1 : 0);
}

/* ---- scanner ----------------------------------------------------------------- */

JNIEXPORT jlong JNICALL FN(scannerNew)(JNIEnv *env, jclass cls)
{
    (void)env; (void)cls;
    return P2H(cp_scanner_new());
}

JNIEXPORT void JNICALL FN(scannerFree)(JNIEnv *env, jclass cls, jlong s)
{
    (void)env; (void)cls;
    cp_scanner_free(H2P(cp_scanner, s));
}

JNIEXPORT void JNICALL FN(copyPlane)(JNIEnv *env, jclass cls, jobject src, jint rowStride, jint w,
                                     jint h, jobject dst)
{
    const uint8_t *s;
    uint8_t *d;
    jlong scap, dcap;
    jint y;
    (void)cls;
    if (!src || !dst || w <= 0 || h <= 0 || rowStride < w)
        return;
    s = (const uint8_t *)(*env)->GetDirectBufferAddress(env, src);
    d = (uint8_t *)(*env)->GetDirectBufferAddress(env, dst);
    if (!s || !d)
        return;
    scap = (*env)->GetDirectBufferCapacity(env, src);
    dcap = (*env)->GetDirectBufferCapacity(env, dst);
    /* the last row of a padded plane may be only w bytes long */
    if (dcap < (jlong)w * h || scap < (jlong)rowStride * (h - 1) + w)
        return;
    if (rowStride == w) {
        memcpy(d, s, (size_t)w * (size_t)h);
        return;
    }
    for (y = 0; y < h; y++)
        memcpy(d + (size_t)y * (size_t)w, s + (size_t)y * (size_t)rowStride, (size_t)w);
}

/* quads (optional): [count, then per code: decoded, x0, y0, x1, y1, x2, y2, x3, y3] */
#define QUAD_INTS 9

JNIEXPORT jint JNICALL FN(scan)(JNIEnv *env, jclass cls, jlong s, jlong rx, jobject y, jint w, jint h,
                                jintArray quads, jintArray regions)
{
    const uint8_t *p;
    jlong cap;
    jint ev, n, i, k, room, buf[1 + CP_SCAN_MAX_QUADS * QUAD_INTS];
    cp_scan_quad q[CP_SCAN_MAX_QUADS];
    (void)cls;
    if (!s || !rx || !y || w <= 0 || h <= 0)
        return 0;
    p = (const uint8_t *)(*env)->GetDirectBufferAddress(env, y);
    cap = (*env)->GetDirectBufferCapacity(env, y);
    if (!p || cap < (jlong)w * h)
        return 0;
    if (regions) {
        /* [count, {modules, driftPixels, x0,y0,...,x3,y3}...] */
        jint packed[1 + 10 * CP_SCAN_MAX_QUADS];
        cp_grid grids[CP_SCAN_MAX_QUADS];
        if ((*env)->GetArrayLength(env, regions) != 1 + 10 * CP_SCAN_MAX_QUADS) return 0;
        (*env)->GetIntArrayRegion(env, regions, 0, 1, packed);
        int count = packed[0];
        if (count < 0 || count > CP_SCAN_MAX_QUADS) return 0;
        (*env)->GetIntArrayRegion(env, regions, 1, 10 * count, packed + 1);
        for (i = 0; i < count; i++) {
            const jint *a = packed + 1 + i * 10;
            grids[i].size = a[0];
            grids[i].drift = a[1] >= 0 && a[1] <= CP_SCAN_MAX_DIM ? a[1] : 0;
            for (k = 0; k < 4; k++)
                grids[i].corners[k] = (struct quirc_point){a[2 + 2*k], a[3 + 2*k]};
        }
        ev = cp_scan_seeded(H2P(cp_scanner, s), H2P(cp_rx, rx), p, w, h, grids, &count);
        packed[0] = count;
        for (i = 0; i < count; i++) {
            jint *a = packed + 1 + i * 10;
            a[0] = grids[i].size;
            a[1] = 0;
            for (k = 0; k < 4; k++) {
                a[2 + 2*k] = grids[i].corners[k].x;
                a[3 + 2*k] = grids[i].corners[k].y;
            }
        }
        (*env)->SetIntArrayRegion(env, regions, 0, 1 + (count > 0 ? count * 10 : 0), packed);
    } else {
        ev = cp_scanner_scan(H2P(cp_scanner, s), H2P(cp_rx, rx), p, w, h, NULL);
    }
    if (quads) {
        room = ((*env)->GetArrayLength(env, quads) - 1) / QUAD_INTS;
        if (room >= 0) {
            n = cp_scanner_last_quads(H2P(cp_scanner, s), q, room < CP_SCAN_MAX_QUADS ? room : CP_SCAN_MAX_QUADS);
            buf[0] = n;
            for (i = 0; i < n; i++) {
                jint *o = buf + 1 + i * QUAD_INTS;
                o[0] = q[i].decoded;
                for (k = 0; k < 4; k++) {
                    o[1 + 2 * k] = q[i].x[k];
                    o[2 + 2 * k] = q[i].y[k];
                }
            }
            (*env)->SetIntArrayRegion(env, quads, 0, 1 + n * QUAD_INTS, buf);
        }
    }
    return ev;
}

/* ---- results ----------------------------------------------------------------- */

JNIEXPORT void JNICALL FN(progress)(JNIEnv *env, jclass cls, jlong rx, jlongArray out)
{
    cp_progress p;
    jlong v[PROGRESS_SIZE];
    jsize n;
    (void)cls;
    if (!out)
        return;
    cp_rx_progress(H2P(cp_rx, rx), &p);   /* zeroes p when rx is NULL */
    v[0] = p.proto;
    v[1] = p.kind;
    v[2] = (jlong)p.have;
    v[3] = (jlong)p.need;
    v[4] = (jlong)p.total_bytes;
    v[5] = (jlong)p.bytes_per_sec;
    v[6] = p.codes_in_frame;
    v[7] = (jlong)p.session_id;   /* unsigned 32-bit, Kotlin masks */
    n = (*env)->GetArrayLength(env, out);
    (*env)->SetLongArrayRegion(env, out, 0, n < PROGRESS_SIZE ? n : PROGRESS_SIZE, v);
}

JNIEXPORT jbyteArray JNICALL FN(takePlain)(JNIEnv *env, jclass cls, jlong rx)
{
    size_t n = 0;
    uint8_t *p;
    jbyteArray a;
    (void)cls;
    p = cp_rx_take_plain(H2P(cp_rx, rx), &n);
    if (!p)
        return NULL;
    a = (*env)->NewByteArray(env, (jsize)n);
    if (a)
        (*env)->SetByteArrayRegion(env, a, 0, (jsize)n, (const jbyte *)p);
    cp_free(p);
    return a;
}

JNIEXPORT jobject JNICALL FN(takeResult)(JNIEnv *env, jclass cls, jlong rx)
{
    cp_result *r;
    jobject obj = NULL;
    jstring mime = NULL, text = NULL;
    jobjectArray paths = NULL, names = NULL;
    jlongArray sizes = NULL;
    size_t i;
    (void)cls;

    r = cp_rx_take_result(H2P(cp_rx, rx));
    if (!r)
        return NULL;

    mime = new_string_utf8(env, r->mime, strnlen(r->mime, sizeof r->mime));
    if (!mime)
        goto out;
    if (r->text) {
        text = new_string_utf8(env, r->text, r->text_len);
        if (!text)
            goto out;
    }
    paths = new_string_array(env, r->paths, r->count);
    if (!paths)
        goto out;
    names = new_string_array(env, r->names, r->count);
    if (!names)
        goto out;
    sizes = (*env)->NewLongArray(env, (jsize)r->count);
    if (!sizes)
        goto out;
    for (i = 0; i < r->count; i++) {
        jlong v = (jlong)r->sizes[i];
        (*env)->SetLongArrayRegion(env, sizes, (jsize)i, 1, &v);
    }
    obj = (*env)->NewObject(env, g_result_cls, g_result_ctor, (jint)r->kind, mime, text,
                            r->text_truncated ? JNI_TRUE : JNI_FALSE, paths, names, sizes);
out:
    if (!obj)
        LOGE("takeResult: could not build NativeResult (%zu files)", r->count);
    cp_result_free(r);
    return obj;
}
