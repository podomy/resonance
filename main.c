#include <stdio.h>
#include <stdint.h>
#include "heap/heap.h"

#define MAX_HANDLERS 5

typedef struct {
    uint64_t nanoseconds;
} Timestamp;

typedef void (*callback_func)(void);

typedef struct {
    Timestamp executed_at;
    void (*callback_func)(void);
    int handler_count;
} Event;

typedef struct {
   Timestamp submitted_at;
   MinHeap min_heap_tree;
} CallbackPayload;

void handler_one(void) {
    printf("Running handler one\n");
}

int main() {
    Event event;
    event.executed_at.nanoseconds = 10000ULL;
    event.callback_func = handler_one;
    event.handler_count = 1;

    event.callback_func();

    return 0;
}
