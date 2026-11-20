/* Diamond Engine — log ring buffer implementation. */
#include "diamond/dlog.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

typedef struct DlogEntry {
    int seq;
    int level;
    char tag[16];
    char text[DLOG_LINE_MAX];
} DlogEntry;

static pthread_mutex_t g_dlog_lock = PTHREAD_MUTEX_INITIALIZER;
static DlogEntry g_dlog[DLOG_CAPACITY];
static int g_dlog_head;    /* index of the next slot to write */
static int g_dlog_total;   /* number of lines ever produced   */

static const char *dlog_prefix(DlogLevel level) {
    switch (level) {
        case DLOG_WARN:  return "W/";
        case DLOG_ERROR: return "E/";
        case DLOG_LOGIC: return "L/";
        default:         return "I/";
    }
}

void dlog_init(void) {
    pthread_mutex_lock(&g_dlog_lock);
    memset(g_dlog, 0, sizeof(g_dlog));
    g_dlog_head = 0;
    g_dlog_total = 0;
    pthread_mutex_unlock(&g_dlog_lock);
}

void dlog_shutdown(void) { dlog_init(); }

void dlog_trim(void) {
    int i;
    pthread_mutex_lock(&g_dlog_lock);
    for (i = 0; i < DLOG_CAPACITY; ++i) g_dlog[i].seq = -1;
    pthread_mutex_unlock(&g_dlog_lock);
}

void dlog_raw(DlogLevel level, const char *tag, const char *message) {
    DlogEntry *e;
    if (!message) message = "";

    pthread_mutex_lock(&g_dlog_lock);
    e = &g_dlog[g_dlog_head];
    e->seq = g_dlog_total;
    e->level = (int)level;
    snprintf(e->tag, sizeof(e->tag), "%s", tag ? tag : "engine");
    snprintf(e->text, sizeof(e->text), "%s", message);
    g_dlog_head = (g_dlog_head + 1) % DLOG_CAPACITY;
    g_dlog_total++;
    pthread_mutex_unlock(&g_dlog_lock);

#if defined(__ANDROID__)
    {
        int prio = ANDROID_LOG_INFO;
        if (level == DLOG_WARN)  prio = ANDROID_LOG_WARN;
        if (level == DLOG_ERROR) prio = ANDROID_LOG_ERROR;
        __android_log_print(prio, tag ? tag : "Diamond", "%s", message);
    }
#endif
}

void dlog_write(DlogLevel level, const char *tag, const char *fmt, ...) {
    char buffer[DLOG_LINE_MAX];
    va_list args;
    if (!fmt) return;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    dlog_raw(level, tag, buffer);
}

int dlog_total(void) {
    int total;
    pthread_mutex_lock(&g_dlog_lock);
    total = g_dlog_total;
    pthread_mutex_unlock(&g_dlog_lock);
    return total;
}

int dlog_poll(int since_seq, char *out, int cap, int *out_lines) {
    int i, used = 0, lines = 0, count, first;
    if (out_lines) *out_lines = 0;
    if (!out || cap <= 1) return dlog_total();
    out[0] = '\0';

    pthread_mutex_lock(&g_dlog_lock);
    count = g_dlog_total < DLOG_CAPACITY ? g_dlog_total : DLOG_CAPACITY;
    if (since_seq < g_dlog_total - count) since_seq = g_dlog_total - count;
    first = (g_dlog_head - (g_dlog_total - since_seq)) % DLOG_CAPACITY;
    if (first < 0) first += DLOG_CAPACITY;

    for (i = 0; i < g_dlog_total - since_seq; ++i) {
        const DlogEntry *e = &g_dlog[(first + i) % DLOG_CAPACITY];
        int written;
        if (e->seq < since_seq || used >= cap - 1) break;
        written = snprintf(out + used, (size_t)(cap - used - 1), "%s%s: %s\n",
                           dlog_prefix((DlogLevel)e->level),
                           e->tag[0] ? e->tag : "engine", e->text);
        if (written <= 0) break;
        if (written >= cap - used - 1) { used = cap - 1; break; }
        used += written;
        lines++;
    }
    out[used] = '\0';
    if (out_lines) *out_lines = lines;
    pthread_mutex_unlock(&g_dlog_lock);
    return g_dlog_total;
}
