// Diamond Engine — .diamond serializer.
//
// Everything the editor knows goes in: nodes, baked meshes (so a phone never
// re-parses OBJ text), RGBA images (so a phone never re-decodes PNG), the block
// graph and the generated LuaC source. On load the engine can therefore skip
// text parsing entirely, which is what makes cold start instant on a 1 GB phone.
#include "diamond/dformat.h"

#include <cstdlib>
#include <cstring>

extern "C" {
#include "diamond/dscene.h"
#include "diamond/dutil.h"
#include "diamond/dlog.h"
#include "diamond/deflate.h"
#include "diamond/dlogic.h"
}

namespace {

constexpr uint8_t kSectionScene = 1;
constexpr uint8_t kSectionMeshes = 2;
constexpr uint8_t kSectionImages = 3;
constexpr uint8_t kSectionBlocks = 4;
constexpr uint8_t kSectionLogic = 5;
constexpr uint8_t kSectionMeta = 6;

struct Writer {
    unsigned char *data = nullptr;
    size_t length = 0;
    size_t capacity = 0;
    bool failed = false;

    bool reserve(size_t extra) {
        if (failed) return false;
        const size_t need = length + extra;
        if (need <= capacity) return true;
        size_t cap = capacity ? capacity : 4096;
        while (cap < need) cap *= 2;
        unsigned char *grown = static_cast<unsigned char *>(std::realloc(data, cap));
        if (!grown) { failed = true; return false; }
        data = grown;
        capacity = cap;
        return true;
    }
    void u8(uint8_t v) { if (!reserve(1)) return; data[length++] = v; }
    void u16(uint16_t v) { if (!reserve(2)) return; data[length++] = (unsigned char)(v & 0xFF); data[length++] = (unsigned char)(v >> 8); }
    void u32(uint32_t v) {
        if (!reserve(4)) return;
        data[length++] = (unsigned char)(v & 0xFF);
        data[length++] = (unsigned char)((v >> 8) & 0xFF);
        data[length++] = (unsigned char)((v >> 16) & 0xFF);
        data[length++] = (unsigned char)((v >> 24) & 0xFF);
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void f32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
    void bytes(const void *src, size_t count) {
        if (!count) return;
        if (!reserve(count)) return;
        std::memcpy(data + length, src, count);
        length += count;
    }
    void string(const char *text) {
        const uint32_t count = text ? static_cast<uint32_t>(std::strlen(text)) : 0u;
        u32(count);
        bytes(text, count);
    }
    void release() { std::free(data); data = nullptr; length = capacity = 0; }
};

struct Reader {
    const unsigned char *data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    bool failed = false;

