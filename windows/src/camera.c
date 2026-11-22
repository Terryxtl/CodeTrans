/* camera.c - see camera.h
 *
 * Prefer the Android analysis target (1920x1080, up to 60 fps). The reader
 * is then asked for NV12 (luma is its first plane), falling
 * back to YUY2 and RGB32; Media Foundation inserts the MJPEG decoder / video
 * processor as needed. */
#define COBJMACROS
#include "camera.h"
#include "scanner.h"
#include "compat.h"

#include <initguid.h>   /* MF attribute / format GUIDs and IIDs defined in this file */
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef HRESULT (WINAPI *PFN_MFStartup)(ULONG, DWORD);
typedef HRESULT (WINAPI *PFN_MFShutdown)(void);
typedef HRESULT (WINAPI *PFN_MFCreateAttributes)(IMFAttributes **, UINT32);
typedef HRESULT (WINAPI *PFN_MFCreateMediaType)(IMFMediaType **);
typedef HRESULT (WINAPI *PFN_MFEnumDeviceSources)(IMFAttributes *, IMFActivate ***, UINT32 *);
typedef HRESULT (WINAPI *PFN_MFCreateSourceReaderFromMediaSource)(IMFMediaSource *, IMFAttributes *,
                                                                  IMFSourceReader **);

static struct {
    INIT_ONCE once;
    BOOL ok;
    PFN_MFStartup startup;
    PFN_MFShutdown shutdown;
    PFN_MFCreateAttributes create_attributes;
    PFN_MFCreateMediaType create_media_type;
    PFN_MFEnumDeviceSources enum_device_sources;
    PFN_MFCreateSourceReaderFromMediaSource create_reader;
} MF = { INIT_ONCE_STATIC_INIT };

static FARPROC sym(HMODULE m, const char *name) { return m ? GetProcAddress(m, name) : NULL; }

static BOOL CALLBACK mf_load_once(PINIT_ONCE once, void *param, void **ctx)
{
    /* System32 only: never pick up a planted DLL from the exe's directory. */
    HMODULE plat = compat_load_system_dll(L"mfplat.dll");
    HMODULE mf = compat_load_system_dll(L"mf.dll");
    HMODULE rw = compat_load_system_dll(L"mfreadwrite.dll");
    (void)once; (void)param; (void)ctx;
    *(FARPROC *)&MF.startup = sym(plat, "MFStartup");
    *(FARPROC *)&MF.shutdown = sym(plat, "MFShutdown");
    *(FARPROC *)&MF.create_attributes = sym(plat, "MFCreateAttributes");
    *(FARPROC *)&MF.create_media_type = sym(plat, "MFCreateMediaType");
    *(FARPROC *)&MF.enum_device_sources = sym(mf, "MFEnumDeviceSources");
    *(FARPROC *)&MF.create_reader = sym(rw, "MFCreateSourceReaderFromMediaSource");
    MF.ok = MF.startup && MF.shutdown && MF.create_attributes && MF.create_media_type &&
            MF.enum_device_sources && MF.create_reader;
    return TRUE;
}

static BOOL mf_load(void)
{
    InitOnceExecuteOnce(&MF.once, mf_load_once, NULL, NULL);
    return MF.ok;
}

struct camera {
    HANDLE           thread;
    volatile LONG    quit;
    volatile LONG    refs;          /* thread + owner; the last one frees */
    camera_callbacks cb;
    camera_info      info;
    uint8_t         *y;
    size_t           ycap;
};

static void cam_unref(camera *c)
{
    if (InterlockedDecrement(&c->refs) == 0) {
        free(c->y);
        free(c);
    }
}

static void report(camera *c, int state)
{
    c->info.state = state;
    if (c->cb.on_state && !c->quit)
        c->cb.on_state(c->cb.user, &c->info);
}

static void fail(camera *c, const wchar_t *msg, HRESULT hr)
{
    if (hr)
        _snwprintf(c->info.err, 159, L"%ls (0x%08lX)", msg, (unsigned long)hr);
    else
        _snwprintf(c->info.err, 159, L"%ls", msg);
    c->info.err[159] = 0;
    report(c, CAM_FAILED);
}

/* ---- pixel formats --------------------------------------------------- */

