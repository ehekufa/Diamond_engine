/* Diamond Engine — minimal linear algebra (C ABI, usable from C99 and C++). */
#ifndef DIAMOND_DMATH_H
#define DIAMOND_DMATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Column-major 4x4, the layout OpenGL expects. */
void dmat4_identity(float *m);
void dmat4_multiply(float *out, const float *a, const float *b);
void dmat4_perspective(float *m, float fov_degrees, float aspect, float near_plane, float far_plane);
void dmat4_ortho(float *m, float left, float right, float bottom, float top, float near_plane, float far_plane);
void dmat4_look_at(float *m, const float *eye, const float *center, const float *up);
void dmat4_translate(float *m, float x, float y, float z);
void dmat4_rotate_x(float *m, float degrees);
void dmat4_rotate_y(float *m, float degrees);
void dmat4_rotate_z(float *m, float degrees);
void dmat4_scale(float *m, float x, float y, float z);
/* out = T * Rz * Ry * Rx * S */
void dmat4_from_trs(float *out, const float *trs9);
/* Transforms a point (w = 1) and writes 4 components. */
void dmat4_transform_point(const float *m, const float *p, float *out4);

/* 3D helpers used by the camera and by the block conditions. */
void dvec3_cross(float *out, const float *a, const float *b);
float dvec3_dot(const float *a, const float *b);
void dvec3_normalize(float *v);
float dmath_clamp(float value, float low, float high);
float dmath_lerp(float a, float b, float t);
float dmath_radians(float degrees);

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DMATH_H */
