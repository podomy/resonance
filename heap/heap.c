#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

typedef struct {
    int *data;
    size_t size;
    size_t capacity;
} MinHeap;

void swap(int *a, int *b) {
    int temp = *a;
    *a = *b;
    *b = temp;
}

// Create the heap.
MinHeap* heap_create(size_t capacity) {
    MinHeap* h = malloc(sizeof(MinHeap));
    h->data = malloc(capacity * sizeof(int));
    h->size = 0;
    h->capacity = capacity;
    return h;
}

// Cleanup.
void heap_free(MinHeap *h) {
    free(h->data);
    free(h);
}

// Push and bubble up.
void heap_push(MinHeap* h, int value) {
    // Resize array if full.
    if (h->size >= h->capacity) {
        h->capacity *= 2;
        h->data = realloc(h->data, h->capacity * sizeof(int));
    }

    // Place the value at the end.
    size_t index = h->size;
    h->data[index] = value;
    h->size++;

    // Bubble up to maintain min-heap property.
    while(index > 0) {
        size_t parent = (index - 1) / 2;
        if (h->data[index] < h->data[parent]) {
            swap(&h->data[index], &h->data[parent]);
            index = parent;
        } else {
            break;
        }
    }
}

// Pop an element.
int heap_pop(MinHeap *h) {
    if(h->size == 0) {
        fprintf(stderr, "heap underflow!\n");
        exit(EXIT_FAILURE);
    }

    int min_val = h->data[0];

    h->size--;
    if (h->size > 0) {
        h->data[0] = h->data[h->size];

        size_t index = 0;
        while(true) {
            size_t left = 2 * index + 1;
            size_t right = 2 * index + 2;
            size_t smallest = index;

            // Find the smallest among parent and children.
            if (left < h->size && h->data[left] < h->data[smallest]) {
                smallest = left;
            }
            if(right < h->size && h->data[right] < h->data[smallest]) {
                smallest = right;
            }

            // If root is smaller than both children, we are done.
            if(smallest == index) {
                break;
            }

            // Otherwise swap and keep going down.
            swap(&h->data[index], &h->data[smallest]);
            index = smallest;
        }
    }

    return min_val;
}
