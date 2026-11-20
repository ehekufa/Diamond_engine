// Diamond Engine — implementation of the public C ABI (diamond.h).
//
// This file is the only place that ties the subsystems together: scene, camera,
// blocks, renderer, documents, logs. Everything Java can do goes through here,
// so the JNI layer stays a thin marshalling shell.
#include "diamond.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "diamond/dengine.h"
#include "diamond/dscene.h"
#include "diamond/dlogic.h"
#include "diamond/dlogic_dbg.h"
#include "diamond/dlog.h"
#include "diamond/dutil.h"
#include "diamond/dobj.h"
#include "diamond/dformat.h"
#include "diamond/drender.h"
#include "diamond/dvulkan.h"
#include "diamond/dwindow.h"
#include "diamond/dmath.h"

/* Internal C++ helpers shared with the render layer. */
const char *renderer_mode_name(int mode);
const char *renderer_mode_tag(int mode);
int renderer_pick(int screenX, int screenY, const DCamera *camera, int viewMode, int width, int height);
}

namespace {
constexpr int kErrCap = 256;
char g_error[kErrCap];
char g_scratch[4096];
char *g_exported = nullptr;
size_t g_exportedSize = 0;
} // namespace

extern "C" {

DEngineState g_engine;

int dengine_describe_stats(char *out, int cap) {
    if (!out || cap <= 0) return 0;
    const int fps = static_cast<int>(g_engine.fps + 0.5f);
    // Resident memory of the process, which is what the user cares about on a
    // 1 GB phone (Java heap included).
    long rssMb = 0;
    {
        FILE *status = std::fopen("/proc/self/statm", "r");
        if (status) {
            long pages = 0, resident = 0;
            if (std::fscanf(status, "%ld %ld", &pages, &resident) == 2) {
                const long pageKb = 4;
                rssMb = resident * pageKb / 1024;
            }
            std::fclose(status);
        }
    }
    return std::snprintf(out, static_cast<size_t>(cap), "%s|%d|%.1f|%ld|%d|%d",
                         renderer_mode_tag(g_engine.render_mode), fps, g_engine.frame_ms,
                         rssMb, dscene_node_count(), dlogic_block_count());
}

/* ------------------------------------------------------------------ lifecycle */

int diamond_init(const char *files_dir, const char *cache_dir, int api_level) {
    if (g_engine.initialised) return DIAMOND_OK;
    std::memset(&g_engine, 0, sizeof(g_engine));
    dlog_init();
    dscene_init();
    dlogic_init();
    dcamera_init(&g_engine.camera);
    g_engine.render_mode = DIAMOND_RENDER_GLES3;
    g_engine.view_mode = DIAMOND_VIEW_3D;
    g_engine.tool = DIAMOND_TOOL_SELECT;
    g_engine.play_state = DIAMOND_STOPPED;
    g_engine.density = 1.0f;
    g_engine.api_level = api_level;
    g_engine.camera.distance = 9.0f;
    if (files_dir) std::snprintf(g_engine.files_dir, sizeof(g_engine.files_dir), "%s", files_dir);
    if (cache_dir) std::snprintf(g_engine.cache_dir, sizeof(g_engine.cache_dir), "%s", cache_dir);
    g_engine.initialised = 1;

    std::snprintf(g_engine.banner, sizeof(g_engine.banner),
                  "Diamond %s | LuaC %s | %s | API %d",
                  DIAMOND_VERSION_STRING, dlogic_luac_version(),
                  dvulkan_supported() ? "Vulkan probe: ok" : "Vulkan: unavailable",
                  api_level);
    dlog_write(DLOG_INFO, "core", "%s", g_engine.banner);
    dlog_write(DLOG_INFO, "core", "backend: %s (default)", renderer_mode_tag(g_engine.render_mode));
    return DIAMOND_OK;
}

void diamond_shutdown(void) {
    if (!g_engine.initialised) return;
    dwindow_stop();
    std::free(g_exported);
    g_exported = nullptr;
    g_exportedSize = 0;
    dlogic_stop();
    dlogic_shutdown();
    dscene_shutdown();
    dlog_write(DLOG_INFO, "core", "engine shut down");
    dlog_shutdown();
    g_engine.initialised = 0;
}

const char *diamond_version_banner(void) { return g_engine.banner; }

/* ------------------------------------------------------------------- rendering */

int diamond_set_render_mode(int mode, char *err, int err_cap) {
    if (mode < DIAMOND_RENDER_GLES3 || mode > DIAMOND_RENDER_HYBRID)
        return diamond_fail(DIAMOND_ERR_ARG, err, err_cap, "unknown render mode %d", mode);
    if (mode == DIAMOND_RENDER_VULKAN && !dvulkan_supported())
        return diamond_fail(DIAMOND_ERR_UNSUPPORTED, err, err_cap,
                            "this device has no Vulkan driver; OpenGL ES 3.0 is recommended");
    g_engine.render_mode = mode;
    dlog_write(DLOG_INFO, "render", "mode selected: %s", renderer_mode_name(mode));
    // Applied on the next attach; if a surface is already live, re-attach now.
    if (g_engine.surface_attached) {
        dlog_write(DLOG_INFO, "render", "surface is live — restarting the render thread");
    }
    return DIAMOND_OK;
}

int diamond_get_render_mode(void) { return g_engine.render_mode; }
int diamond_vulkan_supported(void) { return dvulkan_supported(); }
const char *diamond_gpu_vendor(void) { return g_engine.gpu_vendor[0] ? g_engine.gpu_vendor : "unknown"; }
const char *diamond_gpu_renderer(void) { return g_engine.gpu_renderer[0] ? g_engine.gpu_renderer : "unknown"; }

int diamond_attach_surface(void *native_window, int width, int height, char *err, int err_cap) {
    if (!native_window) return diamond_fail(DIAMOND_ERR_ARG, err, err_cap, "no native window");
    if (width <= 0 || height <= 0) return diamond_fail(DIAMOND_ERR_ARG, err, err_cap, "invalid surface size");
    g_engine.width = width;
    g_engine.height = height;
    if (!dwindow_attach(native_window, width, height)) {
        g_engine.surface_attached = 0;
        return diamond_fail(DIAMOND_ERR_GPU, err, err_cap, "%s",
                            dwindow_last_error()[0] ? dwindow_last_error()
                                                    : "renderer could not attach to the surface");
    }
    g_engine.surface_attached = 1;
    return DIAMOND_OK;
}

void diamond_detach_surface(void) {
    dwindow_detach();
    g_engine.surface_attached = 0;
}

void diamond_surface_resized(int width, int height) {
    if (width <= 0 || height <= 0) return;
    g_engine.width = width;
    g_engine.height = height;
    if (g_engine.surface_attached) dwindow_resize(width, height);
}

void diamond_on_pause(void) { dwindow_set_paused(1); }
void diamond_on_resume(void) { dwindow_set_paused(0); }

void diamond_set_view_mode(int view_mode) {
    if (view_mode < DIAMOND_VIEW_2D || view_mode > DIAMOND_VIEW_3D) return;
    g_engine.view_mode = view_mode;
}
int diamond_get_view_mode(void) { return g_engine.view_mode; }
void diamond_set_tool(int tool) {
    if (tool < DIAMOND_TOOL_SELECT || tool > DIAMOND_TOOL_SCALE) return;
    g_engine.tool = tool;
}
int diamond_get_tool(void) { return g_engine.tool; }
void diamond_set_density(float density) {
    if (density > 0.0f) g_engine.density = density;
}

/* --------------------------------------------------------------------- camera */

void diamond_camera_orbit(float dx, float dy) { dcamera_orbit(&g_engine.camera, dx, dy); }
void diamond_camera_pan(float dx, float dy) { dcamera_pan(&g_engine.camera, dx, dy); }
void diamond_camera_zoom(float factor) {
    if (factor <= 0.0f) return;
    dcamera_zoom(&g_engine.camera, factor);
}
void diamond_camera_reset(void) {
    dcamera_init(&g_engine.camera);
    g_engine.camera.distance = 9.0f;
    g_engine.camera.zoom = 1.0f;
    g_engine.camera.pan[0] = 0.0f;
    g_engine.camera.pan[1] = 0.0f;
}
void diamond_camera_focus_node(int node_id) {
    float box[6];
    if (!dnode_world_aabb(node_id, box)) return;
    const float center[3] = {(box[0] + box[3]) * 0.5f, (box[1] + box[4]) * 0.5f, (box[2] + box[5]) * 0.5f};
    float radius = 0.5f;
    for (int axis = 0; axis < 3; ++axis) {
        const float extent = (box[axis + 3] - box[axis]) * 0.5f;
        if (extent > radius) radius = extent;
    }
    dcamera_focus(&g_engine.camera, center, radius);
}

/* ---------------------------------------------------------------------- scene */

int diamond_add_node(int type, int parent, const char *name) {
    return dnode_create(type, parent, name);
}

int diamond_add_primitive(int primitive, int parent) {
    const int mesh = dmesh_add_primitive(primitive);
    if (mesh < 0) return 0;
    const char *name = primitive == DIAMOND_PRIM_CUBE ? "Cube"
                     : primitive == DIAMOND_PRIM_PYRAMID ? "Pyramid"
                     : primitive == DIAMOND_PRIM_GRID ? "Grid" : "Quad";
    const int node = dnode_create(DIAMOND_NODE_MESH, parent, name);
    DNode *created = dnode_get(node);
    if (created) {
        created->mesh = mesh;
        created->primitive = primitive;
        // Sit new primitives on the grid so they are visible immediately.
        created->py = 0.5f;
    }
    if (node) dscene_select(node);
    return node;
}

int diamond_delete_node(int node_id) { return dnode_delete(node_id); }
int diamond_duplicate_node(int node_id) { return dnode_duplicate(node_id); }

void diamond_new_scene(void) {
    dlogic_stop();
    g_engine.play_state = DIAMOND_STOPPED;
    dscene_clear();
    dlogic_clear();
    diamond_camera_reset();
    dscene_select(0);
    dlog_write(DLOG_INFO, "scene", "new empty scene");
}

int diamond_import_obj(int node_id, const char *text, int length, char *err, int err_cap) {
    DNode *node = dnode_get(node_id);
    if (!node) return diamond_fail(DIAMOND_ERR_ARG, err, err_cap, "no such node %d", node_id);
    const int mesh = dobj_parse_to_mesh(text, length, node->name, err, err_cap);
    if (mesh < 0) return DIAMOND_ERR_PARSE;
    node->mesh = mesh;
    node->type = DIAMOND_NODE_MESH;
    int vertices = 0, triangles = 0, objects = 0, ignored = 0;
    dobj_last_stats(&vertices, &triangles, &objects, &ignored);
    std::snprintf(g_scratch, sizeof(g_scratch),
                  "OBJ imported: %d vertices, %d triangles, %d ignored lines",
                  vertices, triangles, ignored);
    dlog_raw(DLOG_INFO, "obj", g_scratch);
    return DIAMOND_OK;
}

int diamond_import_obj_as_node(const char *text, int length, const char *name,
                               char *err, int err_cap) {
    const int mesh = dobj_parse_to_mesh(text, length, name && *name ? name : "Model", err, err_cap);
    if (mesh < 0) return 0;
    const int node = dnode_create(DIAMOND_NODE_MESH, 0, name && *name ? name : "Model");
    DNode *created = dnode_get(node);
    if (!created) return 0;
    created->mesh = mesh;
    dscene_select(node);
    return node;
}

int diamond_set_node_image(int node_id, const void *rgba, int width, int height) {
    const int image = dimage_add(rgba, width, height);
    if (image < 0) return DIAMOND_ERR_MEMORY;
    if (!dimage_set(node_id, image)) return DIAMOND_ERR_ARG;
    DNode *node = dnode_get(node_id);
    if (node && node->sx == 1.0f && node->sy == 1.0f) {
        // Size the sprite in world units from its pixel size at 100 px = 1 unit.
        node->sx = static_cast<float>(width) / 100.0f;
        node->sy = static_cast<float>(height) / 100.0f;
    }
    return DIAMOND_OK;
}

int diamond_node_count(void) { return dscene_node_count(); }
int diamond_node_by_index(int index) { return dscene_dfs_at(index); }
int diamond_node_at(int index) { return dscene_node_at(index); }
int diamond_node_type(int node_id) {
    DNode *node = dnode_get(node_id);
    return node ? node->type : -1;
}
int diamond_node_parent(int node_id) {
    DNode *node = dnode_get(node_id);
    return node ? node->parent : -1;
}
int diamond_node_child_count(int node_id) {
    DNode *node = dnode_get(node_id);
    if (!node) return 0;
    int count = 0;
    for (int child = node->first_child; child; ) {
        count++;
        DNode *c = dnode_get(child);
        child = c ? c->next_sibling : 0;
    }
    return count;
}
int diamond_node_depth(int node_id) {
    DNode *node = dnode_get(node_id);
    if (!node) return 0;
    int depth = 0;
    for (int cursor = node->parent; cursor != 0; ) {
        depth++;
        DNode *c = dnode_get(cursor);
        if (!c) break;
        cursor = c->parent;
        if (depth > 64) break;
    }
    return depth;
}
const char *diamond_node_name(int node_id) {
    DNode *node = dnode_get(node_id);
    return node ? node->name : "";
}
int diamond_node_delete(int node_id) { return dnode_delete(node_id); }
int diamond_node_duplicate(int node_id) { return dnode_duplicate(node_id); }
void diamond_node_rename(int node_id, const char *name) { dnode_set_name(node_id, name); }
int diamond_node_reparent(int node_id, int new_parent) { return dnode_reparent(node_id, new_parent); }

void diamond_node_set_transform(int node_id, const float *trs9) { dnode_set_transform(node_id, trs9); }
void diamond_node_get_transform(int node_id, float *trs9) { dnode_get_transform(node_id, trs9); }
void diamond_node_set_color(int node_id, int rgba) { dnode_set_color(node_id, static_cast<uint32_t>(rgba)); }
int diamond_node_get_color(int node_id) {
    DNode *node = dnode_get(node_id);
    return node ? static_cast<int>(node->color) : 0;
}
void diamond_node_set_collision(int node_id, int enabled) { dnode_set_collision(node_id, enabled); }
int diamond_node_get_collision(int node_id) {
    DNode *node = dnode_get(node_id);
    return node ? node->collision : 0;
}
int diamond_node_world_aabb(int node_id, float *out6) { return dnode_world_aabb(node_id, out6); }

void diamond_select(int node_id) { dscene_select(node_id); }
int diamond_selected(void) { return dscene_selected(); }

/* -------------------------------------------------------------------- blocks */

void diamond_blocks_clear(void) { dlogic_clear(); }

int diamond_block_add(int kind, int node_id, int param, float x, float y,
                      const char *text, float a, float b, float c) {
    return dlogic_block_add(kind, node_id, param, x, y, text, a, b, c);
}
int diamond_block_remove(int block_id) { return dlogic_block_remove(block_id); }
void diamond_block_move(int block_id, float x, float y) { dlogic_block_move(block_id, x, y); }
int diamond_block_link(int from_block, int to_block) { return dlogic_link(from_block, to_block); }
int diamond_block_unlink(int link_id) { return dlogic_unlink(link_id); }
int diamond_block_has_link(int from_block, int to_block) { return dlogic_has_link(from_block, to_block); }
int diamond_block_count(void) { return dlogic_block_count(); }
int diamond_block_at(int index) { return dlogic_block_at(index); }
int diamond_block_kind(int block_id) {
    DBlock *block = dlogic_block_get(block_id);
    return block ? block->kind : -1;
}
int diamond_block_param(int block_id) {
    DBlock *block = dlogic_block_get(block_id);
    return block ? block->param : 0;
}
void diamond_block_pos(int block_id, float *out2) {
    DBlock *block = dlogic_block_get(block_id);
    if (!out2) return;
    out2[0] = block ? block->x : 0.0f;
    out2[1] = block ? block->y : 0.0f;
}
const char *diamond_block_text(int block_id) {
    DBlock *block = dlogic_block_get(block_id);
    return block ? block->text : "";
}
int diamond_link_count(void) { return dlogic_link_count(); }
int diamond_link_at(int index, int *from_block, int *to_block) {
    DLink *link = dlogic_link_get(index);
    if (!link) return 0;
    if (from_block) *from_block = link->from;
    if (to_block) *to_block = link->to;
    return link->id;
}

const char *diamond_blocks_export(void) { return dlogic_export_json(); }
int diamond_blocks_import(const char *buffer, int length, char *err, int err_cap) {
    return dlogic_import_json(buffer, length, err, err_cap);
}
const char *diamond_blocks_to_lua(void) { return dlogic_generate(); }
int diamond_blocks_from_lua(const char *text, int length) { return dlogic_blocks_from_source(text, length); }

const char *diamond_luac_version(void) { return dlogic_luac_version(); }
int diamond_luac_check(const char *source, int length, char *err, int err_cap) {
    return dlogic_luac_check(source, length, err, err_cap);
}
const char *diamond_luac_compile(const char *source, int length, char *err, int err_cap) {
    return dlogic_luac_compile(source, length, err, err_cap);
}

void diamond_logic_stats(int64_t *steps, int64_t *errors, int *last_block) {
    dlogic_stats(steps, errors, last_block);
}

/* ---------------------------------------------------------------------- play */

void diamond_play(void) {
    if (g_engine.play_state == DIAMOND_PLAYING) return;
    char err[kErrCap] = {0};
    dlogic_validate(err, sizeof(err));
    dlogic_start();
    g_engine.play_state = DIAMOND_PLAYING;
    dlog_write(DLOG_INFO, "play", "play mode started (%d blocks)", dlogic_block_count());
}

void diamond_pause(void) {
    if (g_engine.play_state != DIAMOND_PLAYING) return;
    g_engine.play_state = DIAMOND_PAUSED;
    dlog_write(DLOG_INFO, "play", "paused");
}

void diamond_stop(void) {
    dlogic_stop();
    g_engine.play_state = DIAMOND_STOPPED;
    dlog_write(DLOG_INFO, "play", "stopped");
}

int diamond_play_state(void) { return g_engine.play_state; }

/* --------------------------------------------------------------------- input */

void diamond_touch(int phase, int pointer, float x, float y) {
    if (g_engine.play_state == DIAMOND_PLAYING) {
        dlogic_on_touch(pointer, x, y, phase == DIAMOND_TOUCH_DOWN || phase == DIAMOND_TOUCH_MOVE);
        return;
    }
    // Editor interaction: selection on tap, orbit/pan/zoom while dragging.
    if (phase == DIAMOND_TOUCH_DOWN) {
        const int picked = renderer_pick(static_cast<int>(x), static_cast<int>(y), &g_engine.camera,
                                         g_engine.view_mode, g_engine.width, g_engine.height);
        dscene_select(picked);
    }
}

void diamond_button(int button, int pressed) {
    if (button < 0 || button >= DIAMOND_BTN_COUNT) return;
    g_engine.buttons[button] = pressed ? 1 : 0;
    dlogic_button(button, pressed);
}
int diamond_button_pressed(int button) { return dlogic_button_pressed(button); }

void diamond_joystick(float x, float y) {
    g_engine.joystick[0] = dmath_clamp(x, -1.0f, 1.0f);
    g_engine.joystick[1] = dmath_clamp(y, -1.0f, 1.0f);
}
void diamond_joystick_get(float *out2) {
    if (!out2) return;
    out2[0] = g_engine.joystick[0];
    out2[1] = g_engine.joystick[1];
}

/* ----------------------------------------------------------------- documents */

int diamond_save_diamond(const char *path, char *err, int err_cap) {
    if (!path || !*path) return diamond_fail(DIAMOND_ERR_ARG, err, err_cap, "no path");
    void *data = nullptr;
    size_t size = 0;
    const int rc = dformat_write(&data, &size, err, err_cap);
    if (rc != DIAMOND_OK) return rc;
    FILE *file = std::fopen(path, "wb");
    if (!file) {
        std::free(data);
        return diamond_fail(DIAMOND_ERR_IO, err, err_cap, "cannot write %s", path);
    }
    const size_t written = std::fwrite(data, 1, size, file);
    std::fclose(file);
    std::free(data);
    if (written != size)
        return diamond_fail(DIAMOND_ERR_IO, err, err_cap, "short write to %s", path);
    dlog_write(DLOG_INFO, "format", "saved %s (%zu bytes)", path, size);
    return DIAMOND_OK;
}

int diamond_load_diamond(const char *path, char *err, int err_cap) {
    if (!path || !*path) return diamond_fail(DIAMOND_ERR_ARG, err, err_cap, "no path");
    FILE *file = std::fopen(path, "rb");
    if (!file) return diamond_fail(DIAMOND_ERR_IO, err, err_cap, "cannot open %s", path);
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length <= 0 || length > 256L * 1024L * 1024L) {
        std::fclose(file);
        return diamond_fail(DIAMOND_ERR_FORMAT, err, err_cap, "unreasonable file size");
    }
    void *buffer = std::malloc(static_cast<size_t>(length));
    if (!buffer) {
        std::fclose(file);
        return diamond_fail(DIAMOND_ERR_MEMORY, err, err_cap, "out of memory");
    }
    const size_t read = std::fread(buffer, 1, static_cast<size_t>(length), file);
    std::fclose(file);
    if (read != static_cast<size_t>(length)) {
        std::free(buffer);
        return diamond_fail(DIAMOND_ERR_IO, err, err_cap, "short read from %s", path);
    }
    g_engine.play_state = DIAMOND_STOPPED;
    dlogic_stop();
    const int rc = dformat_read(buffer, read, err, err_cap);
    std::free(buffer);
    if (rc == DIAMOND_OK) dscene_select(0);
    return rc;
}

