#include "../shared/context.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TRACE_LEN 3
#define RNG_DRAWS 256
#define SEED 1ULL

typedef struct {
    uint64_t t;
    uint64_t seq;
    uint64_t draw;
} TraceEvent;

static TraceEvent* trace_out;
static size_t trace_n;
static size_t trace_cap;

/** Records time, sequence, and one RNG draw. */
static void record(Context* ctx, Timestamp timestamp,
                   uint64_t seq) {
    if (trace_n >= trace_cap) {
        return;
    }
    trace_out[trace_n].t = timestamp.nanoseconds;
    trace_out[trace_n].seq = seq;
    trace_out[trace_n].draw = rng_u64(&ctx->rng);
    trace_n++;
}

/** Fills dest with TRACE_LEN events from a seeded run. */
static bool run_trace(TraceEvent* dest) {
    Context ctx;
    if (!context_init(&ctx, 8, SEED)) {
        return false;
    }
    trace_out = dest;
    trace_n = 0;
    trace_cap = TRACE_LEN;
    if (!context_schedule(&ctx, 10000, record) ||
        !context_schedule(&ctx, 5000, record) ||
        !context_schedule(&ctx, 10000, record)) {
        context_free(&ctx);
        return false;
    }
    context_run(&ctx);
    context_free(&ctx);
    return trace_n == TRACE_LEN;
}

/** Returns 0 if two same-seed runs match. */
int main(void) {
    Rng a;
    Rng b;
    rng_seed(&a, SEED);
    rng_seed(&b, SEED);
    for (size_t i = 0; i < RNG_DRAWS; i++) {
        if (rng_u64(&a) != rng_u64(&b)) {
            fprintf(stderr, "rng mismatch at %zu\n", i);
            return 1;
        }
    }

    TraceEvent first[TRACE_LEN];
    TraceEvent second[TRACE_LEN];
    if (!run_trace(first) || !run_trace(second)) {
        fprintf(stderr, "trace run failed\n");
        return 1;
    }
    if (memcmp(first, second, sizeof(first)) != 0) {
        fprintf(stderr, "event trace mismatch\n");
        return 1;
    }
    return 0;
}
