#version 450
// Diamond Engine — Vulkan scene fragment stage (flat-shaded, no textures yet).
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main() {
    vec3 n = normalize(vNormal);
    // Fixed key light from above/behind the camera, same as the GLES3 backend,
    // so switching render modes does not change how a scene looks.
    vec3 light = normalize(vec3(-0.4, 0.8, 0.45));
    float diffuse = max(dot(n, light), 0.0) * 0.75;
    vec3 lit = vColor.rgb * (0.45 + diffuse);
    fragColor = vec4(lit, vColor.a);
}