void camera_extract_luma(int fmt, const uint8_t *scan0, LONG pitch, int w, int h, uint8_t *y)
{
    for (int r = 0; r < h; r++) {
        const uint8_t *row = scan0 + (ptrdiff_t)r * pitch;
        uint8_t *out = y + (size_t)r * w;
        switch (fmt) {
        case CAM_FMT_NV12:
            memcpy(out, row, (size_t)w);
            break;
        case CAM_FMT_YUY2:
            for (int x = 0; x < w; x++)
                out[x] = row[2 * x];
            break;
        default: /* RGB32: B, G, R, X; BT.601 luma */
            for (int x = 0; x < w; x++) {
                const uint8_t *p = row + 4 * x;
                out[x] = (uint8_t)((p[0] * 29u + p[1] * 150u + p[2] * 77u) >> 8);
            }
            break;
        }
    }
}

static int fmt_of(const GUID *sub)
{
    if (IsEqualGUID(sub, &MFVideoFormat_NV12)) return CAM_FMT_NV12;
    if (IsEqualGUID(sub, &MFVideoFormat_YUY2)) return CAM_FMT_YUY2;
    if (IsEqualGUID(sub, &MFVideoFormat_RGB32)) return CAM_FMT_RGB32;
    return 0;
}

static void fourcc_name(const GUID *sub, char out[8])
{
    DWORD f = sub->Data1;
    if (IsEqualGUID(sub, &MFVideoFormat_RGB32)) {
        strcpy(out, "RGB32");
        return;
    }
    for (int i = 0; i < 4; i++) {
        char ch = (char)((f >> (8 * i)) & 0xFF);
        out[i] = ch >= 32 && ch < 127 ? ch : '?';
    }
    out[4] = 0;
}

/* ---- format choice ---------------------------------------------------- */

int camera_prefer_mode(camera_mode a, camera_mode b)
{
    if (!b.w) return 1;
    int wide_a = a.w * 9 == a.h * 16, wide_b = b.w * 9 == b.h * 16;
    if (wide_a != wide_b) return wide_a;
    int pixels_a = a.w * a.h, pixels_b = b.w * b.h;
    int above_a = pixels_a >= 1920 * 1080, above_b = pixels_b >= 1920 * 1080;
    if (above_a != above_b) return above_a;
    if (pixels_a != pixels_b) return above_a ? pixels_a < pixels_b : pixels_a > pixels_b;
    /* Highest available rate up to 60. If every mode is faster, use the slowest. */
    if (a.fps != b.fps) {
        if (!a.fps || !b.fps) return a.fps > b.fps;
        if (a.fps > 60) return b.fps > 60 && a.fps < b.fps;
        return b.fps > 60 || a.fps > b.fps;
    }
    return a.native_luma > b.native_luma;
}

static int read_mode(IMFMediaType *mt, camera_mode *mode, GUID *sub)
{
    UINT64 fs = 0, fr = 0;
    UINT32 num, den;
    if (FAILED(IMFMediaType_GetGUID(mt, &MF_MT_SUBTYPE, sub)) ||
        FAILED(IMFMediaType_GetUINT64(mt, &MF_MT_FRAME_SIZE, &fs)))
        return 0;
    mode->w = (int)(fs >> 32);
    mode->h = (int)(UINT32)fs;
    if (mode->w < 320 || mode->h < 240 || mode->w > CP_SCAN_MAX_DIM || mode->h > CP_SCAN_MAX_DIM)
        return 0;
    IMFMediaType_GetUINT64(mt, &MF_MT_FRAME_RATE, &fr);
    num = (UINT32)(fr >> 32);
    den = (UINT32)fr;
    mode->fps = den ? (int)(((UINT64)num + den / 2) / den) : 0;
    mode->native_luma = fmt_of(sub) != 0;
    return 1;
}

