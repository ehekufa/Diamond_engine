/* Diamond Engine — DEFLATE (RFC 1951) encoder and decoder.
 *
 * Encoder: greedy LZ77 (32 KiB window, hash chains), one dynamic-Huffman block
 * per payload, transparent fallback to stored blocks when the data is
 * incompressible.
 * Decoder: stored / fixed / dynamic blocks, bit-exact for any RFC 1951 stream
 * (including zlib-produced ones, which the tests verify).
 *
 * Code-length limiting: Huffman trees are built with the classic two-queue
 * algorithm. If the resulting depth exceeds the RFC limit (15 bits for
 * literal/distance, 7 for the code-length tree) the builder falls back to a
 * complete balanced code over the same symbol set, which is always valid and
 * only marginally worse. No heap overflow repair, no undefined behaviour.
 */
#include "diamond/deflate.h"

#include <stdlib.h>
#include <string.h>

#define DL_MIN_MATCH   3
#define DL_MAX_MATCH   258
#define DL_WINDOW      32768
#define DL_HASH_BITS   15
#define DL_HASH_SIZE   (1u << DL_HASH_BITS)
#define DL_MAX_CHAIN   32
#define DL_MAX_SYMBOLS 288
#define DL_MAX_BITS    15
#define DL_CL_MAX_BITS 7
#define DL_FIXED_LIT_CODES 288
#define DL_FIXED_DIST_CODES 30

static const uint16_t g_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t g_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t g_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577};
static const uint8_t g_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t g_clc_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static int dl_len_code(int length) {
    int i;
    for (i = 28; i >= 0; --i)
        if (length >= g_len_base[i]) return i;
    return 0;
}

static int dl_dist_code(int distance) {
    int i;
    for (i = 29; i >= 0; --i)
        if (distance >= g_dist_base[i]) return i;
    return 0;
}

/* ======================================================================== */
/*  Bit writer                                                              */
/* ======================================================================== */

typedef struct BitWriter {
    unsigned char *data;
    size_t capacity;
    size_t bitpos;
    int failed;
} BitWriter;

static void bw_init(BitWriter *bw, unsigned char *data, size_t capacity) {
    bw->data = data;
    bw->capacity = capacity;
    bw->bitpos = 0;
    bw->failed = 0;
}

static void bw_put(BitWriter *bw, uint32_t value, int count) {
    int i;
    if (bw->failed) return;
    for (i = 0; i < count; ++i) {
        size_t byte = bw->bitpos >> 3;
        if (byte >= bw->capacity) { bw->failed = 1; return; }
        if (value & (1u << i)) bw->data[byte] |= (unsigned char)(1u << (bw->bitpos & 7));
        bw->bitpos++;
    }
}

/* ======================================================================== */
/*  Huffman code construction                                               */
/* ======================================================================== */

typedef struct HCode {
    uint16_t code;    /* already bit-reversed, ready for bw_put() */
    uint8_t length;
} HCode;

/* Fills `codes[0..count)` with canonical codes derived from `lengths`. */
static void huff_assign_codes(const uint8_t *lengths, int count, HCode *codes) {
    uint16_t bl_count[DL_MAX_BITS + 1];
    uint16_t next_code[DL_MAX_BITS + 1];
    uint32_t value = 0;
    int i, bits;

    memset(bl_count, 0, sizeof(bl_count));
    for (i = 0; i < count; ++i) {
        codes[i].length = lengths[i];
        codes[i].code = 0;
        if (lengths[i]) bl_count[lengths[i]]++;
    }
    next_code[0] = 0;
    for (bits = 1; bits <= DL_MAX_BITS; ++bits) {
        value = (value + bl_count[bits - 1]) << 1;
        next_code[bits] = (uint16_t)value;
    }
    for (i = 0; i < count; ++i) {
        int length = lengths[i];
        if (!length) continue;
        {
            uint16_t code = next_code[length]++;
            uint16_t reversed = 0;
            int b;
            for (b = 0; b < length; ++b)
                if (code & (1u << b)) reversed |= (uint16_t)(1u << (length - 1 - b));
            codes[i].code = reversed;
        }
    }
}

