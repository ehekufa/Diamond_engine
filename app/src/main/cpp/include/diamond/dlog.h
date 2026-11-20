/* Diamond Engine — log ring buffer (C99, thread safe, no allocations per line) */
#ifndef DIAMOND_DLOG_H
#define DIAMOND_DLOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DLOG_CAPACITY 256
#define DLOG_LINE_MAX 240

typedef enum DlogLevel {
    DLOG_INFO  = 0,
    DLOG_WARN  = 1,
    DLOG_ERROR = 2,
    DLOG_LOGIC = 3
} DlogLevel;

void dlog_init(void);
void dlog_shutdown(void);

/* Formatted line, forwarded to logcat when built for Android. */
void dlog_write(DlogLevel level, const char *tag, const char *fmt, ...);
/* Plain line (already formatted by the caller). */
void dlog_raw(DlogLevel level, const char *tag, const char *message);

/* Monotonic sequence number of the next line to be produced. */
int dlog_total(void);

/* Copies every line with seq >= since_seq into `out` (newline separated,
 * NUL terminated, truncated at `cap`). Writes the number of lines copied to
 * *out_lines. Returns the new "since" value (== dlog_total() on success). */
int dlog_poll(int since_seq, char *out, int cap, int *out_lines);

/* Test helper: forget every stored line but keep the sequence counter. */
void dlog_trim(void);

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DLOG_H */
