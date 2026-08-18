#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

typedef struct {
    int *data;
    size_t size;
    size_t capacity;
} MinHeap;

void swap(int *a, int *b);

// Create the heap.
MinHeap* heap_create(size_t capacity);

// Cleanup.
void heap_free(MinHeap *h);

// Push and bubble up.
void heap_push(MinHeap* h, int value);

// Pop an element.
int heap_pop(MinHeap *h);

#endif
