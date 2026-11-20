// Diamond Engine — OpenGL ES 2.0 "Potato mode".
//
// Targets the phones the project cares about most: no GLES3, no depth-24
// config, 512 MB free RAM, Adreno 200 / Mali-400 class GPUs. Consequences:
//   * no VAOs (not in ES2), no instancing, no texture arrays;
//   * one shader, no lighting, no rim term — flat colour plus a single ambient
//     factor keeps the fragment shader at ~12 instructions;
//   * the grid is limited to 8 lines per side;
//   * sprites are drawn with the same shader through a 4-vertex quad.
#include "renderer.h"

#include <GLES2/gl2.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "diamond/dlog.h"
#include "diamond/dmath.h"
#include "diamond/dutil.h"
}

namespace diamond {

namespace {

const char *kVertexShader = R"(#version 100
precision highp float;
attribute vec3 aPos;
attribute vec4 aColor;
uniform mat4 uMvp;
varying vec4 vColor;
varying vec2 vUv;
void main() {
    vColor = aColor;
    vUv = vec2(0.0);
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char *kVertexShaderTextured = R"(#version 100
precision highp float;
attribute vec3 aPos;
attribute vec2 aUv;
uniform mat4 uMvp;
varying vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char *kFragmentShader = R"(#version 100
precision mediump float;
varying vec4 vColor;
varying vec2 vUv;
uniform vec4 uColor;
uniform sampler2D uTex;
uniform float uUseTexture;
void main() {
    vec4 base = uColor;
    if (uUseTexture > 0.5) base *= texture2D(uTex, vUv);
    gl_FragColor = vec4(base.rgb * 0.85, base.a);
}
)";

GLuint compile(GLenum type, const char *source, const char *label) {
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[256] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        dlog_write(DLOG_ERROR, "gl2", "%s compile failed: %s", label, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

struct GpuMesh {
    int meshIndex = -1;
    int imageIndex = -1;
    GLuint vbo = 0;
    GLuint ibo = 0;
    GLuint texture = 0;
    int indexCount = 0;
    int vertexCount = 0;
};

constexpr int kMaxMeshes = 128;

class Gles2Backend final : public IRenderBackend {
public:
    ~Gles2Backend() override { detach(); }

    const char *name() const override { return "OpenGL ES 2.0 (potato)"; }
    int mode() const override { return DIAMOND_RENDER_GLES2; }

    bool attach(void *window, int width, int height, char *err, int errCap) override {
        detach();
        width_ = width;
        height_ = height;
        if (!window) {
            diamond_fail(DIAMOND_ERR_ARG, err, errCap, "no native window");
            return false;
        }
        context_ = gl_context_create(window, err, errCap);
        if (!context_) return false;
        gl_context_info(context_, &esVersion_, nullptr, nullptr, nullptr, &rendererName_);

        program_ = build_program(kVertexShader, kFragmentShader, "gl2-colour");
        texturedProgram_ = build_program(kVertexShaderTextured, kFragmentShader, "gl2-textured");
        if (!program_ || !texturedProgram_) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "ES2 shaders failed to build");
            detach();
            return false;
        }
        im_ = im_batch_create();
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        dlog_write(DLOG_INFO, "gl2", "potato backend attached, ES%d, %dx%d, %s",
                   esVersion_, width_, height_, rendererName_);
        attached_ = true;
        return true;
    }

    void detach() override {
        for (int i = 0; i < meshCount_; ++i) {
            if (meshes_[i].vbo) glDeleteBuffers(1, &meshes_[i].vbo);
            if (meshes_[i].ibo) glDeleteBuffers(1, &meshes_[i].ibo);
            if (meshes_[i].texture) glDeleteTextures(1, &meshes_[i].texture);
        }
        meshCount_ = 0;
        if (program_) glDeleteProgram(program_);
        if (texturedProgram_) glDeleteProgram(texturedProgram_);
        program_ = texturedProgram_ = 0;
        uMvp_ = uColor_ = uTex_ = uUseTexture_ = -1;
        im_batch_destroy(im_);
        im_ = nullptr;
        if (context_) gl_context_destroy(context_);
        context_ = nullptr;
        attached_ = false;
    }

    void resize(int width, int height) override { width_ = width; height_ = height; }

    bool draw(const FrameContext &frame) override {
        if (!attached_ || !context_ || gl_context_lost(context_)) return false;
        const double start = dtime_seconds();
        glViewport(0, 0, frame.width, frame.height);
        glClear(GL_COLOR_BUFFER_BIT);

        float view[16], projection[16], vp[16];
        dcamera_matrices(frame.camera, frame.viewMode, frame.width, frame.height, view, projection);
        dmat4_multiply(vp, projection, view);

        // --- grid + selection, immediate mode -------------------------------
        im_batch_clear(im_);
        im_batch_grid(im_, 8, 1.0f, 0x382A2F3A, 0x70505A70);
        if (frame.selected > 0) {
            float box[6];
            if (dnode_world_aabb(frame.selected, box)) im_batch_box(im_, box, 0xFF37D67Au);
        }
        const int vertices = im_batch_vertex_count(im_);
        if (vertices > 0) {
            glUseProgram(program_);
            glUniformMatrix4fv(uMvp_, 1, GL_FALSE, vp);
            glUniform4f(uColor_, 1.0f, 1.0f, 1.0f, 1.0f);
            glUniform1f(uUseTexture_, 0.0f);
            glBindBuffer(GL_ARRAY_BUFFER, immediateVbo_);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 10 * vertices),
                         im_batch_vertices(im_), GL_DYNAMIC_DRAW);
            const GLint posLoc = glGetAttribLocation(program_, "aPos");
            const GLint colorLoc = glGetAttribLocation(program_, "aColor");
            glEnableVertexAttribArray(static_cast<GLuint>(posLoc));
            glVertexAttribPointer(static_cast<GLuint>(posLoc), 3, GL_FLOAT, GL_FALSE,
                                  sizeof(float) * 10, static_cast<const void *>(nullptr));
            if (colorLoc >= 0) {
                glEnableVertexAttribArray(static_cast<GLuint>(colorLoc));
                glVertexAttribPointer(static_cast<GLuint>(colorLoc), 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                      sizeof(float) * 10, reinterpret_cast<const void *>(sizeof(float) * 6));
            }
            glDrawArrays(GL_LINES, 0, vertices);
            drawCalls_++;
        }

