/* Diamond Engine — scene graph storage (C99).
 *
 * The scene is a plain array of nodes with first-child / next-sibling links.
 * Everything (editor UI, .diamond serializer, block executor, renderer) walks
 * the same arrays, so there is exactly one source of truth.
 */
#ifndef DIAMOND_DSCENE_H
#define DIAMOND_DSCENE_H

#include <stdint.h>

#include "diamond.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DNODE_NAME_MAX 40
#define DMESH_NAME_MAX 32
#define DNODE_ROOT 0

typedef struct DNode {
    int id;
    int type;        /* DiamondNodeType          */
    int parent;      /* 0 = root                 */
    int first_child;
    int next_sibling;
    int prev_sibling;
    char name[DNODE_NAME_MAX];
    float px, py, pz;   /* position            */
    float rx, ry, rz;   /* rotation, degrees   */
    float sx, sy, sz;   /* scale               */
    uint32_t color;     /* ABGR (0xAABBGGRR)   */
    int collision;      /* AABB collision flag */
    int mesh;           /* index or -1         */
    int image;          /* index or -1         */
    int primitive;      /* DiamondPrimitive/-1 */
    int visible;
} DNode;

typedef struct DMesh {
    char name[DMESH_NAME_MAX];
    int vertex_count;
    int index_count;
    float *positions;   /* 3 floats per vertex */
    float *normals;     /* 3 floats per vertex, may be NULL */
    float *uvs;         /* 2 floats per vertex, may be NULL */
    uint16_t *indices;  /* triangles           */
    float bounds[6];    /* xmin,ymin,zmin,xmax,ymax,zmax */
} DMesh;

typedef struct DImage {
    int width;
    int height;
    uint8_t *rgba;      /* width*height*4 */
} DImage;

/* ---- lifecycle -------------------------------------------------------- */
void dscene_init(void);
void dscene_shutdown(void);
void dscene_clear(void);                 /* fresh empty scene + root node   */

/* ---- nodes ------------------------------------------------------------ */
DNode *dnode_get(int id);
int    dnode_create(int type, int parent, const char *name);
int    dnode_delete(int id);             /* recursive, never deletes root   */
int    dnode_duplicate(int id);
int    dnode_reparent(int id, int parent);
void   dnode_set_name(int id, const char *name);
void   dnode_get_transform(int id, float *out9);
void   dnode_set_transform(int id, const float *in9);
void   dnode_set_color(int id, uint32_t abgr);
void   dnode_set_collision(int id, int enabled);
int    dnode_world_aabb(int id, float *out6);

int    dscene_node_count(void);          /* excluding root                  */
int    dscene_dfs_at(int index);         /* depth-first id, -1 if out of range */
int    dscene_node_at(int index);
int    dscene_flat_count(void);          /* including all slots             */

/* ---- meshes ----------------------------------------------------------- */
/* Takes ownership of the buffers on success; they are freed by dscene. */
int    dmesh_add(const char *name, int vertex_count, float *positions,
                 float *normals, float *uvs, int index_count, uint16_t *indices);
DMesh *dmesh_get(int index);
int    dmesh_count(void);
int    dmesh_add_primitive(int primitive);   /* built-in cube / pyramid / quad */
void   dmesh_compute_bounds(DMesh *mesh);

/* ---- images ----------------------------------------------------------- */
/* Copies `rgba` (width*height*4 bytes). Returns the image index or -1. */
int    dimage_add(const void *rgba, int width, int height);
DImage *dimage_get(int index);
int    dimage_count(void);
int    dimage_set(int node_id, int image_index);

/* ---- selection -------------------------------------------------------- */
void   dscene_select(int id);
int    dscene_selected(void);

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DSCENE_H */
