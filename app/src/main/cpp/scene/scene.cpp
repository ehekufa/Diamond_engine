// Diamond Engine — scene graph implementation (C++23, RTTI/exceptions off).
//
// Plain reallocating arrays, no STL containers: the scene is walked every
// frame by the renderer and by the logic executor, so cache locality matters
// more than iterator convenience on a 1 GB phone.
#include "diamond/dscene.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "diamond/dlog.h"
}

namespace {

struct Scene {
    Scene() = default;

    DNode *nodes = nullptr;
    int nodeCount = 0;
    int nodeCap = 0;

    DMesh *meshes = nullptr;
    int meshCount = 0;
    int meshCap = 0;

    DImage *images = nullptr;
    int imageCount = 0;
    int imageCap = 0;

    int selected = 0;
    int initialized = 0;
};

Scene g_scene;

constexpr uint32_t kDefaultColor = 0xFFB0B0B0u; // ABGR, light grey
constexpr const char *kDefaultName = "Node";

void copy_name(char *dst, size_t cap, const char *src) {
    if (!src || !*src) src = kDefaultName;
    size_t length = std::strlen(src);
    if (length >= cap) length = cap - 1;
    for (size_t i = 0; i < length; ++i) {
        char c = src[i];
        dst[i] = (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
    }
    dst[length] = '\0';
}

bool ensure_nodes(int needed) {
    if (needed <= g_scene.nodeCap) return true;
    int cap = g_scene.nodeCap ? g_scene.nodeCap : 32;
    while (cap < needed) cap *= 2;
    DNode *grown = static_cast<DNode *>(std::realloc(g_scene.nodes, sizeof(DNode) * static_cast<size_t>(cap)));
    if (!grown) return false;
    for (int i = g_scene.nodeCap; i < cap; ++i) std::memset(&grown[i], 0, sizeof(DNode));
    g_scene.nodes = grown;
    g_scene.nodeCap = cap;
    return true;
}

bool ensure_meshes(int needed) {
    if (needed <= g_scene.meshCap) return true;
    int cap = g_scene.meshCap ? g_scene.meshCap : 16;
    while (cap < needed) cap *= 2;
    DMesh *grown = static_cast<DMesh *>(std::realloc(g_scene.meshes, sizeof(DMesh) * static_cast<size_t>(cap)));
    if (!grown) return false;
    for (int i = g_scene.meshCap; i < cap; ++i) std::memset(&grown[i], 0, sizeof(DMesh));
    g_scene.meshes = grown;
    g_scene.meshCap = cap;
    return true;
}

bool ensure_images(int needed) {
    if (needed <= g_scene.imageCap) return true;
    int cap = g_scene.imageCap ? g_scene.imageCap : 8;
    while (cap < needed) cap *= 2;
    DImage *grown = static_cast<DImage *>(std::realloc(g_scene.images, sizeof(DImage) * static_cast<size_t>(cap)));
    if (!grown) return false;
    for (int i = g_scene.imageCap; i < cap; ++i) std::memset(&grown[i], 0, sizeof(DImage));
    g_scene.images = grown;
    g_scene.imageCap = cap;
    return true;
}

void unlink_from_parent(DNode &node) {
    DNode *parent = dnode_get(node.parent);
    if (!parent) return;
    if (parent->first_child == node.id) {
        parent->first_child = node.next_sibling;
    } else {
        DNode *prev = dnode_get(node.prev_sibling);
        if (prev) prev->next_sibling = node.next_sibling;
    }
    DNode *next = dnode_get(node.next_sibling);
    if (next) next->prev_sibling = node.prev_sibling;
    node.parent = 0;
    node.next_sibling = 0;
    node.prev_sibling = 0;
}

void append_child(int parentId, int childId) {
    DNode *parent = dnode_get(parentId);
    DNode *child = dnode_get(childId);
    if (!parent || !child) return;
    child->parent = parentId;
    child->next_sibling = 0;
    if (parent->first_child == 0) {
        parent->first_child = childId;
        child->prev_sibling = 0;
        return;
    }
    int cursor = parent->first_child;
    DNode *node = dnode_get(cursor);
    while (node && node->next_sibling) {
        cursor = node->next_sibling;
        node = dnode_get(cursor);
    }
    if (node) {
        node->next_sibling = childId;
        child->prev_sibling = cursor;
    }
}

void free_subtree(int id) {
    DNode *node = dnode_get(id);
    if (!node) return;
    int child = node->first_child;
    while (child) {
        DNode *c = dnode_get(child);
        int next = c ? c->next_sibling : 0;
        free_subtree(child);
        child = next;
    }
    node->first_child = 0;
}

void dfs_collect(int id, int *out, int *count, int limit) {
    if (id == 0) return;
    if (*count >= limit) return;
    out[(*count)++] = id;
    DNode *node = dnode_get(id);
    if (!node) return;
    int child = node->first_child;
    while (child) {
        DNode *c = dnode_get(child);
        int next = c ? c->next_sibling : 0;
        dfs_collect(child, out, count, limit);
        child = next;
    }
}

float radians(float degrees) { return degrees * 3.14159265358979f / 180.0f; }

// Transform of a node applied to its local AABB corners.
void world_aabb_corners(const DNode &node, const float bounds[6], float *out6) {
    const float cx = (bounds[0] + bounds[3]) * 0.5f;
    const float cy = (bounds[1] + bounds[4]) * 0.5f;
    const float cz = (bounds[2] + bounds[5]) * 0.5f;
    const float hx = (bounds[3] - bounds[0]) * 0.5f;
    const float hy = (bounds[4] - bounds[1]) * 0.5f;
    const float hz = (bounds[5] - bounds[2]) * 0.5f;

    const float rx = radians(node.rx), ry = radians(node.ry), rz = radians(node.rz);
    const float cxr = std::cos(rx), sxr = std::sin(rx);
    const float cyr = std::cos(ry), syr = std::sin(ry);
    const float czr = std::cos(rz), szr = std::sin(rz);

    // R = Rz * Ry * Rx, scaled per axis.
    const float m00 = cyr * czr;
    const float m01 = cyr * szr;
    const float m02 = -syr;
    const float m10 = sxr * syr * czr - cxr * szr;
    const float m11 = sxr * syr * szr + cxr * czr;
    const float m12 = sxr * cyr;
    const float m20 = cxr * syr * czr + sxr * szr;
    const float m21 = cxr * syr * szr - sxr * czr;
    const float m22 = cxr * cyr;

    const float ex = (std::fabs(m00) * hx * node.sx) + (std::fabs(m01) * hy * node.sy) + (std::fabs(m02) * hz * node.sz);
    const float ey = (std::fabs(m10) * hx * node.sx) + (std::fabs(m11) * hy * node.sy) + (std::fabs(m12) * hz * node.sz);
    const float ez = (std::fabs(m20) * hx * node.sx) + (std::fabs(m21) * hy * node.sy) + (std::fabs(m22) * hz * node.sz);

    const float wx = node.px + (m00 * cx * node.sx + m01 * cy * node.sy + m02 * cz * node.sz);
    const float wy = node.py + (m10 * cx * node.sx + m11 * cy * node.sy + m12 * cz * node.sz);
    const float wz = node.pz + (m20 * cx * node.sx + m21 * cy * node.sy + m22 * cz * node.sz);

    out6[0] = wx - ex; out6[1] = wy - ey; out6[2] = wz - ez;
    out6[3] = wx + ex; out6[4] = wy + ey; out6[5] = wz + ez;
}

} // namespace

extern "C" {

void dscene_init(void) {
    if (g_scene.initialized) return;
    g_scene.nodes = nullptr; g_scene.nodeCount = 0; g_scene.nodeCap = 0;
    g_scene.meshes = nullptr; g_scene.meshCount = 0; g_scene.meshCap = 0;
    g_scene.images = nullptr; g_scene.imageCount = 0; g_scene.imageCap = 0;
    g_scene.selected = 0;
    g_scene.initialized = 1;
    dscene_clear();
}

void dscene_shutdown(void) {
    dscene_clear();
    std::free(g_scene.nodes);
    std::free(g_scene.meshes);
    for (int i = 0; i < g_scene.imageCount; ++i) std::free(g_scene.images[i].rgba);
    std::free(g_scene.images);
    g_scene.nodes = nullptr; g_scene.nodeCount = 0; g_scene.nodeCap = 0;
    g_scene.meshes = nullptr; g_scene.meshCount = 0; g_scene.meshCap = 0;
    g_scene.images = nullptr; g_scene.imageCount = 0; g_scene.imageCap = 0;
    g_scene.selected = 0; g_scene.initialized = 0;
}

void dscene_clear(void) {
    for (int i = 0; i < g_scene.meshCount; ++i) {
        DMesh &mesh = g_scene.meshes[i];
        std::free(mesh.positions); mesh.positions = nullptr;
        std::free(mesh.normals);   mesh.normals = nullptr;
        std::free(mesh.uvs);       mesh.uvs = nullptr;
        std::free(mesh.indices);   mesh.indices = nullptr;
        std::memset(&mesh, 0, sizeof(mesh));
    }
    g_scene.meshCount = 0;
    for (int i = 0; i < g_scene.imageCount; ++i) std::free(g_scene.images[i].rgba);
    g_scene.imageCount = 0;
    g_scene.nodeCount = 0;
    g_scene.selected = 0;

    if (!ensure_nodes(8)) return;
    DNode &root = g_scene.nodes[0];
    std::memset(&root, 0, sizeof(root));
    root.id = 0;
    root.type = DIAMOND_NODE_EMPTY;
    root.parent = 0;
    root.first_child = 0;
    root.next_sibling = 0;
    root.prev_sibling = 0;
    root.mesh = -1;
    root.image = -1;
    root.primitive = -1;
    root.sx = root.sy = root.sz = 1.0f;
    root.visible = 1;
    root.color = 0xFF404040u;
    copy_name(root.name, sizeof(root.name), "Scene");
    g_scene.nodeCount = 1;
}

DNode *dnode_get(int id) {
    if (id <= 0 || id >= g_scene.nodeCount) return nullptr;
    return &g_scene.nodes[id];
}

int dnode_create(int type, int parent, const char *name) {
    if (!g_scene.initialized) dscene_init();
    if (!dnode_get(parent) && parent != 0) parent = 0;
    if (type < DIAMOND_NODE_EMPTY || type > DIAMOND_NODE_MESH) type = DIAMOND_NODE_EMPTY;
    if (!ensure_nodes(g_scene.nodeCount + 1)) return 0;

    const int id = g_scene.nodeCount++;
    DNode &node = g_scene.nodes[id];
    std::memset(&node, 0, sizeof(node));
    node.id = id;
    node.type = type;
    node.mesh = -1;
    node.image = -1;
    node.primitive = -1;
    node.sx = node.sy = node.sz = 1.0f;
    node.visible = 1;
    node.color = kDefaultColor;
    node.px = node.py = node.pz = 0.0f;
    node.rx = node.ry = node.rz = 0.0f;
    copy_name(node.name, sizeof(node.name), name);
    append_child(parent, id);
    return id;
}

int dnode_delete(int id) {
    if (id <= 0) return 0;
    DNode *node = dnode_get(id);
    if (!node) return 0;
    free_subtree(id);
    unlink_from_parent(*node);
    if (g_scene.selected == id) g_scene.selected = 0;
    // The slot itself is zeroed; ids stay stable for the lifetime of a session
    // only for live nodes, which is exactly what the Java model expects.
    std::memset(node, 0, sizeof(*node));
    return 1;
}

int dnode_duplicate(int id) {
    DNode *source = dnode_get(id);
    if (!source) return 0;
    const int copy = dnode_create(source->type, source->parent, source->name);
    DNode *created = dnode_get(copy);
    if (!created) return 0;
    const int mesh = created->mesh, image = created->image, prim = created->primitive;
    const int next = created->next_sibling, prev = created->prev_sibling;
    std::memcpy(created, source, sizeof(DNode));
    created->id = copy;
    created->first_child = 0;
    created->parent = source->parent;
    created->next_sibling = next;
    created->prev_sibling = prev;
    created->mesh = mesh == -1 ? source->mesh : mesh;
    created->image = image == -1 ? source->image : image;
    created->primitive = prim == -1 ? source->primitive : prim;
    std::memcpy(created->name, source->name, sizeof(created->name));
    return copy;
}

int dnode_reparent(int id, int parent) {
    DNode *node = dnode_get(id);
    if (!node || id == 0) return 0;
    if (parent == id) return 0;
    if (!dnode_get(parent) && parent != 0) return 0;
    // Reject cycles: walking up from `parent` must never reach `id`.
    for (int cursor = parent; cursor != 0; ) {
        if (cursor == id) return 0;
        DNode *c = dnode_get(cursor);
        if (!c) break;
        cursor = c->parent;
    }
    unlink_from_parent(*node);
    append_child(parent, id);
    return 1;
}

void dnode_set_name(int id, const char *name) {
    DNode *node = dnode_get(id);
    if (!node) return;
    copy_name(node->name, sizeof(node->name), name);
}

void dnode_get_transform(int id, float *out9) {
    if (!out9) return;
    DNode *node = dnode_get(id);
    if (!node) {
        for (int i = 0; i < 9; ++i) out9[i] = (i == 3 || i == 4 || i == 5 || i == 6 || i == 7 || i == 8) ? 0.0f : 0.0f;
        return;
    }
    out9[0] = node->px; out9[1] = node->py; out9[2] = node->pz;
    out9[3] = node->rx; out9[4] = node->ry; out9[5] = node->rz;
    out9[6] = node->sx; out9[7] = node->sy; out9[8] = node->sz;
}

void dnode_set_transform(int id, const float *in9) {
    DNode *node = dnode_get(id);
    if (!node || !in9) return;
    // Reject NaN/Inf coming from a Java parse so the renderer never sees them.
    for (int i = 0; i < 9; ++i)
        if (!(in9[i] == in9[i]) || std::fabs(in9[i]) > 1e9f) return;
    node->px = in9[0]; node->py = in9[1]; node->pz = in9[2];
    node->rx = in9[3]; node->ry = in9[4]; node->rz = in9[5];
    node->sx = in9[6]; node->sy = in9[7]; node->sz = in9[8];
}

void dnode_set_color(int id, uint32_t abgr) {
    DNode *node = dnode_get(id);
    if (node) node->color = abgr;
}

void dnode_set_collision(int id, int enabled) {
    DNode *node = dnode_get(id);
    if (node) node->collision = enabled ? 1 : 0;
}

static bool node_local_bounds(const DNode *node, float out6[6]) {
    if (node->mesh >= 0) {
        DMesh *mesh = dmesh_get(node->mesh);
        if (mesh) { std::memcpy(out6, mesh->bounds, sizeof(float) * 6); return true; }
    }
    if (node->image >= 0) {
        DImage *image = dimage_get(node->image);
        if (image) {
            out6[0] = -0.5f * image->width / 100.0f; out6[1] = -0.5f * image->height / 100.0f; out6[2] = 0.0f;
            out6[3] = 0.5f * image->width / 100.0f;  out6[4] = 0.5f * image->height / 100.0f;  out6[5] = 0.0f;
            return true;
        }
    }
    if (node->primitive >= 0) {
        out6[0] = -0.5f; out6[1] = -0.5f; out6[2] = -0.5f;
        out6[3] = 0.5f;  out6[4] = 0.5f;  out6[5] = 0.5f;
        return true;
    }
    // Empty node / point: a small cube keeps it selectable and droppable.
    out6[0] = out6[1] = out6[2] = -0.15f;
    out6[3] = out6[4] = out6[5] = 0.15f;
    return true;
}

int dnode_world_aabb(int id, float *out6) {
    DNode *node = dnode_get(id);
    if (!node || !out6) return 0;
    float local[6];
    node_local_bounds(node, local);
    world_aabb_corners(*node, local, out6);
    return 1;
}

int dscene_node_count(void) { return g_scene.nodeCount - 1; }
int dscene_flat_count(void) { return g_scene.nodeCount; }

int dscene_dfs_at(int index) {
    if (index < 0) return -1;
    int *buffer = static_cast<int *>(std::malloc(sizeof(int) * static_cast<size_t>(g_scene.nodeCount + 1)));
    if (!buffer) return -1;
    int count = 0;
    dfs_collect(0, buffer, &count, g_scene.nodeCount);
    int result = (index + 1 < count) ? buffer[index + 1] : -1; // skip root
    std::free(buffer);
    return result;
}

int dscene_node_at(int index) {
    if (index <= 0 || index >= g_scene.nodeCount) return -1;
    return index;
}

int dmesh_add(const char *name, int vertex_count, float *positions, float *normals,
              float *uvs, int index_count, uint16_t *indices) {
    if (vertex_count <= 0 || !positions || index_count <= 0 || !indices) return -1;
    if (!ensure_meshes(g_scene.meshCount + 1)) return -1;
    const int index = g_scene.meshCount++;
    DMesh &mesh = g_scene.meshes[index];
    std::memset(&mesh, 0, sizeof(mesh));
    copy_name(mesh.name, sizeof(mesh.name), name ? name : "Mesh");
    mesh.vertex_count = vertex_count;
    mesh.index_count = index_count;
    mesh.positions = positions;
    mesh.normals = normals;
    mesh.uvs = uvs;
    mesh.indices = indices;
    dmesh_compute_bounds(&mesh);
    return index;
}

DMesh *dmesh_get(int index) {
    if (index < 0 || index >= g_scene.meshCount) return nullptr;
    return &g_scene.meshes[index];
}

int dmesh_count(void) { return g_scene.meshCount; }

void dmesh_compute_bounds(DMesh *mesh) {
    if (!mesh || !mesh->positions || mesh->vertex_count <= 0) return;
    mesh->bounds[0] = mesh->bounds[1] = mesh->bounds[2] = 1e30f;
    mesh->bounds[3] = mesh->bounds[4] = mesh->bounds[5] = -1e30f;
    for (int i = 0; i < mesh->vertex_count; ++i) {
        const float *p = &mesh->positions[i * 3];
        if (p[0] < mesh->bounds[0]) mesh->bounds[0] = p[0];
        if (p[1] < mesh->bounds[1]) mesh->bounds[1] = p[1];
        if (p[2] < mesh->bounds[2]) mesh->bounds[2] = p[2];
        if (p[0] > mesh->bounds[3]) mesh->bounds[3] = p[0];
        if (p[1] > mesh->bounds[4]) mesh->bounds[4] = p[1];
        if (p[2] > mesh->bounds[5]) mesh->bounds[5] = p[2];
    }
}

int dimage_add(const void *rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) return -1;
    if (width > 4096 || height > 4096) return -1;
    if (!ensure_images(g_scene.imageCount + 1)) return -1;
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    uint8_t *copy = static_cast<uint8_t *>(std::malloc(bytes));
    if (!copy) return -1;
    std::memcpy(copy, rgba, bytes);
    const int index = g_scene.imageCount++;
    g_scene.images[index].rgba = copy;
    g_scene.images[index].width = width;
    g_scene.images[index].height = height;
    return index;
}

DImage *dimage_get(int index) {
    if (index < 0 || index >= g_scene.imageCount) return nullptr;
    return &g_scene.images[index];
}

int dimage_count(void) { return g_scene.imageCount; }

int dimage_set(int node_id, int image_index) {
    DNode *node = dnode_get(node_id);
    if (!node) return 0;
    if (image_index >= 0 && !dimage_get(image_index)) return 0;
    node->image = image_index;
    if (image_index >= 0) node->type = DIAMOND_NODE_SPRITE;
    return 1;
}

void dscene_select(int id) {
    if (id != 0 && !dnode_get(id)) id = 0;
    g_scene.selected = id;
}

int dscene_selected(void) { return g_scene.selected; }

} // extern "C"