/* Picks and sets the native type, then the output format. Fills c->info. */
static HRESULT choose_format(camera *c, IMFSourceReader *rd, int *fmt)
{
    const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    static const GUID *const outs[3] = { &MFVideoFormat_NV12, &MFVideoFormat_YUY2, &MFVideoFormat_RGB32 };
    IMFMediaType *mt = NULL, *best = NULL, *out = NULL, *cur = NULL;
    camera_mode best_mode = {0};
    HRESULT hr;
    GUID sub;
    UINT64 fs;

    for (DWORD i = 0; SUCCEEDED(IMFSourceReader_GetNativeMediaType(rd, stream, i, &mt)); i++) {
        camera_mode mode;
        if (read_mode(mt, &mode, &sub) && camera_prefer_mode(mode, best_mode)) {
            if (best)
                IMFMediaType_Release(best);
            best = mt;
            best_mode = mode;
            c->info.fps = mode.fps;
            fourcc_name(&sub, c->info.format);
        } else {
            IMFMediaType_Release(mt);
        }
        mt = NULL;
    }
    if (best) {
        IMFSourceReader_SetCurrentMediaType(rd, stream, NULL, best);   /* else: the device default */
        IMFMediaType_Release(best);
    }
    hr = MF.create_media_type(&out);
    if (FAILED(hr))
        return hr;
    for (int k = 0; k < 3; k++) {
        IMFMediaType_SetGUID(out, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
        IMFMediaType_SetGUID(out, &MF_MT_SUBTYPE, outs[k]);
        hr = IMFSourceReader_SetCurrentMediaType(rd, stream, NULL, out);
        if (SUCCEEDED(hr))
            break;
    }
    IMFMediaType_Release(out);
    if (FAILED(hr))
        return hr;
    hr = IMFSourceReader_GetCurrentMediaType(rd, stream, &cur);
    if (FAILED(hr))
        return hr;
    if (FAILED(IMFMediaType_GetGUID(cur, &MF_MT_SUBTYPE, &sub)) || !(*fmt = fmt_of(&sub)) ||
        FAILED(IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &fs))) {
        IMFMediaType_Release(cur);
        return MF_E_INVALIDMEDIATYPE;
    }
    c->info.width = (int)(fs >> 32);
    c->info.height = (int)(UINT32)fs;
    UINT64 rate;
    if (SUCCEEDED(IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_RATE, &rate)) && (UINT32)rate)
        c->info.fps = (int)(((rate >> 32) + (UINT32)rate / 2) / (UINT32)rate);
    IMFMediaType_Release(cur);
    return S_OK;
}

/* ---- frames ------------------------------------------------------------ */

static void deliver(camera *c, IMFSample *s, int fmt)
{
    IMFMediaBuffer *mb = NULL;
    IMF2DBuffer *b2 = NULL;
    int w = c->info.width, h = c->info.height;
    size_t need = (size_t)w * h;
    BYTE *scan0 = NULL;
    LONG pitch = 0;

    if (w <= 0 || h <= 0 || w > CP_SCAN_MAX_DIM || h > CP_SCAN_MAX_DIM ||
        FAILED(IMFSample_ConvertToContiguousBuffer(s, &mb)))
        return;
    if (c->ycap < need) {
        uint8_t *p = (uint8_t *)realloc(c->y, need);
        if (!p)
            goto out;
        c->y = p;
        c->ycap = need;
    }
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(mb, &IID_IMF2DBuffer, (void **)&b2)) &&
        SUCCEEDED(IMF2DBuffer_Lock2D(b2, &scan0, &pitch))) {
        camera_extract_luma(fmt, scan0, pitch, w, h, c->y);
        IMF2DBuffer_Unlock2D(b2);
    } else {
        BYTE *p = NULL;
        DWORD maxlen = 0, len = 0;
        size_t row;
        if (FAILED(IMFMediaBuffer_Lock(mb, &p, &maxlen, &len)))
            goto out;
        row = fmt == CAM_FMT_NV12 ? (size_t)w : fmt == CAM_FMT_YUY2 ? (size_t)w * 2 : (size_t)w * 4;
        if (len >= row * h) {
            /* RGB32 without a 2D buffer is bottom-up */
            if (fmt == CAM_FMT_RGB32)
                camera_extract_luma(fmt, p + row * (h - 1), -(LONG)row, w, h, c->y);
            else
                camera_extract_luma(fmt, p, (LONG)row, w, h, c->y);
            IMFMediaBuffer_Unlock(mb);
        } else {
            IMFMediaBuffer_Unlock(mb);
            goto out;
        }
    }
    if (c->cb.on_frame && !c->quit)
        c->cb.on_frame(c->cb.user, c->y, w, h);
out:
    if (b2)
        IMF2DBuffer_Release(b2);
    IMFMediaBuffer_Release(mb);
}

/* ---- capture thread ---------------------------------------------------- */

