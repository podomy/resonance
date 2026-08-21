#include "shared/context.h"
#include <inttypes.h>
#include <stdio.h>

#define MOVE_DT_NS 1000ULL
#define MOVE_STOP_NS 4000ULL
#define STEP_NM 1000000000LL

// Moves the node named in payload and schedules the next step.
static void on_move(Context* ctx, Timestamp timestamp,
                    uint64_t seq, uint64_t payload) {
    (void)seq;
    Node* node = context_find_node(ctx, payload);
    if (node == NULL) {
        return;
    }
    node->x_nm += STEP_NM;
    printf("%" PRIu64 " %" PRIu64 " %" PRId64 " %" PRId64
           "\n",
           timestamp.nanoseconds, node->id, node->x_nm,
           node->y_nm);
    uint64_t next = timestamp.nanoseconds + MOVE_DT_NS;
    if (next <= MOVE_STOP_NS) {
        context_schedule(ctx, next, on_move, payload);
    }
}

int main(void) {
    Context ctx;
    uint64_t node_id;
    if (!context_init(&ctx, 8, 1)) {
        return 1;
    }
    if (!context_add_node(&ctx, 0, 0, &node_id)) {
        context_free(&ctx);
        return 1;
    }
    if (!context_schedule(&ctx, MOVE_DT_NS, on_move,
                          node_id)) {
        context_free(&ctx);
        return 1;
    }
    context_run(&ctx);
    context_free(&ctx);
    return 0;
}
