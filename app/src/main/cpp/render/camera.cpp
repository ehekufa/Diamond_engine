// Diamond Engine — shared camera.
#include "diamond/drender.h"

#include <cmath>
#include <cstring>

extern "C" {
#include "diamond/dmath.h"
#include "diamond/dscene.h"
}

namespace {
constexpr float kPitchLimit = 89.0f;
constexpr float kMinDistance = 0.5f;
constexpr float kMaxDistance = 400.0f;
constexpr float kMinZoom = 0.15f;
constexpr float kMaxZoom = 12.0f;
} // namespace

extern "C" {

void dcamera_init(DCamera *camera) {
    if (!camera) return;
    std::memset(camera, 0, sizeof(*camera));
    camera->target[0] = 0.0f;
    camera->target[1] = 0.0f;
    camera->target[2] = 0.0f;
    camera->distance = 8.0f;
    camera->yaw = 35.0f;
    camera->pitch = 28.0f;
    camera->fov = 55.0f;
    camera->near_plane = 0.05f;
    camera->far_plane = 500.0f;
    camera->zoom = 1.0f;
}

void dcamera_orbit(DCamera *camera, float dx, float dy) {
    if (!camera) return;
    camera->yaw += dx * 0.35f;
    camera->pitch = dmath_clamp(camera->pitch - dy * 0.35f, -kPitchLimit, kPitchLimit);
    while (camera->yaw > 360.0f) camera->yaw -= 360.0f;
    while (camera->yaw < -360.0f) camera->yaw += 360.0f;
}

void dcamera_pan(DCamera *camera, float dx, float dy) {
    if (!camera) return;
    // Pan in the camera plane: right vector and up vector (world space).
    const float yaw = dmath_radians(camera->yaw);
    const float scale = camera->distance * 0.0016f;
    camera->target[0] -= std::cos(yaw) * dx * scale;
    camera->target[2] += std::sin(yaw) * dx * scale;
    camera->target[1] += dy * scale;
}

void dcamera_zoom(DCamera *camera, float factor) {
    if (!camera) return;
    camera->distance = dmath_clamp(camera->distance * factor, kMinDistance, kMaxDistance);
    camera->zoom = dmath_clamp(camera->zoom * factor, kMinZoom, kMaxZoom);
}

void dcamera_focus(DCamera *camera, const float *target, float radius) {
    if (!camera || !target) return;
    camera->target[0] = target[0];
    camera->target[1] = target[1];
    camera->target[2] = target[2];
    camera->distance = dmath_clamp(radius * 3.0f + 2.0f, kMinDistance, kMaxDistance);
}

void dcamera_eye(const DCamera *camera, float *out3) {
    if (!camera || !out3) return;
    const float yaw = dmath_radians(camera->yaw);
    const float pitch = dmath_radians(camera->pitch);
    const float horizontal = std::cos(pitch) * camera->distance;
    out3[0] = camera->target[0] + std::sin(yaw) * horizontal;
    out3[1] = camera->target[1] + std::sin(pitch) * camera->distance;
    out3[2] = camera->target[2] + std::cos(yaw) * horizontal;
}

void dcamera_matrices(const DCamera *camera, int view_mode, int width, int height,
                      float *view, float *projection) {
    if (!camera || !view || !projection) return;
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;

    if (view_mode == DIAMOND_VIEW_2D) {
        // Orthographic, pixel-ish units: 1 world unit = zoom * height / 8.
        const float half_height = (static_cast<float>(height > 0 ? height : 720) / 8.0f) / camera->zoom;
        const float half_width = half_height * aspect;
        const float center_x = camera->target[0] + camera->pan[0];
        const float center_y = camera->target[1] + camera->pan[1];
        dmat4_ortho(projection, center_x - half_width, center_x + half_width,
                    center_y - half_height, center_y + half_height, -1000.0f, 1000.0f);
        dmat4_identity(view);
        return;
    }

    float eye[3];
    dcamera_eye(camera, eye);
    const float up[3] = {0.0f, 1.0f, 0.0f};
    float target[3] = {camera->target[0], camera->target[1], camera->target[2]};
    if (view_mode == DIAMOND_VIEW_25D) {
        // Fixed isometric-ish angle for 2.5D: no free orbit, longer lens.
        const float iso_yaw = dmath_radians(45.0f);
        const float iso_pitch = dmath_radians(35.264f);
        eye[0] = target[0] + std::sin(iso_yaw) * std::cos(iso_pitch) * camera->distance;
        eye[1] = target[1] + std::sin(iso_pitch) * camera->distance;
        eye[2] = target[2] + std::cos(iso_yaw) * std::cos(iso_pitch) * camera->distance;
    }
    dmat4_look_at(view, eye, target, up);
    dmat4_perspective(projection, camera->fov, aspect, camera->near_plane, camera->far_plane);
}

int dcamera_screen_to_world_2d(const DCamera *camera, int width, int height,
                               float screen_x, float screen_y, float *out3) {
    if (!camera || !out3 || width <= 0 || height <= 0) return 0;
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float half_height = (static_cast<float>(height) / 8.0f) / camera->zoom;
    const float half_width = half_height * aspect;
    const float normalized_x = (screen_x / static_cast<float>(width)) * 2.0f - 1.0f;
    const float normalized_y = 1.0f - (screen_y / static_cast<float>(height)) * 2.0f;
    out3[0] = camera->target[0] + camera->pan[0] + normalized_x * half_width;
    out3[1] = camera->target[1] + camera->pan[1] + normalized_y * half_height;
    out3[2] = 0.0f;
    return 1;
}

} // extern "C"
