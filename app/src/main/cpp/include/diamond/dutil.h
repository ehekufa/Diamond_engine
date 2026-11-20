/* Diamond Engine — small C99 utility layer (arena, strings, hash, time). */
#ifndef DIAMOND_DUTIL_H
#define DIAMOND_DUTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ arena */
typedef struct DArenaBlock DArenaBlock;
typedef struct DArena {
    DArenaBlock *head;
    size_t used;
    size_t total;
} DArena;

void  darena_init(DArena *arena);
void *darena_alloc(DArena *arena, size_t bytes);            /* 16B aligned */
void *darena_calloc(DArena *arena, size_t bytes);
char *darena_strdup(DArena *arena, const char *text);
void  darena_release(DArena *arena);                        /* frees all   */

/* --------------------------------------------------------------- strings */
typedef struct DBuf {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} DBuf;

void dbuf_init(DBuf *buf);
int  dbuf_reserve(DBuf *buf, size_t extra);
int  dbuf_add(DBuf *buf, const char *text);
int  dbuf_addn(DBuf *buf, const char *text, size_t length);
int  dbuf_printf(DBuf *buf, const char *fmt, ...);
int  dbuf_add_byte(DBuf *buf, unsigned char value);
void dbuf_free(DBuf *buf);
const char *dbuf_cstr(DBuf *buf);   /* guarantees NUL termination */

/* --------------------------------------------------------------- hashing */
uint32_t dhash_fnv1a(const void *data, size_t length);
uint32_t dhash_fnv1a_str(const char *text);
uint32_t dcrc32(const void *data, size_t length);

/* ------------------------------------------------------------------ time */
int64_t dtime_millis(void);     /* monotonic clock, milliseconds */
int64_t dtime_micros(void);
double  dtime_seconds(void);
void    dtime_sleep_ms(int millis);

/* ------------------------------------------------------------------- api */
/* Fills `err` (if non-NULL) and returns `code`. Never throws, never exits. */
int diamond_fail(int code, char *err, int err_cap, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DUTIL_H */
