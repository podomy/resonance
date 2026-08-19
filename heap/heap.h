#ifndef HEAP_H
#define HEAP_H

#include "../shared/events.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    Event* data;
    size_t size;
    size_t capacity;
} MinHeap;

void swap(Event* a, Event* b);

// Create the heap.
MinHeap* heap_create(size_t capacity);

// Cleanup.
void heap_free(MinHeap* h);

// Push and bubble up.
void heap_push(MinHeap* h, Event value);

// Pop an element.
bool heap_pop(MinHeap* h, Event* out);

#endif
