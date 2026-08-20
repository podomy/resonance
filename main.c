#include "shared/context.h"
#include <inttypes.h>
#include <stdio.h>

/** Prints time, sequence, and one RNG draw. */
static void on_event(Context* ctx, Timestamp timestamp,
                     uint64_t seq) {
    uint64_t draw = rng_u64(&ctx->rng);
    printf("%" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
           timestamp.nanoseconds, seq, draw);
}

int main(void) {
    Context ctx;
    if (!context_init(&ctx, 8, 1)) {
        return 1;
    }
    if (!context_schedule(&ctx, 10000, on_event) ||
        !context_schedule(&ctx, 5000, on_event) ||
        !context_schedule(&ctx, 10000, on_event)) {
        context_free(&ctx);
        return 1;
    }
    context_run(&ctx);
    context_free(&ctx);
    return 0;
}


