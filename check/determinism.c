#include "../shared/context.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
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

static void record(Context* ctx, Timestamp timestamp,
                   uint64_t seq, uint8_t* payload,
                   uint64_t payload_len, uint64_t from_id,
                   uint64_t to_id, uint16_t src_port,
                   uint16_t dst_port) {
    (void)payload;
    (void)payload_len;
    (void)from_id;
    (void)to_id;
    (void)src_port;
    (void)dst_port;
    if (trace_n >= trace_cap)
        return;
    trace_out[trace_n].t = timestamp.nanoseconds;
    trace_out[trace_n].seq = seq;
    trace_out[trace_n].draw = rng_u64(&ctx->rng);
    trace_n++;
}

static bool run_trace(TraceEvent* dest) {
    Context ctx;

    if (!context_init(&ctx, 8, SEED))
        return (false);
    trace_out = dest;
    trace_n = 0;
    trace_cap = TRACE_LEN;
    if (!context_schedule(&ctx, 10000, record, 0) ||
        !context_schedule(&ctx, 5000, record, 0) ||
        !context_schedule(&ctx, 10000, record, 0)) {
        context_free(&ctx);
        return (false);
    }
    context_run(&ctx);
    context_free(&ctx);
    return (trace_n == TRACE_LEN);
}

int main(void) {
    Rng a, b;
    TraceEvent first[TRACE_LEN];
    TraceEvent second[TRACE_LEN];
    size_t i;

    rng_seed(&a, SEED);
    rng_seed(&b, SEED);
    for (i = 0; i < RNG_DRAWS; i++)
        assert(rng_u64(&a) == rng_u64(&b));
    assert(run_trace(first));
    assert(run_trace(second));
    assert(memcmp(first, second, sizeof(first)) == 0);
    return (0);
}
