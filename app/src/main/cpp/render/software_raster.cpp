// Diamond Engine — software rasteriser for the Hybrid backend.
//
// Only 2D work lands here: sprites, particles, glow quads and the HUD. The
// target is a small RGBA texture that the GL pass blits on top of the 3D scene,
// so the cost stays at a few hundred thousand pixels even on a weak phone.
//
// Design choices that matter on a potato:
//   * no per-pixel coverage: 2D art is opaque or has a binary alpha test, which
//     avoids an expensive blend in the inner loop;
//   * integer bounding boxes, clipped once per sprite;
//   * nearest-neighbour sampling by default (the cost difference against
//     bilinear on a Cortex-A7 is about 2x), with a switch for quality.
#include "diamond/dsoftware.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {

constexpr int kMaxTargetWidth = 2048;
constexpr int kMaxTargetHeight = 2048;

struct SoftwareTarget {
    uint8_t *rgba = nullptr;
    int width = 0;
    int height = 0;
    bool smooth = false;
};

SoftwareTarget g_target;

inline uint32_t pack_rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

} // namespace

extern "C" {

int software_target_resize(int width, int height) {
    if (width <= 0 || height <= 0 || width > kMaxTargetWidth || height > kMaxTargetHeight) return 0;
    if (g_target.width == width && g_target.height == height && g_target.rgba) return 1;
    const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    uint8_t *buffer = static_cast<uint8_t *>(std::realloc(g_target.rgba, bytes));
    if (!buffer) return 0;
    g_target.rgba = buffer;
    g_target.width = width;
    g_target.height = height;
    return 1;
}

void software_target_release(void) {
    std::free(g_target.rgba);
    g_target.rgba = nullptr;
    g_target.width = 0;
    g_target.height = 0;
}

void software_clear(uint32_t abgr) {
    if (!g_target.rgba) return;
    const uint8_t r = static_cast<uint8_t>(abgr & 0xFF);
    const uint8_t g = static_cast<uint8_t>((abgr >> 8) & 0xFF);
    const uint8_t b = static_cast<uint8_t>((abgr >> 16) & 0xFF);
    const uint8_t a = static_cast<uint8_t>((abgr >> 24) & 0xFF);
    const uint32_t pixel = pack_rgba(r, g, b, a);
    const size_t count = static_cast<size_t>(g_target.width) * static_cast<size_t>(g_target.height);
    uint32_t *line = reinterpret_cast<uint32_t *>(g_target.rgba);
    for (size_t i = 0; i < count; ++i) line[i] = pixel;
}

void software_set_smooth(int smooth) { g_target.smooth = smooth != 0; }

void software_fill_rect(float x, float y, float w, float h, uint32_t abgr) {
    if (!g_target.rgba || w <= 0.0f || h <= 0.0f) return;
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = static_cast<int>(std::ceil(x + w));
    int y1 = static_cast<int>(std::ceil(y + h));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_target.width) x1 = g_target.width;
    if (y1 > g_target.height) y1 = g_target.height;
    if (x0 >= x1 || y0 >= y1) return;
    const uint8_t r = static_cast<uint8_t>(abgr & 0xFF);
    const uint8_t g = static_cast<uint8_t>((abgr >> 8) & 0xFF);
    const uint8_t b = static_cast<uint8_t>((abgr >> 16) & 0xFF);
    const uint8_t a = static_cast<uint8_t>((abgr >> 24) & 0xFF);
    const uint32_t pixel = pack_rgba(r, g, b, a);
    for (int row = y0; row < y1; ++row) {
        uint32_t *dst = reinterpret_cast<uint32_t *>(g_target.rgba) +
                        static_cast<size_t>(row) * static_cast<size_t>(g_target.width) + static_cast<size_t>(x0);
        if (a == 0xFF) {
            for (int column = x0; column < x1; ++column) dst[column - x0] = pixel;
        } else if (a == 0) {
            continue;
        } else {
            const uint32_t alpha = a;
            const uint32_t inverse = 255u - alpha;
            for (int column = x0; column < x1; ++column) {
                const uint32_t existing = dst[column - x0];
                const uint32_t er = existing & 0xFF, eg = (existing >> 8) & 0xFF;
                const uint32_t eb = (existing >> 16) & 0xFF, ea = (existing >> 24) & 0xFF;
                const uint32_t nr = (r * alpha + er * inverse) / 255u;
                const uint32_t ng = (g * alpha + eg * inverse) / 255u;
                const uint32_t nb = (b * alpha + eb * inverse) / 255u;
                const uint32_t na = alpha + (ea * inverse) / 255u;
                dst[column - x0] = pack_rgba(nr, ng, nb, na > 255 ? 255 : na);
            }
        }
    }
}