    bool need(size_t count) {
        if (failed) return false;
        if (pos + count > size) { failed = true; return false; }
        return true;
    }
    uint8_t u8() { if (!need(1)) return 0; return data[pos++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = (uint16_t)(data[pos] | (data[pos + 1] << 8)); pos += 2; return v; }
    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = (uint32_t)data[pos] | ((uint32_t)data[pos + 1] << 8) |
                     ((uint32_t)data[pos + 2] << 16) | ((uint32_t)data[pos + 3] << 24);
        pos += 4;
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32() { uint32_t bits = u32(); float v; std::memcpy(&v, &bits, 4); return v; }
    const unsigned char *take(size_t count) {
        if (!need(count)) return nullptr;
        const unsigned char *p = data + pos;
        pos += count;
        return p;
    }
};

void align_writer(Writer &w) { while (w.length & 3u) w.u8(0); }
void align_reader(Reader &r) { while (r.pos & 3u) r.u8(); }

void begin_section(Writer &w, uint8_t id) {
    // Section headers live on 4-byte boundaries: read() aligns after every
    // section, so the writer must pad *before* the tag or the stream desyncs.
    align_writer(w);
    w.u8('S');
    w.u8(id);
    w.u32(0); // size patched later
}

void end_section(Writer &w, size_t start) {
    const uint32_t payload = static_cast<uint32_t>(w.length - start - 6);
    w.data[start + 2] = (unsigned char)(payload & 0xFF);
    w.data[start + 3] = (unsigned char)((payload >> 8) & 0xFF);
    w.data[start + 4] = (unsigned char)((payload >> 16) & 0xFF);
    w.data[start + 5] = (unsigned char)((payload >> 24) & 0xFF);
}

// ------------------------------------------------------------------ writers

void write_scene(Writer &w) {
    const int first = 1;
    const int count = dscene_flat_count() - 1;
    begin_section(w, kSectionScene);
    const size_t start = w.length - 6;
    w.i32(count);
    for (int i = 0; i < count; ++i) {
        const int id = dscene_node_at(first + i);
        DNode *node = dnode_get(id);
        if (!node) continue;
        w.i32(node->id);
        w.i32(node->type);
        w.i32(node->parent);
        w.string(node->name);
        w.f32(node->px); w.f32(node->py); w.f32(node->pz);
        w.f32(node->rx); w.f32(node->ry); w.f32(node->rz);
        w.f32(node->sx); w.f32(node->sy); w.f32(node->sz);
        w.u32(node->color);
        w.i32(node->collision);
        w.i32(node->mesh);
        w.i32(node->image);
        w.i32(node->primitive);
        w.i32(node->visible);
    }
    end_section(w, start);
}

void write_meshes(Writer &w) {
    begin_section(w, kSectionMeshes);
    const size_t start = w.length - 6;
    const int count = dmesh_count();
    w.i32(count);
    for (int i = 0; i < count; ++i) {
        DMesh *mesh = dmesh_get(i);
        if (!mesh) { w.i32(0); w.i32(0); w.i32(0); w.i32(0); w.string(""); continue; }
        w.string(mesh->name);
        w.i32(mesh->vertex_count);
        w.i32(mesh->index_count);
        for (int f = 0; f < 6; ++f) w.f32(mesh->bounds[f]);
        w.i32(mesh->normals ? 1 : 0);
        w.i32(mesh->uvs ? 1 : 0);
        if (mesh->positions) w.bytes(mesh->positions, sizeof(float) * 3 * static_cast<size_t>(mesh->vertex_count));
        if (mesh->normals && mesh->normals) w.bytes(mesh->normals, sizeof(float) * 3 * static_cast<size_t>(mesh->vertex_count));
        if (mesh->uvs && mesh->uvs) w.bytes(mesh->uvs, sizeof(float) * 2 * static_cast<size_t>(mesh->vertex_count));
        if (mesh->indices) w.bytes(mesh->indices, sizeof(uint16_t) * static_cast<size_t>(mesh->index_count));
    }
    end_section(w, start);
}

void write_images(Writer &w) {
    begin_section(w, kSectionImages);
    const size_t start = w.length - 6;
    const int count = dimage_count();
    w.i32(count);
    for (int i = 0; i < count; ++i) {
        DImage *image = dimage_get(i);
        if (!image || !image->rgba) { w.i32(0); w.i32(0); w.u32(0); continue; }
        w.i32(image->width);
        w.i32(image->height);
        const uint32_t bytes = static_cast<uint32_t>(image->width) * static_cast<uint32_t>(image->height) * 4u;
        w.u32(bytes);
        w.bytes(image->rgba, bytes);
    }
    end_section(w, start);
}

void write_blocks(Writer &w) {
    begin_section(w, kSectionBlocks);
    const size_t start = w.length - 6;
    w.i32(dlogic_block_count());
    for (int i = 0; i < dlogic_block_count(); ++i) {
        const int id = dlogic_block_at(i);
        DBlock *block = dlogic_block_get(id);
        if (!block) continue;
        w.i32(block->id);
        w.i32(block->kind);
        w.i32(block->node);
        w.i32(block->param);
        w.string(block->text);
        w.f32(block->x); w.f32(block->y);
        w.f32(block->a); w.f32(block->b); w.f32(block->c);
    }
    w.i32(dlogic_link_count());
    for (int i = 0; i < dlogic_link_count(); ++i) {
        DLink *link = dlogic_link_get(i);
        if (!link) continue;
        w.i32(link->from);
        w.i32(link->to);
    }
    end_section(w, start);
}

void write_logic(Writer &w) {
    begin_section(w, kSectionLogic);
    const size_t start = w.length - 6;
    const char *lua = dlogic_generated_source();
    w.string(lua ? lua : "");
    end_section(w, start);
}

void write_meta(Writer &w) {
    begin_section(w, kSectionMeta);
    const size_t start = w.length - 6;
    w.string("Diamond Scene");
    w.i32(diamond_get_render_mode());
    w.i32(diamond_get_view_mode());
    w.i32(dscene_node_count());
    w.i32(dmesh_count());
    w.i32(dimage_count());
    w.i32(dlogic_block_count());
    end_section(w, start);
}

// ------------------------------------------------------------------ readers

void read_scene(Reader &r) {
    dscene_clear();
    const int count = r.i32();
    if (count < 0 || count > 200000) { r.failed = true; return; }
    for (int i = 0; i < count && !r.failed; ++i) {
        const int id = r.i32();
        const int type = r.i32();
        const int parent = r.i32();
        const uint32_t nameLength = r.u32();
        if (nameLength > 200) { r.failed = true; return; }
        char name[DNODE_NAME_MAX];
        const unsigned char *raw = r.take(nameLength);
        if (!raw) return;
        const size_t copy = nameLength < sizeof(name) - 1 ? nameLength : sizeof(name) - 1;
        std::memcpy(name, raw, copy);
        name[copy] = '\0';
        const int created = dnode_create(type, parent, name);
        DNode *node = dnode_get(created);
        if (!node) { r.failed = true; return; }
        node->px = r.f32(); node->py = r.f32(); node->pz = r.f32();
        node->rx = r.f32(); node->ry = r.f32(); node->rz = r.f32();
        node->sx = r.f32(); node->sy = r.f32(); node->sz = r.f32();
        node->color = r.u32();
        node->collision = r.i32();
        node->mesh = r.i32();
        node->image = r.i32();
        node->primitive = r.i32();
        node->visible = r.i32();
        (void)id;
    }
}

void read_meshes(Reader &r) {
    const int count = r.i32();
    if (count < 0 || count > 4096) { r.failed = true; return; }
    for (int i = 0; i < count && !r.failed; ++i) {
        const uint32_t nameLength = r.u32();
        if (nameLength > 200) { r.failed = true; return; }
        const unsigned char *raw = r.take(nameLength);
        if (!raw) return;
        char name[DMESH_NAME_MAX];
        const size_t copy = nameLength < sizeof(name) - 1 ? nameLength : sizeof(name) - 1;
        std::memcpy(name, raw, copy);
        name[copy] = '\0';
        const int vertices = r.i32();
        const int indices = r.i32();
        float bounds[6];
        for (int f = 0; f < 6; ++f) bounds[f] = r.f32();
        const int hasNormals = r.i32();
        const int hasUvs = r.i32();
        if (r.failed || vertices <= 0 || vertices > 70000 || indices <= 0 || indices > 400000) {
            r.failed = true;
            return;
        }
        float *positions = static_cast<float *>(std::malloc(sizeof(float) * 3 * static_cast<size_t>(vertices)));
        float *normals = hasNormals ? static_cast<float *>(std::malloc(sizeof(float) * 3 * static_cast<size_t>(vertices))) : nullptr;
        float *uvs = hasUvs ? static_cast<float *>(std::malloc(sizeof(float) * 2 * static_cast<size_t>(vertices))) : nullptr;
        uint16_t *indexData = static_cast<uint16_t *>(std::malloc(sizeof(uint16_t) * static_cast<size_t>(indices)));
        if (!positions || !indexData || (hasNormals && !normals) || (hasUvs && !uvs)) {
            std::free(positions); std::free(normals); std::free(uvs); std::free(indexData);
            r.failed = true;
            return;
        }
        const unsigned char *praw = r.take(sizeof(float) * 3 * static_cast<size_t>(vertices));
        if (!praw) { std::free(positions); std::free(normals); std::free(uvs); std::free(indexData); return; }
        std::memcpy(positions, praw, sizeof(float) * 3 * static_cast<size_t>(vertices));
        if (hasNormals) {
            const unsigned char *nraw = r.take(sizeof(float) * 3 * static_cast<size_t>(vertices));
            if (!nraw) { std::free(positions); std::free(normals); std::free(uvs); std::free(indexData); return; }
            std::memcpy(normals, nraw, sizeof(float) * 3 * static_cast<size_t>(vertices));
        }
        if (hasUvs) {
            const unsigned char *uraw = r.take(sizeof(float) * 2 * static_cast<size_t>(vertices));
            if (!uraw) { std::free(positions); std::free(normals); std::free(uvs); std::free(indexData); return; }
            std::memcpy(uvs, uraw, sizeof(float) * 2 * static_cast<size_t>(vertices));
        }
        const unsigned char *iraw = r.take(sizeof(uint16_t) * static_cast<size_t>(indices));
        if (!iraw) { std::free(positions); std::free(normals); std::free(uvs); std::free(indexData); return; }
        std::memcpy(indexData, iraw, sizeof(uint16_t) * static_cast<size_t>(indices));
        const int mesh = dmesh_add(name, vertices, positions, normals, uvs, indices, indexData);
        if (mesh < 0) {
            std::free(positions); std::free(normals); std::free(uvs); std::free(indexData);
            r.failed = true;
            return;
        }
        DMesh *added = dmesh_get(mesh);
        if (added) std::memcpy(added->bounds, bounds, sizeof(bounds));
    }
}

void read_images(Reader &r) {
    const int count = r.i32();
    if (count < 0 || count > 1024) { r.failed = true; return; }
    for (int i = 0; i < count && !r.failed; ++i) {
        const int width = r.i32();
        const int height = r.i32();
        const uint32_t bytes = r.u32();
        if (width < 0 || height < 0 || width > 4096 || height > 4096) { r.failed = true; return; }
        const unsigned char *raw = r.take(bytes);
        if (!raw && bytes) return;
        if (width > 0 && height > 0) {
            if (static_cast<uint32_t>(width) * static_cast<uint32_t>(height) * 4u != bytes) { r.failed = true; return; }
            if (dimage_add(raw, width, height) < 0) { r.failed = true; return; }
        }
    }
}

void read_blocks(Reader &r) {
    dlogic_clear();
    const int count = r.i32();
    if (count < 0 || count > 50000) { r.failed = true; return; }
    for (int i = 0; i < count && !r.failed; ++i) {
        const int id = r.i32();
        const int kind = r.i32();
        const int node = r.i32();
        const int param = r.i32();
        const uint32_t textLength = r.u32();
        if (textLength > 4096) { r.failed = true; return; }
        const unsigned char *raw = r.take(textLength);
        if (!raw && textLength) return;
        char text[128];
        const size_t copy = textLength < sizeof(text) - 1 ? textLength : sizeof(text) - 1;
        if (raw && copy) std::memcpy(text, raw, copy);
        text[copy] = '\0';
        const float x = r.f32(), y = r.f32();
        const float a = r.f32(), b = r.f32(), c = r.f32();
        dlogic_block_add_explicit(id, kind, node, param, text, x, y, a, b, c);
    }
    const int links = r.i32();
    if (links < 0 || links > 50000) { r.failed = true; return; }
    for (int i = 0; i < links && !r.failed; ++i) {
        const int from = r.i32();
        const int to = r.i32();
        dlogic_link(from, to);
    }
}

void read_logic(Reader &r) {
    const uint32_t length = r.u32();
    if (length > 1024 * 1024) { r.failed = true; return; }
    const unsigned char *raw = r.take(length);
    if (!raw && length) return;
    char *text = static_cast<char *>(std::malloc(static_cast<size_t>(length) + 1));
    if (!text) { r.failed = true; return; }
    if (length) std::memcpy(text, raw, length);
    text[length] = '\0';
    dlogic_set_source(text);
    std::free(text);
}

void read_meta(Reader &r) {
    const uint32_t nameLength = r.u32();
    if (nameLength > 4096) { r.failed = true; return; }
    if (!r.take(nameLength)) return;
    const int renderMode = r.i32();
    const int viewMode = r.i32();
    (void)r.i32(); (void)r.i32(); (void)r.i32(); (void)r.i32();
    if (renderMode >= DIAMOND_RENDER_GLES3 && renderMode <= DIAMOND_RENDER_HYBRID)
        diamond_set_render_mode(renderMode, nullptr, 0);
    if (viewMode >= DIAMOND_VIEW_2D && viewMode <= DIAMOND_VIEW_3D)
        diamond_set_view_mode(viewMode);
}

} // namespace

extern "C" {

int dformat_write(void **out, size_t *out_size, char *err, int err_cap) {
    if (!out || !out_size) return DIAMOND_ERR_ARG;
    *out = nullptr;
    *out_size = 0;

    Writer payload;
    write_scene(payload);
    write_meshes(payload);
    write_images(payload);
    write_blocks(payload);
    write_logic(payload);
    write_meta(payload);
    align_writer(payload);
    if (payload.failed) {
        payload.release();
        return diamond_fail(DIAMOND_ERR_MEMORY, err, err_cap, "diamond: out of memory while serialising");
    }

    void *compressed = nullptr;
    size_t compressedSize = 0;
    const bool deflated = ddeflate_compress(payload.data, payload.length, &compressed, &compressedSize) == 0;
    const unsigned char *stored = deflated ? static_cast<const unsigned char *>(compressed) : payload.data;
    const size_t storedSize = deflated ? compressedSize : payload.length;

    const size_t total = 20 + storedSize;
    unsigned char *file = static_cast<unsigned char *>(std::malloc(total));
    if (!file) {
        if (deflated) ddeflate_free(compressed);
        payload.release();
        return diamond_fail(DIAMOND_ERR_MEMORY, err, err_cap, "diamond: out of memory");
    }
    std::memcpy(file, DMND_MAGIC, 4);
    file[4] = (unsigned char)(DMND_VERSION & 0xFF);
    file[5] = (unsigned char)(DMND_VERSION >> 8);
    file[6] = deflated ? 1 : 0;
    file[7] = 0;
    const uint32_t crc = dcrc32(payload.data, payload.length);
    file[8] = (unsigned char)(crc & 0xFF);
    file[9] = (unsigned char)((crc >> 8) & 0xFF);
    file[10] = (unsigned char)((crc >> 16) & 0xFF);
    file[11] = (unsigned char)((crc >> 24) & 0xFF);
    const uint32_t rawSize = static_cast<uint32_t>(payload.length);
    file[12] = (unsigned char)(rawSize & 0xFF);
    file[13] = (unsigned char)((rawSize >> 8) & 0xFF);
    file[14] = (unsigned char)((rawSize >> 16) & 0xFF);
    file[15] = (unsigned char)((rawSize >> 24) & 0xFF);
    const uint32_t storedSize32 = static_cast<uint32_t>(storedSize);
    file[16] = (unsigned char)(storedSize32 & 0xFF);
    file[17] = (unsigned char)((storedSize32 >> 8) & 0xFF);
    file[18] = (unsigned char)((storedSize32 >> 16) & 0xFF);
    file[19] = (unsigned char)((storedSize32 >> 24) & 0xFF);
    std::memcpy(file + 20, stored, storedSize);

    if (deflated) ddeflate_free(compressed);
    payload.release();
    *out = file;
    *out_size = total;
    return DIAMOND_OK;
}

int dformat_is_diamond(const void *buffer, size_t size) {
    if (!buffer || size < 20) return 0;
    return std::memcmp(buffer, DMND_MAGIC, 4) == 0;
}

int dformat_probe(const void *buffer, size_t size, int *version, int *deflated,
                  size_t *payload_size) {
    if (!dformat_is_diamond(buffer, size)) return 0;
    const unsigned char *bytes = static_cast<const unsigned char *>(buffer);
    if (version) *version = bytes[4] | (bytes[5] << 8);
    if (deflated) *deflated = bytes[6] & 1;
    if (payload_size) {
        const uint32_t raw = (uint32_t)bytes[12] | ((uint32_t)bytes[13] << 8) |
                             ((uint32_t)bytes[14] << 16) | ((uint32_t)bytes[15] << 24);
        *payload_size = raw;
    }
    return 1;
}

int dformat_read(const void *buffer, size_t size, char *err, int err_cap) {
    if (!buffer || size < 20) return diamond_fail(DIAMOND_ERR_FORMAT, err, err_cap, "not a .diamond file");
    const unsigned char *bytes = static_cast<const unsigned char *>(buffer);
    if (!dformat_is_diamond(buffer, size))
        return diamond_fail(DIAMOND_ERR_FORMAT, err, err_cap, "bad magic (expected DMND)");
    const int version = bytes[4] | (bytes[5] << 8);
    if (version != DMND_VERSION)
        return diamond_fail(DIAMOND_ERR_FORMAT, err, err_cap, "unsupported version %d", version);
    const int deflated = bytes[6] & 1;
    const uint32_t crc = (uint32_t)bytes[8] | ((uint32_t)bytes[9] << 8) |
                         ((uint32_t)bytes[10] << 16) | ((uint32_t)bytes[11] << 24);
    const uint32_t rawSize = (uint32_t)bytes[12] | ((uint32_t)bytes[13] << 8) |
                             ((uint32_t)bytes[14] << 16) | ((uint32_t)bytes[15] << 24);
    const uint32_t storedSize = (uint32_t)bytes[16] | ((uint32_t)bytes[17] << 8) |
                                ((uint32_t)bytes[18] << 16) | ((uint32_t)bytes[19] << 24);
    if (20 + static_cast<size_t>(storedSize) > size)
        return diamond_fail(DIAMOND_ERR_FORMAT, err, err_cap, "truncated payload");
    if (rawSize > 256u * 1024u * 1024u)
        return diamond_fail(DIAMOND_ERR_FORMAT, err, err_cap, "payload too large (%u bytes)", rawSize);

    unsigned char *payload = static_cast<unsigned char *>(std::malloc(rawSize ? rawSize : 1));
    if (!payload) return diamond_fail(DIAMOND_ERR_MEMORY, err, err_cap, "out of memory");
    if (deflated) {
        if (ddeflate_decompress(bytes + 20, storedSize, payload, rawSize) != 0) {
            std::free(payload);
            return diamond_fail(DIAMOND_ERR_FORMAT, err, err_cap, "corrupt compressed payload");
        }
    } else {
        std::memcpy(payload, bytes + 20, rawSize);
    }
    if (dcrc32(payload, rawSize) != crc) {
        std::free(payload);
        return diamond_fail(DIAMOND_ERR_FORMAT, err, err_cap, "checksum mismatch");
    }

    Reader reader{payload, rawSize, 0, false};
    while (reader.pos + 6 <= reader.size && !reader.failed) {
        const uint8_t tag = reader.u8();
        if (tag != 'S') break;
        const uint8_t id = reader.u8();
        const uint32_t sectionSize = reader.u32();
        if (reader.pos + sectionSize > reader.size) { reader.failed = true; break; }
        Reader section{payload + reader.pos, sectionSize, 0, false};
        switch (id) {
            case kSectionScene:  read_scene(section); break;
            case kSectionMeshes: read_meshes(section); break;
            case kSectionImages: read_images(section); break;
            case kSectionBlocks: read_blocks(section); break;
            case kSectionLogic:  read_logic(section); break;
            case kSectionMeta:   read_meta(section); break;
            default: break;
        }
        reader.pos += sectionSize;
        align_reader(reader);
        if (section.failed) { reader.failed = true; break; }
    }
    std::free(payload);
    if (reader.failed)
        return diamond_fail(DIAMOND_ERR_FORMAT, err, err_cap, "malformed section stream");

    dscene_select(0);
    dlog_write(DLOG_INFO, "format", "loaded scene: %d nodes, %d meshes, %d images, %d blocks",
               dscene_node_count(), dmesh_count(), dimage_count(), dlogic_block_count());
    return DIAMOND_OK;
}

} // extern "C"
