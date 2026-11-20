// Diamond Engine — render thread and ANativeWindow ownership.
//
// Java never drives rendering: SurfaceHolder gives us an ANativeWindow, this
// file spawns the loop and the loop owns the whole frame. Java only pokes
// state through the C API, which is why there is no JNI call per frame.
#include "diamond/dwindow.h"

#include <android/native_window.h>

#include <cstdio>
#include <cstring>

extern "C" {
#include "diamond/dlog.h"
#include "diamond/dutil.h"
#include "diamond/dengine.h"
#include "diamond/dlogic.h"
#include "diamond/dscene.h"
#include "diamond/drender.h"
}

#include "../render/renderer.h"

namespace {

struct RenderLoop {
    pthread_t thread;
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
    bool threadRunning = false;
    bool shouldStop = false;
    bool surfaceDirty = false;
    ANativeWindow *window = nullptr;
    ANativeWindow *pendingWindow = nullptr;
    int pendingWidth = 0;
    int pendingHeight = 0;
    diamond::BackendSelection selection;
    bool attached = false;
    char lastError[192] = {0};
    float fpsAverage = 0.0f;
    double fpsWindowStart = 0.0;
    int fpsFrames = 0;
    bool paused = false;
    double lastFrame = 0.0;
};

RenderLoop g_loop;

void fill_frame(diamond::FrameContext &frame, float delta, double now) {
    frame.width = g_engine.width;
    frame.height = g_engine.height;
    frame.camera = &g_engine.camera;
    frame.viewMode = g_engine.view_mode;
    frame.selected = dscene_selected();
    frame.playing = g_engine.play_state == DIAMOND_PLAYING;
    frame.joystick[0] = g_engine.joystick[0];
    frame.joystick[1] = g_engine.joystick[1];
    for (int i = 0; i < DIAMOND_BTN_COUNT; ++i) frame.buttons[i] = g_engine.buttons[i];
    frame.delta = delta;
    frame.time = now;
}

void update_stats(const diamond::IRenderBackend *backend, float frameMs) {
    DRenderStats stats = backend ? backend->stats() : DRenderStats{};
    stats.frame_ms = frameMs;
    stats.fps = static_cast<int>(g_loop.fpsAverage > 0.0f ? g_loop.fpsAverage : 0.0f);
    g_engine.frame_ms = frameMs;
    g_engine.fps = g_loop.fpsAverage;
    stats.width = g_engine.width;
    stats.height = g_engine.height;
    g_render_stats = stats;
}

bool attach_pending() {
    pthread_mutex_lock(&g_loop.lock);
    ANativeWindow *window = g_loop.pendingWindow;
    const int width = g_loop.pendingWidth;
    const int height = g_loop.pendingHeight;
    g_loop.pendingWindow = nullptr;
    g_loop.surfaceDirty = false;
    pthread_mutex_unlock(&g_loop.lock);

    if (!window) return false;
    diamond::renderer_close(g_loop.selection);
    g_loop.window = window;
    g_engine.width = width;
    g_engine.height = height;
    int mode = g_engine.render_mode;
    if (!diamond::renderer_open(g_loop.selection, mode, window, width, height)) {
        snprintf(g_loop.lastError, sizeof(g_loop.lastError), "%s", g_loop.selection.error);
        dlog_write(DLOG_WARN, "render", "mode %d failed: %s — falling back to OpenGL ES 3.0",
                   mode, g_loop.selection.error);
        // Automatic fallback keeps the editor usable instead of showing a black
        // viewport; the UI reports the switch in the log console.
        if (mode != DIAMOND_RENDER_GLES3) {
            if (diamond::renderer_open(g_loop.selection, DIAMOND_RENDER_GLES3, window, width, height)) {
                g_engine.render_mode = DIAMOND_RENDER_GLES3;
                dlog_write(DLOG_INFO, "render", "fallback to OpenGL ES 3.0 succeeded");
            }
        }
    }
    g_loop.attached = g_loop.selection.backend != nullptr;
    if (g_loop.attached) {
        g_loop.selection.backend->setDensity(g_engine.density);
        dlog_write(DLOG_INFO, "render", "surface %dx%d on %s", width, height,
                   diamond::renderer_mode_name(g_loop.selection.active));
    }
    return g_loop.attached;
}

void *render_thread(void *) {
    dlog_write(DLOG_INFO, "render", "render thread started");
    double previous = dtime_seconds();
    while (true) {
        pthread_mutex_lock(&g_loop.lock);
        const bool stop = g_loop.shouldStop;
        const bool dirty = g_loop.surfaceDirty;
        const bool paused = g_loop.paused;
        pthread_mutex_unlock(&g_loop.lock);
        if (stop) break;

        if (dirty) attach_pending();

        if (!g_loop.attached) {
            dtime_sleep_ms(16);
            previous = dtime_seconds();
            continue;
        }
        if (paused) {
            dtime_sleep_ms(32);
            previous = dtime_seconds();
            g_engine.fps = 0.0f;
            continue;
        }

        const double now = dtime_seconds();
        float delta = static_cast<float>(now - previous);
        if (delta > 0.1f) delta = 0.1f;
        previous = now;

        if (g_engine.play_state == DIAMOND_PLAYING) dlogic_update(delta);

        diamond::FrameContext frame;
        fill_frame(frame, delta, now);
        const double frameStart = dtime_seconds();
        const bool alive = g_loop.selection.backend->draw(frame);
        const float frameMs = static_cast<float>((dtime_seconds() - frameStart) * 1000.0);
        if (!alive) {
            dlog_write(DLOG_WARN, "render", "backend reported a lost surface");
            g_loop.attached = false;
            dtime_sleep_ms(16);
            continue;
        }
        update_stats(g_loop.selection.backend, frameMs);

        g_engine.frame_count++;
        g_loop.fpsFrames++;
        g_engine.last_frame_time = now;
        if (now - g_loop.fpsWindowStart >= 1.0) {
            const float measured = static_cast<float>(g_loop.fpsFrames) /
                                   static_cast<float>(now - g_loop.fpsWindowStart);
            g_loop.fpsAverage = g_loop.fpsAverage > 0.0f
                                    ? g_loop.fpsAverage * 0.6f + measured * 0.4f
                                    : measured;
            g_loop.fpsFrames = 0;
            g_loop.fpsWindowStart = now;
        }

        // Cap to ~60 fps on devices whose compositor is happy with it; letting a
        // 120 Hz panel run free just burns battery in an editor.
        const float target = 1.0f / 60.0f;
        const double elapsed = dtime_seconds() - now;
        if (elapsed < target) dtime_sleep_ms(static_cast<int>((target - elapsed) * 1000.0f));
    }
    dlog_write(DLOG_INFO, "render", "render thread stopped");
    return nullptr;
}

} // namespace