/* Balanced (complete) code lengths over `used` symbols: `2^(k+1) - used`
 * symbols get length k, the rest length k+1. Always satisfies Kraft equality
 * and always fits the 15 bit limit for our alphabets. */
static void huff_balanced_lengths(int used, uint8_t *lengths, const int *symbols) {
    int k = 0, short_count, i;
    while ((1 << (k + 1)) <= used) k++;
    short_count = (1 << (k + 1)) - used;
    for (i = 0; i < used; ++i)
        lengths[symbols[i]] = (uint8_t)(i < short_count ? k : k + 1);
}

/* Builds Huffman code lengths for `count` symbols. At least one frequency must
 * be non-zero (callers guarantee that). */
static void huff_build(const uint32_t *freq, int count, int limit, HCode *codes) {
    uint8_t lengths[DL_MAX_SYMBOLS];
    int symbols[DL_MAX_SYMBOLS];
    uint64_t weight[DL_MAX_SYMBOLS * 2];
    int parent[DL_MAX_SYMBOLS * 2];
    int leaf_queue[DL_MAX_SYMBOLS];
    int node_queue[DL_MAX_SYMBOLS];
    int used = 0, leaf_head = 0, node_head = 0, node_tail = 0, node_count = 0;
    int i, j;

    memset(lengths, 0, sizeof(lengths));
    for (i = 0; i < count; ++i) if (freq[i]) symbols[used++] = i;

    if (count > DL_MAX_SYMBOLS) return;
    if (used == 0) { huff_assign_codes(lengths, count, codes); return; }
    if (used == 1) {
        lengths[symbols[0]] = 1;
        huff_assign_codes(lengths, count, codes);
        return;
    }

    /* Insertion sort by ascending frequency (<= 288 entries). */
    for (i = 1; i < used; ++i) {
        int key = symbols[i];
        for (j = i - 1; j >= 0 && freq[symbols[j]] > freq[key]; --j) symbols[j + 1] = symbols[j];
        symbols[j + 1] = key;
    }

    for (i = 0; i < used; ++i) {
        leaf_queue[i] = i;
        weight[i] = freq[symbols[i]];
        parent[i] = -1;
    }
    node_count = used;

    while ((used - leaf_head) + (node_tail - node_head) > 1) {
        int pick[2];
        int k;
        for (k = 0; k < 2; ++k) {
            int have_leaf = leaf_head < used;
            int have_node = node_head < node_tail;
            if (have_leaf && (!have_node || weight[leaf_queue[leaf_head]] <= weight[node_queue[node_head]]))
                pick[k] = leaf_queue[leaf_head++];
            else if (have_node)
                pick[k] = node_queue[node_head++];
            else
                pick[k] = -1;
        }
        if (pick[1] < 0) break;
        weight[node_count] = weight[pick[0]] + weight[pick[1]];
        parent[node_count] = -1;
        parent[pick[0]] = node_count;
        parent[pick[1]] = node_count;
        node_queue[node_tail++] = node_count;
        node_count++;
    }

    {
        int max_depth = 0;
        for (i = 0; i < used; ++i) {
            int depth = 0, cursor = leaf_queue[i];
            while (parent[cursor] >= 0) { depth++; cursor = parent[cursor]; }
            if (depth == 0) depth = 1;
            lengths[symbols[i]] = (uint8_t)(depth > 255 ? 255 : depth);
            if (depth > max_depth) max_depth = depth;
        }
        if (max_depth > limit) {
            memset(lengths, 0, sizeof(lengths));
            huff_balanced_lengths(used, lengths, symbols);
        }
    }
    huff_assign_codes(lengths, count, codes);
}

/* ======================================================================== */
/*  LZ77                                                                    */
/* ======================================================================== */

