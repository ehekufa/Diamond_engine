// Diamond Engine — Wavefront OBJ parser.
//
// Deliberately small and allocation-light:
//   * one pass over the text, no std::string, no iostream (both cost >100 KiB
//     of code and pull in locale tables);
//   * vertex data is de-duplicated by (v, vt, vn) triple, which is what GL
//     needs anyway because indices address the combined vertex;
//   * polygons with 4+ corners are fan-triangulated (OBJ allows n-gons);
//   * malformed lines are skipped and counted instead of aborting the import,
//     so a slightly broken export from Blender still opens.
#include "diamond/dobj.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "diamond/dscene.h"
#include "diamond/dutil.h"
#include "diamond/dlog.h"
}

namespace {

struct ParseState {
    int vertexCount = 0;
    int triangleCount = 0;
    int objectCount = 0;
    int ignoredLines = 0;
};

ParseState g_stats;

// Growable float/uint16 arrays with manual growth (no exceptions).
struct FloatArray {
    float *data = nullptr;
    int count = 0;
    int cap = 0;
    bool push3(float a, float b, float c) {
        if (count + 3 > cap) {
            int cap2 = cap ? cap * 2 : 256;
            while (cap2 < count + 3) cap2 *= 2;
            float *grown = static_cast<float *>(std::realloc(data, sizeof(float) * static_cast<size_t>(cap2)));
            if (!grown) return false;
            data = grown;
            cap = cap2;
        }
        data[count++] = a; data[count++] = b; data[count++] = c;
        return true;
    }
    bool push2(float a, float b) {
        if (count + 2 > cap) {
            int cap2 = cap ? cap * 2 : 256;
            while (cap2 < count + 2) cap2 *= 2;
            float *grown = static_cast<float *>(std::realloc(data, sizeof(float) * static_cast<size_t>(cap2)));
            if (!grown) return false;
            data = grown;
            cap = cap2;
        }
        data[count++] = a; data[count++] = b;
        return true;
    }
    void release() { std::free(data); data = nullptr; count = cap = 0; }
};

struct IndexArray {
    uint16_t *data = nullptr;
    int count = 0;
    int cap = 0;
    bool push(uint16_t value) {
        if (count + 1 > cap) {
            int cap2 = cap ? cap * 2 : 512;
            while (cap2 < count + 1) cap2 *= 2;
            uint16_t *grown = static_cast<uint16_t *>(std::realloc(data, sizeof(uint16_t) * static_cast<size_t>(cap2)));
            if (!grown) return false;
            data = grown;
            cap = cap2;
        }
        data[count++] = value;
        return true;
    }
    void release() { std::free(data); data = nullptr; count = cap = 0; }
};

struct MeshAccumulator {
    FloatArray positions;   // deduplicated output
    FloatArray normals;
    FloatArray uvs;
    IndexArray indices;
};

// Raw OBJ arrays.
FloatArray g_objPositions;
FloatArray g_objNormals;
FloatArray g_objUvs;

constexpr int kMaxFaceVertices = 64;

bool skip_spaces(const char *&cursor, const char *end) {
    while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r')) ++cursor;
    return cursor < end;
}

bool read_float(const char *&cursor, const char *end, float &out) {
    if (!skip_spaces(cursor, end)) return false;
    char buffer[64];
    int length = 0;
    while (cursor < end && length < 63) {
        char c = *cursor;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') break;
        buffer[length++] = c;
        ++cursor;
    }
    buffer[length] = '\0';
    if (length == 0) return false;
    char *stop = nullptr;
    double value = std::strtod(buffer, &stop);
    if (stop == buffer) return false;
    out = static_cast<float>(value);
    return true;
}

// Parses "12", "12/7", "12//3", "12/7/3" -> absolute 0-based indices or -1.
void parse_corner(const char *token, int length, int vCount, int vtCount, int vnCount,
                  int &vi, int &ti, int &ni) {
    vi = ti = ni = -1;
    int part = 0;
    int start = 0;
    int values[3] = {0, 0, 0};
    bool present[3] = {false, false, false};
    for (int i = 0; i <= length; ++i) {
        if (i == length || token[i] == '/') {
            if (i > start) {
                char buffer[32];
                int n = i - start;
                if (n > 31) n = 31;
                std::memcpy(buffer, token + start, static_cast<size_t>(n));
                buffer[n] = '\0';
                values[part] = static_cast<int>(std::strtol(buffer, nullptr, 10));
                present[part] = true;
            }
            part++;
            start = i + 1;
            if (part > 2) break;
        }
    }
    auto resolve = [](int raw, int count) -> int {
        if (raw == 0) return -1;
        if (raw < 0) return count + raw;      // 1-based, negative = relative
        return raw - 1;
    };
    vi = present[0] ? resolve(values[0], vCount) : -1;
    ti = present[1] ? resolve(values[1], vtCount) : -1;
    ni = present[2] ? resolve(values[2], vnCount) : -1;
}

// De-duplication is done through a tiny open-addressing table keyed by the
// packed (vi, ti, ni) triple. 8 KiB is enough for scenes with < 50 k vertices.
struct VertexKey {
    int vi, ti, ni;
};

constexpr int kKeyTableSize = 8192; // power of two
VertexKey g_keyTable[kKeyTableSize];
int g_keyValue[kKeyTableSize];
bool g_keyUsed[kKeyTableSize];

void key_table_reset() {
    std::memset(g_keyUsed, 0, sizeof(g_keyUsed));
}

unsigned key_hash(int vi, int ti, int ni) {
    unsigned h = static_cast<unsigned>(vi) * 73856093u;
    h ^= static_cast<unsigned>(ti) * 19349663u;
    h ^= static_cast<unsigned>(ni) * 83492791u;
    return h;
}

int find_or_add_vertex(MeshAccumulator &mesh, int vi, int ti, int ni,
                       const FloatArray &positions, const FloatArray &uvs, const FloatArray &normals) {
    if (vi < 0 || vi * 3 + 2 >= positions.count) return -1;
    {
        unsigned slot = key_hash(vi, ti, ni) & (kKeyTableSize - 1);
        unsigned probe = 0;
        while (g_keyUsed[slot] && probe < kKeyTableSize) {
            if (g_keyTable[slot].vi == vi && g_keyTable[slot].ti == ti && g_keyTable[slot].ni == ni)
                return g_keyValue[slot];
            slot = (slot + 1) & (kKeyTableSize - 1);
            ++probe;
        }
        if (probe < kKeyTableSize) {
            int index = mesh.positions.count / 3;
            if (index > 65535) return -1; // 16-bit indices: 65 k vertices per mesh
            if (!mesh.positions.push3(positions.data[vi * 3], positions.data[vi * 3 + 1], positions.data[vi * 3 + 2]))
                return -1;
            if (ti >= 0 && ti * 2 + 1 < uvs.count) {
                if (!mesh.uvs.push2(uvs.data[ti * 2], uvs.data[ti * 2 + 1])) return -1;
            } else {
                if (!mesh.uvs.push2(0.0f, 0.0f)) return -1;
            }
            if (ni >= 0 && ni * 3 + 2 < normals.count) {
                if (!mesh.normals.push3(normals.data[ni * 3], normals.data[ni * 3 + 1], normals.data[ni * 3 + 2]))
                    return -1;
            } else {
                if (!mesh.normals.push3(0.0f, 0.0f, 1.0f)) return -1;
            }
            g_keyTable[slot].vi = vi;
            g_keyTable[slot].ti = ti;
            g_keyTable[slot].ni = ni;
            g_keyValue[slot] = index;
            g_keyUsed[slot] = true;
            return index;
        }
        return -1;
    }
}

void compute_missing_normals(MeshAccumulator &mesh) {
    // OBJ files without "vn" get flat normals from the triangle plane.
    const int vertices = mesh.positions.count / 3;
    const int triangles = mesh.indices.count / 3;
    float *accum = static_cast<float *>(std::calloc(static_cast<size_t>(vertices) * 3, sizeof(float)));
    if (!accum) return;
    for (int t = 0; t < triangles; ++t) {
        int a = mesh.indices.data[t * 3], b = mesh.indices.data[t * 3 + 1], c = mesh.indices.data[t * 3 + 2];
        if (a < 0 || b < 0 || c < 0) continue;
        const float *pa = &mesh.positions.data[a * 3];
        const float *pb = &mesh.positions.data[b * 3];
        const float *pc = &mesh.positions.data[c * 3];
        float ux = pb[0] - pa[0], uy = pb[1] - pa[1], uz = pb[2] - pa[2];
        float vx = pc[0] - pa[0], vy = pc[1] - pa[1], vz = pc[2] - pa[2];
        float nx = uy * vz - uz * vy;
        float ny = uz * vx - ux * vz;
        float nz = ux * vy - uy * vx;
        accum[a * 3] += nx; accum[a * 3 + 1] += ny; accum[a * 3 + 2] += nz;
        accum[b * 3] += nx; accum[b * 3 + 1] += ny; accum[b * 3 + 2] += nz;
        accum[c * 3] += nx; accum[c * 3 + 1] += ny; accum[c * 3 + 2] += nz;
    }
    for (int v = 0; v < vertices; ++v) {
        float nx = accum[v * 3], ny = accum[v * 3 + 1], nz = accum[v * 3 + 2];
        float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length > 1e-6f) { nx /= length; ny /= length; nz /= length; }
        else { nx = 0.0f; ny = 0.0f; nz = 1.0f; }
        mesh.normals.data[v * 3] = nx;
        mesh.normals.data[v * 3 + 1] = ny;
        mesh.normals.data[v * 3 + 2] = nz;
    }
    std::free(accum);
}

} // namespace

