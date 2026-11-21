/* Tiny platform shim: mutex, monotonic clock, randomness, UTF-8 file paths.
 * POSIX (Linux / Android) uses pthread; _WIN32 uses SRWLOCK (Vista+) and the
 * wide-char CRT file APIs, so the Windows build never needs pthread. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "cp_internal.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <direct.h>
#include <errno.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#endif

#if defined(_WIN32)

typedef char cp_srw_size_check[sizeof(SRWLOCK) == sizeof(void *) ? 1 : -1];

void cp_mutex_init(cp_mutex *m) { InitializeSRWLock((PSRWLOCK)&m->srw); }
void cp_mutex_destroy(cp_mutex *m) { (void)m; }
void cp_mutex_lock(cp_mutex *m) { AcquireSRWLockExclusive((PSRWLOCK)&m->srw); }
void cp_mutex_unlock(cp_mutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)&m->srw); }

uint64_t cp_mono_ms(void)
{
    static LARGE_INTEGER freq;   /* benign: every thread computes the same value */
    LARGE_INTEGER now;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (uint64_t)(now.QuadPart / (freq.QuadPart / 1000 ? freq.QuadPart / 1000 : 1));
}

static wchar_t *utf8_to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0)
        return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) {
        free(w);
        return NULL;
    }
    return w;
}

FILE *cp_fopen_utf8(const char *path, const char *mode)
{
    wchar_t *wp = utf8_to_wide(path), *wm = utf8_to_wide(mode);
    FILE *f = NULL;
    if (wp && wm)
        f = _wfopen(wp, wm);
    free(wp);
    free(wm);
    return f;
}

int cp_mkdir_utf8(const char *path)
{
    wchar_t *wp = utf8_to_wide(path);
    int r = -1;
    if (wp) {
        if (CreateDirectoryW(wp, NULL))
            r = 0;
        else if (GetLastError() == ERROR_ALREADY_EXISTS) {
            DWORD a = GetFileAttributesW(wp);
            r = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? 0 : -1;
        }
    }
    free(wp);
    return r;
}

static void os_entropy(uint8_t *buf, size_t n)
{
    LARGE_INTEGER qpc;
    uint64_t v[4];
    QueryPerformanceCounter(&qpc);
    v[0] = (uint64_t)qpc.QuadPart;
    v[1] = ((uint64_t)GetCurrentProcessId() << 32) | GetCurrentThreadId();
    v[2] = (uint64_t)GetTickCount64();
    v[3] = (uint64_t)(uintptr_t)&qpc ^ (uint64_t)(uintptr_t)buf;
    memcpy(buf, v, n < sizeof v ? n : sizeof v);
}

#else /* POSIX */

void cp_mutex_init(cp_mutex *m) { pthread_mutex_init(&m->m, NULL); }
void cp_mutex_destroy(cp_mutex *m) { pthread_mutex_destroy(&m->m); }
void cp_mutex_lock(cp_mutex *m) { pthread_mutex_lock(&m->m); }
void cp_mutex_unlock(cp_mutex *m) { pthread_mutex_unlock(&m->m); }

uint64_t cp_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

FILE *cp_fopen_utf8(const char *path, const char *mode) { return fopen(path, mode); }

int cp_mkdir_utf8(const char *path)
{
    struct stat st;
    if (mkdir(path, 0700) == 0)
        return 0;
    if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;
    return -1;
}

static void os_entropy(uint8_t *buf, size_t n)
{
    struct timespec ts;
    uint64_t v[4];
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, buf, n);
        close(fd);
        if (r == (ssize_t)n)
            return;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    v[0] = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 30);
    clock_gettime(CLOCK_REALTIME, &ts);
    v[1] = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 30);
    v[2] = (uint64_t)getpid();
    v[3] = (uint64_t)(uintptr_t)&ts ^ (uint64_t)(uintptr_t)buf;
    memcpy(buf, v, n < sizeof v ? n : sizeof v);
}

#endif

uint32_t cp_random_u32(void)
{
    static uint32_t counter;   /* only mixes in, races are harmless */
    uint8_t seed[36], h[32];
    uint32_t c = ++counter;
    memset(seed, 0, sizeof seed);
    os_entropy(seed, 32);
    memcpy(seed + 32, &c, 4);
    cp_sha256(seed, sizeof seed, h);
    return cp_be32r(h);
}