typedef struct LzToken {
    uint16_t length;   /* < 256: literal byte, otherwise match length */
    uint16_t distance; /* 0 for literals                              */
} LzToken;

static size_t lz_collect(const unsigned char *input, size_t size, LzToken *tokens) {
    int32_t *head = (int32_t *)malloc(sizeof(int32_t) * DL_HASH_SIZE);
    int32_t *prev = (int32_t *)malloc(sizeof(int32_t) * (size ? size : 1));
    size_t count = 0, pos = 0;

    if (!head || !prev) { free(head); free(prev); return (size_t)-1; }
    memset(head, 0xFF, sizeof(int32_t) * DL_HASH_SIZE);

#define DL_HASH3(at) ((((unsigned)input[at] << 10) ^ \
                       ((unsigned)input[at + 1] << 5) ^ \
                       (unsigned)input[at + 2]) & (DL_HASH_SIZE - 1))
#define DL_INSERT(at) do { \
        uint32_t h_ = DL_HASH3(at); \
        prev[at] = head[h_]; \
        head[h_] = (int32_t)(at); \
    } while (0)

    while (pos < size) {
        size_t best_len = 0, best_dist = 0;

        if (pos + DL_MIN_MATCH <= size) {
            int32_t candidate = head[DL_HASH3(pos)];
            int chain = DL_MAX_CHAIN;
            size_t max_len = size - pos;
            if (max_len > DL_MAX_MATCH) max_len = DL_MAX_MATCH;
            while (candidate >= 0 && chain-- > 0) {
                size_t distance = pos - (size_t)candidate;
                size_t length = 0;
                if (distance == 0 || distance > DL_WINDOW) break;
                while (length < max_len &&
                       input[candidate + length] == input[pos + length]) length++;
                if (length > best_len) {
                    best_len = length;
                    best_dist = distance;
                    if (length >= max_len) break;
                }
                candidate = prev[candidate];
            }
        }

        if (best_len >= DL_MIN_MATCH) {
            size_t end = pos + best_len;
            tokens[count].length = (uint16_t)best_len;
            tokens[count].distance = (uint16_t)best_dist;
            count++;
            while (pos < end) {
                if (pos + DL_MIN_MATCH <= size) DL_INSERT(pos);
                pos++;
            }
        } else {
            tokens[count].length = input[pos];
            tokens[count].distance = 0;
            count++;
            if (pos + DL_MIN_MATCH <= size) DL_INSERT(pos);
            pos++;
        }
    }
#undef DL_HASH3
#undef DL_INSERT
    free(head);
    free(prev);
    return count;
}

/* ======================================================================== */
/*  Block emission                                                          */
/* ======================================================================== */

static void emit_tokens(BitWriter *bw, const LzToken *tokens, size_t count,
                        const HCode *lit, const HCode *dist) {
    size_t i;
    for (i = 0; i < count; ++i) {
        if (tokens[i].distance == 0) {
            bw_put(bw, lit[tokens[i].length].code, lit[tokens[i].length].length);
        } else {
            int lc = dl_len_code(tokens[i].length);
            int dc = dl_dist_code(tokens[i].distance);
            bw_put(bw, lit[257 + lc].code, lit[257 + lc].length);
            bw_put(bw, (uint32_t)(tokens[i].length - g_len_base[lc]), g_len_extra[lc]);
            bw_put(bw, dist[dc].code, dist[dc].length);
            bw_put(bw, (uint32_t)(tokens[i].distance - g_dist_base[dc]), g_dist_extra[dc]);
        }
    }
    bw_put(bw, lit[256].code, lit[256].length);
}

