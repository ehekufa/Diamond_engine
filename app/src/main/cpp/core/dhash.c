/* Diamond Engine — FNV-1a and CRC-32 (needed by the .diamond container). */
#include "diamond/dutil.h"

uint32_t dhash_fnv1a(const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    uint32_t hash = 2166136261u;
    size_t i;
    if (!bytes) return hash;
    for (i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

uint32_t dhash_fnv1a_str(const char *text) {
    return dhash_fnv1a(text, text ? __builtin_strlen(text) : 0);
}

static uint32_t g_crc_table[256];
static int g_crc_ready;

static void crc_build_table(void) {
    uint32_t i, j, value;
    for (i = 0; i < 256; ++i) {
        value = i;
        for (j = 0; j < 8; ++j)
            value = (value & 1u) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
        g_crc_table[i] = value;
    }
    g_crc_ready = 1;
}

uint32_t dcrc32(const void *data, size_t length) {
    const unsigned char *bytes = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    if (!g_crc_ready) crc_build_table();
    if (!bytes) return 0;
    for (i = 0; i < length; ++i)
        crc = g_crc_table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
