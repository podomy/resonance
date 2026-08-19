#ifndef EVENTS_H
#define EVENTS_H

#include <stdint.h>

typedef struct {
    uint64_t nanoseconds;
} Timestamp;

typedef struct {
    Timestamp executed_at;
    void (*callback_func)();
    uint64_t sequence_number;
} Event;

typedef void (*callback_func)(Event, Timestamp, uint64_t);

#endif