static void emit_fixed_block(BitWriter *bw, const LzToken *tokens, size_t count, int final) {
    uint8_t lit_lengths[DL_FIXED_LIT_CODES];
    uint8_t dist_lengths[DL_FIXED_DIST_CODES];
    HCode lit[DL_FIXED_LIT_CODES];
    HCode dist[DL_FIXED_DIST_CODES];
    int s;

    for (s = 0; s < 144; ++s) lit_lengths[s] = 8;
    for (s = 144; s < 256; ++s) lit_lengths[s] = 9;
    for (s = 256; s < 280; ++s) lit_lengths[s] = 7;
    for (s = 280; s < 288; ++s) lit_lengths[s] = 8;
    for (s = 0; s < 30; ++s) dist_lengths[s] = 5;
    huff_assign_codes(lit_lengths, DL_FIXED_LIT_CODES, lit);
    huff_assign_codes(dist_lengths, DL_FIXED_DIST_CODES, dist);

    bw_put(bw, final ? 1u : 0u, 1);
    bw_put(bw, 1, 2);
    emit_tokens(bw, tokens, count, lit, dist);
}

static void emit_dynamic_block(BitWriter *bw, const LzToken *tokens, size_t count, int final) {
    uint32_t lit_freq[DL_MAX_SYMBOLS];
    uint32_t dist_freq[DL_FIXED_DIST_CODES];
    HCode lit[DL_MAX_SYMBOLS];
    HCode dist[DL_FIXED_DIST_CODES];
    uint8_t lit_lengths[DL_MAX_SYMBOLS];
    uint8_t dist_lengths[DL_FIXED_DIST_CODES];
    uint8_t combined[DL_MAX_SYMBOLS + DL_FIXED_DIST_CODES];
    uint8_t rle_symbol[DL_MAX_SYMBOLS + DL_FIXED_DIST_CODES];
    uint8_t rle_extra[DL_MAX_SYMBOLS + DL_FIXED_DIST_CODES];
    uint32_t cl_freq[19];
    HCode cl_codes[19];
    int hlit = 286, hdist = 30, hclen = 19, combined_length = 0, rle_count = 0;
    int i;
    size_t t;

    memset(lit_freq, 0, sizeof(lit_freq));
    memset(dist_freq, 0, sizeof(dist_freq));
    for (t = 0; t < count; ++t) {
        if (tokens[t].distance == 0) {
            lit_freq[tokens[t].length]++;
        } else {
            lit_freq[257 + dl_len_code(tokens[t].length)]++;
            dist_freq[dl_dist_code(tokens[t].distance)]++;
        }
    }
    lit_freq[256] = 1;
    if (dist_freq[0] == 0 && dist_freq[1] == 0) {
        /* HDIST must describe at least one code. */
        dist_freq[0] = 1;
    }

    huff_build(lit_freq, 286, DL_MAX_BITS, lit);
    huff_build(dist_freq, 30, DL_MAX_BITS, dist);
    for (i = 0; i < 286; ++i) lit_lengths[i] = lit[i].length;
    for (i = 0; i < 30; ++i) dist_lengths[i] = dist[i].length;

    while (hlit > 257 && lit_lengths[hlit - 1] == 0) hlit--;
    while (hdist > 1 && dist_lengths[hdist - 1] == 0) hdist--;
    for (i = 0; i < hlit; ++i) combined[combined_length++] = lit_lengths[i];
    for (i = 0; i < hdist; ++i) combined[combined_length++] = dist_lengths[i];

    /* Run-length encode with 16 (repeat previous 3–6), 17 (zeros 3–10),
     * 18 (zeros 11–138). */
    i = 0;
    while (i < combined_length) {
        int run = 1;
        uint8_t value = combined[i];
        while (i + run < combined_length && combined[i + run] == value) run++;
        if (value == 0 && run >= 3) {
            int left = run;
            while (left >= 11) {
                int n = left > 138 ? 138 : left;
                rle_symbol[rle_count] = 18;
                rle_extra[rle_count] = (uint8_t)(n - 11);
                rle_count++;
                left -= n;
            }
            while (left >= 3) {
                int n = left > 10 ? 10 : left;
                rle_symbol[rle_count] = 17;
                rle_extra[rle_count] = (uint8_t)(n - 3);
                rle_count++;
                left -= n;
            }
            while (left-- > 0) { rle_symbol[rle_count] = 0; rle_extra[rle_count] = 0; rle_count++; }
        } else if (value != 0 && run >= 4) {
            int left = run - 1;
            rle_symbol[rle_count] = value;
            rle_extra[rle_count] = 0;
            rle_count++;
            while (left >= 3) {
                int n = left > 6 ? 6 : left;
                rle_symbol[rle_count] = 16;
                rle_extra[rle_count] = (uint8_t)(n - 3);
                rle_count++;
                left -= n;
            }
            while (left-- > 0) { rle_symbol[rle_count] = value; rle_extra[rle_count] = 0; rle_count++; }
        } else {
            int k;
            for (k = 0; k < run; ++k) {
                rle_symbol[rle_count] = value;
                rle_extra[rle_count] = 0;
                rle_count++;
            }
        }
        i += run;
    }

    memset(cl_freq, 0, sizeof(cl_freq));
    for (i = 0; i < rle_count; ++i) cl_freq[rle_symbol[i]]++;
    huff_build(cl_freq, 19, DL_CL_MAX_BITS, cl_codes);

    bw_put(bw, final ? 1u : 0u, 1);
    bw_put(bw, 2, 2);
    bw_put(bw, (uint32_t)(hlit - 257), 5);
    bw_put(bw, (uint32_t)(hdist - 1), 5);
    while (hclen > 4 && cl_freq[g_clc_order[hclen - 1]] == 0) hclen--;
    bw_put(bw, (uint32_t)(hclen - 4), 4);
    for (i = 0; i < hclen; ++i) bw_put(bw, cl_codes[g_clc_order[i]].length, 3);
    for (i = 0; i < rle_count; ++i) {
        int symbol = rle_symbol[i];
        bw_put(bw, cl_codes[symbol].code, cl_codes[symbol].length);
        if (symbol == 16) bw_put(bw, rle_extra[i], 2);
        else if (symbol == 17) bw_put(bw, rle_extra[i], 3);
        else if (symbol == 18) bw_put(bw, rle_extra[i], 7);
    }
    emit_tokens(bw, tokens, count, lit, dist);
}

