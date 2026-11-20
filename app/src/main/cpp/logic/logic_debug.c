/* Diamond Engine — debugging wrapper implementation (C99). */
#include "diamond/dlogic_dbg.h"

#include <stdio.h>
#include <string.h>

#include "diamond/dlog.h"

void dlogic_ctx_reset(DLogicCtx *ctx, int budget_per_frame) {
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
    ctx->budget_per_frame = budget_per_frame > 0 ? budget_per_frame : DL_BUDGET_DEFAULT;
    ctx->budget = ctx->budget_per_frame;
    ctx->current_block = 0;
}

void dlogic_ctx_frame(DLogicCtx *ctx) {
    if (!ctx) return;
    ctx->budget = ctx->budget_per_frame;
    ctx->aborted = 0;
}

int dlogic_ctx_fail(DLogicCtx *ctx, int block_id, int result_code, const char *what) {
    if (!ctx) return result_code;
    ctx->errors++;
    snprintf(ctx->last_error, sizeof(ctx->last_error), "block %d: %s (code %d)",
             block_id, what ? what : "failed", result_code);
    /* One line per failure, not per frame: the console stays readable and the
     * ring buffer keeps the newest 256 entries anyway. */
    dlog_raw(DLOG_LOGIC, "logic", ctx->last_error);
    return result_code;
}

int dlogic_step(DLogicCtx *ctx, int block_id) {
    if (!ctx) return 1;
    if (ctx->budget <= 0) {
        if (!ctx->aborted) {
            ctx->aborted = 1;
            ctx->errors++;
            snprintf(ctx->last_error, sizeof(ctx->last_error),
                     "block %d: frame budget of %d steps exhausted (possible infinite loop)",
                     block_id, ctx->budget_per_frame);
            dlog_raw(DLOG_ERROR, "logic", ctx->last_error);
        }
        return 0;
    }
    ctx->budget--;
    ctx->steps++;
    ctx->current_block = block_id;
    return 1;
}
