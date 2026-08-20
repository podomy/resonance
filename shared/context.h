#ifndef CONTEXT_H
#define CONTEXT_H

#include "../heap/heap.h"
#include "../rng/rng.h"
#include "events.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct Context {
    Timestamp clock;
    MinHeap* heap;
    Rng rng;
    uint64_t next_seq;
};

/** Initializes clock, heap, and RNG. */
bool context_init(Context* ctx, size_t heap_capacity,
                  uint64_t seed);

/** Releases heap storage. Safe on NULL. */
void context_free(Context* ctx);

/** Queues cb at t. Rejects t earlier than the clock. */
bool context_schedule(Context* ctx, uint64_t nanoseconds,
                      EventCallback cb);

/** Pops and runs events until the heap is empty. */
void context_run(Context* ctx);

#endif
