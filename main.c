#include "shared/context.h"
#include <inttypes.h>
#include <stdio.h>

#define MOVE_DT_NS 1000ULL
#define MOVE_STOP_NS 4000ULL

// Moves the node named in payload and schedules the next
// step.
static void on_move(Context* ctx, Timestamp timestamp,
                    uint64_t seq, uint64_t payload) {
    (void)seq;
    Node* node = context_find_node(ctx, payload);
    if (node == NULL) {
        return;
    }

    node->x_nm += node->vx_nm_per_ns * (int64_t)MOVE_DT_NS;
    node->y_nm += node->vy_nm_per_ns * (int64_t)MOVE_DT_NS;

    printf("%" PRIu64 " %" PRIu64 " %" PRId64 " %" PRId64
           "\n",
           timestamp.nanoseconds, node->id, node->x_nm,
           node->y_nm);
    for (size_t i = 0; i < ctx->nodes.len; i++) {
        Node* other = &ctx->nodes.data[i];
        RadioPath path;
        if (other->id == node->id) {
            continue;
        }
        if (!radio_path(&ctx->grid, &ctx->radio, node->x_nm,
                        node->y_nm, other->x_nm,
                        other->y_nm, &path)) {
            printf("  %" PRIu64 " -> %" PRIu64 " blocked\n",
                   node->id, other->id);
            continue;
        }
        printf("  %" PRIu64 " -> %" PRIu64 " delay %" PRIu64
               "\n",
               node->id, other->id, path.delay_ns);
    }
    uint64_t next = timestamp.nanoseconds + MOVE_DT_NS;
    if (next <= MOVE_STOP_NS) {
        context_schedule(ctx, next, on_move, payload);
    }
}

int main(void) {
    Context ctx;

    uint64_t node_id_a;
    uint64_t node_id_b;

    if (!context_init(&ctx, 8, 1)) {
        return 1;
    }

    // Adding nodes.
    if (!context_add_node(&ctx, 0, 0, 1000000, 0,
                          &node_id_a)) {
        context_free(&ctx);
        return 1;
    }
    if (!context_add_node(&ctx, 0, 0, 1000000, 1000000,
                          &node_id_b)) {
        context_free(&ctx);
        return 1;
    }

    // Schedule two moves on each node.
    if (!context_schedule(&ctx, MOVE_DT_NS, on_move,
                          node_id_a)) {
        context_free(&ctx);
        return 1;
    }
    if (!context_schedule(&ctx, MOVE_DT_NS, on_move,
                          node_id_b)) {
        context_free(&ctx);
        return 1;
    }

    context_run(&ctx);
    context_free(&ctx);
    return 0;
}
