#include "context.h"

/** Initializes clock, heap, and RNG. */
bool context_init(Context* ctx, size_t heap_capacity,
                  uint64_t seed) {
    if (ctx == NULL) {
        return false;
    }
    ctx->clock.nanoseconds = 0;
    ctx->next_seq = 0;
    ctx->heap = heap_create(heap_capacity);
    if (ctx->heap == NULL) {
        return false;
    }
    rng_seed(&ctx->rng, seed);
    return true;
}

/** Releases heap storage. Safe on NULL. */
void context_free(Context* ctx) {
    if (ctx == NULL) {
        return;
    }
    heap_free(ctx->heap);
    ctx->heap = NULL;
}

/** Queues cb at t. Rejects t earlier than the clock. */
bool context_schedule(Context* ctx, uint64_t nanoseconds,
                      EventCallback cb) {
    if (ctx == NULL || ctx->heap == NULL || cb == NULL) {
        return false;
    }
    if (nanoseconds < ctx->clock.nanoseconds) {
        return false;
    }

    Event event;
    event.executed_at.nanoseconds = nanoseconds;
    event.callback_func = cb;
    event.sequence_number = ctx->next_seq;
    
    if (!heap_push(ctx->heap, event)) {
        return false;
    }
    ctx->next_seq++;

    return true;
}

/** Pops and runs events until the heap is empty. */
void context_run(Context* ctx) {
    if (ctx == NULL || ctx->heap == NULL) {
        return;
    }
    Event event;
    while (heap_pop(ctx->heap, &event)) {
        if (event.executed_at.nanoseconds >
            ctx->clock.nanoseconds) {
            ctx->clock = event.executed_at;
        }
        event.callback_func(ctx, event.executed_at,
                            event.sequence_number);
    }
}