/* ======================================================================== */
/*  Encoder entry points                                                    */
/* ======================================================================== */

int ddeflate_compress(const void *data, size_t size, void **out, size_t *out_size) {
    const unsigned char *input = (const unsigned char *)data;
    LzToken *tokens = NULL;
    unsigned char *buffer;
    BitWriter bw;
    size_t token_count, capacity;
    size_t dynamic_bytes;

    if (!out || !out_size) return 1;
    *out = NULL;
    *out_size = 0;

    if (!input || size == 0) {
        unsigned char *empty = (unsigned char *)calloc(4, 1);
        if (!empty) return 2;
        bw_init(&bw, empty, 4);
        bw_put(&bw, 1, 1);
        bw_put(&bw, 1, 2);
        bw_put(&bw, 0, 7);
        *out = empty;
        *out_size = (bw.bitpos + 7) / 8;
        return 0;
    }

    tokens = (LzToken *)malloc(sizeof(LzToken) * (size + 1));
    if (!tokens) return 2;
    token_count = lz_collect(input, size, tokens);
    if (token_count == (size_t)-1) { free(tokens); return 2; }

    capacity = size + size / 8 + 4096;
    buffer = (unsigned char *)calloc(capacity, 1);
    if (!buffer) { free(tokens); return 2; }

    bw_init(&bw, buffer, capacity);
    emit_dynamic_block(&bw, tokens, token_count, 1);
    if (bw.failed) {
        bw_init(&bw, buffer, capacity);
        emit_fixed_block(&bw, tokens, token_count, 1);
    }
    dynamic_bytes = bw.failed ? capacity : (bw.bitpos + 7) / 8;
    free(tokens);

    if (bw.failed || dynamic_bytes >= size) {
        size_t blocks = (size + 65534) / 65535;
        unsigned char *stored = (unsigned char *)calloc(size + blocks * 5 + 8, 1);
        size_t offset = 0, written = 0;
        if (!stored) { free(buffer); return 2; }
        do {
            size_t chunk = size - offset;
            int final;
            if (chunk > 65535) chunk = 65535;
            final = (offset + chunk >= size);
            stored[written++] = (unsigned char)(final ? 1 : 0);
            stored[written++] = (unsigned char)(chunk & 0xFF);
            stored[written++] = (unsigned char)((chunk >> 8) & 0xFF);
            stored[written++] = (unsigned char)(~chunk & 0xFF);
            stored[written++] = (unsigned char)((~chunk >> 8) & 0xFF);
            memcpy(stored + written, input + offset, chunk);
            written += chunk;
            offset += chunk;
        } while (offset < size);
        free(buffer);
        *out = stored;
        *out_size = written;
        return 0;
    }

    *out = buffer;
    *out_size = dynamic_bytes;
    return 0;
}

