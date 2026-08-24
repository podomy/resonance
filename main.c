#include "shared/context.h"
#include "udp/udp.h"
#include <inttypes.h>
#include <stdio.h>

#define MOVE_DT_NS 1000ULL
#define MOVE_STOP_NS 4000ULL
#define PING_PORT 5353

static void on_recv(Context* ctx, Timestamp timestamp,
                    uint64_t seq, uint8_t* payload,
                    uint64_t payload_len, uint64_t from_id,
                    uint64_t to_id, uint16_t src_port,
                    uint16_t dst_port) {
    (void)ctx;
    (void)seq;
    printf("udp t=%" PRIu64 " %" PRIu64 ":%" PRIu16
           " -> %" PRIu64 ":%" PRIu16 " len=%" PRIu64 "\n",
           timestamp.nanoseconds, from_id, src_port, to_id,
           dst_port, payload_len);
    if (payload_len > 0)
        fwrite(payload, 1, payload_len, stdout);
    printf("\n");
}

static void on_move(Context* ctx, Timestamp timestamp,
                    uint64_t seq, uint8_t* payload,
                    uint64_t payload_len, uint64_t from_id,
                    uint64_t to_id, uint16_t src_port,
                    uint16_t dst_port) {
    static const uint8_t ping[] = "ping";
    Node* node;
    uint64_t next;

    (void)seq;
    (void)payload;
    (void)payload_len;
    (void)from_id;
    (void)src_port;
    (void)dst_port;
    node = context_find_node(ctx, to_id);
    if (node == NULL)
        return;

    node->x_nm += node->vx_nm_per_ns * (int64_t)MOVE_DT_NS;
    node->y_nm += node->vy_nm_per_ns * (int64_t)MOVE_DT_NS;
    printf("move t=%" PRIu64 " id=%" PRIu64 " x=%" PRId64
           " y=%" PRId64 "\n",
           timestamp.nanoseconds, node->id, node->x_nm,
           node->y_nm);

    if (!udp_mcast(ctx, node->id, PING_PORT, PING_PORT,
                   ping, sizeof(ping) - 1))
        printf("  %" PRIu64 " mcast failed\n", node->id);

    next = timestamp.nanoseconds + MOVE_DT_NS;
    if (next <= MOVE_STOP_NS)
        context_schedule(ctx, next, on_move, node->id);
}

int main(void) {
    Context ctx;
    uint64_t a, b, c;

    if (!context_init(&ctx, 8, 1))
        return (1);
    if (!context_add_node(&ctx, 0, 0, 1000000, 0, &a) ||
        !context_add_node(&ctx, 0, 0, 2000000, 2000000,
                          &b) ||
        !context_add_node(&ctx, 0, 0, 0, 0, &c) ||
        !udp_bind(&ctx, a, PING_PORT, on_recv) ||
        !udp_bind(&ctx, b, PING_PORT, on_recv) ||
        !udp_bind(&ctx, c, PING_PORT, on_recv) ||
        !context_schedule(&ctx, MOVE_DT_NS, on_move, a) ||
        !context_schedule(&ctx, MOVE_DT_NS, on_move, b) ||
        !context_schedule(&ctx, MOVE_DT_NS, on_move, c)) {
        context_free(&ctx);
        return (1);
    }
    context_run(&ctx);
    context_free(&ctx);
    return (0);
}
