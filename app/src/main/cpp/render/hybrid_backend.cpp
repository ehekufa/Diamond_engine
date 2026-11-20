// Diamond Engine — Hybrid backend: OpenGL ES 3.0 for 3D, software for 2D.
//
// Why this exists at all: on a 2014 phone the cost of 200 alpha-blended
// sprites is mostly driver overhead, while the same 200 sprites drawn by the
// CPU into a 720p surface cost a few milliseconds of NEON work and zero draw
// calls. The hybrid path keeps the 3D scene on the GPU (lighting, meshes) and
// composites every 2D element — sprites, particles, HUD, selection marquee —
// with the software rasteriser, then blits the result as one textured quad.
#include "renderer.h"

#include <GLES3/gl3.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "diamond/dlog.h"
#include "diamond/dmath.h"
#include "diamond/dsoftware.h"
#include "diamond/dutil.h"
}

namespace diamond {

namespace {

// Reuse the GL3 shaders for the 3D pass.
const char *kVertexShader = R"(#version 300 es
precision highp float;
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
uniform mat4 uMvp;
uniform mat4 uModel;
out vec3 vNormal;
out vec2 vUv;
void main() {
    vNormal = mat3(uModel) * aNormal;
    vUv = aUv;
    gl_Position = uMvp * vec4(aPos, 1.0);
}
)";

const char *kFragmentShader = R"(#version 300 es
precision mediump float;
in vec3 vNormal;
in vec2 vUv;
uniform vec4 uColor;
uniform sampler2D uTex;
uniform int uUseTexture;
out vec4 fragColor;
void main() {
    vec4 base = uColor;
    if (uUseTexture == 1) base *= texture(uTex, vUv);
    vec3 n = normalize(vNormal);
    float diffuse = max(dot(n, normalize(vec3(-0.4, -1.0, -0.55))), 0.0) * 0.8;
    fragColor = vec4(base.rgb * (0.45 + diffuse), base.a);
}
)";

const char *kCompositeVertex = R"(#version 300 es
precision highp float;
layout(location = 0) in vec2 aPos;
layout(location = 2) in vec2 aUv;
out vec2 vUv;
void main() {
    vUv = aUv;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

const char *kCompositeFragment = R"(#version 300 es
precision mediump float;
in vec2 vUv;
uniform sampler2D uTex;
out vec4 fragColor;
void main() { fragColor = texture(uTex, vUv); }
)";

class HybridBackend final : public IRenderBackend {
public:
    ~HybridBackend() override { detach(); }

    const char *name() const override { return "Hybrid (GL 3D + software 2D)"; }
    int mode() const override { return DIAMOND_RENDER_HYBRID; }

