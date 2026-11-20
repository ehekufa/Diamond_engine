// Diamond Engine — EGL context management.
//
// One EGL surface per ANativeWindow, chosen EGLConfig per backend:
//   GLES3  requests EGL_OPENGL_ES3_BIT + depth/stencil
//   GLES2  requests EGL_OPENGL_ES2_BIT only, no depth, RGB565 when available
//   HYBRID GLES3 for the 3D pass, the software pass only touches its own
//          texture and blits through GL
//
// Context loss (eglSwapBuffers returning EGL_CONTEXT_LOST, or a surface that
// vanished on screen-off) is handled by rebuilding the context on the next
// frame instead of dying, which matters a lot on cheap drivers.
#include "renderer.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <EGL/eglext.h>
#include <android/native_window.h>

#include <cstdio>

extern "C" {
#include "diamond/dlog.h"
#include "diamond/dutil.h"
}

namespace diamond {

struct GlContext {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    EGLConfig config = nullptr;
    ANativeWindow *window = nullptr;
    int width = 0;
    int height = 0;
    int esVersion = 3;
    bool lost = false;
    char vendor[64] = {0};
    char renderer[128] = {0};
};

namespace {

EGLConfig choose_config(EGLDisplay display, int esVersion) {
    EGLint attributes[24];
    int index = 0;
    attributes[index++] = EGL_SURFACE_TYPE;
    attributes[index++] = EGL_WINDOW_BIT;
    attributes[index++] = EGL_RENDERABLE_TYPE;
    attributes[index++] = (esVersion >= 3) ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT;
    attributes[index++] = EGL_RED_SIZE;
    attributes[index++] = 8;
    attributes[index++] = EGL_GREEN_SIZE;
    attributes[index++] = 8;
    attributes[index++] = EGL_BLUE_SIZE;
    attributes[index++] = 8;
    attributes[index++] = EGL_ALPHA_SIZE;
    attributes[index++] = 8;
    attributes[index++] = EGL_DEPTH_SIZE;
    attributes[index++] = (esVersion >= 3) ? 24 : 0;
    attributes[index++] = EGL_STENCIL_SIZE;
    attributes[index++] = 0;
    attributes[index++] = EGL_SAMPLE_BUFFERS;
    attributes[index++] = 0;
    attributes[index++] = EGL_NONE;

    EGLConfig configs[64];
    EGLint count = 0;
    if (!eglChooseConfig(display, attributes, configs, 64, &count) || count == 0) {
        return nullptr;
    }
    // Prefer the first config that is *not* a window-transparent RGB565 one.
    for (EGLint i = 0; i < count; ++i) {
        EGLint red = 0, alpha = 0;
        eglGetConfigAttrib(display, configs[i], EGL_RED_SIZE, &red);
        eglGetConfigAttrib(display, configs[i], EGL_ALPHA_SIZE, &alpha);
        if (red >= 8 && alpha >= 8) return configs[i];
    }
    return configs[0];
}

} // namespace

GlContext *gl_context_create(void *window, char *err, int errCap) {
    GlContext *ctx = new GlContext();
    if (!ctx) return nullptr;
    ctx->window = static_cast<ANativeWindow *>(window);
    ctx->esVersion = 3;

    ctx->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ctx->display == EGL_NO_DISPLAY) {
        diamond_fail(DIAMOND_ERR_GPU, err, errCap, "EGL: no default display");
        delete ctx;
        return nullptr;
    }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(ctx->display, &major, &minor)) {
        diamond_fail(DIAMOND_ERR_GPU, err, errCap, "EGL: eglInitialize failed (0x%x)", eglGetError());
        delete ctx;
        return nullptr;
    }

    ctx->config = choose_config(ctx->display, ctx->esVersion);
    EGLint client_version = 3;
    if (!ctx->config) {
        // No ES3 config: drop to ES2, which is exactly the potato target.
        ctx->esVersion = 2;
        client_version = 2;
        ctx->config = choose_config(ctx->display, ctx->esVersion);
    }
    if (!ctx->config) {
        diamond_fail(DIAMOND_ERR_GPU, err, errCap, "EGL: no suitable config");
        eglTerminate(ctx->display);
        delete ctx;
        return nullptr;
    }

    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, client_version, EGL_NONE};
    ctx->context = eglCreateContext(ctx->display, ctx->config, EGL_NO_CONTEXT, context_attributes);
    if (ctx->context == EGL_NO_CONTEXT && client_version == 3) {
        client_version = 2;
        const EGLint fallback_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        ctx->context = eglCreateContext(ctx->display, ctx->config, EGL_NO_CONTEXT, fallback_attributes);
        if (ctx->context != EGL_NO_CONTEXT) ctx->esVersion = 2;
    }
    if (ctx->context == EGL_NO_CONTEXT) {
        diamond_fail(DIAMOND_ERR_GPU, err, errCap, "EGL: context creation failed (0x%x)", eglGetError());
        eglTerminate(ctx->display);
        delete ctx;
        return nullptr;
    }

    ctx->surface = eglCreateWindowSurface(ctx->display, ctx->config, ctx->window, nullptr);
    if (ctx->surface == EGL_NO_SURFACE) {
        diamond_fail(DIAMOND_ERR_GPU, err, errCap, "EGL: window surface failed (0x%x)", eglGetError());
        eglDestroyContext(ctx->display, ctx->context);
        eglTerminate(ctx->display);
        delete ctx;
        return nullptr;
    }
    if (!eglMakeCurrent(ctx->display, ctx->surface, ctx->surface, ctx->context)) {
        diamond_fail(DIAMOND_ERR_GPU, err, errCap, "EGL: eglMakeCurrent failed (0x%x)", eglGetError());
        eglDestroySurface(ctx->display, ctx->surface);
        eglDestroyContext(ctx->display, ctx->context);
        eglTerminate(ctx->display);
        delete ctx;
        return nullptr;
    }

    EGLint w = 0, h = 0;
    eglQuerySurface(ctx->display, ctx->surface, EGL_WIDTH, &w);
    eglQuerySurface(ctx->display, ctx->surface, EGL_HEIGHT, &h);
    ctx->width = w;
    ctx->height = h;

    const char *vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
    const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    if (vendor) snprintf(ctx->vendor, sizeof(ctx->vendor), "%s", vendor);
    if (renderer) snprintf(ctx->renderer, sizeof(ctx->renderer), "%s", renderer);
    dlog_write(DLOG_INFO, "egl", "ES%d %dx%d | %s | %s", ctx->esVersion, w, h,
               ctx->vendor, ctx->renderer);
    return ctx;
}

