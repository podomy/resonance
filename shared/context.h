#ifndef CONTEXT_H
#define CONTEXT_H

#include "../heap/heap.h"
#include "../node/node.h"
#include "../rng/rng.h"
#include "../udp/udp.h"
#include "../world/world.h"
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
    MediumGrid grid;
    RadioParams radio;
    UdpBind udp_binds[UDP_BIND_MAX];
    size_t udp_bind_n;
};

bool context_init(Context* ctx, size_t heap_capacity,
                  uint64_t seed);
void context_free(Context* ctx);
bool context_add_node(Context* ctx, int64_t x_nm,
                      int64_t y_nm, int64_t vx, int64_t vy,
                      uint64_t* out_id);
Node* context_find_node(Context* ctx, uint64_t id);
bool context_remove_node(Context* ctx, uint64_t id);
bool context_schedule(Context* ctx, uint64_t nanoseconds,
                      EventCallback cb, uint64_t to_id);
bool context_send(Context* ctx, uint64_t from_id,
                  uint64_t to_id, const uint8_t* payload,
                  uint64_t payload_len,
                  EventCallback on_recv);
bool context_enqueue(Context* ctx, uint64_t t,
                     EventCallback cb,
                     const uint8_t* payload,
                     uint64_t payload_len, uint64_t from_id,
                     uint64_t to_id, uint16_t src_port,
                     uint16_t dst_port);
// Pops and runs events until the heap is empty. Handlers
// must not destroy ctx.
void context_run(Context* ctx);

#endif
