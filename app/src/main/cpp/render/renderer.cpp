// Diamond Engine — backend selection, fallback logic and picking.
#include "renderer.h"

#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include "diamond/dlog.h"
#include "diamond/dmath.h"
#include "diamond/dutil.h"
}

namespace diamond {

const char *renderer_mode_name(int mode) {
    switch (mode) {
        case DIAMOND_RENDER_GLES3:  return "OpenGL ES 3.0";
        case DIAMOND_RENDER_VULKAN: return "Vulkan";
        case DIAMOND_RENDER_GLES2:  return "OpenGL ES 2.0 (potato)";
        case DIAMOND_RENDER_HYBRID: return "Hybrid GL + software";
        default:                    return "unknown";
    }
}

const char *renderer_mode_tag(int mode) {
    switch (mode) {
        case DIAMOND_RENDER_GLES3:  return "GL3";
        case DIAMOND_RENDER_VULKAN: return "VK";
        case DIAMOND_RENDER_GLES2:  return "GL2";
        case DIAMOND_RENDER_HYBRID: return "HYB";
        default:                    return "???";
    }
}

bool renderer_open(BackendSelection &selection, int mode, void *window, int width, int height) {
    selection.backend = nullptr;
    selection.requested = mode;
    selection.active = 0;
    selection.error[0] = '\0';

    IRenderBackend *backend = nullptr;
    switch (mode) {
        case DIAMOND_RENDER_GLES3:  backend = createGles3Backend(); break;
        case DIAMOND_RENDER_GLES2:  backend = createGles2Backend(); break;
        case DIAMOND_RENDER_HYBRID: backend = createHybridBackend(); break;
        case DIAMOND_RENDER_VULKAN: backend = createVulkanBackend(); break;
        default:                    backend = createGles3Backend(); break;
    }
    if (!backend) {
        snprintf(selection.error, sizeof(selection.error),
                 "%s backend is not part of this build", renderer_mode_name(mode));
        return false;
    }
    char error[192] = {0};
    if (!backend->attach(window, width, height, error, sizeof(error))) {
        snprintf(selection.error, sizeof(selection.error), "%s",
                 error[0] ? error : "backend failed to attach");
        dlog_write(DLOG_WARN, "render", "%s unavailable: %s", renderer_mode_name(mode), selection.error);
        delete backend;
        return false;
    }
    selection.backend = backend;
    selection.active = backend->mode();
    dlog_write(DLOG_INFO, "render", "active backend: %s", renderer_mode_name(selection.active));
    return true;
}

void renderer_close(BackendSelection &selection) {
    if (selection.backend) {
        selection.backend->detach();
        delete selection.backend;
    }
    selection.backend = nullptr;
    selection.active = 0;
}

int renderer_pick(int screenX, int screenY, const DCamera &camera, int viewMode,
                  int width, int height) {
    if (width <= 0 || height <= 0) return 0;

    if (viewMode == DIAMOND_VIEW_2D) {
        float world[3];
        dcamera_screen_to_world_2d(&camera, width, height, static_cast<float>(screenX),
                                   static_cast<float>(screenY), world);
        // Pick the topmost sprite/empty whose AABB contains the point, scanning
        // in reverse draw order so the last drawn object wins.
        const int total = dscene_flat_count();
        for (int i = total - 1; i >= 1; --i) {
            DNode *node = dnode_get(i);
            if (!node || !node->visible) continue;
            float box[6];
            if (!dnode_world_aabb(i, box)) continue;
            if (world[0] >= box[0] && world[0] <= box[3] &&
                world[1] >= box[1] && world[1] <= box[4]) return i;
        }
        return 0;
    }

    // 3D / 2.5D: build a ray from the camera and test every node AABB.
    float view[16], projection[16];
    dcamera_matrices(&camera, viewMode, width, height, view, projection);

    // Inverse of projection*view is expensive; instead project each AABB centre
    // and pick the closest whose projected bounds contain the pointer. This is
    // what the editor needs (touch accuracy, not physics accuracy) and it costs
    // one matrix multiply per node.
    float vp[16];
    dmat4_multiply(vp, projection, view);
    int best = 0;
    float bestDepth = 1e30f;
    const int total = dscene_flat_count();
    for (int i = 1; i < total; ++i) {
        DNode *node = dnode_get(i);
        if (!node || !node->visible) continue;
        float box[6];
        if (!dnode_world_aabb(i, box)) continue;
        const float center[3] = {(box[0] + box[3]) * 0.5f, (box[1] + box[4]) * 0.5f, (box[2] + box[5]) * 0.5f};
        float clip[4];
        dmat4_transform_point(vp, center, clip);
        if (clip[3] <= 0.0f) continue;
        const float ndcX = clip[0] / clip[3];
        const float ndcY = clip[1] / clip[3];
        const float screenCenterX = (ndcX * 0.5f + 0.5f) * static_cast<float>(width);
        const float screenCenterY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(height);

        // Approximate radius on screen from the largest extent.
        const float extent = box[3] - box[0] > box[4] - box[1]
                                 ? (box[3] - box[0]) * 0.5f
                                 : (box[4] - box[1]) * 0.5f;
        const float radius = extent * static_cast<float>(height) * 0.5f /
                             (std::tan(dmath_radians(camera.fov) * 0.5f) * (clip[3] + 0.0001f));
        const float dx = screenCenterX - static_cast<float>(screenX);
        const float dy = screenCenterY - static_cast<float>(screenY);
        if (dx * dx + dy * dy > radius * radius + 256.0f) continue;
        if (clip[3] < bestDepth) {
            bestDepth = clip[3];
            best = i;
        }
    }
    return best;
}

} // namespace diamond

/* C-linkage bridge used by api/diamond_api.cpp and the JNI layer. */
extern "C" {
const char *renderer_mode_name(int mode) { return diamond::renderer_mode_name(mode); }
const char *renderer_mode_tag(int mode) { return diamond::renderer_mode_tag(mode); }
int renderer_pick(int screenX, int screenY, const DCamera *camera, int viewMode,
                  int width, int height) {
    if (!camera) return 0;
    return diamond::renderer_pick(screenX, screenY, *camera, viewMode, width, height);
}
}
