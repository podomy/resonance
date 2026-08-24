#include "../shared/context.h"
#include <assert.h>
#include <stdint.h>

#define MOVE_DT_NS 1000ULL
#define MOVE_STOP_NS 4000ULL
#define PING_PORT 5353

static unsigned recv_n;

static void on_recv(Context* ctx, Timestamp timestamp,
                    uint64_t seq, uint8_t* payload,
                    uint64_t payload_len, uint64_t from_id,
                    uint64_t to_id, uint16_t src_port,
                    uint16_t dst_port) {
    (void)ctx;
    (void)timestamp;
    (void)seq;
    (void)payload;
    (void)from_id;
    (void)to_id;
    (void)src_port;
    (void)dst_port;
    if (payload_len == 4)
        recv_n++;
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
    udp_mcast(ctx, node->id, PING_PORT, PING_PORT, ping,
              sizeof(ping) - 1);
    next = timestamp.nanoseconds + MOVE_DT_NS;
    if (next <= MOVE_STOP_NS)
        context_schedule(ctx, next, on_move, node->id);
}

int main(void) {
    Context ctx;
    uint64_t a, b, c;

    recv_n = 0;
    assert(context_init(&ctx, 8, 1));
    assert(context_add_node(&ctx, 0, 0, 1000000, 0, &a));
    assert(
        context_add_node(&ctx, 0, 0, 2000000, 2000000, &b));
    assert(context_add_node(&ctx, 0, 0, 0, 0, &c));
    assert(udp_bind(&ctx, a, PING_PORT, on_recv));
    assert(udp_bind(&ctx, b, PING_PORT, on_recv));
    assert(udp_bind(&ctx, c, PING_PORT, on_recv));
    assert(context_schedule(&ctx, MOVE_DT_NS, on_move, a));
    assert(context_schedule(&ctx, MOVE_DT_NS, on_move, b));
    assert(context_schedule(&ctx, MOVE_DT_NS, on_move, c));
    context_run(&ctx);
    context_free(&ctx);
    assert(recv_n > 0);
    return (0);
}
