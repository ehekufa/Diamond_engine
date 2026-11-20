/* Diamond Engine — render layer public surface (C ABI).
 *
 * Four backends implement IRenderBackend (render/renderer.h in C++):
 *   GLES3   — full 2D/2.5D/3D, per-pixel lighting, line grid
 *   GLES2   — "potato": no lighting, no MSAA, minimal shaders
 *   HYBRID  — GL draws 3D, a software rasteriser draws 2D sprites/particles
 *   VULKAN  — experimental, falls back with a precise message
 *
 * The camera is shared by every backend so switching modes never changes the
 * framing.
 */
#ifndef DIAMOND_DRENDER_H
#define DIAMOND_DRENDER_H

#include "diamond.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DCamera {
    /* Orbit camera around `target`. */
    float target[3];
    float distance;
    float yaw;      /* degrees, around Y */
    float pitch;    /* degrees, -89..89 */
    float fov;      /* degrees          */
    float near_plane;
    float far_plane;
    /* 2D pan/zoom offset applied in 2D and 2.5D view modes. */
    float pan[2];
    float zoom;
} DCamera;

void  dcamera_init(DCamera *camera);
void  dcamera_orbit(DCamera *camera, float dx, float dy);
void  dcamera_pan(DCamera *camera, float dx, float dy);
void  dcamera_zoom(DCamera *camera, float factor);
void  dcamera_focus(DCamera *camera, const float *target, float radius);

/* Builds view and projection for the current surface size and view mode. */
void  dcamera_matrices(const DCamera *camera, int view_mode, int width, int height,
                       float *view, float *projection);
void  dcamera_eye(const DCamera *camera, float *out3);

/* Screen (pixels, origin top-left) to world ray. Returns 1 when the ray hits
 * the plane z = 0 in 2D mode, which is what tap-selection uses. */
int   dcamera_screen_to_world_2d(const DCamera *camera, int width, int height,
                                 float screen_x, float screen_y, float *out3);

/* Picking for 3D: returns the id of the node whose AABB the ray hits. */
int   drender_pick(int screen_x, int screen_y);

/* Backend switching + statistics. */
int   drender_set_mode(int mode, char *err, int err_cap);
int   drender_mode(void);
void  drender_set_view_mode(int view_mode);
int   drender_view_mode(void);

/* Frame statistics filled by the active backend. */
typedef struct DRenderStats {
    int backend;        /* DiamondRenderMode                */
    int fps;
    float frame_ms;
    int draw_calls;
    int triangles;
    int sprites;
    int width;
    int height;
} DRenderStats;

void drender_stats(DRenderStats *out);

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DRENDER_H */
