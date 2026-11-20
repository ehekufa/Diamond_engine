/* Diamond Engine — lightweight debugging wrapper for generated logic (C99).
 *
 * LuaC compiles Lua to native code and deliberately ships no bytecode
 * inspector. That is fine for production and terrible while authoring, so this
 * header provides the two things a phone editor actually needs:
 *
 *   ENGINE_SAFE_CALL(ctx, block_id, call)  - runs `call`, catches failures,
 *                                            attributes them to a block id and
 *                                            writes one readable log line.
 *   dlogic_step(ctx)                       - instruction budget: a script that
 *                                            loops forever costs at most
 *                                            DL_BUDGET steps per frame and
 *                                            then reports itself instead of
 *                                            freezing the UI.
 *
 * Both are plain C99 macros with no allocation and no setjmp on the hot path.
 */
#ifndef DIAMOND_DLOGIC_DBG_H
#define DIAMOND_DLOGIC_DBG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DL_BUDGET_DEFAULT 60000
#define DL_ERROR_MAX 160

typedef struct DLogicCtx {
    int64_t steps;       /* instructions executed since start            */
    int64_t errors;      /* number of failed ENGINE_SAFE_CALL invocations */
    int budget;          /* remaining steps for the current frame        */
    int budget_per_frame;
    int current_block;   /* block id currently executing                 */
    int aborted;         /* set when the budget is exhausted             */
    char last_error[DL_ERROR_MAX];
} DLogicCtx;

void dlogic_ctx_reset(DLogicCtx *ctx, int budget_per_frame);
/* Refills the budget at the top of a frame. */
void dlogic_ctx_frame(DLogicCtx *ctx);

/* Records a failure for `block_id` and returns `result_code`. */
int dlogic_ctx_fail(DLogicCtx *ctx, int block_id, int result_code, const char *what);

/* One instruction: returns 0 when the frame budget is exhausted. */
int dlogic_step(DLogicCtx *ctx, int block_id);

/* Runs `stmt` (a statement, not an expression) once the budget allows it.
 * When the budget is gone the statement is skipped and the failure is logged,
 * so a runaway script cannot freeze the frame. */
#define ENGINE_SAFE_CALL(ctx, block_id, stmt)                     \
    do {                                                          \
        if (dlogic_step((ctx), (block_id))) {                   \
            stmt;                                                 \
        } else {                                                  \
            (void)dlogic_ctx_fail((ctx), (block_id), -2,          \
                                  "instruction budget exceeded"); \
        }                                                         \
    } while (0)

/* Same for status-returning calls: keeps the failing code and a message. */
#define ENGINE_SAFE_STATUS(ctx, block_id, result_code, what) \
    ((result_code) == 0 ? 0 : dlogic_ctx_fail((ctx), (block_id), (result_code), (what)))

#ifdef __cplusplus
}
#endif
#endif /* DIAMOND_DLOGIC_DBG_H */