        // --- objects --------------------------------------------------------
        triangles_ = 0;
        const int count = dscene_flat_count();
        for (int i = 1; i < count; ++i) {
            DNode *node = dnode_get(i);
            if (!node || !node->visible) continue;
            if (node->mesh < 0 && node->image < 0) continue;
            float model[16], mvp[16];
            dnode_world_matrix(node, model);
            dmat4_multiply(mvp, vp, model);
            const uint32_t color = dnode_resolved_color(node);
            GpuMesh *gpu = node->mesh >= 0 ? meshGpu(node->mesh) : spriteGpu(node->image);
            if (!gpu) continue;
            if (gpu->texture) {
                glUseProgram(texturedProgram_);
                glUniformMatrix4fv(glGetUniformLocation(texturedProgram_, "uMvp"), 1, GL_FALSE, mvp);
                const GLint uTex = glGetUniformLocation(texturedProgram_, "uTex");
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, gpu->texture);
                glUniform1i(uTex, 0);
                const GLint uColor = glGetUniformLocation(texturedProgram_, "uColor");
                const GLint uUse = glGetUniformLocation(texturedProgram_, "uUseTexture");
                glUniform4f(uColor,
                            static_cast<float>(color & 0xFF) / 255.0f,
                            static_cast<float>((color >> 8) & 0xFF) / 255.0f,
                            static_cast<float>((color >> 16) & 0xFF) / 255.0f,
                            static_cast<float>((color >> 24) & 0xFF) / 255.0f);
                glUniform1f(uUse, 1.0f);
                bindAndDraw(texturedProgram_, gpu, true);
                glUniform1f(uUse, 0.0f);
            } else {
                glUseProgram(program_);
                glUniformMatrix4fv(uMvp_, 1, GL_FALSE, mvp);
                glUniform4f(uColor_,
                            static_cast<float>(color & 0xFF) / 255.0f,
                            static_cast<float>((color >> 8) & 0xFF) / 255.0f,
                            static_cast<float>((color >> 16) & 0xFF) / 255.0f,
                            static_cast<float>((color >> 24) & 0xFF) / 255.0f);
                glUniform1f(uUseTexture_, 0.0f);
                bindAndDraw(program_, gpu, false);
            }
            triangles_ += gpu->indexCount / 3;
            drawCalls_++;
        }

        // --- play overlay ---------------------------------------------------
        if (frame.playing) drawPlayOverlay(frame, vp);

        gl_context_swap(context_);
        frameMs_ = static_cast<float>((dtime_seconds() - start) * 1000.0);
        return true;
    }

    DRenderStats stats() const override {
        DRenderStats s{};
        s.backend = DIAMOND_RENDER_GLES2;
        s.frame_ms = frameMs_;
        s.draw_calls = drawCalls_;
        s.triangles = triangles_;
        s.width = width_;
        s.height = height_;
        return s;
    }

