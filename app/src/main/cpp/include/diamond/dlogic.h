/* Diamond Engine — visual logic: blocks, LuaC code generation, safe execution.
 *
 * The block graph is the single authoring format. It is converted to Lua source
 * (dlogic_generate) which is what LuaC compiles natively (luac_compile_bundle_c)
 * or what the built-in executor runs when LuaC is unavailable.
 *
 * Debug story: LuaC has no bytecode inspector, so every executed statement and
 * every call into the engine goes through ENGINE_SAFE_CALL / dlogic_step(),
 * which decrements a per-frame instruction budget and records the block id
 * that was running when something failed. The UI gets "block 12: engine_move
 * failed: unknown node" instead of a native crash.
 */
#ifndef DIAMOND_DLOGIC_H
#define DIAMOND_DLOGIC_H

#include <stdint.h>

#include "diamond.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Block kinds — values are part of the Java contract. */
typedef enum DBlockKind {
    DBLOCK_ON_START     = 0,
    DBLOCK_ON_UPDATE    = 1,
    DBLOCK_ON_TOUCH     = 2,
    DBLOCK_ON_COLLIDE   = 3,
    DBLOCK_MOVE         = 10,
    DBLOCK_ROTATE       = 11,
    DBLOCK_SET_COLOR    = 12,
    DBLOCK_LOG          = 13,
    DBLOCK_SET_POS      = 14,
    DBLOCK_SET_SCALE    = 15,
    DBLOCK_IF_BUTTON    = 20,
    DBLOCK_IF_TIMER     = 21,
    DBLOCK_IF_COLLIDE   = 22,
    DBLOCK_IF_VAR       = 23,
    DBLOCK_END          = 90,
    DBLOCK_KIND_COUNT   = 91
} DBlockKind;

typedef struct DBlock {
    int id;
    int kind;
    int node;      /* target node id, 0 = the node that owns the graph */
    int param;     /* button index, primitive, comparison op           */
    char text[64];
    float x, y;
    float a, b, c;
} DBlock;

typedef struct DLink {
    int id;
    int from;
    int to;
} DLink;

/* ---- graph editing ---------------------------------------------------- */
void   dlogic_init(void);
void   dlogic_shutdown(void);
void   dlogic_clear(void);

int    dlogic_block_add(int kind, int node, int param, float x, float y,
                        const char *text, float a, float b, float c);
/* Restores a block with an explicit id (used by the .diamond loader). */
int    dlogic_block_add_explicit(int id, int kind, int node, int param,
                                 const char *text, float x, float y,
                                 float a, float b, float c);
int    dlogic_block_remove(int id);
void   dlogic_block_move(int id, float x, float y);
DBlock *dlogic_block_get(int id);
int    dlogic_block_count(void);
int    dlogic_block_at(int index);

int    dlogic_link(int from, int to);
int    dlogic_unlink(int link_id);
int    dlogic_has_link(int from, int to);
int    dlogic_link_count(void);
DLink *dlogic_link_get(int index);

/* ---- code generation -------------------------------------------------- */
/* Produces Lua source for the whole graph. The returned string is owned by the
 * engine and stays valid until the next graph mutation. */
const char *dlogic_generate(void);
const char *dlogic_generated_source(void);
void        dlogic_set_source(const char *text);

/* Parses the generated (or hand edited) source back into blocks — only used
 * when a scene ships code without a block graph. */
int dlogic_blocks_from_source(const char *text, int length);

/* JSON export/import for the Java block editor (positions + kinds + links). */
const char *dlogic_export_json(void);
const char *dlogic_kind_name(int kind);
int dlogic_validate(char *err, int err_cap);
int dlogic_import_json(const char *json, int length, char *err, int err_cap);

/* ---- execution -------------------------------------------------------- */
void dlogic_set_program(const char *source, int length);  /* compiles/validates */
int  dlogic_start(void);      /* fires OnStart chains                 */
void dlogic_update(float dt); /* fires OnUpdate chains, steps budget   */
void dlogic_stop(void);
int  dlogic_playing(void);

/* Input hooks, called from the Java touch handler. */
void dlogic_on_touch(int pointer, float x, float y, int down);
void dlogic_on_collide(int node_a, int node_b);
void dlogic_button(int button, int pressed);
int  dlogic_button_pressed(int button);

/* Debug statistics (surfaced in the log console). */
void dlogic_stats(int64_t *steps, int64_t *errors, int *last_block);

/* ---- engine callbacks used by the executor and by LuaC --------------- */
/* These are the C ABI functions named in the brief:
 *   engine_set_pos / engine_move / engine_rotate / engine_is_btn_pressed /
 *   engine_check_aabb
 * They are exported from the .so so compiled LuaC code can call them directly. */
DIAMOND_API void  engine_set_pos(int node, float x, float y, float z);
DIAMOND_API void  engine_move(int node, float dx, float dy, float dz);
DIAMOND_API void  engine_rotate(int node, float rx, float ry, float rz);
DIAMOND_API int   engine_is_btn_pressed(int button);
DIAMOND_API int   engine_check_aabb(int a, int b);
DIAMOND_API void  engine_log(const char *message);
DIAMOND_API float engine_time(void);
DIAMOND_API float engine_delta(void);
DIAMOND_API void  engine_set_scale(int node, float sx, float sy, float sz);
DIAMOND_API void  engine_set_color_rgba(int node, int r, int g, int b, int a);
DIAMOND_API int   engine_timer(int blockId, float seconds);
DIAMOND_API int   engine_var(const char *name, int comparison, float value);

/* LuaC host glue: returns 0 if LuaC is not compiled in. */
int dlogic_luac_available(void);
const char *dlogic_luac_version(void);
int dlogic_luac_check(const char *source, int length, char *err, int err_cap);
const char *dlogic_luac_compile(const char *source, int length, char *err, int err_cap);
void dlogic_luac_free(char *text);
int  dlogic_generate_into(char *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DLOGIC_H */
