/* Diamond Engine — monotonic clock. */
#include "diamond/dutil.h"

#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <errno.h>
#endif

int64_t dtime_millis(void) {
#if defined(_WIN32)
    return (int64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

int64_t dtime_micros(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (int64_t)((now.QuadPart * 1000000) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
#endif
}

double dtime_seconds(void) {
    return (double)dtime_micros() / 1000000.0;
}

void dtime_sleep_ms(int millis) {
    if (millis <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)millis);
#else
    {
        struct timespec ts;
        ts.tv_sec = millis / 1000;
        ts.tv_nsec = (long)(millis % 1000) * 1000000L;
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
    }
#endif
}
