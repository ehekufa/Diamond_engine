/* Diamond Engine — block graph executor (C99).
 *
 * This is the USE_BUILTIN_FALLBACK path: the same graph that becomes Lua for
 * LuaC is interpreted here directly. It is small on purpose (no compiler,
 * no allocator, no string building) and it is what runs on a phone that ships
 * without the LuaC sources, so the editor is always playable.
 *
 * Every action goes through a validation step: node ids are checked, NaN is
 * rejected, and failures are attributed to the block id that caused them.
 */
#include "diamond/dlogic.h"
#include "diamond/dlogic_dbg.h"
#include "diamond/dscene.h"
#include "diamond/dlog.h"

#include <math.h>
#include <string.h>

#define DLOGIC_MAX_DEPTH 32

static DLogicCtx g_ctx;
static int g_playing;
static float g_time;
static float g_delta;
static int g_button_state[DIAMOND_BTN_COUNT];
static float g_touch_x, g_touch_y;
static int g_touch_down;
static float g_timer_accum[4096];

static void engine_log_line(const char *message) {
    dlog_raw(DLOG_LOGIC, "logic", message ? message : "");
}

void engine_log(const char *message) { engine_log_line(message); }
float engine_time(void) { return g_time; }
float engine_delta(void) { return g_delta; }
int engine_is_btn_pressed(int button) {
    if (button < 0 || button >= DIAMOND_BTN_COUNT) return 0;
    return g_button_state[button];
}

void engine_set_pos(int node, float x, float y, float z) {
    DNode *target = dnode_get(node);
    if (!target) { dlog_write(DLOG_ERROR, "logic", "engine_set_pos: node %d does not exist", node); return; }
    if (!isfinite(x) || !isfinite(y) || !isfinite(z)) { dlog_write(DLOG_ERROR, "logic", "engine_set_pos: non-finite value"); return; }
    target->px = x; target->py = y; target->pz = z;
}

void engine_move(int node, float dx, float dy, float dz) {
    DNode *target = dnode_get(node);
    if (!target) { dlog_write(DLOG_ERROR, "logic", "engine_move: node %d does not exist", node); return; }
    if (!isfinite(dx) || !isfinite(dy) || !isfinite(dz)) { dlog_write(DLOG_ERROR, "logic", "engine_move: non-finite delta"); return; }
    target->px += dx; target->py += dy; target->pz += dz;
}

void engine_rotate(int node, float rx, float ry, float rz) {
    DNode *target = dnode_get(node);
    if (!target) { dlog_write(DLOG_ERROR, "logic", "engine_rotate: node %d does not exist", node); return; }
    if (!isfinite(rx) || !isfinite(ry) || !isfinite(rz)) return;
    target->rx += rx; target->ry += ry; target->rz += rz;
}

int engine_check_aabb(int a, int b) {
    float box_a[6], box_b[6];
    if (!dnode_world_aabb(a, box_a) || !dnode_world_aabb(b, box_b)) return 0;
    if (box_a[3] < box_b[0] || box_b[3] < box_a[0]) return 0;
    if (box_a[4] < box_b[1] || box_b[4] < box_a[1]) return 0;
    if (box_a[5] < box_b[2] || box_b[5] < box_a[2]) return 0;
    return 1;
}

/* Sets a full colour from four 0-255 channels. */
static void set_color_channels(int node, int r, int g, int b, int a) {
    DNode *target = dnode_get(node);
    if (!target) return;
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    a = a <= 0 ? 255 : (a > 255 ? 255 : a);
    target->color = ((unsigned)a << 24) | ((unsigned)b << 16) | ((unsigned)g << 8) | (unsigned)r;
}

static void set_color_packed(int node, int rgba) {
    DNode *target = dnode_get(node);
    if (!target) return;
    target->color = (unsigned)rgba;
}

static void set_scale3(int node, float x, float y, float z) {
    DNode *target = dnode_get(node);
    if (!target || !isfinite(x) || !isfinite(y) || !isfinite(z)) return;
    target->sx = x; target->sy = y; target->sz = z;
}

static int var_compare(const char *name, int comparison, float value);

