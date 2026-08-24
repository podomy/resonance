#include "context.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_CELL_NM 1000000000ULL
#define DEFAULT_GRID_N 8ULL
#define DEFAULT_RANGE_NM 2500000000ULL

// Initializes clock, heap, node list, grid, and RNG.
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
    ctx->grid.cells = NULL;
    ctx->radio.range_nm = DEFAULT_RANGE_NM;
    ctx->udp_bind_n = 0;
    ctx->heap = heap_create(heap_capacity);
    if (ctx->heap == NULL) {
        return false;
    }
    if (!nodelist_init(&ctx->nodes, 8)) {
        heap_free(ctx->heap);
        ctx->heap = NULL;
        return false;
    }
    if (!mediumgrid_init(&ctx->grid, 0, 0, DEFAULT_CELL_NM,
                         DEFAULT_GRID_N, DEFAULT_GRID_N,
                         MATERIAL_AIR)) {
        nodelist_free(&ctx->nodes);
        heap_free(ctx->heap);
        ctx->heap = NULL;
        return false;
    }
    rng_seed(&ctx->rng, seed);
    return true;
}

// Releases heap, node, and grid storage. Safe on NULL.
void context_free(Context* ctx) {
    if (ctx == NULL) {
        return;
    }
    if (ctx->heap != NULL) {
        Event ev;
        while (heap_pop(ctx->heap, &ev))
            free(ev.payload);
    }
    heap_free(ctx->heap);
    ctx->heap = NULL;
    nodelist_free(&ctx->nodes);
    mediumgrid_free(&ctx->grid);
}

// Appends a node and assigns a monotonic id.
bool context_add_node(Context* ctx, int64_t x_nm,
                      int64_t y_nm, int64_t vx, int64_t vy,
                      uint64_t* out_id) {
    if (ctx == NULL) {
        return false;
    }
    Node node;
    node.id = ctx->next_node_id;
    node.x_nm = x_nm;
    node.y_nm = y_nm;
    node.vx_nm_per_ns = vx;
    node.vy_nm_per_ns = vy;
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

static bool enqueue(Context* ctx, uint64_t t,
                    EventCallback cb,
                    const uint8_t* payload,
                    uint64_t payload_len, uint64_t from_id,
                    uint64_t to_id, uint16_t src_port,
                    uint16_t dst_port) {
    Event ev;

    if (ctx == NULL || ctx->heap == NULL || cb == NULL)
        return (false);
    if (t < ctx->clock.nanoseconds)
        return (false);
    if (payload_len > 0 && payload == NULL)
        return (false);

    ev.payload = NULL;
    ev.payload_len = payload_len;
    if (payload_len > 0) {
        ev.payload = malloc(payload_len);
        if (ev.payload == NULL)
            return (false);
        memcpy(ev.payload, payload, payload_len);
    }

    ev.executed_at.nanoseconds = t;
    ev.callback_func = cb;
    ev.sequence_number = ctx->next_seq;
    ev.from_id = from_id;
    ev.to_id = to_id;
    ev.src_port = src_port;
    ev.dst_port = dst_port;
    if (!heap_push(ctx->heap, ev)) {
        free(ev.payload);
        return (false);
    }
    ctx->next_seq++;
    return (true);
}

bool context_schedule(Context* ctx, uint64_t nanoseconds,
                      EventCallback cb, uint64_t to_id) {
    return (enqueue(ctx, nanoseconds, cb, NULL, 0, 0, to_id,
                    0, 0));
}

void context_run(Context* ctx) {
    Event event;

    if (ctx == NULL || ctx->heap == NULL)
        return;
    while (heap_pop(ctx->heap, &event)) {
        if (event.executed_at.nanoseconds >
            ctx->clock.nanoseconds)
            ctx->clock = event.executed_at;
        event.callback_func(
            ctx, event.executed_at, event.sequence_number,
            event.payload, event.payload_len, event.from_id,
            event.to_id, event.src_port, event.dst_port);
        free(event.payload);
    }
}

bool context_send(Context* ctx, uint64_t from_id,
                  uint64_t to_id, const uint8_t* payload,
                  uint64_t payload_len,
                  EventCallback on_recv) {
    Node *from, *to;
    RadioPath path;
    uint64_t arrive;

    if (ctx == NULL || on_recv == NULL)
        return (false);
    from = context_find_node(ctx, from_id);
    to = context_find_node(ctx, to_id);
    if (from == NULL || to == NULL)
        return (false);
    if (!radio_path(&ctx->grid, &ctx->radio, from->x_nm,
                    from->y_nm, to->x_nm, to->y_nm, &path))
        return (false);
    arrive = ctx->clock.nanoseconds + path.delay_ns;
    return (enqueue(ctx, arrive, on_recv, payload,
                    payload_len, from_id, to_id, 0, 0));
}

bool context_enqueue(Context* ctx, uint64_t t,
                     EventCallback cb,
                     const uint8_t* payload,
                     uint64_t payload_len, uint64_t from_id,
                     uint64_t to_id, uint16_t src_port,
                     uint16_t dst_port) {
    return (enqueue(ctx, t, cb, payload, payload_len,
                    from_id, to_id, src_port, dst_port));
}
