// Diamond Engine — block graph to LuaC source.
//
// The generated file is ordinary Lua that LuaC compiles to native code. It
// only calls the engine's C ABI (engine_move, engine_rotate, ...), so the same
// text works in three places:
//   * compiled by LuaC on a desktop/JIT host,
//   * executed by the built-in interpreter on the phone (USE_BUILTIN_FALLBACK),
//   * shipped inside .diamond for later tooling.
#include "diamond/dlogic.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "diamond/dscene.h"
#include "diamond/dutil.h"
}

namespace {

constexpr int kMaxDepth = 32;

struct Codegen {
    DBuf source;
    int emitted;
    int errors;
};

const char *block_label(const DBlock *block) {
    static char buffer[96];
    const char *base = dlogic_kind_name(block->kind);
    if (block->id > 0) {
        std::snprintf(buffer, sizeof(buffer), "%s#%d", base, block->id);
        return buffer;
    }
    return base;
}

char *sanitize_comment(const char *text, char *out, size_t cap) {
    size_t written = 0;
    for (const char *cursor = text; cursor && *cursor && written + 1 < cap; ++cursor) {
        char c = *cursor;
        if (c == '\n' || c == '\r') c = ' ';
        if (c == '"') c = '\'';
        out[written++] = c;
    }
    out[written] = '\0';
    return out;
}

void indent(DBuf *buf, int depth) {
    for (int i = 0; i < depth; ++i) dbuf_add(buf, "  ");
}

// Depth-first emission following the "next" links of a chain.
void emit_chain(Codegen &gen, int start_block, int depth, int *visited, int visited_count) {
    int current = start_block;
    int guard = 0;
    while (current != 0 && guard++ < 256) {
        DBlock *block = dlogic_block_get(current);
        if (!block) return;
        for (int i = 0; i < visited_count; ++i)
            if (visited[i] == current) {
                indent(&gen.source, depth);
                dbuf_printf(&gen.source, "-- cycle stopped at block %d\n", current);
                return;
            }
        if (visited_count < kMaxDepth) visited[visited_count++] = current;

        char comment[96];
        indent(&gen.source, depth);
        dbuf_printf(&gen.source, "-- %s\n", sanitize_comment(block_label(block), comment, sizeof(comment)));

        switch (block->kind) {
            case DBLOCK_MOVE: {
                indent(&gen.source, depth);
                dbuf_printf(&gen.source,
                            "engine_move(%d, %.3f, %.3f, %.3f)\n",
                            block->node, block->a, block->b, block->c);
                break;
            }
            case DBLOCK_ROTATE: {
                indent(&gen.source, depth);
                dbuf_printf(&gen.source,
                            "engine_rotate(%d, %.3f, %.3f, %.3f)\n",
                            block->node, block->a, block->b, block->c);
                break;
            }
            case DBLOCK_SET_POS: {
                indent(&gen.source, depth);
                dbuf_printf(&gen.source,
                            "engine_set_pos(%d, %.3f, %.3f, %.3f)\n",
                            block->node, block->a, block->b, block->c);
                break;
            }
            case DBLOCK_SET_SCALE: {
                indent(&gen.source, depth);
                dbuf_printf(&gen.source,
                            "engine_set_scale(%d, %.3f, %.3f, %.3f)\n",
                            block->node, block->a, block->b, block->c);
                break;
            }
            case DBLOCK_SET_COLOR: {
                indent(&gen.source, depth);
                const unsigned rgba = static_cast<unsigned>(block->param);
                dbuf_printf(&gen.source, "engine_set_color_rgba(%d, %u, %u, %u, %u)\n",
                            block->node, rgba & 0xFFu, (rgba >> 8) & 0xFFu,
                            (rgba >> 16) & 0xFFu, (rgba >> 24) & 0xFFu);
                break;
            }
            case DBLOCK_LOG: {
                char escaped[128];
                size_t out = 0;
                const char *text = block->text[0] ? block->text : "log";
                for (const char *cursor = text; *cursor && out + 2 < sizeof(escaped); ++cursor) {
                    if (*cursor == '"' || *cursor == '\\') escaped[out++] = '\\';
                    if (*cursor == '\n') continue;
                    escaped[out++] = *cursor;
                }
                escaped[out] = '\0';
                indent(&gen.source, depth);
                dbuf_printf(&gen.source, "engine_log(\"block %d: %s\")\n", block->id, escaped);
                break;
            }
            case DBLOCK_IF_BUTTON:
            case DBLOCK_IF_TIMER:
            case DBLOCK_IF_COLLIDE:
            case DBLOCK_IF_VAR: {
                indent(&gen.source, depth);
                if (block->kind == DBLOCK_IF_BUTTON) {
                    dbuf_printf(&gen.source, "if engine_is_btn_pressed(%d) then\n", block->param);
                } else if (block->kind == DBLOCK_IF_TIMER) {
                    dbuf_printf(&gen.source, "if engine_timer(%d, %.3f) then\n", block->id,
                                block->a > 0.0f ? block->a : 1.0f);
                } else if (block->kind == DBLOCK_IF_COLLIDE) {
                    dbuf_printf(&gen.source, "if engine_check_aabb(%d, %d) then\n", block->node, block->param);
                } else {
                    char name[48];
                    sanitize_comment(block->text[0] ? block->text : "flag", name, sizeof(name));
                    dbuf_printf(&gen.source, "if engine_var(\"%s\", %d, %.3f) then\n",
                                name, block->param, block->b);
                }
                // Body: everything the condition links to.
                for (int l = 0; l < dlogic_link_count(); ++l) {
                    DLink *link = dlogic_link_get(l);
                    if (link && link->from == block->id) {
                        int nested[kMaxDepth];
                        int nestedCount = 0;
                        for (int k = 0; k < visited_count; ++k) nested[nestedCount++] = visited[k];
                        emit_chain(gen, link->to, depth + 1, nested, nestedCount);
                    }
                }
                indent(&gen.source, depth);
                dbuf_add(&gen.source, "end\n");
                break;
            }
            case DBLOCK_END:
                return;
            default:
                break;
        }

        // Continue with the (single) outgoing link of an action block.
        int next = 0;
        for (int l = 0; l < dlogic_link_count(); ++l) {
            DLink *link = dlogic_link_get(l);
            if (link && link->from == current) { next = link->to; break; }
        }
        if (block->kind >= DBLOCK_IF_BUTTON && block->kind <= DBLOCK_IF_VAR) return;
        current = next;
    }
}

void emit_events(Codegen &gen, int kind, const char *functionName, const char *signature) {
    dbuf_printf(&gen.source, "function %s(%s)\n", functionName, signature);
    int emitted_any = 0;
    for (int i = 0; i < dlogic_block_count(); ++i) {
        DBlock *block = dlogic_block_get(dlogic_block_at(i));
        if (!block || block->kind != kind) continue;
        emitted_any = 1;
        int visited[kMaxDepth];
        int visitedCount = 0;
        visited[visitedCount++] = block->id;
        int next = 0;
        for (int l = 0; l < dlogic_link_count(); ++l) {
            DLink *link = dlogic_link_get(l);
            if (link && link->from == block->id) { next = link->to; break; }
        }
        if (next) {
            char comment[96];
            indent(&gen.source, 1);
            dbuf_printf(&gen.source, "-- %s\n", sanitize_comment(block_label(block), comment, sizeof(comment)));
            emit_chain(gen, next, 1, visited, visitedCount);
            gen.emitted++;
        }
    }
    if (!emitted_any) indent(&gen.source, 1);
    if (!emitted_any) dbuf_add(&gen.source, "-- no blocks\n");
    dbuf_add(&gen.source, "end\n\n");
}

} // namespace

