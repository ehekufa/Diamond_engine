// Diamond Engine — OpenGL ES 3.0 backend (recommended, full 2D/2.5D/3D).
//
// What it draws per frame:
//   * a ground grid (immediate mode, one VBO)
//   * every visible node: meshes with normals + directional light, sprites as
//     textured quads, empty nodes as a small wire cube when selected
//   * selection highlight and transform gizmo
//   * the play overlay (virtual joystick + A/B buttons) when playing
//
// Everything is one shader pair and two buffers, so a driver bug surface stays
// small — which is what makes this the "stable" mode.
#include "renderer.h"

#include <GLES3/gl3.h>

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

const char *kVertexShader = R"(#version 300 es
precision highp float;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
uniform mat4 uMvp;
uniform mat4 uModel;
out vec3 vNormal;
out vec2 vUv;
out vec4 vColor;
out vec3 vWorld;
void main() {
    vNormal = mat3(uModel) * aNormal;
    vUv = aUv;
    vColor = aColor;
    vWorld = (uModel * vec4(aPos, 1.0)).xyz;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char *kFragmentShader = R"(#version 300 es
precision mediump float;
in vec3 vNormal;
in vec2 vUv;
in vec4 vColor;
in vec3 vWorld;
uniform vec3 uCamPos;
uniform vec4 uColor;
uniform vec4 uLight;      // xyz direction, w intensity
uniform sampler2D uTex;
uniform int uUseTexture;
out vec4 fragColor;
void main() {
    vec4 base = vColor.a > 0.0 ? vColor : uColor;
    if (uUseTexture == 1) base *= texture(uTex, vUv);
    vec3 n = normalize(vNormal);
    vec3 l = normalize(-uLight.xyz);
    float diffuse = max(dot(n, l), 0.0) * uLight.w;
    float ambient = 0.45;
    vec3 lit = base.rgb * (ambient + diffuse);
    // Cheap rim term: reads as depth on a phone without costing a texture fetch.
    vec3 viewDir = normalize(uCamPos - vWorld);
    float rim = pow(1.0 - max(dot(n, viewDir), 0.0), 3.0) * 0.12;
    fragColor = vec4(lit + rim, base.a);
}
)";

struct MeshGpu {
    int meshIndex = -1;
    GLuint vbo = 0;
    GLuint ibo = 0;
    GLuint texture = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int imageIndex = -1;
};

constexpr int kMaxMeshGpu = 256;

class Gles3Backend final : public IRenderBackend {
public:
    ~Gles3Backend() override { detach(); }

