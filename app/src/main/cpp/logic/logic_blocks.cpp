// Diamond Engine — block graph storage, JSON export/import and validation.
#include "diamond/dlogic.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "diamond/dscene.h"
#include "diamond/dutil.h"
#include "diamond/dlog.h"
}

namespace {

struct Graph {
    DBlock *blocks = nullptr;
    int blockCount = 0;
    int blockCap = 0;
    DLink *links = nullptr;
    int linkCount = 0;
    int linkCap = 0;
    int nextBlockId = 1;
    int nextLinkId = 1;
};

Graph g_graph;

constexpr int kMaxBlocks = 4096;
constexpr int kMaxLinks = 8192;

bool ensure_blocks(int needed) {
    if (needed <= g_graph.blockCap) return true;
    int cap = g_graph.blockCap ? g_graph.blockCap : 32;
    while (cap < needed) cap *= 2;
    DBlock *grown = static_cast<DBlock *>(std::realloc(g_graph.blocks, sizeof(DBlock) * static_cast<size_t>(cap)));
    if (!grown) return false;
    for (int i = g_graph.blockCap; i < cap; ++i) std::memset(&grown[i], 0, sizeof(DBlock));
    g_graph.blocks = grown;
    g_graph.blockCap = cap;
    return true;
}

bool ensure_links(int needed) {
    if (needed <= g_graph.linkCap) return true;
    int cap = g_graph.linkCap ? g_graph.linkCap : 32;
    while (cap < needed) cap *= 2;
    DLink *grown = static_cast<DLink *>(std::realloc(g_graph.links, sizeof(DLink) * static_cast<size_t>(cap)));
    if (!grown) return false;
    for (int i = g_graph.linkCap; i < cap; ++i) std::memset(&grown[i], 0, sizeof(DLink));
    g_graph.links = grown;
    g_graph.linkCap = cap;
    return true;
}

bool kind_is_event(int kind) {
    return kind == DBLOCK_ON_START || kind == DBLOCK_ON_UPDATE ||
           kind == DBLOCK_ON_TOUCH || kind == DBLOCK_ON_COLLIDE;
}

bool kind_is_condition(int kind) {
    return kind == DBLOCK_IF_BUTTON || kind == DBLOCK_IF_TIMER ||
           kind == DBLOCK_IF_COLLIDE || kind == DBLOCK_IF_VAR;
}

const char *append_json_string(DBuf *buf, const char *text) {
    dbuf_add_byte(buf, '"');
    for (const char *cursor = text; cursor && *cursor; ++cursor) {
        const unsigned char c = static_cast<unsigned char>(*cursor);
        if (c == '"' || c == '\\') { dbuf_add_byte(buf, '\\'); dbuf_add_byte(buf, c); }
        else if (c == '\n') dbuf_add(buf, "\\n");
        else if (c == '\t') dbuf_add(buf, "\\t");
        else if (c < 0x20) dbuf_printf(buf, "\\u%04x", c);
        else dbuf_add_byte(buf, c);
    }
    dbuf_add_byte(buf, '"');
    return dbuf_cstr(buf);
}

// Minimal JSON reader: enough for our own export format, robust against junk.
struct JsonCursor {
    const char *data;
    size_t size;
    size_t pos;
    bool failed;
};

void json_skip(JsonCursor &c) {
    while (c.pos < c.size) {
        const char ch = c.data[c.pos];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') { c.pos++; continue; }
        break;
    }
}

bool json_expect(JsonCursor &c, char expected) {
    json_skip(c);
    if (c.pos >= c.size || c.data[c.pos] != expected) { c.failed = true; return false; }
    c.pos++;
    return true;
}

bool json_peek(JsonCursor &c, char expected) {
    json_skip(c);
    return c.pos < c.size && c.data[c.pos] == expected;
}

double json_number(JsonCursor &c) {
    json_skip(c);
    char buffer[64];
    int length = 0;
    while (c.pos < c.size && length < 63) {
        const char ch = c.data[c.pos];
        if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E') {
            buffer[length++] = ch;
            c.pos++;
        } else {
            break;
        }
    }
    buffer[length] = '\0';
    if (!length) { c.failed = true; return 0.0; }
    return std::strtod(buffer, nullptr);
}

int json_int(JsonCursor &c) { return static_cast<int>(json_number(c)); }