void ddeflate_free(void *memory) { free(memory); }

/* ======================================================================== */
/*  Decoder                                                                 */
/* ======================================================================== */

typedef struct BitReader {
    const unsigned char *data;
    size_t size;
    size_t bitpos;
    int overrun;
} BitReader;

static uint32_t br_get(BitReader *br, int count) {
    uint32_t value = 0;
    int i;
    for (i = 0; i < count; ++i) {
        size_t byte = br->bitpos >> 3;
        if (byte >= br->size) { br->overrun = 1; return value; }
        if (br->data[byte] & (1u << (br->bitpos & 7))) value |= (1u << i);
        br->bitpos++;
    }
    return value;
}

typedef struct HuffTable {
    uint16_t count[DL_MAX_BITS + 1];
    uint16_t symbol[DL_MAX_SYMBOLS];
} HuffTable;

static void huff_table_build(HuffTable *table, const uint8_t *lengths, int count) {
    uint16_t offsets[DL_MAX_BITS + 1];
    int i, total = 0;
    memset(table->count, 0, sizeof(table->count));
    for (i = 0; i < count; ++i) table->count[lengths[i]]++;
    table->count[0] = 0;
    for (i = 1; i <= DL_MAX_BITS; ++i) { offsets[i] = (uint16_t)total; total += table->count[i]; }
    for (i = 0; i < count; ++i)
        if (lengths[i]) table->symbol[offsets[lengths[i]]++] = (uint16_t)i;
}

