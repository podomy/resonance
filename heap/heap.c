#include "../shared/events.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    Event* data;
    size_t size;
    size_t capacity;
} MinHeap;

void swap(Event* a, Event* b) {
    Event temp = *a;
    *a = *b;
    *b = temp;
}

// Create the heap.
MinHeap* heap_create(size_t capacity) {
    MinHeap* h = malloc(sizeof(MinHeap));
    h->data = malloc(capacity * sizeof(*h->data));
    h->size = 0;
    h->capacity = capacity;
    return h;
}

// Cleanup.
void heap_free(MinHeap* h) {
    free(h->data);
    free(h);
}

// Push and bubble up.
void heap_push(MinHeap* h, Event value) {
    // Resize array if full.
    if (h->size >= h->capacity) {
        // If the capacity is 0, we need to set it to 1,
        // because the multiplication below would not work.
        if (h->capacity == 0) {
            h->capacity = 1;
        }

        h->capacity *= 2;
        h->data = realloc(h->data,
                          h->capacity * sizeof(*h->data));
    }

    // Place the value at the end.
    size_t index = h->size;
    h->data[index] = value;
    h->size++;

    // Bubble up to maintain min-heap property.
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        // Comparing the size of the events based on
        // timestamps and sequence numbers.
        if (h->data[index].executed_at.nanoseconds <
                h->data[parent].executed_at.nanoseconds &&
            h->data[index].sequence_number <
                h->data[parent].sequence_number) {
            swap(&h->data[index], &h->data[parent]);
            index = parent;
        } else {
            break;
        }
    }
}

// Pop an element.
bool heap_pop(MinHeap* h, Event* out) {
    if (h->size == 0) {
        return false;
    }

    Event min_event = h->data[0];

    h->size--;
    if (h->size > 0) {
        h->data[0] = h->data[h->size];

        size_t index = 0;
        while (true) {
            size_t left = 2 * index + 1;
            size_t right = 2 * index + 2;
            size_t smallest = index;

            // Find the smallest among parent and children.
            // Comparing the size of the events based on
            // timestamp and the seq number
            if (left < h->size &&
                h->data[left].executed_at.nanoseconds <
                    h->data[smallest]
                        .executed_at.nanoseconds &&
                h->data[left].sequence_number <
                    h->data[smallest].sequence_number) {
                smallest = left;
            }
            // Comparing the size of the events based on
            // timestamp and the seq number
            if (right < h->size &&
                h->data[right].executed_at.nanoseconds <
                    h->data[smallest]
                        .executed_at.nanoseconds &&
                h->data[right].sequence_number <
                    h->data[smallest].sequence_number) {
                smallest = right;
            }

            // If root is smaller than both children, we are
            // done.
            if (smallest == index) {
                break;
            }

            // Otherwise swap and keep going down.
            swap(&h->data[index], &h->data[smallest]);
            index = smallest;
        }
    }

    *out = min_event;
    return true;
}
