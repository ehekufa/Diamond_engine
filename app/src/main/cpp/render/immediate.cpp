// Diamond Engine — immediate mode geometry builder.
//
// Every GL backend needs the same handful of primitives: grid lines, boxes,
// arrows. They all end up in one interleaved buffer (pos xyz + rgba) that is
// re-uploaded per frame, which on mobile is cheaper than maintaining dozens of
// small buffers and state changes.
#include "renderer.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace diamond {

namespace {
constexpr int kInitialCapacity = 4096; // vertices
constexpr float kPi = 3.14159265358979f;
}

struct ImBatch {
    float *data = nullptr;      // 10 floats per vertex: x,y,z, r,g,b,a (as floats)
    int count = 0;              // vertices
    int capacity = 0;
    bool failed = false;

    bool ensure(int extra) {
        if (failed) return false;
        if (count + extra <= capacity) return true;
        int cap = capacity ? capacity : kInitialCapacity;
        while (cap < count + extra) cap *= 2;
        float *grown = static_cast<float *>(std::realloc(data, sizeof(float) * 10 * static_cast<size_t>(cap)));
        if (!grown) { failed = true; return false; }
        data = grown;
        capacity = cap;
        return true;
    }

    // Vertex layout matches what the GL3 shader expects: three floats position,
    // then four bytes of colour stored inside one float slot, plus padding.
    void push(const float *p, uint32_t color) {
        if (!ensure(1)) return;
        float *slot = &data[count * 10];
        slot[0] = p[0]; slot[1] = p[1]; slot[2] = p[2];
        slot[3] = 0.0f; slot[4] = 0.0f; slot[5] = 0.0f;
        // The colour occupies 4 bytes at offset 24 and another 4 at offset 28;
        // we use a 2-float slot (8 bytes) so the attribute stride is 40 bytes.
        unsigned char *bytes = reinterpret_cast<unsigned char *>(&slot[6]);
        bytes[0] = static_cast<unsigned char>(color & 0xFF);
        bytes[1] = static_cast<unsigned char>((color >> 8) & 0xFF);
        bytes[2] = static_cast<unsigned char>((color >> 16) & 0xFF);
        bytes[3] = static_cast<unsigned char>((color >> 24) & 0xFF);
        bytes[4] = 0; bytes[5] = 0; bytes[6] = 0; bytes[7] = 0;
        count++;
    }
};

ImBatch *im_batch_create() {
    ImBatch *batch = new ImBatch();
    if (!batch) return nullptr;
    if (!batch->ensure(kInitialCapacity)) {
        delete batch;
        return nullptr;
    }
    return batch;
}

void im_batch_destroy(ImBatch *batch) {
    if (!batch) return;
    std::free(batch->data);
    delete batch;
}

void im_batch_clear(ImBatch *batch) {
    if (batch) batch->count = 0;
}

void im_batch_line(ImBatch *batch, const float *a, const float *b, uint32_t color) {
    if (!batch || !a || !b) return;
    batch->push(a, color);
    batch->push(b, color);
}

void im_batch_triangle(ImBatch *batch, const float *a, const float *b, const float *c, uint32_t color) {
    if (!batch || !a || !b || !c) return;
    batch->push(a, color);
    batch->push(b, color);
    batch->push(c, color);
}

void im_batch_box(ImBatch *batch, const float *bounds6, uint32_t color) {
    if (!batch || !bounds6) return;
    const float x0 = bounds6[0], y0 = bounds6[1], z0 = bounds6[2];
    const float x1 = bounds6[3], y1 = bounds6[4], z1 = bounds6[5];
    const float corners[8][3] = {
        {x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0},
        {x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}};
    const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}};
    for (int i = 0; i < 12; ++i)
        im_batch_line(batch, corners[edges[i][0]], corners[edges[i][1]], color);
}

void im_batch_grid(ImBatch *batch, int lines, float spacing, uint32_t color, uint32_t axis_color) {
    if (!batch || lines <= 0) return;
    const float half = static_cast<float>(lines) * 0.5f * spacing;
    for (int i = 0; i <= lines; ++i) {
        const float offset = -half + static_cast<float>(i) * spacing;
        const uint32_t lineColor = (std::fabs(offset) < 1e-4f) ? axis_color : color;
        const float a[3] = {-half, 0.0f, offset};
        const float b[3] = {half, 0.0f, offset};
        im_batch_line(batch, a, b, lineColor);
        const float c[3] = {offset, 0.0f, -half};
        const float d[3] = {offset, 0.0f, half};
        im_batch_line(batch, c, d, lineColor);
    }
}

int im_batch_vertex_count(const ImBatch *batch) { return batch ? batch->count : 0; }

const float *im_batch_vertices(const ImBatch *batch) { return batch ? batch->data : nullptr; }

} // namespace diamond

// ---------------------------------------------------------------------------
//  Scene helpers shared by the GL backends (kept here so every backend uses
//  exactly the same colour and matrix rules).
// ---------------------------------------------------------------------------
#include "diamond/dmath.h"
#include "diamond/dscene.h"

namespace diamond {

uint32_t dnode_resolved_color(const DNode *node) {
    if (!node) return 0xFFB0B0B0u;
    uint32_t color = node->color;
    if ((color >> 24) == 0) color |= 0xFF000000u; // never fully transparent
    return color;
}

bool dnode_world_matrix(const DNode *node, float *out16) {
    if (!node || !out16) return false;
    const float trs[9] = {node->px, node->py, node->pz,
                          node->rx, node->ry, node->rz,
                          node->sx, node->sy, node->sz};
    dmat4_from_trs(out16, trs);
    return true;
}

void scene_collect_visible(FrameContext &frame, int *outIds, int *outCount, int capacity) {
    if (!outIds || !outCount) return;
    int count = 0;
    const int total = dscene_flat_count();
    for (int i = 1; i < total && count < capacity; ++i) {
        DNode *node = dnode_get(i);
        if (!node || !node->visible) continue;
        outIds[count++] = node->id;
    }
    *outCount = count;
    (void)frame;
}

} // namespace diamond
