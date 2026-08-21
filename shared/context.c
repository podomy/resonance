#include "context.h"

// Initializes clock, heap, node list, and RNG.
bool context_init(Context* ctx, size_t heap_capacity,
                  uint64_t seed) {
    if (ctx == NULL) {
        return false;
    }
    ctx->clock.nanoseconds = 0;
    ctx->next_seq = 0;
    ctx->next_node_id = 0;
    ctx->nodes.data = NULL;
    ctx->nodes.len = 0;
    ctx->nodes.cap = 0;
    ctx->nodes.by_id.data = NULL;
    ctx->nodes.by_id.cap = 0;
    ctx->heap = heap_create(heap_capacity);
    if (ctx->heap == NULL) {
        return false;
    }
    if (!nodelist_init(&ctx->nodes, 8)) {
        heap_free(ctx->heap);
        ctx->heap = NULL;
        return false;
    }
    rng_seed(&ctx->rng, seed);
    return true;
}

// Releases heap and node storage. Safe on NULL.
void context_free(Context* ctx) {
    if (ctx == NULL) {
        return;
    }
    heap_free(ctx->heap);
    ctx->heap = NULL;
    nodelist_free(&ctx->nodes);
}

// Appends a node and assigns a monotonic id.
bool context_add_node(Context* ctx, int64_t x_nm,
                      int64_t y_nm, uint64_t* out_id) {
    if (ctx == NULL) {
        return false;
    }
    Node node;
    node.id = ctx->next_node_id;
    node.x_nm = x_nm;
    node.y_nm = y_nm;
    if (!nodelist_push(&ctx->nodes, node)) {
        return false;
    }
    if (out_id != NULL) {
        *out_id = node.id;
    }
    ctx->next_node_id++;
    return true;
}

// Returns the node with id, or NULL.
Node* context_find_node(Context* ctx, uint64_t id) {
    if (ctx == NULL) {
        return NULL;
    }
    return nodelist_find(&ctx->nodes, id);
}

// Swap-remove the node with this id.
bool context_remove_node(Context* ctx, uint64_t id) {
    if (ctx == NULL) {
        return false;
    }
    return nodelist_remove_by_id(&ctx->nodes, id);
}

// Queues cb at t. Rejects t earlier than the clock.
bool context_schedule(Context* ctx, uint64_t nanoseconds,
                      EventCallback cb, uint64_t payload) {
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
    event.payload = payload;

    if (!heap_push(ctx->heap, event)) {
        return false;
    }
    ctx->next_seq++;

    return true;
}

// Pops and runs events until the heap is empty.
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
                            event.sequence_number,
                            event.payload);
    }
}