/* Public ABI: the same operations, reachable from code that LuaC compiles.
 * Keeping thin wrappers (instead of exporting the internals directly) means a
 * backend tick keeps its DLogicCtx for safe-call bookkeeping. */
void engine_set_scale(int node, float x, float y, float z) { set_scale3(node, x, y, z); }
void engine_set_color_rgba(int node, int r, int g, int b, int a) {
    set_color_channels(node, r, g, b, a);
}
int engine_var(const char *name, int comparison, float value) {
    return var_compare(name, comparison, value);
}

/* Timer helper used by the "If Variable / Timer" condition. */
static int timer_elapsed(const DLogicCtx *ctx, int block_id, float seconds) {
    if (block_id < 0 || block_id >= (int)(sizeof(g_timer_accum) / sizeof(g_timer_accum[0]))) return 0;
    if (seconds <= 0.0f) seconds = 1.0f;
    g_timer_accum[block_id] += g_delta;
    if (g_timer_accum[block_id] >= seconds) {
        g_timer_accum[block_id] -= seconds;
        (void)ctx;
        return 1;
    }
    return 0;
}

int engine_timer(int block_id, float seconds) { return timer_elapsed(&g_ctx, block_id, seconds); }

static int var_compare(const char *name, int comparison, float value) {
    /* Variables are exposed through the scene name -> value map in the future;
     * today the only built-in variable is the scene clock, which covers the
     * "wait N seconds" pattern people actually use on mobile. */
    float current = 0.0f;
    if (!name) return 0;
    if (strcmp(name, "time") == 0) current = g_time;
    else if (strcmp(name, "delta") == 0) current = g_delta;
    else if (strcmp(name, "touch_x") == 0) current = g_touch_x;
    else if (strcmp(name, "touch_y") == 0) current = g_touch_y;
    if (comparison == 0) return current > value;
    if (comparison == 1) return current < value;
    return fabsf(current - value) < 0.0001f;
}

static int block_chain_guard[DLOGIC_MAX_DEPTH];

static void run_action(const DBlock *block) {
    if (!block) return;
    switch (block->kind) {
        case DBLOCK_MOVE:
            ENGINE_SAFE_CALL(&g_ctx, block->id, engine_move(block->node, block->a, block->b, block->c));
            break;
        case DBLOCK_ROTATE:
            ENGINE_SAFE_CALL(&g_ctx, block->id, engine_rotate(block->node, block->a, block->b, block->c));
            break;
        case DBLOCK_SET_POS:
            ENGINE_SAFE_CALL(&g_ctx, block->id, engine_set_pos(block->node, block->a, block->b, block->c));
            break;
        case DBLOCK_SET_SCALE:
            ENGINE_SAFE_CALL(&g_ctx, block->id, set_scale3(block->node, block->a, block->b, block->c));
            break;
        case DBLOCK_SET_COLOR:
            /* param carries the packed RGBA8888 colour (same layout as
             * DNode::color), so the block editor round-trips it unchanged. */
            ENGINE_SAFE_CALL(&g_ctx, block->id, set_color_packed(block->node, block->param));
            break;
        case DBLOCK_LOG:
            ENGINE_SAFE_CALL(&g_ctx, block->id, engine_log(block->text));
            break;
        default:
            break;
    }
}

static int condition_holds(const DBlock *block) {
    if (!block) return 0;
    switch (block->kind) {
        case DBLOCK_IF_BUTTON:
            return engine_is_btn_pressed(block->param);
        case DBLOCK_IF_TIMER:
            return timer_elapsed(&g_ctx, block->id, block->a);
        case DBLOCK_IF_COLLIDE:
            return engine_check_aabb(block->node, block->param);
        case DBLOCK_IF_VAR:
            return var_compare(block->text, block->param, block->b);
        default:
            return 0;
    }
}

/* Executes a chain starting at `start`; returns when the chain ends or the
 * budget/cycle guard stops it. */