    const char *name() const override { return "OpenGL ES 3.0"; }
    int mode() const override { return DIAMOND_RENDER_GLES3; }

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
        gl_context_info(context_, &esVersion_, nullptr, nullptr, nullptr, nullptr);
        if (esVersion_ < 3) {
            diamond_fail(DIAMOND_ERR_UNSUPPORTED, err, errCap,
                         "device only reports OpenGL ES %d", esVersion_);
            gl_context_destroy(context_);
            context_ = nullptr;
            return false;
        }
        program_.uMvp = -1; program_.uModel = -1; program_.uColor = -1;
        program_.uLight = -1; program_.uCamPos = -1; program_.uTex = -1;
        {
            int es = 3;
            if (!gl_program_link(&program_, kVertexShader, kFragmentShader, "gles3", &es)) {
                diamond_fail(DIAMOND_ERR_GPU, err, errCap, "shader link failed");
                gl_context_destroy(context_);
                context_ = nullptr;
                return false;
            }
        }

        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 6, (void *)0);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(float) * 6 + 4,
                              (void *)(sizeof(float) * 6));
        glBindVertexArray(0);

        im_ = im_batch_create();
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glClearColor(0.09f, 0.10f, 0.12f, 1.0f);
        dlog_write(DLOG_INFO, "gl3", "backend attached (%dx%d, ES%d)", width_, height_, esVersion_);
        attached_ = true;
        return true;
    }

    void detach() override {
        if (!context_) {
            attached_ = false;
            return;
        }
        for (int i = 0; i < meshCount_; ++i) {
            if (meshes_[i].vbo) glDeleteBuffers(1, &meshes_[i].vbo);
            if (meshes_[i].ibo) glDeleteBuffers(1, &meshes_[i].ibo);
            if (meshes_[i].texture) glDeleteTextures(1, &meshes_[i].texture);
        }
        meshCount_ = 0;
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        vbo_ = 0;
        vao_ = 0;
        gl_program_release(&program_);
        im_batch_destroy(im_);
        im_ = nullptr;
        gl_context_destroy(context_);
        context_ = nullptr;
        attached_ = false;
    }

    void resize(int width, int height) override {
        width_ = width;
        height_ = height;
    }

    bool draw(const FrameContext &frame) override {
        if (!attached_ || !context_) return false;
        if (gl_context_lost(context_)) return false;

        frameMsStart_ = dtime_seconds();
        glViewport(0, 0, frame.width, frame.height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float view[16], projection[16];
        dcamera_matrices(frame.camera, frame.viewMode, frame.width, frame.height, view, projection);

        float eye[3];
        dcamera_eye(frame.camera, eye);

        glUseProgram(gl_program_id(&program_));
        glUniform3f(program_.uCamPos, eye[0], eye[1], eye[2]);
        glUniform4f(program_.uLight, -0.4f, -1.0f, -0.55f, 0.85f);
        glUniform1i(useTextureLocation_, 0);
        glUniform1i(program_.uTex, 0);

        drawGrid(frame, view, projection);
        drawScene(frame, view, projection, eye);
        drawOverlay(frame, view, projection);

        gl_context_swap(context_);
        frameMs_ = static_cast<float>((dtime_seconds() - frameMsStart_) * 1000.0);
        frames_++;
        return true;
    }

    DRenderStats stats() const override {
        DRenderStats s{};
        s.backend = DIAMOND_RENDER_GLES3;
        s.frame_ms = frameMs_;
        s.draw_calls = drawCalls_;
        s.triangles = triangles_;
        s.sprites = sprites_;
        s.width = width_;
        s.height = height_;
        return s;
    }

protected:
    void drawGrid(const FrameContext &frame, const float *view, const float *projection) {
        if (!im_) return;
        im_batch_clear(im_);
        if (frame.viewMode != DIAMOND_VIEW_2D) {
            im_batch_grid(im_, 16, 1.0f, 0x502A2F3A, 0x90606A80);
        } else {
            im_batch_grid(im_, 20, 1.0f, 0x3A2A2F3A, 0x70606A80);
        }
        float vp[16], mvp[16];
        dmat4_multiply(vp, projection, view);
        dmat4_identity(mvp);
        dmat4_multiply(mvp, vp, mvp);
        uploadImmediate(mvp);
        drawCalls_++;
    }

    void drawScene(const FrameContext &frame, const float *view, const float *projection, const float *eye) {
        float vp[16];
        dmat4_multiply(vp, projection, view);
        triangles_ = 0;
        sprites_ = 0;

        const int count = dscene_flat_count();
        for (int i = 1; i < count; ++i) {
            DNode *node = dnode_get(i);
            if (!node || !node->visible) continue;
            if (node->type == DIAMOND_NODE_SPRITE && node->image >= 0 && frame.viewMode != DIAMOND_VIEW_3D) {
                sprites_++;
                continue; // drawn by the sprite batch path below
            }
            float model[16], mvp[16], trs[9] = {node->px, node->py, node->pz,
                                                node->rx, node->ry, node->rz,
                                                node->sx, node->sy, node->sz};
            dmat4_from_trs(model, trs);
            dmat4_multiply(mvp, vp, model);
            glUniformMatrix4fv(program_.uMvp, 1, GL_FALSE, mvp);
            glUniformMatrix4fv(program_.uModel, 1, GL_FALSE, model);
            const uint32_t color = node->color;
            glUniform4f(program_.uColor,
                        static_cast<float>(color & 0xFF) / 255.0f,
                        static_cast<float>((color >> 8) & 0xFF) / 255.0f,
                        static_cast<float>((color >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((color >> 24) & 0xFF) / 255.0f);

            bool drawn = false;
            if (node->mesh >= 0) {
                MeshGpu *gpu = meshGpu(node->mesh);
                if (gpu) {
                    if (gpu->texture) {
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, gpu->texture);
                        glUniform1i(useTextureLocation_, 1);
                    } else {
                        glUniform1i(useTextureLocation_, 0);
                    }
                    glBindVertexArray(vao_);
                    glBindBuffer(GL_ARRAY_BUFFER, gpu->vbo);
                    glEnableVertexAttribArray(0);
                    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void *)0);
                    glEnableVertexAttribArray(1);
                    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void *)(sizeof(float) * 3));
                    glEnableVertexAttribArray(2);
                    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void *)(sizeof(float) * 6));
                    glDisableVertexAttribArray(3);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu->ibo);
                    glDrawElements(GL_TRIANGLES, gpu->indexCount, GL_UNSIGNED_SHORT, nullptr);
                    triangles_ += gpu->indexCount / 3;
                    drawn = true;
                }
            }
            if (!drawn && node->image >= 0) {
                // A sprite in 3D view: draw its quad mesh through the same path.
                MeshGpu *gpu = spriteGpu(node->image);
                if (gpu) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, gpu->texture);
                    glUniform1i(useTextureLocation_, 1);
                    glBindVertexArray(vao_);
                    glBindBuffer(GL_ARRAY_BUFFER, gpu->vbo);
                    glEnableVertexAttribArray(0);
                    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void *)0);
                    glEnableVertexAttribArray(1);
                    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void *)(sizeof(float) * 3));
                    glEnableVertexAttribArray(2);
                    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 8, (void *)(sizeof(float) * 6));
                    glDisableVertexAttribArray(3);
                    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu->ibo);
                    glDrawElements(GL_TRIANGLES, gpu->indexCount, GL_UNSIGNED_SHORT, nullptr);
                    triangles_ += gpu->indexCount / 3;
                    sprites_++;
                }
            }
            glUniform1i(useTextureLocation_, 0);
            glBindVertexArray(0);
            drawCalls_++;
        }
        (void)eye;
    }

    void drawOverlay(const FrameContext &frame, const float *view, const float *projection) {
        if (!im_) return;
        im_batch_clear(im_);
        const int selected = frame.selected;
        if (selected > 0) {
            float box[6];
            if (dnode_world_aabb(selected, box)) {
                im_batch_box(im_, box, 0xFF37D67Au); // accent green, ABGR
                // Transform gizmo: three axes from the node origin.
                DNode *node = dnode_get(selected);
                if (node) {
                    float origin[3] = {node->px, node->py, node->pz};
                    float tip_x[3] = {origin[0] + 1.2f, origin[1], origin[2]};
                    float tip_y[3] = {origin[0], origin[1] + 1.2f, origin[2]};
                    float tip_z[3] = {origin[0], origin[1], origin[2] + 1.2f};
                    im_batch_line(im_, origin, tip_x, 0xFF6060FFu);
                    im_batch_line(im_, origin, tip_y, 0xFF60FF60u);
                    im_batch_line(im_, origin, tip_z, 0xFFFF6060u);
                }
            }
        }
        float vp[16], mvp[16];
        dmat4_multiply(vp, projection, view);
        dmat4_identity(mvp);
        dmat4_multiply(mvp, vp, mvp);
        uploadImmediate(mvp);
        drawCalls_++;
    }

    void uploadImmediate(const float *mvp) {
        const int vertices = im_batch_vertex_count(im_);
        if (vertices <= 0) return;
        glUseProgram(gl_program_id(&program_));
        glUniformMatrix4fv(program_.uMvp, 1, GL_FALSE, mvp);
        glUniformMatrix4fv(program_.uModel, 1, GL_FALSE, mvp);
        glUniform1i(useTextureLocation_, 0);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 10 * vertices),
                     im_batch_vertices(im_), GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 10, (void *)0);
        glDisableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(float) * 10,
                              (void *)(sizeof(float) * 6));
        glDrawArrays(GL_LINES, 0, vertices);
        glBindVertexArray(0);
    }

    MeshGpu *meshGpu(int meshIndex) {
        for (int i = 0; i < meshCount_; ++i)
            if (meshes_[i].meshIndex == meshIndex && meshes_[i].imageIndex == -1) return &meshes_[i];
        if (meshCount_ >= kMaxMeshGpu) return nullptr;
        DMesh *mesh = dmesh_get(meshIndex);
        if (!mesh || !mesh->positions || !mesh->indices) return nullptr;
        MeshGpu &gpu = meshes_[meshCount_++];
        gpu.meshIndex = meshIndex;
        gpu.vertexCount = mesh->vertex_count;
        gpu.indexCount = mesh->index_count;
        // Interleaved pos(3) normal(3) uv(2).
        const int stride = 8;
        float *packed = static_cast<float *>(std::malloc(sizeof(float) * stride * static_cast<size_t>(mesh->vertex_count)));
        if (!packed) { meshCount_--; return nullptr; }
        for (int v = 0; v < mesh->vertex_count; ++v) {
            std::memcpy(&packed[v * stride], &mesh->positions[v * 3], sizeof(float) * 3);
            if (mesh->normals) std::memcpy(&packed[v * stride + 3], &mesh->normals[v * 3], sizeof(float) * 3);
            else { packed[v * stride + 3] = 0.0f; packed[v * stride + 4] = 1.0f; packed[v * stride + 5] = 0.0f; }
            if (mesh->uvs) std::memcpy(&packed[v * stride + 6], &mesh->uvs[v * 2], sizeof(float) * 2);
            else { packed[v * stride + 6] = 0.0f; packed[v * stride + 7] = 0.0f; }
        }
        glGenBuffers(1, &gpu.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * stride * mesh->vertex_count),
                     packed, GL_STATIC_DRAW);
        glGenBuffers(1, &gpu.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(uint16_t) * mesh->index_count),
                     mesh->indices, GL_STATIC_DRAW);
        std::free(packed);
        return &gpu;
    }

    MeshGpu *spriteGpu(int imageIndex) {
        for (int i = 0; i < meshCount_; ++i)
            if (meshes_[i].imageIndex == imageIndex) return &meshes_[i];
        if (meshCount_ >= kMaxMeshGpu) return nullptr;
        DImage *image = dimage_get(imageIndex);
        if (!image || !image->rgba) return nullptr;
        MeshGpu &gpu = meshes_[meshCount_++];
        gpu.imageIndex = imageIndex;
        gpu.meshIndex = -1;
        const float aspect = static_cast<float>(image->height) / static_cast<float>(image->width > 0 ? image->width : 1);
        const float hw = 0.5f, hh = 0.5f * aspect;
        const float vertexData[] = {
            -hw, -hh, 0.0f, 0, 0, 1, 0, 0,
             hw, -hh, 0.0f, 0, 0, 1, 1, 0,
             hw,  hh, 0.0f, 0, 0, 1, 1, 1,
            -hw,  hh, 0.0f, 0, 0, 1, 0, 1};
        const uint16_t indexData[] = {0, 1, 2, 0, 2, 3};
        glGenBuffers(1, &gpu.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(vertexData), vertexData, GL_STATIC_DRAW);
        glGenBuffers(1, &gpu.ibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.ibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indexData), indexData, GL_STATIC_DRAW);
        glGenTextures(1, &gpu.texture);
        glBindTexture(GL_TEXTURE_2D, gpu.texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->width, image->height, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image->rgba);
        gpu.vertexCount = 4;
        gpu.indexCount = 6;
        return &gpu;
    }

    GlContext *context_ = nullptr;
    GlProgram program_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLint useTextureLocation_ = -1;
    int esVersion_ = 3;
    MeshGpu meshes_[kMaxMeshGpu];
    int meshCount_ = 0;
    ImBatch *im_ = nullptr;
    bool attached_ = false;
    double frameMsStart_ = 0.0;
    float frameMs_ = 0.0f;
    int drawCalls_ = 0;
    int triangles_ = 0;
    int sprites_ = 0;
    long frames_ = 0;
};

} // namespace

IRenderBackend *createGles3Backend() { return new Gles3Backend(); }

} // namespace diamond
