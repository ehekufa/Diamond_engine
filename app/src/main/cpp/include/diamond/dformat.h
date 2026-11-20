/* Diamond Engine — .diamond container (magic "DMND").
 *
 * Layout (all integers little endian):
 *
 *   offset  size  field
 *   0       4     magic    "DMND"
 *   4       2     version  (1)
 *   6       2     flags    bit0 = payload deflated
 *   8       4     crc32    of the *uncompressed* payload
 *   12      4     raw size of the payload
 *   16      4     stored size (what follows)
 *   20      ..    payload (deflated when flags bit0 is set)
 *
 * Payload (a small tagged-section stream, every section 4-byte aligned):
 *   'S' u8 | id u8 | size u32 | data
 *   id 1  SCENE    scene nodes (flat records, parent-first order)
 *   id 2  MESHES   baked mesh arrays (no text parsing on load)
 *   id 3  IMAGES   raw RGBA8888
 *   id 4  BLOCKS   block graph (kind, params, positions, links)
 *   id 5  LOGIC    generated LuaC source as UTF-8 text
 *   id 6  META     name\0 + render mode + view mode + counters
 *
 *  The container is written and read by format/diamond_format.cpp. It works on
 *  memory only (callers own the buffer), which is what lets the same code back
 *  the file API, the Java clipboard and the unit tests.
 */
#ifndef DIAMOND_DFORMAT_H
#define DIAMOND_DFORMAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMND_MAGIC "DMND"
#define DMND_VERSION 1
#define DMND_FLAG_DEFLATED 1u

/* Serializes the whole editor state into a malloc'ed buffer. */
int dformat_write(void **out, size_t *out_size, char *err, int err_cap);

/* Restores the editor state from a buffer. The buffer is not modified. */
int dformat_read(const void *buffer, size_t size, char *err, int err_cap);

/* Magic sniffing for the file picker ("is this really our scene?"). */
int dformat_is_diamond(const void *buffer, size_t size);

/* Container header inspection, used by the log console. */
int dformat_probe(const void *buffer, size_t size, int *version, int *deflated,
                  size_t *payload_size);

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DFORMAT_H */