extern "C" {

/* The generated text lives in these statics; they survive until the next call
 * to dlogic_generate(), which is exactly the contract documented in dlogic.h. */
static DBuf g_generated;
static DBuf g_manual;
static int g_manual_valid = 0;

const char *dlogic_generate(void) {
    Codegen gen;
    dbuf_free(&g_generated);
    dbuf_init(&gen.source);
    gen.emitted = 0;
    gen.errors = 0;

    dbuf_add(&gen.source, "-- Diamond Engine - generated from visual blocks\n");
    dbuf_add(&gen.source, "-- Edit here only after switching the block editor to code mode.\n\n");
    dbuf_add(&gen.source, "function on_start()\n");
    {
        int any = 0;
        for (int i = 0; i < dlogic_block_count(); ++i) {
            DBlock *block = dlogic_block_get(dlogic_block_at(i));
            if (!block || block->kind != DBLOCK_ON_START) continue;
            any = 1;
            int visited[kMaxDepth];
            int visitedCount = 0;
            visited[visitedCount++] = block->id;
            int next = 0;
            for (int l = 0; l < dlogic_link_count(); ++l) {
                DLink *link = dlogic_link_get(l);
                if (link && link->from == block->id) { next = link->to; break; }
            }
            if (next) emit_chain(gen, next, 1, visited, visitedCount);
        }
        if (!any) dbuf_add(&gen.source, "  -- no blocks\n");
    }
    dbuf_add(&gen.source, "end\n\n");

    emit_events(gen, DBLOCK_ON_UPDATE, "on_update", "dt");
    emit_events(gen, DBLOCK_ON_TOUCH, "on_touch", "x, y, down");
    emit_events(gen, DBLOCK_ON_COLLIDE, "on_collide", "a, b");

    dbuf_add(&gen.source, "function update(dt)\n  on_update(dt)\nend\n");

    g_generated = gen.source;
    g_manual_valid = 0;
    return dbuf_cstr(&g_generated);
}

int dlogic_generate_into(char *out, int cap) {
    if (!out || cap <= 1) return 0;
    const char *source = dlogic_generate();
    if (!source) { out[0] = '\0'; return 0; }
    const size_t length = std::strlen(source);
    const size_t copy = length < static_cast<size_t>(cap - 1) ? length : static_cast<size_t>(cap - 1);
    std::memcpy(out, source, copy);
    out[copy] = '\0';
    return static_cast<int>(copy);
}

const char *dlogic_generated_source(void) {
    if (g_manual_valid) return dbuf_cstr(&g_manual);
    if (g_generated.data) return dbuf_cstr(&g_generated);
    return dlogic_generate();
}

void dlogic_set_source(const char *text) {
    dbuf_free(&g_manual);
    dbuf_init(&g_manual);
    if (text) dbuf_add(&g_manual, text);
    g_manual_valid = 1;
}

/* Rebuilds a block graph from generated Lua by scanning one line per action.
 * The generated syntax is fixed, so a tiny line reader is enough and we keep
 * the exact block ids that were emitted in the trailing "#id" comment. */
int dlogic_blocks_from_source(const char *text, int length) {
    if (!text || length <= 0) return DIAMOND_ERR_ARG;
    dlogic_clear();
    const char *cursor = text;
    const char *end = text + length;
    int currentEvent = -1;

    while (cursor < end) {
        const char *lineEnd = cursor;
        while (lineEnd < end && *lineEnd != '\n') ++lineEnd;
        const int lineLength = static_cast<int>(lineEnd - cursor);
        char line[256];
        const int copy = lineLength < 255 ? lineLength : 255;
        std::memcpy(line, cursor, static_cast<size_t>(copy));
        line[copy] = '\0';

        if (std::strstr(line, "function on_start")) currentEvent = DBLOCK_ON_START;
        else if (std::strstr(line, "function on_update")) currentEvent = DBLOCK_ON_UPDATE;
        else if (std::strstr(line, "function on_touch")) currentEvent = DBLOCK_ON_TOUCH;
        else if (std::strstr(line, "function on_collide")) currentEvent = DBLOCK_ON_COLLIDE;
        else if (std::strstr(line, "function update") || std::strstr(line, "function main")) currentEvent = -1;

        int node = 0;
        float a = 0, b = 0, c = 0;
        int kind = -1;
        if (std::sscanf(line, " engine_move(%d, %f, %f, %f)", &node, &a, &b, &c) == 4) kind = DBLOCK_MOVE;
        else if (std::sscanf(line, " engine_rotate(%d, %f, %f, %f)", &node, &a, &b, &c) == 4) kind = DBLOCK_ROTATE;
        else if (std::sscanf(line, " engine_set_pos(%d, %f, %f, %f)", &node, &a, &b, &c) == 4) kind = DBLOCK_SET_POS;
        else if (std::sscanf(line, " engine_set_scale(%d, %f, %f, %f)", &node, &a, &b, &c) == 4) kind = DBLOCK_SET_SCALE;
        else if (std::strstr(line, "engine_log(")) kind = DBLOCK_LOG;

        if (kind >= 0) {
            const int id = dlogic_block_add(kind, node, 0,
                                            40.0f + static_cast<float>(dlogic_block_count() % 4) * 180.0f,
                                            60.0f + static_cast<float>(dlogic_block_count() / 4) * 90.0f,
                                            line, a, b, c);
            if (currentEvent >= 0 && id) {
                // Reuse the event block: one per event kind.
                int eventBlock = 0;
                for (int i = 0; i < dlogic_block_count(); ++i) {
                    DBlock *candidate = dlogic_block_get(dlogic_block_at(i));
                    if (candidate && candidate->kind == currentEvent) { eventBlock = candidate->id; break; }
                }
                if (!eventBlock) {
                    eventBlock = dlogic_block_add(currentEvent, 0, 0, 20.0f,
                                                  60.0f + static_cast<float>(currentEvent) * 120.0f,
                                                  "", 0, 0, 0);
                }
                if (eventBlock) dlogic_link(eventBlock, id);
            }
        }
        cursor = lineEnd + 1;
    }
    return DIAMOND_OK;
}

} // extern "C"
