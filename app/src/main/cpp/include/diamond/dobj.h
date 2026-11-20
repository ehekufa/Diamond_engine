/* Diamond Engine — Wavefront OBJ loader (C++23 inside, C ABI outside). */
#ifndef DIAMOND_DOBJ_H
#define DIAMOND_DOBJ_H

#ifdef __cplusplus
extern "C" {
#endif

/* Parses OBJ text (v / vt / vn / f, polygon fan triangulation, negative
 * indices, "v/vt/vn", "v//vn", "v" face formats). The resulting mesh is added
 * to the scene and its index returned, or -1 with `err` filled. */
int dobj_parse_to_mesh(const char *text, int length, const char *name,
                       char *err, int err_cap);

/* Statistics for the log console, valid after the last parse call. */
void dobj_last_stats(int *vertices, int *triangles, int *objects, int *ignored);

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DOBJ_H */