void gl_context_destroy(GlContext *ctx) {
    if (!ctx) return;
    if (ctx->display != EGL_NO_DISPLAY) {
        eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (ctx->surface != EGL_NO_SURFACE) eglDestroySurface(ctx->display, ctx->surface);
        if (ctx->context != EGL_NO_CONTEXT) eglDestroyContext(ctx->display, ctx->context);
        eglTerminate(ctx->display);
    }
    delete ctx;
}

void gl_context_info(const GlContext *ctx, int *es_version, int *width, int *height,
                     const char **vendor, const char **renderer) {
    if (!ctx) return;
    if (es_version) *es_version = ctx->esVersion;
    if (width) *width = ctx->width;
    if (height) *height = ctx->height;
    if (vendor) *vendor = ctx->vendor;
    if (renderer) *renderer = ctx->renderer;
}

bool gl_context_lost(const GlContext *ctx) { return ctx ? ctx->lost : true; }

void gl_context_reset_lost(GlContext *ctx) { if (ctx) ctx->lost = false; }

void gl_context_swap(GlContext *ctx) {
    if (!ctx || ctx->display == EGL_NO_DISPLAY) return;
    if (!eglSwapBuffers(ctx->display, ctx->surface)) {
        const EGLint error = eglGetError();
        if (error == EGL_CONTEXT_LOST || error == EGL_BAD_SURFACE) {
            ctx->lost = true;
            dlog_write(DLOG_WARN, "egl", "swap failed (0x%x) — recreating context", error);
        }
    }
}

} // namespace diamond