static DWORD WINAPI cam_main(void *arg)
{
    camera *c = (camera *)arg;
    const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    IMFAttributes *attr = NULL, *rattr = NULL;
    IMFActivate **devs = NULL;
    IMFMediaSource *src = NULL;
    IMFSourceReader *rd = NULL;
    UINT32 n = 0;
    BOOL co, mf = FALSE;
    HRESULT hr;
    int fmt = 0;

    co = SUCCEEDED(CoInitializeEx(NULL, COINIT_MULTITHREADED));
    report(c, CAM_OPENING);
    if (!mf_load()) {
        fail(c, L"此系统没有 Media Foundation(Windows N 版需安装“媒体功能包”)", 0);
        goto out;
    }
    hr = MF.startup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        fail(c, L"Media Foundation 启动失败", hr);
        goto out;
    }
    mf = TRUE;
    hr = MF.create_attributes(&attr, 1);
    if (SUCCEEDED(hr))
        hr = IMFAttributes_SetGUID(attr, &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                   &MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr))
        hr = MF.enum_device_sources(attr, &devs, &n);
    if (FAILED(hr) || n == 0) {
        fail(c, L"没有找到摄像头", FAILED(hr) ? hr : 0);
        goto out;
    }
    c->info.count = (int)n;
    c->info.index = (int)((UINT32)c->info.index % n);
    {
        WCHAR *name = NULL;
        UINT32 len = 0;
        if (SUCCEEDED(IMFActivate_GetAllocatedString(devs[c->info.index], &MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                     &name, &len)) && name) {
            lstrcpynW(c->info.name, name, 128);
            CoTaskMemFree(name);
        }
    }
    report(c, CAM_OPENING);   /* now with the device name and count */
    hr = IMFActivate_ActivateObject(devs[c->info.index], &IID_IMFMediaSource, (void **)&src);
    if (FAILED(hr)) {
        fail(c, L"无法打开摄像头(可能正被其他程序使用,或在隐私设置中被禁止)", hr);
        goto out;
    }
    hr = MF.create_attributes(&rattr, 1);
    if (SUCCEEDED(hr))   /* lets the reader convert to RGB32 when the device offers nothing else */
        IMFAttributes_SetUINT32(rattr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    hr = MF.create_reader(src, rattr, &rd);
    if (FAILED(hr)) {
        fail(c, L"无法读取摄像头画面", hr);
        goto out;
    }
    hr = choose_format(c, rd, &fmt);
    if (FAILED(hr)) {
        fail(c, L"摄像头不支持可用的画面格式", hr);
        goto out;
    }
    report(c, CAM_RUNNING);

    while (!c->quit) {
        DWORD si = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample *s = NULL;
        hr = IMFSourceReader_ReadSample(rd, stream, 0, &si, &flags, &ts, &s);
        if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
            if (s)
                IMFSample_Release(s);
            if (!c->quit)
                fail(c, L"摄像头已断开", FAILED(hr) ? hr : 0);
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            IMFMediaType *cur = NULL;
            UINT64 fs;
            if (SUCCEEDED(IMFSourceReader_GetCurrentMediaType(rd, stream, &cur))) {
                if (SUCCEEDED(IMFMediaType_GetUINT64(cur, &MF_MT_FRAME_SIZE, &fs))) {
                    c->info.width = (int)(fs >> 32);
                    c->info.height = (int)(UINT32)fs;
                }
                IMFMediaType_Release(cur);
            }
        }
        if (s) {
            deliver(c, s, fmt);
            IMFSample_Release(s);
        }
    }

out:
    if (rd)
        IMFSourceReader_Release(rd);
    if (src) {
        IMFMediaSource_Shutdown(src);
        IMFMediaSource_Release(src);
    }
    if (devs) {
        for (UINT32 i = 0; i < n; i++)
            IMFActivate_Release(devs[i]);
        CoTaskMemFree(devs);
    }
    if (rattr)
        IMFAttributes_Release(rattr);
    if (attr)
        IMFAttributes_Release(attr);
    if (mf)
        MF.shutdown();
    if (co)
        CoUninitialize();
    if (c->info.state != CAM_FAILED)
        report(c, CAM_STOPPED);
    cam_unref(c);
    return 0;
}

camera *camera_open(int index, const camera_callbacks *cb)
{
    camera *c = (camera *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->cb = *cb;
    c->info.index = index < 0 ? 0 : index;
    c->refs = 2;
    c->thread = CreateThread(NULL, 0, cam_main, c, 0, NULL);
    if (!c->thread) {
        free(c);
        return NULL;
    }
    return c;
}

void camera_close(camera *c)
{
    if (!c)
        return;
    InterlockedExchange(&c->quit, 1);
    /* ReadSample returns with the next frame; a hung driver must not hang the UI.
     * On timeout the thread keeps its own reference and frees the camera later;
     * `quit` already stops every callback that has not started yet. */
    WaitForSingleObject(c->thread, 4000);
    CloseHandle(c->thread);
    cam_unref(c);
}