/* --------------------------------------------------------------------------
 *  Built-in primitives — cube, pyramid, quad, grid.
 *  These exist so "Add 3D model" can be tested without any asset at all,
 *  which matters on a fresh install with no SD card content.
 * ------------------------------------------------------------------------ */
extern "C" {

static int primitive_cube(void) {
    static const float p[] = {
        -0.5f,-0.5f, 0.5f,  0.5f,-0.5f, 0.5f,  0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f,
        -0.5f,-0.5f,-0.5f, -0.5f, 0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f,-0.5f,-0.5f,
        -0.5f,-0.5f,-0.5f,  0.5f,-0.5f,-0.5f,  0.5f,-0.5f, 0.5f, -0.5f,-0.5f, 0.5f,
        -0.5f, 0.5f,-0.5f, -0.5f, 0.5f, 0.5f,  0.5f, 0.5f, 0.5f,  0.5f, 0.5f,-0.5f,
         0.5f,-0.5f,-0.5f,  0.5f, 0.5f,-0.5f,  0.5f, 0.5f, 0.5f,  0.5f,-0.5f, 0.5f,
        -0.5f,-0.5f,-0.5f, -0.5f,-0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f,-0.5f};
    static const float n[] = {
         0,0, 1,  0,0, 1,  0,0, 1,  0,0, 1,
         0,0,-1,  0,0,-1,  0,0,-1,  0,0,-1,
         0,-1,0,  0,-1,0,  0,-1,0,  0,-1,0,
         0, 1,0,  0, 1,0,  0, 1,0,  0, 1,0,
         1,0, 0,  1,0, 0,  1,0, 0,  1,0, 0,
        -1,0, 0, -1,0, 0, -1,0, 0, -1,0, 0};
    static const float uv[] = {
        0,0, 1,0, 1,1, 0,1,   0,0, 1,0, 1,1, 0,1,
        0,0, 1,0, 1,1, 0,1,   0,0, 1,0, 1,1, 0,1,
        0,0, 1,0, 1,1, 0,1,   0,0, 1,0, 1,1, 0,1};
    static const uint16_t idx[] = {
        0,1,2, 0,2,3,  4,5,6, 4,6,7,  8,9,10, 8,10,11,
        12,13,14, 12,14,15,  16,17,18, 16,18,19,  20,21,22, 20,22,23};
    float *positions = static_cast<float *>(std::malloc(sizeof(p)));
    float *normals = static_cast<float *>(std::malloc(sizeof(n)));
    float *uvs = static_cast<float *>(std::malloc(sizeof(uv)));
    uint16_t *indices = static_cast<uint16_t *>(std::malloc(sizeof(idx)));
    if (!positions || !normals || !uvs || !indices) {
        std::free(positions); std::free(normals); std::free(uvs); std::free(indices);
        return -1;
    }
    std::memcpy(positions, p, sizeof(p));
    std::memcpy(normals, n, sizeof(n));
    std::memcpy(uvs, uv, sizeof(uv));
    std::memcpy(indices, idx, sizeof(idx));
    return dmesh_add("Cube", 24, positions, normals, uvs, 36, indices);
}

static int primitive_pyramid(void) {
    static const float p[] = {
        -0.5f,0.0f, 0.5f,  0.5f,0.0f, 0.5f,  0.0f,0.9f, 0.0f,
         0.5f,0.0f, 0.5f,  0.5f,0.0f,-0.5f,  0.0f,0.9f, 0.0f,
         0.5f,0.0f,-0.5f, -0.5f,0.0f,-0.5f,  0.0f,0.9f, 0.0f,
        -0.5f,0.0f,-0.5f, -0.5f,0.0f, 0.5f,  0.0f,0.9f, 0.0f,
        -0.5f,0.0f,-0.5f,  0.5f,0.0f,-0.5f,  0.5f,0.0f, 0.5f,
        -0.5f,0.0f,-0.5f,  0.5f,0.0f, 0.5f, -0.5f,0.0f, 0.5f};
    static const float n[] = {
         0.0f, 0.6f, 0.8f,  0.0f, 0.6f, 0.8f,  0.0f, 0.6f, 0.8f,
         0.8f, 0.6f, 0.0f,  0.8f, 0.6f, 0.0f,  0.8f, 0.6f, 0.0f,
         0.0f, 0.6f,-0.8f,  0.0f, 0.6f,-0.8f,  0.0f, 0.6f,-0.8f,
        -0.8f, 0.6f, 0.0f, -0.8f, 0.6f, 0.0f, -0.8f, 0.6f, 0.0f,
         0.0f,-1.0f, 0.0f,  0.0f,-1.0f, 0.0f,  0.0f,-1.0f, 0.0f,
         0.0f,-1.0f, 0.0f,  0.0f,-1.0f, 0.0f,  0.0f,-1.0f, 0.0f};
    static const float uv[] = {
        0,0, 1,0, 0.5f,1,  0,0, 1,0, 0.5f,1,  0,0, 1,0, 0.5f,1,
        0,0, 1,0, 0.5f,1,  0,0, 1,0, 1,1,      0,0, 1,0, 1,1};
    uint16_t idx[18];
    for (int i = 0; i < 18; ++i) idx[i] = static_cast<uint16_t>(i);
    const int vertices = 18;
    float *positions = static_cast<float *>(std::malloc(sizeof(float) * 3 * vertices));
    float *normals = static_cast<float *>(std::malloc(sizeof(float) * 3 * vertices));
    float *uvs = static_cast<float *>(std::malloc(sizeof(float) * 2 * vertices));
    uint16_t *indices = static_cast<uint16_t *>(std::malloc(sizeof(idx)));
    if (!positions || !normals || !uvs || !indices) {
        std::free(positions); std::free(normals); std::free(uvs); std::free(indices);
        return -1;
    }
    std::memcpy(positions, p, sizeof(p));
    std::memcpy(normals, n, sizeof(n));
    std::memcpy(uvs, uv, sizeof(uv));
    std::memcpy(indices, idx, sizeof(idx));
    return dmesh_add("Pyramid", vertices, positions, normals, uvs, 18, indices);
}

static int primitive_quad(void) {
    static const float p[] = {-0.5f,-0.5f,0.0f, 0.5f,-0.5f,0.0f, 0.5f,0.5f,0.0f, -0.5f,0.5f,0.0f};
    static const float n[] = {0,0,1, 0,0,1, 0,0,1, 0,0,1};
    static const float uv[] = {0,0, 1,0, 1,1, 0,1};
    static const uint16_t idx[] = {0,1,2, 0,2,3};
    float *positions = static_cast<float *>(std::malloc(sizeof(p)));
    float *normals = static_cast<float *>(std::malloc(sizeof(n)));
    float *uvs = static_cast<float *>(std::malloc(sizeof(uv)));
    uint16_t *indices = static_cast<uint16_t *>(std::malloc(sizeof(idx)));
    if (!positions || !normals || !uvs || !indices) {
        std::free(positions); std::free(normals); std::free(uvs); std::free(indices);
        return -1;
    }
    std::memcpy(positions, p, sizeof(p));
    std::memcpy(normals, n, sizeof(n));
    std::memcpy(uvs, uv, sizeof(uv));
    std::memcpy(indices, idx, sizeof(idx));
    return dmesh_add("Quad", 4, positions, normals, uvs, 6, indices);
}

int dmesh_add_primitive(int primitive) {
    switch (primitive) {
        case DIAMOND_PRIM_CUBE:    return primitive_cube();
        case DIAMOND_PRIM_PYRAMID: return primitive_pyramid();
        case DIAMOND_PRIM_QUAD:
        case DIAMOND_PRIM_GRID:    return primitive_quad();
        default:                   return -1;
    }
}

} // extern "C"
