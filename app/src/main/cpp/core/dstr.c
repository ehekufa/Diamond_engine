/* Diamond Engine — growable string buffer + error helper. */
#include "diamond/dutil.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void dbuf_init(DBuf *buf) {
    if (!buf) return;
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
    buf->failed = 0;
}

int dbuf_reserve(DBuf *buf, size_t extra) {
    size_t need;
    char *grown;
    if (!buf || buf->failed) return 0;
    need = buf->length + extra + 1;
    if (need <= buf->capacity) return 1;
    {
        size_t capacity = buf->capacity ? buf->capacity : 64;
        while (capacity < need) capacity *= 2;
        grown = (char *)realloc(buf->data, capacity);
        if (!grown) { buf->failed = 1; return 0; }
        buf->data = grown;
        buf->capacity = capacity;
    }
    return 1;
}

int dbuf_addn(DBuf *buf, const char *text, size_t length) {
    if (!buf || buf->failed) return 0;
    if (!text || length == 0) {
        if (!dbuf_reserve(buf, 0)) return 0;
        buf->data[buf->length] = '\0';
        return 1;
    }
    if (!dbuf_reserve(buf, length)) return 0;
    memcpy(buf->data + buf->length, text, length);
    buf->length += length;
    buf->data[buf->length] = '\0';
    return 1;
}

int dbuf_add(DBuf *buf, const char *text) {
    return dbuf_addn(buf, text, text ? strlen(text) : 0);
}

int dbuf_add_byte(DBuf *buf, unsigned char value) {
    if (!dbuf_reserve(buf, 1)) return 0;
    buf->data[buf->length++] = (char)value;
    buf->data[buf->length] = '\0';
    return 1;
}

int dbuf_printf(DBuf *buf, const char *fmt, ...) {
    va_list args;
    int needed;
    if (!buf || buf->failed || !fmt) return 0;
    va_start(args, fmt);
    needed = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (needed < 0) { buf->failed = 1; return 0; }
    if (!dbuf_reserve(buf, (size_t)needed)) return 0;
    va_start(args, fmt);
    vsnprintf(buf->data + buf->length, (size_t)needed + 1, fmt, args);
    va_end(args);
    buf->length += (size_t)needed;
    return 1;
}

void dbuf_free(DBuf *buf) {
    if (!buf) return;
    free(buf->data);
    dbuf_init(buf);
}

const char *dbuf_cstr(DBuf *buf) {
    if (!buf) return "";
    if (!buf->data) {
        if (!dbuf_reserve(buf, 0)) return "";
        buf->data[0] = '\0';
    }
    return buf->data;
}

int diamond_fail(int code, char *err, int err_cap, const char *fmt, ...) {
    if (err && err_cap > 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(err, (size_t)err_cap, fmt, args);
        va_end(args);
    }
    return code;
}
