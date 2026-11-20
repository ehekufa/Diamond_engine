// Diamond Engine — linear algebra kernel.
//
// Column-major matrices (OpenGL convention). Everything is float32: mobile
// GPUs consume floats, and double math on armeabi-v7a would double the work
// for no visible gain.
#include "diamond/dmath.h"

#include <cmath>
#include <cstring>

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

extern "C" {

void dmat4_identity(float *m) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void dmat4_multiply(float *out, const float *a, const float *b) {
    float r[16];
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[column * 4 + k];
            r[column * 4 + row] = sum;
        }
    }
    std::memcpy(out, r, sizeof(r));
}

void dmat4_perspective(float *m, float fov_degrees, float aspect, float near_plane, float far_plane) {
    const float f = 1.0f / std::tan(dmath_radians(fov_degrees) * 0.5f);
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = f / (aspect > 0.0f ? aspect : 1.0f);
    m[5] = f;
    m[10] = (far_plane + near_plane) / (near_plane - far_plane);
    m[11] = -1.0f;
    m[14] = (2.0f * far_plane * near_plane) / (near_plane - far_plane);
}

void dmat4_ortho(float *m, float left, float right, float bottom, float top, float near_plane, float far_plane) {
    std::memset(m, 0, sizeof(float) * 16);
    m[0] = 2.0f / (right - left);
    m[5] = 2.0f / (top - bottom);
    m[10] = -2.0f / (far_plane - near_plane);
    m[12] = -(right + left) / (right - left);
    m[13] = -(top + bottom) / (top - bottom);
    m[14] = -(far_plane + near_plane) / (far_plane - near_plane);
    m[15] = 1.0f;
}

void dmat4_look_at(float *m, const float *eye, const float *center, const float *up) {
    float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
    float s[3], u[3];
    dvec3_normalize(f);
    dvec3_cross(s, f, up);
    dvec3_normalize(s);
    dvec3_cross(u, s, f);

    std::memset(m, 0, sizeof(float) * 16);
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    m[12] = -dvec3_dot(s, eye);
    m[13] = -dvec3_dot(u, eye);
    m[14] = dvec3_dot(f, eye);
    m[15] = 1.0f;
}

void dmat4_translate(float *m, float x, float y, float z) {
    dmat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

void dmat4_rotate_x(float *m, float degrees) {
    const float c = std::cos(dmath_radians(degrees));
    const float s = std::sin(dmath_radians(degrees));
    dmat4_identity(m);
    m[5] = c; m[6] = s; m[9] = -s; m[10] = c;
}

void dmat4_rotate_y(float *m, float degrees) {
    const float c = std::cos(dmath_radians(degrees));
    const float s = std::sin(dmath_radians(degrees));
    dmat4_identity(m);
    m[0] = c; m[2] = -s; m[8] = s; m[10] = c;
}

void dmat4_rotate_z(float *m, float degrees) {
    const float c = std::cos(dmath_radians(degrees));
    const float s = std::sin(dmath_radians(degrees));
    dmat4_identity(m);
    m[0] = c; m[1] = s; m[4] = -s; m[5] = c;
}

void dmat4_scale(float *m, float x, float y, float z) {
    dmat4_identity(m);
    m[0] = x; m[5] = y; m[10] = z;
}

void dmat4_from_trs(float *out, const float *trs9) {
    float t[16], rx[16], ry[16], rz[16], s[16], tmp[16];
    dmat4_translate(t, trs9[0], trs9[1], trs9[2]);
    dmat4_rotate_x(rx, trs9[3]);
    dmat4_rotate_y(ry, trs9[4]);
    dmat4_rotate_z(rz, trs9[5]);
    dmat4_scale(s, trs9[6], trs9[7], trs9[8]);

    dmat4_multiply(tmp, ry, rx);     // Ry * Rx
    dmat4_multiply(tmp, rz, tmp);    // Rz * Ry * Rx
    dmat4_multiply(tmp, tmp, s);     // R * S
    dmat4_multiply(out, t, tmp);     // T * R * S
}

void dmat4_transform_point(const float *m, const float *p, float *out4) {
    out4[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    out4[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    out4[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
    out4[3] = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
}

void dvec3_cross(float *out, const float *a, const float *b) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float dvec3_dot(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void dvec3_normalize(float *v) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 1e-6f) {
        v[0] /= length; v[1] /= length; v[2] /= length;
    } else {
        v[0] = 0.0f; v[1] = 0.0f; v[2] = 1.0f;
    }
}

float dmath_clamp(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

float dmath_lerp(float a, float b, float t) { return a + (b - a) * t; }

float dmath_radians(float degrees) { return degrees * kPi / 180.0f; }

} // extern "C"