extern "C" {

DRenderStats g_render_stats;

int dwindow_attach(void *window, int width, int height);

int dwindow_start(void) {
    pthread_mutex_lock(&g_loop.lock);
    if (g_loop.threadRunning) {
        pthread_mutex_unlock(&g_loop.lock);
        return 1;
    }
    g_loop.shouldStop = false;
    pthread_mutex_unlock(&g_loop.lock);
    if (pthread_create(&g_loop.thread, nullptr, render_thread, nullptr) != 0) {
        dlog_write(DLOG_ERROR, "render", "cannot create the render thread");
        return 0;
    }
    pthread_mutex_lock(&g_loop.lock);
    g_loop.threadRunning = true;
    pthread_mutex_unlock(&g_loop.lock);
    return 1;
}

void dwindow_stop(void) {
    pthread_mutex_lock(&g_loop.lock);
    if (!g_loop.threadRunning) {
        pthread_mutex_unlock(&g_loop.lock);
        return;
    }
    g_loop.shouldStop = true;
    pthread_mutex_unlock(&g_loop.lock);
    pthread_join(g_loop.thread, nullptr);
    pthread_mutex_lock(&g_loop.lock);
    g_loop.threadRunning = false;
    pthread_mutex_unlock(&g_loop.lock);
    diamond::renderer_close(g_loop.selection);
    g_loop.attached = false;
}

int dwindow_attach(void *window, int width, int height) {
    if (!window) return 0;
    pthread_mutex_lock(&g_loop.lock);
    g_loop.pendingWindow = static_cast<ANativeWindow *>(window);
    g_loop.pendingWidth = width;
    g_loop.pendingHeight = height;
    g_loop.surfaceDirty = true;
    pthread_mutex_unlock(&g_loop.lock);
    if (!dwindow_start()) return 0;
    // Wait for the first attach so Java can report a synchronous failure.
    for (int i = 0; i < 200 && !g_loop.attached; ++i) dtime_sleep_ms(2);
    return g_loop.attached ? 1 : 0;
}

void dwindow_detach(void) {
    pthread_mutex_lock(&g_loop.lock);
    g_loop.pendingWindow = nullptr;
    g_loop.surfaceDirty = false;
    const bool hadBackend = g_loop.attached;
    g_loop.attached = false;
    pthread_mutex_unlock(&g_loop.lock);
    if (hadBackend) {
        // The backend must be destroyed on a thread where no GL work is queued;
        // the loop notices !attached and parks itself.
        diamond::renderer_close(g_loop.selection);
    }
    ANativeWindow_release(g_loop.window);
    g_loop.window = nullptr;
    g_engine.surface_attached = 0;
}

void dwindow_resize(int width, int height) {
    g_engine.width = width;
    g_engine.height = height;
    if (g_loop.selection.backend) g_loop.selection.backend->resize(width, height);
}

void dwindow_set_paused(int paused) {
    pthread_mutex_lock(&g_loop.lock);
    g_loop.paused = paused != 0;
    pthread_mutex_unlock(&g_loop.lock);
}

int dwindow_attached(void) { return g_loop.attached ? 1 : 0; }

const char *dwindow_last_error(void) { return g_loop.lastError; }

} // extern "C"