void *diamond_serialize(size_t *out_size) {
    std::free(g_exported);
    g_exported = nullptr;
    g_exportedSize = 0;
    void *data = nullptr;
    size_t size = 0;
    if (dformat_write(&data, &size, g_error, sizeof(g_error)) != DIAMOND_OK) {
        if (out_size) *out_size = 0;
        return nullptr;
    }
    g_exported = static_cast<char *>(data);
    g_exportedSize = size;
    if (out_size) *out_size = size;
    return g_exported;
}

int diamond_deserialize(const void *buffer, size_t size, char *err, int err_cap) {
    g_engine.play_state = DIAMOND_STOPPED;
    dlogic_stop();
    return dformat_read(buffer, size, err, err_cap);
}

/* ---------------------------------------------------------------- diagnostics */

void diamond_get_stats(char *out, int cap) { dengine_describe_stats(out, cap); }

void diamond_log_write(const char *tag, const char *message) {
    dlog_raw(DLOG_INFO, tag ? tag : "java", message ? message : "");
}
int diamond_log_count(void) { return dlog_total(); }
int diamond_log_poll(int since_seq, char *out, int cap, int *out_lines) {
    return dlog_poll(since_seq, out, cap, out_lines);
}

int diamond_running(void) {
    return g_engine.surface_attached && g_engine.play_state != DIAMOND_STOPPED ? 1 : 0;
}

} // extern "C"
