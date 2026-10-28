#ifndef HEAP_H
#define HEAP_H

#include "../shared/events.h"
#include <stddef.h>

typedef struct {
    Event* data;
    size_t size;
    size_t capacity;
} MinHeap;

// Allocates a min-heap. Returns nullptr on allocation failure.
MinHeap* heap_create(size_t capacity);

// Releases heap storage. Safe on nullptr.
void heap_free(MinHeap* h);

// Inserts value. Returns false on allocation failure.
bool heap_push(MinHeap* h, Event value);

// Removes the next event. Returns false if empty.
bool heap_pop(MinHeap* h, Event* out);

#endif