extern "C" {

int dobj_parse_to_mesh(const char *text, int length, const char *name, char *err, int err_cap) {
    if (!text || length <= 0) return diamond_fail(DIAMOND_ERR_ARG, err, err_cap, "OBJ: empty file");

    g_objPositions.release();
    g_objNormals.release();
    g_objUvs.release();
    key_table_reset();
    g_stats = ParseState{};

    MeshAccumulator mesh;
    int line = 1;
    int cornerIndex[kMaxFaceVertices];
    const char *cursor = text;
    const char *end = text + length;

    while (cursor < end) {
        const char *lineEnd = cursor;
        while (lineEnd < end && *lineEnd != '\n') ++lineEnd;
        if (!skip_spaces(cursor, lineEnd)) { cursor = lineEnd + 1; ++line; continue; }

        if (*cursor == 'v' && cursor + 1 < lineEnd) {
            char kind = cursor[1];
            if (kind == ' ' || kind == '\t') {
                cursor += 2;
                float x, y, z;
                if (read_float(cursor, lineEnd, x) && read_float(cursor, lineEnd, y) && read_float(cursor, lineEnd, z))
                    g_objPositions.push3(x, y, z);
                else
                    g_stats.ignoredLines++;
            } else if (kind == 't' && cursor + 2 <= lineEnd) {
                cursor += 2;
                float u, v;
                if (read_float(cursor, lineEnd, u) && read_float(cursor, lineEnd, v))
                    g_objUvs.push2(u, v);
                else
                    g_stats.ignoredLines++;
            } else if (kind == 'n' && cursor + 2 <= lineEnd) {
                cursor += 2;
                float x, y, z;
                if (read_float(cursor, lineEnd, x) && read_float(cursor, lineEnd, y) && read_float(cursor, lineEnd, z))
                    g_objNormals.push3(x, y, z);
                else
                    g_stats.ignoredLines++;
            }
        } else if (*cursor == 'f' && cursor + 1 < lineEnd) {
            cursor += 1;
            int corners = 0;
            while (corners < kMaxFaceVertices && skip_spaces(cursor, lineEnd)) {
                const char *tokenStart = cursor;
                while (cursor < lineEnd && *cursor != ' ' && *cursor != '\t' && *cursor != '\r') ++cursor;
                int tokenLength = static_cast<int>(cursor - tokenStart);
                if (tokenLength <= 0) break;
                int vi, ti, ni;
                parse_corner(tokenStart, tokenLength, g_objPositions.count / 3,
                             g_objUvs.count / 2, g_objNormals.count / 3, vi, ti, ni);
                int index = find_or_add_vertex(mesh, vi, ti, ni, g_objPositions, g_objUvs, g_objNormals);
                if (index < 0) { g_stats.ignoredLines++; corners = 0; break; }
                cornerIndex[corners++] = index;
            }
            if (corners >= 3) {
                for (int i = 1; i + 1 < corners; ++i) {
                    mesh.indices.push(static_cast<uint16_t>(cornerIndex[0]));
                    mesh.indices.push(static_cast<uint16_t>(cornerIndex[i]));
                    mesh.indices.push(static_cast<uint16_t>(cornerIndex[i + 1]));
                    g_stats.triangleCount++;
                }
            }
        } else if (*cursor == 'o' || *cursor == 'g') {
            g_stats.objectCount++;
        } else {
            g_stats.ignoredLines++;
        }
        cursor = lineEnd + 1;
        ++line;
    }

    if (mesh.positions.count == 0 || mesh.indices.count == 0) {
        mesh.positions.release(); mesh.normals.release(); mesh.uvs.release(); mesh.indices.release();
        return diamond_fail(DIAMOND_ERR_PARSE, err, err_cap, "OBJ: no usable geometry (line %d)", line);
    }

    compute_missing_normals(mesh);
    g_stats.vertexCount = mesh.positions.count / 3;

    const int index = dmesh_add(name && *name ? name : "OBJ", mesh.positions.count / 3,
                                mesh.positions.data, mesh.normals.data, mesh.uvs.data,
                                mesh.indices.count, mesh.indices.data);
    if (index < 0) {
        mesh.positions.release(); mesh.normals.release(); mesh.uvs.release(); mesh.indices.release();
        return diamond_fail(DIAMOND_ERR_MEMORY, err, err_cap, "OBJ: out of memory");
    }

    // Ownership moved to the scene: forget the local pointers.
    mesh.positions.data = nullptr; mesh.positions.count = mesh.positions.cap = 0;
    mesh.normals.data = nullptr;   mesh.normals.count = mesh.normals.cap = 0;
    mesh.uvs.data = nullptr;       mesh.uvs.count = mesh.uvs.cap = 0;
    mesh.indices.data = nullptr;   mesh.indices.count = mesh.indices.cap = 0;

    g_objPositions.release();
    g_objNormals.release();
    g_objUvs.release();

    dlog_write(DLOG_INFO, "obj", "parsed '%s': %d verts, %d tris, %d ignored lines",
               name ? name : "OBJ", g_stats.vertexCount, g_stats.triangleCount, g_stats.ignoredLines);
    return index;
}

void dobj_last_stats(int *vertices, int *triangles, int *objects, int *ignored) {
    if (vertices) *vertices = g_stats.vertexCount;
    if (triangles) *triangles = g_stats.triangleCount;
    if (objects) *objects = g_stats.objectCount;
    if (ignored) *ignored = g_stats.ignoredLines;
}

} // extern "C"
