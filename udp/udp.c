#include "udp.h"
#include "../shared/context.h"

/*
 * Test-only UDP. Delivery is radio_path plus enqueue.
 * Not used when a real binary talks through a TUN.
 */

// Listener for (node_id, port), or NULL.
static EventCallback bind_of(Context* ctx, uint64_t node_id,
                             uint16_t port) {
    size_t i;

    for (i = 0; i < ctx->udp_bind_n; i++) {
        if (ctx->udp_binds[i].node_id == node_id &&
            ctx->udp_binds[i].port == port)
            return (ctx->udp_binds[i].cb);
    }
    return (NULL);
}

// Demux a delivered datagram to the bound callback.
static void
on_udp_datagram(Context* ctx, Timestamp timestamp,
                uint64_t seq, uint8_t* payload,
                uint64_t payload_len, uint64_t from_id,
                uint64_t to_id, uint16_t src_port,
                uint16_t dst_port) {
    EventCallback cb;

    cb = bind_of(ctx, to_id, dst_port);
    if (cb == NULL)
        return;
    cb(ctx, timestamp, seq, payload, payload_len, from_id,
       to_id, src_port, dst_port);
}

// Register one listener. Port 0 is invalid.
bool udp_bind(Context* ctx, uint64_t node_id, uint16_t port,
              EventCallback cb) {
    if (ctx == NULL || cb == NULL || port == 0)
        return (false);
    if (context_find_node(ctx, node_id) == NULL)
        return (false);
    if (bind_of(ctx, node_id, port) != NULL)
        return (false);
    if (ctx->udp_bind_n >= UDP_BIND_MAX)
        return (false);
    ctx->udp_binds[ctx->udp_bind_n].node_id = node_id;
    ctx->udp_binds[ctx->udp_bind_n].port = port;
    ctx->udp_binds[ctx->udp_bind_n].cb = cb;
    ctx->udp_bind_n++;
    return (true);
}

// Unicast. False if radio_path blocks or args are bad.
bool udp_send(Context* ctx, uint64_t from_id,
              uint16_t src_port, uint64_t to_id,
              uint16_t dst_port, const uint8_t* buf,
              uint64_t len) {
    Node *from, *to;
    RadioPath path;
    uint64_t arrive;

    if (ctx == NULL || src_port == 0 || dst_port == 0)
        return (false);
    from = context_find_node(ctx, from_id);
    to = context_find_node(ctx, to_id);
    if (from == NULL || to == NULL)
        return (false);
    if (!radio_path(&ctx->grid, &ctx->radio, from->x_nm,
                    from->y_nm, to->x_nm, to->y_nm, &path))
        return (false);
    arrive = ctx->clock.nanoseconds + path.delay_ns;
    return (context_enqueue(ctx, arrive, on_udp_datagram,
                            buf, len, from_id, to_id,
                            src_port, dst_port));
}

// Fan-out. Skips self. A blocked hop is not an error.
bool udp_mcast(Context* ctx, uint64_t from_id,
               uint16_t src_port, uint16_t dst_port,
               const uint8_t* buf, uint64_t len) {
    size_t i;

    if (ctx == NULL || src_port == 0 || dst_port == 0)
        return (false);
    if (context_find_node(ctx, from_id) == NULL)
        return (false);
    for (i = 0; i < ctx->nodes.len; i++) {
        uint64_t id = ctx->nodes.data[i].id;
        if (id == from_id)
            continue;
        udp_send(ctx, from_id, src_port, id, dst_port, buf,
                 len);
    }
    return (true);
}