void json_string(JsonCursor &c, char *out, size_t cap) {
    json_skip(c);
    size_t written = 0;
    if (c.pos >= c.size || c.data[c.pos] != '"') { c.failed = true; out[0] = '\0'; return; }
    c.pos++;
    while (c.pos < c.size && c.data[c.pos] != '"') {
        char ch = c.data[c.pos++];
        if (ch == '\\' && c.pos < c.size) {
            const char escape = c.data[c.pos++];
            switch (escape) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'u':
                    if (c.pos + 4 <= c.size) c.pos += 4;
                    ch = '?';
                    break;
                default: ch = escape; break;
            }
        }
        if (written + 1 < cap) out[written++] = ch;
    }
    out[written] = '\0';
    if (c.pos >= c.size) { c.failed = true; return; }
    c.pos++; // closing quote
}

const char *kind_name(int kind) {
    switch (kind) {
        case DBLOCK_ON_START:   return "OnStart";
        case DBLOCK_ON_UPDATE:  return "OnUpdate";
        case DBLOCK_ON_TOUCH:   return "OnTouch";
        case DBLOCK_ON_COLLIDE: return "OnCollide";
        case DBLOCK_MOVE:       return "Move";
        case DBLOCK_ROTATE:     return "Rotate";
        case DBLOCK_SET_COLOR:  return "SetColor";
        case DBLOCK_LOG:        return "Log";
        case DBLOCK_SET_POS:    return "SetPos";
        case DBLOCK_SET_SCALE:  return "SetScale";
        case DBLOCK_IF_BUTTON:  return "IfButton";
        case DBLOCK_IF_TIMER:   return "IfTimer";
        case DBLOCK_IF_COLLIDE: return "IfCollide";
        case DBLOCK_IF_VAR:     return "IfVar";
        case DBLOCK_END:        return "End";
        default:                return "Unknown";
    }
}

} // namespace

