// Diamond Engine — small file helpers exposed to Java.
//
// Java's own IO is perfectly good for documents, but asset bundles and OBJ
// imports arrive as compressed blobs from the asset manager, so the native side
// needs to read RAW + AAsset and unpack them without a second copy in the heap.
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "diamond/dutil.h"
#include "diamond/dlog.h"

namespace {

/* The engine only installs the AssetManager once, during initialisation. */
AAssetManager *g_assets = nullptr;

char g_lastError[192];

void set_error(const char *text) {
    std::snprintf(g_lastError, sizeof(g_lastError), "%s", text ? text : "");
}

} // namespace

extern "C" {

int dfiles_install_assets(void *asset_manager) {
    g_assets = static_cast<AAssetManager *>(asset_manager);
    dlog_write(DLOG_INFO, "io", "asset manager %s", g_assets ? "installed" : "missing");
    return g_assets ? 1 : 0;
}

const char *dfiles_last_error(void) { return g_lastError; }

/* Reads a whole asset into a malloc'ed, NUL-terminated buffer. */
void *dfiles_read_asset(const char *path, int *out_size) {
    if (out_size) *out_size = 0;
    if (!g_assets || !path) {
        set_error("asset manager is not installed");
        return nullptr;
    }
    AAsset *asset = AAssetManager_open(g_assets, path, AASSET_MODE_BUFFER);
    if (!asset) {
        set_error("asset not found");
        return nullptr;
    }
    // AAsset_getLength64 exists since API 21 but the 32-bit form is enough for
    // the 64 MiB ceiling enforced below; using it keeps older stubs happy.
    const long length = AAsset_getLength(asset);
    if (length <= 0 || length > 64L * 1024L * 1024L) {
        AAsset_close(asset);
        set_error("asset is empty or unreasonably large");
        return nullptr;
    }
    char *buffer = static_cast<char *>(std::malloc(static_cast<size_t>(length) + 1));
    if (!buffer) {
        AAsset_close(asset);
        set_error("out of memory");
        return nullptr;
    }
    const int read = AAsset_read(asset, buffer, static_cast<size_t>(length));
    AAsset_close(asset);
    if (read != static_cast<int>(length)) {
        std::free(buffer);
        set_error("short asset read");
        return nullptr;
    }
    buffer[length] = '\0';
    if (out_size) *out_size = static_cast<int>(length);
    set_error("");
    return buffer;
}

/* Copies a file into Java-managed memory. Returns a malloc'ed buffer the caller
 * (application/file_io.cpp) frees; returns null and fills the error on failure. */
void *dfiles_read_file(const char *path, int *out_size) {
    if (out_size) *out_size = 0;
    if (!path) {
        set_error("no path");
        return nullptr;
    }
    FILE *file = std::fopen(path, "rb");
    if (!file) {
        set_error("cannot open the file");
        return nullptr;
    }
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length < 0 || length > 256L * 1024L * 1024L) {
        std::fclose(file);
        set_error("file size is out of range");
        return nullptr;
    }
    void *buffer = std::malloc(static_cast<size_t>(length) + 1);
    if (!buffer) {
        std::fclose(file);
        set_error("out of memory");
        return nullptr;
    }
    const size_t read = std::fread(buffer, 1, static_cast<size_t>(length), file);
    std::fclose(file);
    if (read != static_cast<size_t>(length)) {
        std::free(buffer);
        set_error("short read");
        return nullptr;
    }
    static_cast<char *>(buffer)[length] = '\0';
    if (out_size) *out_size = static_cast<int>(length);
    return buffer;
}

int dfiles_write_file(const char *path, const void *data, int size) {
    if (!path || (size > 0 && !data)) {
        set_error("invalid arguments");
        return 0;
    }
    FILE *file = std::fopen(path, "wb");
    if (!file) {
        set_error("cannot open the file for writing");
        return 0;
    }
    const size_t written = size > 0 ? std::fwrite(data, 1, static_cast<size_t>(size), file) : 0;
    std::fclose(file);
    if (written != static_cast<size_t>(size)) {
        set_error("short write");
        return 0;
    }
    return 1;
}

} // extern "C"