private:
    GLuint build_program(const char *vertex, const char *fragment, const char *label) {
        GLuint v = compile(GL_VERTEX_SHADER, vertex, label);
        if (!v) return 0;
        GLuint f = compile(GL_FRAGMENT_SHADER, fragment, label);
        if (!f) { glDeleteShader(v); return 0; }
        GLuint id = glCreateProgram();
        glAttachShader(id, v);
        glAttachShader(id, f);
        glLinkProgram(id);
        glDeleteShader(v);
        glDeleteShader(f);
        GLint status = 0;
        glGetProgramiv(id, GL_LINK_STATUS, &status);
        if (!status) {
            char log[256] = {0};
            glGetProgramInfoLog(id, sizeof(log) - 1, nullptr, log);
            dlog_write(DLOG_ERROR, "gl2", "%s link failed: %s", label, log);
            glDeleteProgram(id);
            return 0;
        }
        return id;
    }

    void bindAndDraw(GLuint program, GpuMesh *gpu, bool textured) {
        const GLint posLoc = glGetAttribLocation(program, "aPos");
        glBindBuffer(GL_ARRAY_BUFFER, gpu->vbo);
        glEnableVertexAttribArray(static_cast<GLuint>(posLoc));
        glVertexAttribPointer(static_cast<GLuint>(posLoc), 3, GL_FLOAT, GL_FALSE,
                              sizeof(float) * (textured ? 5 : 3), static_cast<const void *>(nullptr));
        if (textured) {
            const GLint uvLoc = glGetAttribLocation(program, "aUv");
            glEnableVertexAttribArray(static_cast<GLuint>(uvLoc));
            glVertexAttribPointer(static_cast<GLuint>(uvLoc), 2, GL_FLOAT, GL_FALSE,
                                  sizeof(float) * 5, reinterpret_cast<const void *>(sizeof(float) * 3));
        } else {
            const GLint colorLoc = glGetAttribLocation(program, "aColor");
            if (colorLoc >= 0) glDisableVertexAttribArray(static_cast<GLuint>(colorLoc));
        }
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu->ibo);
        glDrawElements(GL_TRIANGLES, gpu->indexCount, GL_UNSIGNED_SHORT, nullptr);
    }

    GpuMesh *meshGpu(int meshIndex) {
        for (int i = 0; i < meshCount_; ++i)
            if (meshes_[i].meshIndex == meshIndex) return &meshes_[i];
        if (meshCount_ >= kMaxMeshes) return nullptr;
        DMesh *mesh = dmesh_get(meshIndex);
        if (!mesh || !mesh->positions || !mesh->indices) return nullptr;
        GpuMesh &gpu = meshes_[meshCount_++];
        gpu.meshIndex = meshIndex;
        gpu.vertexCount = mesh->vertex_count;
        gpu.indexCount = mesh->index_count;
        float *packed = static_cast<float *>(std::malloc(sizeof(float) * 3 * static_cast<size_t>(mesh->vertex_count)));
        if (!packed) { meshCount_--; return nullptr; }
        std::memcpy(packed, mesh->positions, sizeof(float) * 3 * static_cast<size_t>(mesh->vertex_count));
        glGenBuffers(1, &gpu.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 3 * mesh->vertex_count),
                     packed, GL_STATIC_DRAW);
        std::free(packed);
        glGenBuffers(1, &gpu.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(uint16_t) * mesh->index_count),
                     mesh->indices, GL_STATIC_DRAW);
        return &gpu;
    }

    GpuMesh *spriteGpu(int imageIndex) {
        for (int i = 0; i < meshCount_; ++i)
            if (meshes_[i].imageIndex == imageIndex) return &meshes_[i];
        if (meshCount_ >= kMaxMeshes) return nullptr;
        DImage *image = dimage_get(imageIndex);
        if (!image || !image->rgba) return nullptr;
        GpuMesh &gpu = meshes_[meshCount_++];
        gpu.imageIndex = imageIndex;
        const float aspect = static_cast<float>(image->height) / static_cast<float>(image->width > 0 ? image->width : 1);
        const float hw = 0.5f, hh = 0.5f * aspect;
        const float vertexData[] = {
            -hw, -hh, 0.0f, 0.0f, 0.0f,
             hw, -hh, 0.0f, 1.0f, 0.0f,
             hw,  hh, 0.0f, 1.0f, 1.0f,
            -hw,  hh, 0.0f, 0.0f, 1.0f};
        const uint16_t indexData[] = {0, 1, 2, 0, 2, 3};
        glGenBuffers(1, &gpu.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertexData), vertexData, GL_STATIC_DRAW);
        glGenBuffers(1, &gpu.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indexData), indexData, GL_STATIC_DRAW);
        glGenTextures(1, &gpu.texture);
        glBindTexture(GL_TEXTURE_2D, gpu.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->width, image->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image->rgba);
        gpu.vertexCount = 4;
        gpu.indexCount = 6;
        return &gpu;
    }

    void drawPlayOverlay(const FrameContext &frame, const float *vp) {
        // Screen-space overlay in the same ortho projection as the 2D editor.
        float overlay[16];
        dmat4_ortho(overlay, 0.0f, static_cast<float>(width_), 0.0f,
                    static_cast<float>(height_), -1.0f, 1.0f);
        im_batch_clear(im_);
        const float cx = static_cast<float>(width_) * 0.16f;
        const float cy = static_cast<float>(height_) * 0.28f;
        const float radius = static_cast<float>(height_) * 0.16f;
        const float stick[2] = {frame.joystick[0] * radius, frame.joystick[1] * radius};
        float center[3] = {cx, cy, 0.0f};
        float ring[3] = {cx + radius, cy, 0.0f};
        im_batch_line(im_, center, ring, 0x60FFFFFFu);
        float knob_a[3] = {cx + stick[0] - 12.0f, cy + stick[1], 0.0f};
        float knob_b[3] = {cx + stick[0] + 12.0f, cy + stick[1], 0.0f};
        im_batch_line(im_, knob_a, knob_b, 0xFFFFFFFFu);
        // A / B buttons.
        const float bx = static_cast<float>(width_) - radius * 1.2f;
        const float by = static_cast<float>(height_) * 0.30f;
        float a0[3] = {bx, by, 0.0f};
        float a1[3] = {bx + 40.0f, by + 40.0f, 0.0f};
        im_batch_box(im_, a0, 0x00000000u);
        im_batch_line(im_, a0, a1, frame.buttons[DIAMOND_BTN_A] ? 0xFFFFFFFFu : 0x80FFFFFFu);
        float b0[3] = {bx - 70.0f, by - 20.0f, 0.0f};
        float b1[3] = {bx - 30.0f, by + 20.0f, 0.0f};
        im_batch_line(im_, b0, b1, frame.buttons[DIAMOND_BTN_B] ? 0xFFFFFFFFu : 0x80FFFFFFu);

        glUseProgram(program_);
        const int count = im_batch_vertex_count(im_);
        if (count > 0) {
            glUniformMatrix4fv(uMvp_, 1, GL_FALSE, overlay);
            glBindBuffer(GL_ARRAY_BUFFER, immediateVbo_);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 10 * count),
                         im_batch_vertices(im_), GL_DYNAMIC_DRAW);
            const GLint posLoc = glGetAttribLocation(program_, "aPos");
            const GLint colorLoc = glGetAttribLocation(program_, "aColor");
            glEnableVertexAttribArray(static_cast<GLuint>(posLoc));
            glVertexAttribPointer(static_cast<GLuint>(posLoc), 3, GL_FLOAT, GL_FALSE,
                                  sizeof(float) * 10, static_cast<const void *>(nullptr));
            glEnableVertexAttribArray(static_cast<GLuint>(colorLoc));
            glVertexAttribPointer(static_cast<GLuint>(colorLoc), 4, GL_UNSIGNED_BYTE, GL_TRUE,
                                  sizeof(float) * 10, reinterpret_cast<const void *>(sizeof(float) * 6));
            glDrawArrays(GL_LINES, 0, count);
        }
        (void)vp;
    }

    GlContext *context_ = nullptr;
    GLuint program_ = 0;
    GLuint texturedProgram_ = 0;
    GLuint immediateVbo_ = 0;
    GLint uMvp_ = -1, uColor_ = -1, uTex_ = -1, uUseTexture_ = -1;
    int esVersion_ = 2;
    const char *rendererName_ = "";
    GpuMesh meshes_[kMaxMeshes];
    int meshCount_ = 0;
    ImBatch *im_ = nullptr;
    bool attached_ = false;
    float frameMs_ = 0.0f;
    int drawCalls_ = 0;
    int triangles_ = 0;
};

} // namespace

IRenderBackend *createGles2Backend() { return new Gles2Backend(); }

} // namespace diamond
