#version 450
// Diamond Engine — Vulkan scene vertex stage.
// Matches the GLES3 backend's attribute layout (position, normal, uv, colour)
// so a mesh uploaded for one backend works for the other.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUv;
layout(location = 2) out vec4 vColor;

void main() {
    vNormal = mat3(pc.model) * aNormal;
    vUv = aUv;
    vColor = aColor;
    gl_Position = pc.mvp * vec4(aPos, 1.0);
}
