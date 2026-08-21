#include "heap.h"
#include <stdlib.h>

// Swaps two events.
static void swap(Event* a, Event* b) {
    Event temp = *a;
    *a = *b;
    *b = temp;
}

// Returns true if a precedes b by (time, sequence).
static bool event_before(const Event* a, const Event* b) {
    if (a->executed_at.nanoseconds !=
        b->executed_at.nanoseconds) {
        return a->executed_at.nanoseconds <
               b->executed_at.nanoseconds;
    }
    return a->sequence_number < b->sequence_number;
}

// Allocates a min-heap. Returns NULL on allocation failure.
MinHeap* heap_create(size_t capacity) {
    MinHeap* h = malloc(sizeof(*h));
    if (h == NULL) {
        return NULL;
    }
    h->data = NULL;
    if (capacity > 0) {
        h->data = malloc(capacity * sizeof(*h->data));
        if (h->data == NULL) {
            free(h);
            return NULL;
        }
    }
    h->size = 0;
    h->capacity = capacity;
    return h;
}

// Releases heap storage. Safe on NULL.
void heap_free(MinHeap* h) {
    if (h == NULL) {
        return;
    }
    free(h->data);
    free(h);
}

// Inserts value. Returns false on allocation failure.
bool heap_push(MinHeap* h, Event value) {
    if (h == NULL) {
        return false;
    }

    if (h->size >= h->capacity) {
        size_t new_capacity;
        if (h->capacity == 0) {
            new_capacity = 1;
        } else if (h->capacity > ((size_t)-1) / 2) {
            return false;
        } else {
            new_capacity = h->capacity * 2;
        }
        if (new_capacity >
            ((size_t)-1) / sizeof(*h->data)) {
            return false;
        }
        Event* grown = realloc(
            h->data, new_capacity * sizeof(*h->data));
        if (grown == NULL) {
            return false;
        }
        h->data = grown;
        h->capacity = new_capacity;
    }

    size_t index = h->size;
    h->data[index] = value;
    h->size++;

    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (!event_before(&h->data[index],
                          &h->data[parent])) {
            break;
        }
        swap(&h->data[index], &h->data[parent]);
        index = parent;
    }
    return true;
}

// Removes the next event. Returns false if empty.
bool heap_pop(MinHeap* h, Event* out) {
    if (h == NULL || out == NULL || h->size == 0) {
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

            if (left < h->size &&
                event_before(&h->data[left],
                             &h->data[smallest])) {
                smallest = left;
            }
            if (right < h->size &&
                event_before(&h->data[right],
                             &h->data[smallest])) {
                smallest = right;
            }

            if (smallest == index) {
                break;
            }

            swap(&h->data[index], &h->data[smallest]);
            index = smallest;
        }
    }

    *out = min_event;
    return true;
}
