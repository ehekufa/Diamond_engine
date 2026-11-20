/* Diamond Engine — bump allocator. */
#include "diamond/dutil.h"

#include <stdlib.h>
#include <string.h>

#define DARENA_BLOCK_SIZE 8192
#define DARENA_ALIGN 16

struct DArenaBlock {
    DArenaBlock *next;
    size_t used;
    size_t size;
    unsigned char data[];
};

void darena_init(DArena *arena) {
    if (!arena) return;
    arena->head = NULL;
    arena->used = 0;
    arena->total = 0;
}

static DArenaBlock *darena_new_block(size_t minimum) {
    size_t size = minimum > DARENA_BLOCK_SIZE ? minimum : DARENA_BLOCK_SIZE;
    DArenaBlock *block = (DArenaBlock *)malloc(sizeof(DArenaBlock) + size);
    if (!block) return NULL;
    block->next = NULL;
    block->used = 0;
    block->size = size;
    return block;
}

void *darena_alloc(DArena *arena, size_t bytes) {
    DArenaBlock *block;
    size_t aligned;
    void *result;
    if (!arena) return NULL;
    if (bytes == 0) bytes = 1;
    aligned = (bytes + DARENA_ALIGN - 1) & ~(size_t)(DARENA_ALIGN - 1);

    block = arena->head;
    if (!block || block->used + aligned > block->size) {
        DArenaBlock *fresh = darena_new_block(aligned);
        if (!fresh) return NULL;
        fresh->next = arena->head;
        arena->head = fresh;
        block = fresh;
    }
    result = block->data + block->used;
    block->used += aligned;
    arena->used += aligned;
    arena->total += 1;
    memset(result, 0, aligned);
    return result;
}

void *darena_calloc(DArena *arena, size_t bytes) {
    return darena_alloc(arena, bytes); /* darena_alloc already zeroes */
}

char *darena_strdup(DArena *arena, const char *text) {
    size_t length;
    char *copy;
    if (!text) return NULL;
    length = strlen(text);
    copy = (char *)darena_alloc(arena, length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length + 1);
    return copy;
}

void darena_release(DArena *arena) {
    DArenaBlock *block;
    if (!arena) return;
    block = arena->head;
    while (block) {
        DArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    arena->head = NULL;
    arena->used = 0;
    arena->total = 0;
}
