#ifndef EVENTS_H
#define EVENTS_H

#include <stdint.h>

typedef struct {
    uint64_t nanoseconds;
} Timestamp;

typedef struct Context Context;

/*
 * Handler contract: payload points at an engine-owned copy
 * that is freed as soon as the callback returns. Copy out
 * anything you keep. Do not free or destroy ctx inside a
 * handler.
 */
typedef void (*EventCallback)(
    Context* ctx, Timestamp timestamp, uint64_t seq,
    uint8_t* payload, uint64_t payload_len,
    uint64_t from_id, uint64_t to_id, uint16_t src_port,
    uint16_t dst_port);

typedef struct {
    Timestamp executed_at;
    EventCallback callback_func;
    uint64_t sequence_number;
    uint8_t* payload;
    uint64_t payload_len;
    uint64_t from_id;
    uint64_t to_id;
    uint16_t src_port;
    uint16_t dst_port;
} Event;

#endif
