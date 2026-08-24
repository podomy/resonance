#ifndef UDP_H
#define UDP_H

#include "../shared/events.h"
#include <stdbool.h>
#include <stdint.h>

#define UDP_BIND_MAX 16

typedef struct {
    uint64_t node_id;
    uint16_t port;
    EventCallback cb;
} UdpBind;

bool udp_bind(Context* ctx, uint64_t node_id, uint16_t port,
              EventCallback cb);

bool udp_send(Context* ctx, uint64_t from_id,
              uint16_t src_port, uint64_t to_id,
              uint16_t dst_port, const uint8_t* buf,
              uint64_t len);

bool udp_mcast(Context* ctx, uint64_t from_id,
               uint16_t src_port, uint16_t dst_port,
               const uint8_t* buf, uint64_t len);

#endif
