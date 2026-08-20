#ifndef EVENTS_H
#define EVENTS_H

#include <stdint.h>

typedef struct {
    uint64_t nanoseconds;
} Timestamp;

typedef struct Context Context;

typedef void (*EventCallback)(Context* ctx,
                              Timestamp timestamp,
                              uint64_t seq);

typedef struct {
    Timestamp executed_at;
    EventCallback callback_func;
    uint64_t sequence_number;
} Event;

#endif
