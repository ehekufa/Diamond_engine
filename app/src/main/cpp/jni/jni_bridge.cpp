// Diamond Engine — JNI bridge.
//
// Java never touches engine internals: every entry point below is a thin
// marshalling shell over the C ABI in diamond.h. Strings are copied into
// caller-owned buffers so the engine only ever sees UTF-8 C strings.
#include <jni.h>

#include <android/bitmap.h>
#include <android/native_window_jni.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "diamond.h"
#include "diamond/dfiles.h"
#include <android/asset_manager_jni.h>

namespace {

char g_lastError[256];

void set_error(const char *text) {
    std::snprintf(g_lastError, sizeof(g_lastError), "%s", text ? text : "");
}

/* Copies a Java string into the engine, returning a thread_local buffer that
 * lives until the next call on the same thread. Avoids malloc per keystroke in
 * the block editor, which is where most of these come from. */
const char *from_java(JNIEnv *env, jstring value) {
    static thread_local char buffer[8192];
    if (!value) { buffer[0] = '\0'; return buffer; }
    const jsize length = env->GetStringLength(value);
    const jsize copy = length < static_cast<jsize>(sizeof(buffer) - 1)
                           ? length : static_cast<jsize>(sizeof(buffer) - 1);
    env->GetStringUTFRegion(value, 0, copy, buffer);
    buffer[copy] = '\0';
    return buffer;
}

jstring to_java(JNIEnv *env, const char *text) {
    return env->NewStringUTF(text ? text : "");
}

jfloatArray floats(JNIEnv *env, const float *values, int count) {
    jfloatArray array = env->NewFloatArray(count);
    if (array) env->SetFloatArrayRegion(array, 0, count, values);
    return array;
}

bool read_floats(JNIEnv *env, jfloatArray array, float *out, int count) {
    if (!array || env->GetArrayLength(array) < count) return false;
    env->GetFloatArrayRegion(array, 0, count, out);
    return true;
}

} // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_init(JNIEnv *env, jclass,
                                              jstring filesDir, jstring cacheDir,
                                              jint apiLevel) {
    set_error("");
    const int rc = diamond_init(from_java(env, filesDir), from_java(env, cacheDir), apiLevel);
    if (rc != DIAMOND_OK) set_error("engine initialisation failed");
    return rc;
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_shutdown(JNIEnv *, jclass) {
    diamond_shutdown();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_versionBanner(JNIEnv *env, jclass) {
    return to_java(env, diamond_version_banner());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_lastError(JNIEnv *env, jclass) {
    return to_java(env, g_lastError);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_diamond_engine_jni_NativeBridge_installAssets(JNIEnv *env, jclass, jobject assets) {
    if (assets) {
        AAssetManager *manager = AAssetManager_fromJava(env, assets);
        // Returns once; a second call is ignored by dfiles_install_assets().
        if (manager) return dfiles_install_assets(manager) ? JNI_TRUE : JNI_FALSE;
    }
    return JNI_FALSE;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_diamond_engine_jni_NativeBridge_readAsset(JNIEnv *env, jclass, jstring path) {
    int size = 0;
    const char *name = from_java(env, path);
    char copy[512];
    std::snprintf(copy, sizeof(copy), "%s", name);
    void *data = dfiles_read_asset(copy, &size);
    if (!data) return nullptr;
    jbyteArray array = env->NewByteArray(size);
    if (array) env->SetByteArrayRegion(array, 0, size, static_cast<const jbyte *>(data));
    std::free(data);
    return array;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_diamond_engine_jni_NativeBridge_readFile(JNIEnv *env, jclass, jstring path) {
    int size = 0;
    const char *name = from_java(env, path);
    char copy[512];
    std::snprintf(copy, sizeof(copy), "%s", name);
    void *data = dfiles_read_file(copy, &size);
    if (!data) return nullptr;
    jbyteArray array = env->NewByteArray(size);
    if (array) env->SetByteArrayRegion(array, 0, size, static_cast<const jbyte *>(data));
    std::free(data);
    return array;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_diamond_engine_jni_NativeBridge_writeFile(JNIEnv *env, jclass, jstring path,
                                                   jbyteArray data) {
    if (!data) return JNI_FALSE;
    char copy[512];
    std::snprintf(copy, sizeof(copy), "%s", from_java(env, path));
    const jsize size = env->GetArrayLength(data);
    jbyte *bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) return JNI_FALSE;
    const int ok = dfiles_write_file(copy, bytes, size);
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    return ok ? JNI_TRUE : JNI_FALSE;
}

/* ---------------------------------------------------------------- rendering */

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_setRenderMode(JNIEnv *, jclass, jint mode) {
    char error[256] = {0};
    const int rc = diamond_set_render_mode(mode, error, sizeof(error));
    set_error(error);
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_getRenderMode(JNIEnv *, jclass) {
    return diamond_get_render_mode();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_diamond_engine_jni_NativeBridge_vulkanSupported(JNIEnv *, jclass) {
    return diamond_vulkan_supported() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_gpuVendor(JNIEnv *env, jclass) {
    return to_java(env, diamond_gpu_vendor());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_gpuRenderer(JNIEnv *env, jclass) {
    return to_java(env, diamond_gpu_renderer());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_diamond_engine_jni_NativeBridge_attachSurface(JNIEnv *env, jclass, jobject surface) {
    if (!surface) {
        set_error("no surface");
        return JNI_FALSE;
    }
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        set_error("could not obtain the native window");
        return JNI_FALSE;
    }
    const int width = ANativeWindow_getWidth(window);
    const int height = ANativeWindow_getHeight(window);
    char error[256] = {0};
    const int rc = diamond_attach_surface(window, width, height, error, sizeof(error));
    if (rc != DIAMOND_OK) {
        // The engine keeps its own reference only on success.
        ANativeWindow_release(window);
        set_error(error);
        return JNI_FALSE;
    }
    set_error("");
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_detachSurface(JNIEnv *, jclass) {
    diamond_detach_surface();
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_surfaceResized(JNIEnv *, jclass, jint width, jint height) {
    diamond_surface_resized(width, height);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_onPause(JNIEnv *, jclass) { diamond_on_pause(); }

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_onResume(JNIEnv *, jclass) { diamond_on_resume(); }

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_setViewMode(JNIEnv *, jclass, jint mode) {
    diamond_set_view_mode(mode);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_getViewMode(JNIEnv *, jclass) {
    return diamond_get_view_mode();
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_setTool(JNIEnv *, jclass, jint tool) {
    diamond_set_tool(tool);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_getTool(JNIEnv *, jclass) {
    return diamond_get_tool();
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_setDensity(JNIEnv *, jclass, jfloat density) {
    diamond_set_density(density);
}

/* ------------------------------------------------------------------- camera */

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_cameraOrbit(JNIEnv *, jclass, jfloat dx, jfloat dy) {
    diamond_camera_orbit(dx, dy);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_cameraPan(JNIEnv *, jclass, jfloat dx, jfloat dy) {
    diamond_camera_pan(dx, dy);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_cameraZoom(JNIEnv *, jclass, jfloat factor) {
    diamond_camera_zoom(factor);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_cameraReset(JNIEnv *, jclass) { diamond_camera_reset(); }

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_cameraFocusNode(JNIEnv *, jclass, jint node) {
    diamond_camera_focus_node(node);
}

/* -------------------------------------------------------------------- scene */

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_addPrimitive(JNIEnv *, jclass, jint primitive) {
    return diamond_add_primitive(primitive, 0);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_addNode(JNIEnv *env, jclass, jint type,
                                                 jint parent, jstring name) {
    return diamond_add_node(type, parent, from_java(env, name));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_deleteNode(JNIEnv *, jclass, jint node) {
    return diamond_delete_node(node);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_duplicateNode(JNIEnv *, jclass, jint node) {
    return diamond_duplicate_node(node);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_newScene(JNIEnv *, jclass) { diamond_new_scene(); }

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_importObjText(JNIEnv *env, jclass, jint node,
                                                       jstring text) {
    char error[256] = {0};
    const char *source = from_java(env, text);
    const int rc = diamond_import_obj(node, source, static_cast<int>(std::strlen(source)),
                                      error, sizeof(error));
    set_error(error);
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_importObjAsNode(JNIEnv *env, jclass,
                                                         jstring text, jstring name) {
    char error[256] = {0};
    const char *source = from_java(env, text);
    // The name must be copied first: from_java() reuses one buffer per thread.
    char modelName[128];
    std::snprintf(modelName, sizeof(modelName), "%s", from_java(env, name));
    const int node = diamond_import_obj_as_node(source, static_cast<int>(std::strlen(source)),
                                                modelName, error, sizeof(error));
    set_error(error);
    return node;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_setNodeImage(JNIEnv *env, jclass, jint node,
                                                      jobject bitmap) {
    if (!bitmap) {
        set_error("no bitmap");
        return DIAMOND_ERR_ARG;
    }
    AndroidBitmapInfo info;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS) {
        set_error("could not read the bitmap header");
        return DIAMOND_ERR_ARG;
    }
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) {
        set_error("sprites must be ARGB_8888 bitmaps");
        return DIAMOND_ERR_ARG;
    }
    void *pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) {
        set_error("could not lock the bitmap pixels");
        return DIAMOND_ERR_ARG;
    }
    // Android pads rows to the stride, so repack into a tight buffer.
    const int width = static_cast<int>(info.width);
    const int height = static_cast<int>(info.height);
    unsigned char *tight = static_cast<unsigned char *>(
        std::malloc(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u));
    if (!tight) {
        AndroidBitmap_unlockPixels(env, bitmap);
        set_error("out of memory");
        return DIAMOND_ERR_MEMORY;
    }
    for (int y = 0; y < height; ++y) {
        std::memcpy(tight + static_cast<size_t>(y) * static_cast<size_t>(width) * 4u,
                    static_cast<const unsigned char *>(pixels) + static_cast<size_t>(y) * info.stride,
                    static_cast<size_t>(width) * 4u);
    }
    AndroidBitmap_unlockPixels(env, bitmap);
    const int rc = diamond_set_node_image(node, tight, width, height);
    std::free(tight);
    set_error(rc == DIAMOND_OK ? "" : "could not store the sprite");
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeCount(JNIEnv *, jclass) {
    return diamond_node_count();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeAt(JNIEnv *, jclass, jint index) {
    return diamond_node_at(index);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeType(JNIEnv *, jclass, jint node) {
    return diamond_node_type(node);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeParent(JNIEnv *, jclass, jint node) {
    return diamond_node_parent(node);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeChildCount(JNIEnv *, jclass, jint node) {
    return diamond_node_child_count(node);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeDepth(JNIEnv *, jclass, jint node) {
    return diamond_node_depth(node);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeName(JNIEnv *env, jclass, jint node) {
    return to_java(env, diamond_node_name(node));
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeRename(JNIEnv *env, jclass, jint node, jstring name) {
    diamond_node_rename(node, from_java(env, name));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeReparent(JNIEnv *, jclass, jint node, jint parent) {
    return diamond_node_reparent(node, parent);
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeTransform(JNIEnv *env, jclass, jint node) {
    float trs[9] = {0, 0, 0, 0, 0, 0, 1, 1, 1};
    diamond_node_get_transform(node, trs);
    return floats(env, trs, 9);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeSetTransform(JNIEnv *env, jclass, jint node,
                                                          jfloatArray trs) {
    float values[9];
    if (read_floats(env, trs, values, 9)) diamond_node_set_transform(node, values);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeSetColor(JNIEnv *, jclass, jint node, jint rgba) {
    diamond_node_set_color(node, rgba);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeColor(JNIEnv *, jclass, jint node) {
    return diamond_node_get_color(node);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeSetCollision(JNIEnv *, jclass, jint node,
                                                          jboolean enabled) {
    diamond_node_set_collision(node, enabled == JNI_TRUE ? 1 : 0);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeCollision(JNIEnv *, jclass, jint node) {
    return diamond_node_get_collision(node) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_diamond_engine_jni_NativeBridge_nodeAabb(JNIEnv *env, jclass, jint node) {
    float box[6];
    if (!diamond_node_world_aabb(node, box)) return nullptr;
    return floats(env, box, 6);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_select(JNIEnv *, jclass, jint node) {
    diamond_select(node);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_selected(JNIEnv *, jclass) {
    return diamond_selected();
}

/* ------------------------------------------------------------------- blocks */

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_blocksClear(JNIEnv *, jclass) {
    diamond_blocks_clear();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockAdd(JNIEnv *env, jclass, jint kind, jint node,
                                                  jint param, jfloat x, jfloat y, jstring text,
                                                  jfloat a, jfloat b, jfloat c) {
    return diamond_block_add(kind, node, param, x, y, from_java(env, text), a, b, c);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockRemove(JNIEnv *, jclass, jint block) {
    return diamond_block_remove(block);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockMove(JNIEnv *, jclass, jint block,
                                                   jfloat x, jfloat y) {
    diamond_block_move(block, x, y);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockLink(JNIEnv *, jclass, jint from, jint to) {
    return diamond_block_link(from, to);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockUnlink(JNIEnv *, jclass, jint link) {
    return diamond_block_unlink(link);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockCount(JNIEnv *, jclass) {
    return diamond_block_count();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockAt(JNIEnv *, jclass, jint index) {
    return diamond_block_at(index);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockKind(JNIEnv *, jclass, jint block) {
    return diamond_block_kind(block);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockParam(JNIEnv *, jclass, jint block) {
    return diamond_block_param(block);
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockPos(JNIEnv *env, jclass, jint block) {
    float position[2] = {0, 0};
    diamond_block_pos(block, position);
    return floats(env, position, 2);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_blockText(JNIEnv *env, jclass, jint block) {
    return to_java(env, diamond_block_text(block));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_linkCount(JNIEnv *, jclass) {
    return diamond_link_count();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_linkAt(JNIEnv *env, jclass, jint index, jintArray out) {
    int from = 0, to = 0;
    const int id = diamond_link_at(index, &from, &to);
    if (out && env->GetArrayLength(out) >= 2) {
        const jint pair[2] = {from, to};
        env->SetIntArrayRegion(out, 0, 2, pair);
    }
    return id;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_blocksExport(JNIEnv *env, jclass) {
    return to_java(env, diamond_blocks_export());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_blocksImport(JNIEnv *env, jclass, jstring json) {
    char error[256] = {0};
    const char *source = from_java(env, json);
    const int rc = diamond_blocks_import(source, static_cast<int>(std::strlen(source)),
                                         error, sizeof(error));
    set_error(error);
    return rc;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_blocksToLua(JNIEnv *env, jclass) {
    return to_java(env, diamond_blocks_to_lua());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_blocksFromLua(JNIEnv *env, jclass, jstring source) {
    const char *text = from_java(env, source);
    return diamond_blocks_from_lua(text, static_cast<int>(std::strlen(text)));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_luacVersion(JNIEnv *env, jclass) {
    return to_java(env, diamond_luac_version());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_luacCheck(JNIEnv *env, jclass, jstring source) {
    char error[512] = {0};
    const char *text = from_java(env, source);
    const int rc = diamond_luac_check(text, static_cast<int>(std::strlen(text)),
                                      error, sizeof(error));
    if (rc == DIAMOND_OK) return nullptr;
    return env->NewStringUTF(error[0] ? error : "syntax error");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_luacCompile(JNIEnv *env, jclass, jstring source) {
    char error[512] = {0};
    const char *text = from_java(env, source);
    const char *generated = diamond_luac_compile(text, static_cast<int>(std::strlen(text)),
                                                 error, sizeof(error));
    if (!generated) {
        set_error(error[0] ? error : "LuaC is not available in this build");
        return nullptr;
    }
    return to_java(env, generated);
}

/* --------------------------------------------------------------------- play */

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_play(JNIEnv *, jclass) { diamond_play(); }

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_pause(JNIEnv *, jclass) { diamond_pause(); }

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_stop(JNIEnv *, jclass) { diamond_stop(); }

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_playState(JNIEnv *, jclass) {
    return diamond_play_state();
}

/* -------------------------------------------------------------------- input */

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_touch(JNIEnv *, jclass, jint phase, jint pointer,
                                               jfloat x, jfloat y) {
    diamond_touch(phase, pointer, x, y);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_button(JNIEnv *, jclass, jint button, jboolean pressed) {
    diamond_button(button, pressed == JNI_TRUE ? 1 : 0);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_joystick(JNIEnv *, jclass, jfloat x, jfloat y) {
    diamond_joystick(x, y);
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_diamond_engine_jni_NativeBridge_joystickGet(JNIEnv *env, jclass) {
    float axes[2] = {0, 0};
    diamond_joystick_get(axes);
    return floats(env, axes, 2);
}

/* --------------------------------------------------------------- documents */

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_saveDiamond(JNIEnv *env, jclass, jstring path) {
    char error[256] = {0};
    const int rc = diamond_save_diamond(from_java(env, path), error, sizeof(error));
    set_error(error);
    return rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_loadDiamond(JNIEnv *env, jclass, jstring path) {
    char error[256] = {0};
    const int rc = diamond_load_diamond(from_java(env, path), error, sizeof(error));
    set_error(error);
    return rc;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_diamond_engine_jni_NativeBridge_serialize(JNIEnv *env, jclass) {
    size_t size = 0;
    void *data = diamond_serialize(&size);
    if (!data || size == 0) {
        set_error("could not serialise the project");
        return nullptr;
    }
    jbyteArray array = env->NewByteArray(static_cast<jsize>(size));
    if (array) env->SetByteArrayRegion(array, 0, static_cast<jsize>(size),
                                       static_cast<const jbyte *>(data));
    return array;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_deserialize(JNIEnv *env, jclass, jbyteArray data) {
    if (!data) {
        set_error("empty project data");
        return DIAMOND_ERR_ARG;
    }
    const jsize size = env->GetArrayLength(data);
    jbyte *bytes = env->GetByteArrayElements(data, nullptr);
    if (!bytes) {
        set_error("could not read the project data");
        return DIAMOND_ERR_MEMORY;
    }
    char error[256] = {0};
    const int rc = diamond_deserialize(bytes, static_cast<size_t>(size), error, sizeof(error));
    env->ReleaseByteArrayElements(data, bytes, JNI_ABORT);
    set_error(error);
    return rc;
}

/* ------------------------------------------------------------- diagnostics */

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_stats(JNIEnv *env, jclass) {
    char out[256] = {0};
    diamond_get_stats(out, sizeof(out));
    return to_java(env, out);
}

extern "C" JNIEXPORT void JNICALL
Java_com_diamond_engine_jni_NativeBridge_logWrite(JNIEnv *env, jclass, jstring tag,
                                                  jstring message) {
    char tagBuffer[64];
    std::snprintf(tagBuffer, sizeof(tagBuffer), "%s", from_java(env, tag));
    diamond_log_write(tagBuffer, from_java(env, message));
}

extern "C" JNIEXPORT jint JNICALL
Java_com_diamond_engine_jni_NativeBridge_logCount(JNIEnv *, jclass) {
    return diamond_log_count();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_diamond_engine_jni_NativeBridge_logPoll(JNIEnv *env, jclass, jint sinceSeq,
                                                 jintArray outSeq) {
    char buffer[8192];
    int lines = 0;
    const int next = diamond_log_poll(sinceSeq, buffer, sizeof(buffer), &lines);
    if (outSeq && env->GetArrayLength(outSeq) >= 2) {
        const jint pair[2] = {next, lines};
        env->SetIntArrayRegion(outSeq, 0, 2, pair);
    }
    if (lines <= 0) return nullptr;
    return to_java(env, buffer);
}
