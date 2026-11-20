/* Diamond Engine — self-contained DEFLATE (RFC 1951) codec.
 *
 * The .diamond container stores its payload compressed so that a scene with
 * baked textures and meshes stays small on disk and boots instantly. We ship
 * our own implementation (no zlib, no libdeflate): the whole codec is ~600
 * lines of C99 and adds ~4 KiB to the .so, while zlib would add ~90 KiB and a
 * new licence to the APK.
 *
 * Encoding  : greedy LZ77 with hash chains + dynamic Huffman blocks, falling
 *             back to stored blocks when compression would expand the data.
 * Decoding  : full RFC 1951 support (stored / fixed / dynamic blocks), which
 *             also means any byte stream produced by zlib can be read back.
 */
#ifndef DIAMOND_DEFLATE_H
#define DIAMOND_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compresses `size` bytes into a freshly malloc'ed buffer (*out must be
 * released with ddeflate_free). Returns 0 on success, non-zero on failure.
 * The result is a raw deflate stream (no zlib/gzip wrapper). */
int ddeflate_compress(const void *data, size_t size,
                      void **out, size_t *out_size);

/* Inflates a raw deflate stream. `expected_size` must be the exact size the
 * payload had when compressed (the container stores it in its header).
 * Returns 0 on success, non-zero on corrupt input. */
int ddeflate_decompress(const void *data, size_t size,
                        void *out, size_t expected_size);

void ddeflate_free(void *memory);

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DEFLATE_H */