static void run_chain(int start, int depth) {
    int current = start;
    int guard = 0;
    if (depth >= DLOGIC_MAX_DEPTH) return;
    if (block_chain_guard[depth] == start && start != 0) return;
    block_chain_guard[depth] = start;

    while (current != 0 && guard++ < 512) {
        DBlock *block = dlogic_block_get(current);
        if (!block) break;
        if (!dlogic_step(&g_ctx, block->id)) return;

        if (block->kind == DBLOCK_END) return;

        if (block->kind >= DBLOCK_IF_BUTTON && block->kind <= DBLOCK_IF_VAR) {
            if (condition_holds(block)) {
                for (int i = 0; i < dlogic_link_count(); ++i) {
                    DLink *link = dlogic_link_get(i);
                    if (link && link->from == block->id) run_chain(link->to, depth + 1);
                }
            }
        } else {
            run_action(block);
        }

        int next = 0;
        for (int i = 0; i < dlogic_link_count(); ++i) {
            DLink *link = dlogic_link_get(i);
            if (link && link->from == current) { next = link->to; break; }
        }
        current = next;
    }
    block_chain_guard[depth] = 0;
}

static void fire_event(int kind) {
    for (int i = 0; i < dlogic_block_count(); ++i) {
        DBlock *event = dlogic_block_get(dlogic_block_at(i));
        if (!event || event->kind != kind) continue;
        for (int l = 0; l < dlogic_link_count(); ++l) {
            DLink *link = dlogic_link_get(l);
            if (link && link->from == event->id) run_chain(link->to, 0);
        }
    }
}

/* ------------------------------------------------------------------ API */

void dlogic_set_program(const char *source, int length) {
    (void)source;
    (void)length;
    /* The built-in executor always runs the block graph; the source is stored
     * for export. Validation happens in dlogic_validate(). */
}

int dlogic_start(void) {
    dlogic_ctx_reset(&g_ctx, DL_BUDGET_DEFAULT);
    memset(g_timer_accum, 0, sizeof(g_timer_accum));
    memset(block_chain_guard, 0, sizeof(block_chain_guard));
    g_time = 0.0f;
    g_delta = 0.0f;
    g_playing = 1;
    dlogic_ctx_frame(&g_ctx);
    fire_event(DBLOCK_ON_START);
    return 1;
}

void dlogic_update(float dt) {
    if (!g_playing) return;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.25f) dt = 0.25f; /* a stalled frame must not teleport objects */
    g_delta = dt;
    g_time += dt;
    dlogic_ctx_frame(&g_ctx);
    fire_event(DBLOCK_ON_UPDATE);

    /* AABB collisions are evaluated after the update so the block "IfCollide"
     * and the OnCollide event see the same positions the player saw. */
    const int count = dscene_flat_count();
    for (int i = 1; i < count; ++i) {
        DNode *a = dnode_get(i);
        if (!a || !a->collision) continue;
        for (int j = i + 1; j < count; ++j) {
            DNode *b = dnode_get(j);
            if (!b || !b->collision) continue;
            if (engine_check_aabb(i, j)) {
                dlogic_on_collide(i, j);
            }
        }
    }
}

void dlogic_stop(void) {
    g_playing = 0;
    g_time = 0.0f;
    g_delta = 0.0f;
    memset(g_button_state, 0, sizeof(g_button_state));
}

int dlogic_playing(void) { return g_playing; }

void dlogic_on_touch(int pointer, float x, float y, int down) {
    (void)pointer;
    g_touch_x = x;
    g_touch_y = y;
    g_touch_down = down ? 1 : 0;
    if (g_playing) fire_event(DBLOCK_ON_TOUCH);
}

void dlogic_on_collide(int node_a, int node_b) {
    (void)node_a;
    (void)node_b;
    if (g_playing) fire_event(DBLOCK_ON_COLLIDE);
}

void dlogic_button(int button, int pressed) {
    if (button < 0 || button >= DIAMOND_BTN_COUNT) return;
    g_button_state[button] = pressed ? 1 : 0;
}

int dlogic_button_pressed(int button) {
    if (button < 0 || button >= DIAMOND_BTN_COUNT) return 0;
    return g_button_state[button];
}

void dlogic_stats(int64_t *steps, int64_t *errors, int *last_block) {
    if (steps) *steps = g_ctx.steps;
    if (errors) *errors = g_ctx.errors;
    if (last_block) *last_block = g_ctx.current_block;
}