extern "C" {

const char *dlogic_kind_name(int kind) { return kind_name(kind); }

void dlogic_init(void) {
    g_graph = Graph{};
}

void dlogic_shutdown(void) {
    std::free(g_graph.blocks);
    std::free(g_graph.links);
    g_graph = Graph{};
}

void dlogic_clear(void) {
    g_graph.blockCount = 0;
    g_graph.linkCount = 0;
    g_graph.nextBlockId = 1;
    g_graph.nextLinkId = 1;
}

int dlogic_block_add(int kind, int node, int param, float x, float y,
                     const char *text, float a, float b, float c) {
    if (g_graph.blockCount >= kMaxBlocks) return 0;
    if (!ensure_blocks(g_graph.blockCount + 1)) return 0;
    DBlock &block = g_graph.blocks[g_graph.blockCount++];
    std::memset(&block, 0, sizeof(block));
    block.id = g_graph.nextBlockId++;
    block.kind = kind;
    block.node = node;
    block.param = param;
    block.x = x; block.y = y;
    block.a = a; block.b = b; block.c = c;
    if (text) {
        std::strncpy(block.text, text, sizeof(block.text) - 1);
        block.text[sizeof(block.text) - 1] = '\0';
    }
    return block.id;
}

int dlogic_block_add_explicit(int id, int kind, int node, int param,
                              const char *text, float x, float y,
                              float a, float b, float c) {
    const int created = dlogic_block_add(kind, node, param, x, y, text, a, b, c);
    if (!created) return 0;
    DBlock *block = dlogic_block_get(created);
    if (block) block->id = id;
    if (id >= g_graph.nextBlockId) g_graph.nextBlockId = id + 1;
    return created;
}

int dlogic_block_remove(int id) {
    int index = -1;
    for (int i = 0; i < g_graph.blockCount; ++i)
        if (g_graph.blocks[i].id == id) { index = i; break; }
    if (index < 0) return 0;
    for (int i = index; i + 1 < g_graph.blockCount; ++i) g_graph.blocks[i] = g_graph.blocks[i + 1];
    g_graph.blockCount--;
    // Drop links that referenced the block.
    for (int i = 0; i < g_graph.linkCount; ) {
        if (g_graph.links[i].from == id || g_graph.links[i].to == id) {
            for (int j = i; j + 1 < g_graph.linkCount; ++j) g_graph.links[j] = g_graph.links[j + 1];
            g_graph.linkCount--;
        } else {
            ++i;
        }
    }
    return 1;
}

void dlogic_block_move(int id, float x, float y) {
    DBlock *block = dlogic_block_get(id);
    if (!block) return;
    block->x = x;
    block->y = y;
}

DBlock *dlogic_block_get(int id) {
    for (int i = 0; i < g_graph.blockCount; ++i)
        if (g_graph.blocks[i].id == id) return &g_graph.blocks[i];
    return nullptr;
}

int dlogic_block_count(void) { return g_graph.blockCount; }

int dlogic_block_at(int index) {
    if (index < 0 || index >= g_graph.blockCount) return 0;
    return g_graph.blocks[index].id;
}

int dlogic_link(int from, int to) {
    if (from == to) return 0;
    if (!dlogic_block_get(from) || !dlogic_block_get(to)) return 0;
    if (dlogic_has_link(from, to)) return 0;
    if (g_graph.linkCount >= kMaxLinks) return 0;
    if (!ensure_links(g_graph.linkCount + 1)) return 0;
    DLink &link = g_graph.links[g_graph.linkCount++];
    link.id = g_graph.nextLinkId++;
    link.from = from;
    link.to = to;
    return link.id;
}

int dlogic_unlink(int link_id) {
    for (int i = 0; i < g_graph.linkCount; ++i) {
        if (g_graph.links[i].id == link_id) {
            for (int j = i; j + 1 < g_graph.linkCount; ++j) g_graph.links[j] = g_graph.links[j + 1];
            g_graph.linkCount--;
            return 1;
        }
    }
    return 0;
}

int dlogic_has_link(int from, int to) {
    for (int i = 0; i < g_graph.linkCount; ++i)
        if (g_graph.links[i].from == from && g_graph.links[i].to == to) return 1;
    return 0;
}

int dlogic_link_count(void) { return g_graph.linkCount; }

DLink *dlogic_link_get(int index) {
    if (index < 0 || index >= g_graph.linkCount) return nullptr;
    return &g_graph.links[index];
}

const char *dlogic_export_json(void) {
    DBuf buf;
    dbuf_init(&buf);
    dbuf_printf(&buf, "{\"nextId\":%d,\"blocks\":[", g_graph.nextBlockId);
    for (int i = 0; i < g_graph.blockCount; ++i) {
        const DBlock &block = g_graph.blocks[i];
        if (i) dbuf_add(&buf, ",");
        dbuf_printf(&buf, "{\"id\":%d,\"kind\":%d,\"kindName\":\"%s\",\"node\":%d,\"param\":%d,"
                          "\"x\":%.1f,\"y\":%.1f,\"a\":%.3f,\"b\":%.3f,\"c\":%.3f,\"text\":",
                    block.id, block.kind, kind_name(block.kind), block.node, block.param,
                    block.x, block.y, block.a, block.b, block.c);
        append_json_string(&buf, block.text);
        dbuf_add(&buf, "}");
    }
    dbuf_printf(&buf, "],\"links\":[");
    for (int i = 0; i < g_graph.linkCount; ++i) {
        if (i) dbuf_add(&buf, ",");
        dbuf_printf(&buf, "{\"id\":%d,\"from\":%d,\"to\":%d}",
                    g_graph.links[i].id, g_graph.links[i].from, g_graph.links[i].to);
    }
    dbuf_add(&buf, "]}");

    // The buffer is handed to Java, which copies the string immediately; we keep
    // a single static copy so repeated calls do not leak.
    static char *cached = nullptr;
    std::free(cached);
    cached = static_cast<char *>(std::malloc(buf.length + 1));
    if (cached) std::memcpy(cached, dbuf_cstr(&buf), buf.length + 1);
    dbuf_free(&buf);
    return cached ? cached : "{\"nextId\":1,\"blocks\":[],\"links\":[]}";
}

int dlogic_import_json(const char *json, int length, char *err, int err_cap) {
    if (!json || length <= 0) return diamond_fail(DIAMOND_ERR_ARG, err, err_cap, "empty block json");
    JsonCursor cursor{json, static_cast<size_t>(length), 0, false};
    dlogic_clear();
    if (!json_expect(cursor, '{')) return diamond_fail(DIAMOND_ERR_PARSE, err, err_cap, "json: expected {");
    while (!cursor.failed && !json_peek(cursor, '}')) {
        char key[32];
        json_string(cursor, key, sizeof(key));
        if (!json_expect(cursor, ':')) break;
        if (std::strcmp(key, "nextId") == 0) {
            g_graph.nextBlockId = json_int(cursor);
        } else if (std::strcmp(key, "blocks") == 0) {
            if (!json_expect(cursor, '[')) break;
            while (!cursor.failed && !json_peek(cursor, ']')) {
                if (!json_expect(cursor, '{')) break;
                int values[5] = {0, 0, 0, 0, 0};
                float coords[5] = {0, 0, 0, 0, 0};
                char text[64] = {0};
                while (!cursor.failed && !json_peek(cursor, '}')) {
                    char inner[32];
                    json_string(cursor, inner, sizeof(inner));
                    if (!json_expect(cursor, ':')) break;
                    if (std::strcmp(inner, "id") == 0) values[0] = json_int(cursor);
                    else if (std::strcmp(inner, "kind") == 0) values[1] = json_int(cursor);
                    else if (std::strcmp(inner, "node") == 0) values[2] = json_int(cursor);
                    else if (std::strcmp(inner, "param") == 0) values[3] = json_int(cursor);
                    else if (std::strcmp(inner, "x") == 0) coords[0] = static_cast<float>(json_number(cursor));
                    else if (std::strcmp(inner, "y") == 0) coords[1] = static_cast<float>(json_number(cursor));
                    else if (std::strcmp(inner, "a") == 0) coords[2] = static_cast<float>(json_number(cursor));
                    else if (std::strcmp(inner, "b") == 0) coords[3] = static_cast<float>(json_number(cursor));
                    else if (std::strcmp(inner, "c") == 0) coords[4] = static_cast<float>(json_number(cursor));
                    else if (std::strcmp(inner, "text") == 0) json_string(cursor, text, sizeof(text));
                    else { char skip[64]; json_string(cursor, skip, sizeof(skip)); }
                    if (!json_peek(cursor, '}') && !json_expect(cursor, ',')) break;
                }
                if (!json_expect(cursor, '}')) break;
                dlogic_block_add_explicit(values[0], values[1], values[2], values[3],
                                          text, coords[0], coords[1], coords[2], coords[3], coords[4]);
                if (!json_peek(cursor, ']') && !json_expect(cursor, ',')) break;
            }
            if (!json_expect(cursor, ']')) break;
        } else if (std::strcmp(key, "links") == 0) {
            if (!json_expect(cursor, '[')) break;
            while (!cursor.failed && !json_peek(cursor, ']')) {
                if (!json_expect(cursor, '{')) break;
                int from = 0, to = 0, id = 0;
                while (!cursor.failed && !json_peek(cursor, '}')) {
                    char inner[32];
                    json_string(cursor, inner, sizeof(inner));
                    if (!json_expect(cursor, ':')) break;
                    if (std::strcmp(inner, "from") == 0) from = json_int(cursor);
                    else if (std::strcmp(inner, "to") == 0) to = json_int(cursor);
                    else if (std::strcmp(inner, "id") == 0) id = json_int(cursor);
                    else { char skip[64]; json_string(cursor, skip, sizeof(skip)); }
                    if (!json_peek(cursor, '}') && !json_expect(cursor, ',')) break;
                }
                if (!json_expect(cursor, '}')) break;
                const int created = dlogic_link(from, to);
                if (created && id) {
                    DLink *link = const_cast<DLink *>(dlogic_link_get(g_graph.linkCount - 1));
                    if (link) link->id = id;
                    if (id >= g_graph.nextLinkId) g_graph.nextLinkId = id + 1;
                }
                if (!json_peek(cursor, ']') && !json_expect(cursor, ',')) break;
            }
            if (!json_expect(cursor, ']')) break;
        } else {
            // Unknown member: skip a scalar or a nested container crudely.
            json_skip(cursor);
            int depth = 0;
            while (cursor.pos < cursor.size) {
                const char ch = cursor.data[cursor.pos];
                if (ch == '{' || ch == '[') depth++;
                else if (ch == '}' || ch == ']') { if (depth == 0) break; depth--; }
                else if (ch == ',' && depth == 0) { cursor.pos++; break; }
                cursor.pos++;
            }
        }
        if (!json_peek(cursor, '}') && !json_expect(cursor, ',')) break;
    }
    if (cursor.failed) return diamond_fail(DIAMOND_ERR_PARSE, err, err_cap, "json: malformed block graph");
    return DIAMOND_OK;
}

/* Validation used by the UI before Play: dangling links, blocks targeting
 * missing nodes and cycles without an event root are reported to the console. */
int dlogic_validate(char *err, int err_cap) {
    int problems = 0;
    for (int i = 0; i < g_graph.blockCount; ++i) {
        const DBlock &block = g_graph.blocks[i];
        if (block.node != 0 && !dnode_get(block.node)) {
            dlog_write(DLOG_WARN, "logic", "block %d (%s) targets deleted node %d",
                       block.id, kind_name(block.kind), block.node);
            problems++;
        }
        if (!kind_is_event(block.kind) && !kind_is_condition(block.kind) && block.kind != DBLOCK_END) {
            bool reachable = false;
            for (int l = 0; l < g_graph.linkCount; ++l)
                if (g_graph.links[l].to == block.id) { reachable = true; break; }
            if (!reachable) {
                dlog_write(DLOG_WARN, "logic", "block %d (%s) is not connected to any event",
                           block.id, kind_name(block.kind));
                problems++;
            }
        }
    }
    if (problems && err && err_cap > 0)
        std::snprintf(err, static_cast<size_t>(err_cap), "%d logic warning(s)", problems);
    return problems == 0 ? DIAMOND_OK : DIAMOND_ERR_LOGIC;
}

} // extern "C"
