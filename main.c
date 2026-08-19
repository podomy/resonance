#include "heap/heap.h"
#include "shared/events.h"
#include <stdint.h>
#include <stdio.h>

typedef struct {
    Timestamp submitted_at;
    MinHeap* min_heap_tree;
} Context;

void handler_one(Context ctx, Timestamp timestamp,
                 uint64_t seq) {
    printf("Running handler one\n");
}

int main() {
    Event event;
    event.executed_at.nanoseconds = 10000ULL;
    event.callback_func = handler_one;

    event.callback_func();

    return 0;
}
