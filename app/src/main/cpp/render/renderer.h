/* Diamond Engine — render backend interface (C++23, internal header).
 *
 * One interface, four implementations. Nothing in here throws: every method
 * reports failure through its return value, and the renderer falls back to the
 * next backend in the list instead of crashing a driver.
 */
#ifndef DIAMOND_RENDERER_H
#define DIAMOND_RENDERER_H

#include <cstdint>

#include "diamond.h"
#include "diamond/drender.h"
#include "diamond/dscene.h"

#include <GLES3/gl3.h>

namespace diamond {

struct FrameContext {
    int width = 0;
    int height = 0;
    const DCamera *camera = nullptr;
    int viewMode = DIAMOND_VIEW_3D;
    int selected = 0;
    int playing = 0;
    float joystick[2] = {0.0f, 0.0f};
    int buttons[DIAMOND_BTN_COUNT] = {0, 0, 0, 0, 0, 0};
    float delta = 0.0f;
    double time = 0.0;
};

class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    virtual const char *name() const = 0;
    virtual int mode() const = 0;

    /* Surface lifecycle. `window` is an ANativeWindow*. */
    virtual bool attach(void *window, int width, int height, char *err, int errCap) = 0;
    virtual void detach() = 0;
    virtual void resize(int width, int height) = 0;

    /* Draws one frame. Returns false when the surface is gone (the caller then
     * stops the loop until Java attaches a new one). */
    virtual bool draw(const FrameContext &frame) = 0;

    virtual void setDensity(float density) { density_ = density; }

    /* Statistics for the top bar. */
    virtual DRenderStats stats() const {
        DRenderStats s{};
        s.backend = mode();
        s.width = width_;
        s.height = height_;
        return s;
    }

protected:
    float density_ = 1.0f;
    int width_ = 0;
    int height_ = 0;
};

/* GLSL program helper (render/gl_program.cpp). */
struct GlProgram {
    unsigned int id = 0;
    int uMvp = -1, uModel = -1, uColor = -1, uLight = -1, uCamPos = -1, uTex = -1, uUseTexture = -1;
};
bool gl_program_link(GlProgram *program, const GLchar *vertexSource,
                     const GLchar *fragmentSource, const char *label,
                     int *esVersionOut);
void gl_program_release(GlProgram *program);
GLuint gl_program_id(const GlProgram *program);

/* Factories. Each returns nullptr when the backend cannot run on this device. */
IRenderBackend *createGles3Backend();
IRenderBackend *createGles2Backend();
IRenderBackend *createHybridBackend();
IRenderBackend *createVulkanBackend();

/* Shared helpers (implemented in gl_context.cpp / immediate.cpp). */
struct GlContext;
GlContext *gl_context_create(void *window, char *err, int errCap);
void gl_context_destroy(GlContext *context);
void gl_context_swap(GlContext *context);
void gl_context_info(const GlContext *context, int *esVersion, int *width, int *height,
                     const char **vendor, const char **renderer);
bool gl_context_lost(const GlContext *context);
void gl_context_reset_lost(GlContext *context);

/* Immediate-mode geometry used by every GL backend: a single VBO holding the
 * grid, selection boxes, gizmos, wireframes and the play overlay. */
struct ImBatch;
ImBatch *im_batch_create();
void im_batch_destroy(ImBatch *batch);
void im_batch_clear(ImBatch *batch);
void im_batch_line(ImBatch *batch, const float *a, const float *b, uint32_t color);
void im_batch_triangle(ImBatch *batch, const float *a, const float *b, const float *c, uint32_t color);
void im_batch_box(ImBatch *batch, const float *bounds6, uint32_t color);
void im_batch_grid(ImBatch *batch, int lines, float spacing, uint32_t color, uint32_t axis_color);
int  im_batch_vertex_count(const ImBatch *batch);
const float *im_batch_vertices(const ImBatch *batch);   /* xyz + rgba packed */

/* Scene geometry helpers shared by backends. */
uint32_t dnode_resolved_color(const DNode *node);
bool dnode_world_matrix(const DNode *node, float *out16);
void scene_collect_visible(FrameContext &frame, int *outIds, int *outCount, int capacity);

} // namespace diamond
#endif /* DIAMOND_RENDERER_H */

namespace diamond {

/* Runtime backend selection with automatic fallback and its own report. */
struct BackendSelection {
    IRenderBackend *backend = nullptr;
    int requested = 0;
    int active = 0;
    char error[192] = {0};
};

/* Tries `mode`; on failure fills `error` and leaves `backend` null. The caller
 * decides whether to fall back to GLES3 (splash) or surface the message (editor). */
bool renderer_open(BackendSelection &selection, int mode, void *window, int width, int height);

/* Releases the backend returned by renderer_open(). */
void renderer_close(BackendSelection &selection);

/* Human readable description used by the splash screen and the log console. */
const char *renderer_mode_name(int mode);
const char *renderer_mode_tag(int mode);   /* "GL3", "VK", "GL2", "HYB" */

/* Picking shared by all backends (implemented in renderer.cpp). */
int renderer_pick(int screenX, int screenY, const DCamera &camera, int viewMode,
                  int width, int height);

} // namespace diamond