    bool attach(void *window, int width, int height, char *err, int errCap) override {
        detach();
        width_ = width;
        height_ = height;
        context_ = gl_context_create(window, err, errCap);
        if (!context_) return false;
        int esVersion = 0;
        gl_context_info(context_, &esVersion, nullptr, nullptr, nullptr, nullptr);
        if (esVersion < 3) {
            diamond_fail(DIAMOND_ERR_UNSUPPORTED, err, errCap,
                         "hybrid mode needs OpenGL ES 3.0 (device reports ES%d)", esVersion);
            detach();
            return false;
        }
        if (!gl_program_link(&sceneProgram_, kVertexShader, kFragmentShader, "hybrid-3d", nullptr) ||
            !gl_program_link(&compositeProgram_, kCompositeVertex, kCompositeFragment, "hybrid-blit", nullptr)) {
            diamond_fail(DIAMOND_ERR_GPU, err, errCap, "hybrid shaders failed");
            detach();
            return false;
        }
        // Software target is half resolution: 2D art upscales gracefully, and
        // the fill rate drops 4x on a Mali-400.
        softwareWidth_ = width > 0 ? (width + 1) / 2 : 1;
        softwareHeight_ = height > 0 ? (height + 1) / 2 : 1;
        if (!software_target_resize(softwareWidth_, softwareHeight_)) {
            diamond_fail(DIAMOND_ERR_MEMORY, err, errCap, "cannot allocate software 2D target");
            detach();
            return false;
        }
        software_clear(0x00000000u);

        glGenBuffers(1, &quadVbo_);
        const float quad[] = {-1, -1, 0, 0,  1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 0};
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glGenBuffers(1, &quadIbo_);
        const uint16_t indices[] = {0, 1, 2, 0, 2, 3};
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadIbo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

        glGenVertexArrays(1, &quadVao_);
        glBindVertexArray(quadVao_);
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (void *)0);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 5, (void *)(sizeof(float) * 3));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadIbo_);
        glBindVertexArray(0);

        glGenTextures(1, &softwareTexture_);
        glBindTexture(GL_TEXTURE_2D, softwareTexture_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glClearColor(0.09f, 0.10f, 0.12f, 1.0f);
        im_ = im_batch_create();
        attached_ = true;
        dlog_write(DLOG_INFO, "hybrid", "attached: 3D on GPU, 2D on CPU at %dx%d",
                   softwareWidth_, softwareHeight_);
        return true;
    }

    void detach() override {
        software_target_release();
        if (im_) { im_batch_destroy(im_); im_ = nullptr; }
        if (context_) {
            if (quadVbo_) glDeleteBuffers(1, &quadVbo_);
            if (quadIbo_) glDeleteBuffers(1, &quadIbo_);
            if (quadVao_) glDeleteVertexArrays(1, &quadVao_);
            if (softwareTexture_) glDeleteTextures(1, &softwareTexture_);
            gl_program_release(&sceneProgram_);
            gl_program_release(&compositeProgram_);
            gl_context_destroy(context_);
        }
        context_ = nullptr;
        quadVbo_ = quadIbo_ = quadVao_ = softwareTexture_ = 0;
        attached_ = false;
    }

    void resize(int width, int height) override {
        width_ = width;
        height_ = height;
        softwareWidth_ = width > 0 ? (width + 1) / 2 : 1;
        softwareHeight_ = height > 0 ? (height + 1) / 2 : 1;
        software_target_resize(softwareWidth_, softwareHeight_);
    }

    bool draw(const FrameContext &frame) override {
        if (!attached_ || !context_ || gl_context_lost(context_)) return false;
        const double start = dtime_seconds();
        glViewport(0, 0, frame.width, frame.height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        float view[16], projection[16], vp[16];
        dcamera_matrices(frame.camera, frame.viewMode, frame.width, frame.height, view, projection);
        dmat4_multiply(vp, projection, view);
        draw3D(frame, vp);
        draw2D(frame);
        composite();
        gl_context_swap(context_);
        frameMs_ = static_cast<float>((dtime_seconds() - start) * 1000.0);
        return true;
    }

    DRenderStats stats() const override {
        DRenderStats s{};
        s.backend = DIAMOND_RENDER_HYBRID;
        s.frame_ms = frameMs_;
        s.sprites = sprites_;
        s.triangles = triangles_;
        s.draw_calls = drawCalls_;
        s.width = width_;
        s.height = height_;
        return s;
    }

private:
    void draw3D(const FrameContext &frame, const float *vp) {
        triangles_ = 0;
        glUseProgram(gl_program_id(&sceneProgram_));
        glUniform1i(sceneProgram_.uUseTexture, 0);
        glUniform1i(sceneProgram_.uTex, 0);
        glUniform1i(sceneProgram_.uColor, 0);
        // Grid first, then meshes.
        im_batch_clear(im_);
        if (frame.viewMode != DIAMOND_VIEW_2D) im_batch_grid(im_, 16, 1.0f, 0x502A2F3A, 0x90606A80);
        const int gridVertices = im_batch_vertex_count(im_);
        if (gridVertices > 0) {
            glUniformMatrix4fv(sceneProgram_.uMvp, 1, GL_FALSE, vp);
            glUniformMatrix4fv(sceneProgram_.uModel, 1, GL_FALSE, vp);
            glUniform4f(sceneProgram_.uColor, 1, 1, 1, 1);
            glBindBuffer(GL_ARRAY_BUFFER, quadVbo_); // reused as scratch
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 10 * gridVertices),
                         im_batch_vertices(im_), GL_STREAM_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * 10, (void *)0);
            glDisableVertexAttribArray(1);
            glDisableVertexAttribArray(2);
            glDrawArrays(GL_LINES, 0, gridVertices);
        }

        const int count = dscene_flat_count();
        for (int i = 1; i < count; ++i) {
            DNode *node = dnode_get(i);
            if (!node || !node->visible || node->mesh < 0) continue;
            DMesh *mesh = dmesh_get(node->mesh);
            if (!mesh) continue;
            float model[16], mvp[16];
            dnode_world_matrix(node, model);
            dmat4_multiply(mvp, vp, model);
            glUniformMatrix4fv(sceneProgram_.uMvp, 1, GL_FALSE, mvp);
            glUniformMatrix4fv(sceneProgram_.uModel, 1, GL_FALSE, model);
            const uint32_t color = dnode_resolved_color(node);
            glUniform4f(sceneProgram_.uColor,
                        static_cast<float>(color & 0xFF) / 255.0f,
                        static_cast<float>((color >> 8) & 0xFF) / 255.0f,
                        static_cast<float>((color >> 16) & 0xFF) / 255.0f,
                        static_cast<float>((color >> 24) & 0xFF) / 255.0f);
            uploadMesh(mesh);
            triangles_ += mesh->index_count / 3;
            drawCalls_++;
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    void uploadMesh(DMesh *mesh) {
        glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * 8 * mesh->vertex_count),
                     nullptr, GL_STREAM_DRAW);
        // Interleave on the fly into a scratch buffer (no exceptions, no vector).
        const int stride = 8;
        if (scratchCapacity_ < mesh->vertex_count) {
            std::free(scratch_);
            scratch_ = static_cast<float *>(std::malloc(sizeof(float) * stride * static_cast<size_t>(mesh->vertex_count)));
            scratchCapacity_ = scratch_ ? mesh->vertex_count : 0;
            if (!scratch_) return;
        }
        for (int v = 0; v < mesh->vertex_count; ++v) {
            std::memcpy(&scratch_[v * stride], &mesh->positions[v * 3], sizeof(float) * 3);
            if (mesh->normals) std::memcpy(&scratch_[v * stride + 3], &mesh->normals[v * 3], sizeof(float) * 3);
            else { scratch_[v * stride + 3] = 0; scratch_[v * stride + 4] = 1; scratch_[v * stride + 5] = 0; }
            if (mesh->uvs) std::memcpy(&scratch_[v * stride + 6], &mesh->uvs[v * 2], sizeof(float) * 2);
            else { scratch_[v * stride + 6] = 0; scratch_[v * stride + 7] = 0; }
        }
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(float) * stride * mesh->vertex_count),
                     scratch_, GL_STREAM_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(float) * stride, (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(float) * stride, (void *)(sizeof(float) * 3));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(float) * stride, (void *)(sizeof(float) * 6));
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quadIbo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(uint16_t) * mesh->index_count),
                     mesh->indices, GL_STREAM_DRAW);
        glDrawElements(GL_TRIANGLES, mesh->index_count, GL_UNSIGNED_SHORT, nullptr);
    }

    void draw2D(const FrameContext &frame) {
        sprites_ = 0;
        software_clear(0x00000000u);

        const float scaleX = static_cast<float>(softwareWidth_) / static_cast<float>(width_ > 0 ? width_ : 1);
        const float scaleY = static_cast<float>(softwareHeight_) / static_cast<float>(height_ > 0 ? height_ : 1);

        // Sprites: project their world position to screen through the view
        // matrix so 2D elements track the camera exactly like the 3D scene.
        float view[16], projection[16], vp[16];
        dcamera_matrices(frame.camera, frame.viewMode, width_, height_, view, projection);
        dmat4_multiply(vp, projection, view);

        const int count = dscene_flat_count();
        for (int i = 1; i < count; ++i) {
            DNode *node = dnode_get(i);
            if (!node || !node->visible || node->image < 0) continue;
            DImage *image = dimage_get(node->image);
            if (!image || !image->rgba) continue;
            float point[3] = {node->px, node->py, node->pz};
            float clip[4];
            dmat4_transform_point(vp, point, clip);
            if (clip[3] <= 0.0001f) continue;
            const float ndcX = clip[0] / clip[3];
            const float ndcY = clip[1] / clip[3];
            const float screenX = (ndcX * 0.5f + 0.5f) * static_cast<float>(softwareWidth_);
            const float screenY = (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(softwareHeight_);
            const float spriteWidth = image->width * 0.01f * node->sx * 40.0f * scaleX;
            const float spriteHeight = image->height * 0.01f * node->sy * 40.0f * scaleY;
            software_blit_sprite(image->rgba, image->width, image->height,
                                 screenX, screenY, spriteWidth, spriteHeight,
                                 dmath_radians(node->rz), node->color, 1);
            sprites_++;
        }

        // Selection marquee + HUD.
        if (frame.selected > 0) {
            soft_fill_rect_outline(8.0f, 8.0f,
                                   static_cast<float>(softwareWidth_) - 16.0f,
                                   static_cast<float>(softwareHeight_) - 16.0f,
                                   0xFF37D67Au);
        }
        if (frame.playing) drawPlayHud(frame);
    }

    void soft_fill_rect_outline(float x, float y, float w, float h, uint32_t abgr) {
        software_fill_rect(x, y, w, 2.0f, abgr);
        software_fill_rect(x, y + h - 2.0f, w, 2.0f, abgr);
        software_fill_rect(x, y, 2.0f, h, abgr);
        software_fill_rect(x + w - 2.0f, y, 2.0f, h, abgr);
    }

    void drawPlayHud(const FrameContext &frame) {
        const float cx = static_cast<float>(softwareWidth_) * 0.16f;
        const float cy = static_cast<float>(softwareHeight_) * 0.72f;
        const float radius = static_cast<float>(softwareHeight_) * 0.16f;
        soft_fill_rect_outline(cx - radius, cy - radius, radius * 2.0f, radius * 2.0f, 0x60FFFFFFu);
        const float knobX = cx + frame.joystick[0] * radius;
        const float knobY = cy - frame.joystick[1] * radius;
        software_fill_rect(knobX - 12.0f, knobY - 12.0f, 24.0f, 24.0f, 0xFFFFFFFFu);
        const float bx = static_cast<float>(softwareWidth_) - radius * 1.4f;
        const float by = cy;
        software_fill_rect(bx, by, 44.0f, 44.0f,
                           frame.buttons[DIAMOND_BTN_A] ? 0xFFFFD060u : 0x60FFFFFFu);
        software_fill_rect(bx - 70.0f, by + 10.0f, 44.0f, 44.0f,
                           frame.buttons[DIAMOND_BTN_B] ? 0xFF60D0FFu : 0x60FFFFFFu);
    }

    void composite() {
        int width = 0, height = 0;
        const void *pixels = software_target_pixels(&width, &height);
        if (!pixels || width <= 0 || height <= 0) return;
        glUseProgram(gl_program_id(&compositeProgram_));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, softwareTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glUniform1i(compositeProgram_.uTex, 0);
        glDisable(GL_DEPTH_TEST);
        glBindVertexArray(quadVao_);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        glBindVertexArray(0);
        glEnable(GL_DEPTH_TEST);
        drawCalls_++;
    }

    GlContext *context_ = nullptr;
    GlProgram sceneProgram_;
    GlProgram compositeProgram_;
    GLuint quadVbo_ = 0, quadIbo_ = 0, quadVao_ = 0, softwareTexture_ = 0;
    float *scratch_ = nullptr;
    int scratchCapacity_ = 0;
    ImBatch *im_ = nullptr;
    int softwareWidth_ = 1, softwareHeight_ = 1;
    bool attached_ = false;
    float frameMs_ = 0.0f;
    int drawCalls_ = 0, triangles_ = 0, sprites_ = 0;
};

} // namespace

IRenderBackend *createHybridBackend() { return new HybridBackend(); }

} // namespace diamond
