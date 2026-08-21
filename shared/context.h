#ifndef CONTEXT_H
#define CONTEXT_H

#include "../heap/heap.h"
#include "../node/node.h"
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
    uint64_t next_node_id;
    NodeList nodes;
};

// Initializes clock, heap, node list, and RNG.
bool context_init(Context* ctx, size_t heap_capacity,
                  uint64_t seed);

// Releases heap and node storage. Safe on NULL.
void context_free(Context* ctx);

// Appends a node and assigns a monotonic id.
bool context_add_node(Context* ctx, int64_t x_nm,
                      int64_t y_nm, uint64_t* out_id);

// Returns the node with id, or NULL.
Node* context_find_node(Context* ctx, uint64_t id);

// Queues cb at t. Rejects t earlier than the clock.
bool context_schedule(Context* ctx, uint64_t nanoseconds,
                      EventCallback cb, uint64_t payload);

// Pops and runs events until the heap is empty.
void context_run(Context* ctx);

#endif