static int huff_decode(BitReader *br, const HuffTable *table) {
    int code = 0, first = 0, index = 0, length;
    for (length = 1; length <= DL_MAX_BITS; ++length) {
        int count;
        code |= (int)br_get(br, 1);
        if (br->overrun) return -1;
        count = table->count[length];
        if (code - first < count) return table->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_block_data(BitReader *br, unsigned char *out, size_t out_size,
                              size_t *out_pos, const HuffTable *lit,
                              const HuffTable *dist) {
    for (;;) {
        int symbol = huff_decode(br, lit);
        if (symbol < 0) return 1;
        if (symbol < 256) {
            if (*out_pos >= out_size) return 2;
            out[(*out_pos)++] = (unsigned char)symbol;
        } else if (symbol == 256) {
            return 0;
        } else {
            int length_index = symbol - 257;
            int length, distance_index, distance;
            size_t copy;
            if (length_index < 0 || length_index >= 29) return 3;
            length = g_len_base[length_index] + (int)br_get(br, g_len_extra[length_index]);
            distance_index = huff_decode(br, dist);
            if (distance_index < 0 || distance_index >= 30) return 4;
            distance = g_dist_base[distance_index] + (int)br_get(br, g_dist_extra[distance_index]);
            if (distance <= 0 || (size_t)distance > *out_pos) return 5;
            copy = (size_t)length;
            if (*out_pos + copy > out_size) copy = out_size - *out_pos;
            {
                size_t k;
                for (k = 0; k < copy; ++k) {
                    out[*out_pos] = out[*out_pos - (size_t)distance];
                    (*out_pos)++;
                }
            }
        }
        if (br->overrun) return 6;
    }
}

int ddeflate_decompress(const void *data, size_t size, void *out, size_t expected_size) {
    BitReader br;
    unsigned char *output = (unsigned char *)out;
    size_t out_pos = 0;
    int final = 0;

    if (!data || !out || expected_size == 0) return 1;
    br.data = (const unsigned char *)data;
    br.size = size;
    br.bitpos = 0;
    br.overrun = 0;

    while (!final) {
        int type;
        final = (int)br_get(&br, 1);
        type = (int)br_get(&br, 2);
        if (br.overrun) return 2;

        if (type == 0) {
            size_t length, nlength;
            br.bitpos = (br.bitpos + 7) & ~(size_t)7;
            length = br_get(&br, 16);
            nlength = br_get(&br, 16);
            if (br.overrun) return 3;
            if ((length ^ 0xFFFFu) != nlength) return 4;
            if (out_pos + length > expected_size) return 5;
            memcpy(output + out_pos, br.data + (br.bitpos >> 3), length);
            br.bitpos += length * 8;
            out_pos += length;
        } else if (type == 1 || type == 2) {
            HuffTable lit, dist;
            uint8_t lengths[DL_MAX_SYMBOLS];
            int s;
            if (type == 1) {
                for (s = 0; s < 144; ++s) lengths[s] = 8;
                for (s = 144; s < 256; ++s) lengths[s] = 9;
                for (s = 256; s < 280; ++s) lengths[s] = 7;
                for (s = 280; s < 288; ++s) lengths[s] = 8;
                huff_table_build(&lit, lengths, 288);
                for (s = 0; s < 30; ++s) lengths[s] = 5;
                huff_table_build(&dist, lengths, 30);
            } else {
                int hlit = (int)br_get(&br, 5) + 257;
                int hdist = (int)br_get(&br, 5) + 1;
                int hclen = (int)br_get(&br, 4) + 4;
                uint8_t cl_lengths[19];
                HuffTable cl;
                uint8_t all[DL_MAX_SYMBOLS + 32];
                int i, total;
                if (hlit > 286 || hdist > 30) return 6;
                memset(cl_lengths, 0, sizeof(cl_lengths));
                for (i = 0; i < hclen; ++i) cl_lengths[g_clc_order[i]] = (uint8_t)br_get(&br, 3);
                huff_table_build(&cl, cl_lengths, 19);
                total = hlit + hdist;
                memset(all, 0, sizeof(all));
                i = 0;
                while (i < total) {
                    int symbol = huff_decode(&br, &cl);
                    if (symbol < 0) return 7;
                    if (symbol < 16) {
                        all[i++] = (uint8_t)symbol;
                    } else if (symbol == 16) {
                        int repeat = (int)br_get(&br, 2) + 3;
                        uint8_t value = i ? all[i - 1] : 0;
                        while (repeat-- > 0 && i < total) all[i++] = value;
                    } else if (symbol == 17) {
                        int repeat = (int)br_get(&br, 3) + 3;
                        while (repeat-- > 0 && i < total) all[i++] = 0;
                    } else {
                        int repeat = (int)br_get(&br, 7) + 11;
                        while (repeat-- > 0 && i < total) all[i++] = 0;
                    }
                }
                huff_table_build(&lit, all, hlit);
                huff_table_build(&dist, all + hlit, hdist);
            }
            {
                int rc = inflate_block_data(&br, output, expected_size, &out_pos, &lit, &dist);
                if (rc) return 10 + rc;
            }
        } else {
            return 8;
        }
        if (br.overrun) return 9;
        if (out_pos > expected_size) return 10;
    }
    if (out_pos != expected_size) return 11;
    return 0;
}