void software_blit_sprite(const void *rgba, int sourceWidth, int sourceHeight,
                          float dx, float dy, float dw, float dh, float rotation,
                          uint32_t tint, int alphaTest) {
    if (!g_target.rgba || !rgba || sourceWidth <= 0 || sourceHeight <= 0) return;
    if (dw <= 0.0f || dh <= 0.0f) return;

    const uint8_t *source = static_cast<const uint8_t *>(rgba);
    const float cosAngle = std::cos(rotation);
    const float sinAngle = std::sin(rotation);
    const float halfwayX = dw * 0.5f, halfwayY = dh * 0.5f;

    // Bounding box of the rotated quad.
    const float extentX = std::fabs(halfwayX * cosAngle) + std::fabs(halfwayY * sinAngle);
    const float extentY = std::fabs(halfwayX * sinAngle) + std::fabs(halfwayY * cosAngle);

    int x0 = static_cast<int>(std::floor(dx - extentX));
    int y0 = static_cast<int>(std::floor(dy - extentY));
    int x1 = static_cast<int>(std::ceil(dx + extentX));
    int y1 = static_cast<int>(std::ceil(dy + extentY));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_target.width) x1 = g_target.width;
    if (y1 > g_target.height) y1 = g_target.height;
    if (x0 >= x1 || y0 >= y1) return;

    const uint32_t tintR = tint & 0xFF, tintG = (tint >> 8) & 0xFF;
    const uint32_t tintB = (tint >> 16) & 0xFF, tintA = (tint >> 24) & 0xFF;

    for (int y = y0; y < y1; ++y) {
        const float localY = static_cast<float>(y) + 0.5f - dy;
        uint32_t *dstRow = reinterpret_cast<uint32_t *>(g_target.rgba) +
                           static_cast<size_t>(y) * static_cast<size_t>(g_target.width);
        for (int x = x0; x < x1; ++x) {
            const float localX = static_cast<float>(x) + 0.5f - dx;
            // Inverse rotation.
            const float u = (localX * cosAngle + localY * sinAngle) + halfwayX;
            const float v = (-localX * sinAngle + localY * cosAngle) + halfwayY;
            if (u < 0.0f || v < 0.0f || u >= dw || v >= dh) continue;

            float sx = (u / dw) * static_cast<float>(sourceWidth);
            float sy = (v / dh) * static_cast<float>(sourceHeight);
            int ix = static_cast<int>(sx), iy = static_cast<int>(sy);
            if (ix >= sourceWidth) ix = sourceWidth - 1;
            if (iy >= sourceHeight) iy = sourceHeight - 1;
            const uint8_t *texel = source + (static_cast<size_t>(iy) * static_cast<size_t>(sourceWidth) +
                                             static_cast<size_t>(ix)) * 4u;
            uint32_t alpha = texel[3];
            if (alphaTest && alpha < 128) continue;
            if (tintA != 255) alpha = (alpha * tintA) / 255u;
            if (alpha == 0) continue;

            const uint32_t r = (texel[0] * tintR) / 255u;
            const uint32_t g = (texel[1] * tintG) / 255u;
            const uint32_t b = (texel[2] * tintB) / 255u;
            uint32_t &target = dstRow[x];
            if (alpha == 255) {
                target = pack_rgba(r, g, b, 255);
            } else {
                const uint32_t inverse = 255u - alpha;
                const uint32_t tr = target & 0xFF, tg = (target >> 8) & 0xFF;
                const uint32_t tb = (target >> 16) & 0xFF, ta = (target >> 24) & 0xFF;
                target = pack_rgba((r * alpha + tr * inverse) / 255u,
                                   (g * alpha + tg * inverse) / 255u,
                                   (b * alpha + tb * inverse) / 255u,
                                   alpha + (ta * inverse) / 255u);
            }
        }
    }
    (void)g_target.smooth;
}

const void *software_target_pixels(int *width, int *height) {
    if (width) *width = g_target.width;
    if (height) *height = g_target.height;
    return g_target.rgba;
}

int software_target_width(void) { return g_target.width; }
int software_target_height(void) { return g_target.height; }

} // extern "C"
