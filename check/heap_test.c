#include "../heap/heap.h"
#include <stdio.h>

/** No-op callback for heap ordering tests. */
static void nop(Context* ctx, Timestamp ts, uint64_t seq) {
    (void)ctx;
    (void)ts;
    (void)seq;
}

/** Fills event fields used by the heap comparator. */
static Event make_event(uint64_t t, uint64_t seq) {
    Event event;
    event.executed_at.nanoseconds = t;
    event.callback_func = nop;
    event.sequence_number = seq;
    return event;
}

int main(void) {
    MinHeap* h = heap_create(1);
    if (h == NULL) {
        return 1;
    }
    if (!heap_push(h, make_event(10, 2)) ||
        !heap_push(h, make_event(5, 100)) ||
        !heap_push(h, make_event(10, 1))) {
        heap_free(h);
        return 1;
    }

    Event out;
    const uint64_t expect_t[] = {5, 10, 10};
    const uint64_t expect_s[] = {100, 1, 2};
    for (int i = 0; i < 3; i++) {
        if (!heap_pop(h, &out)) {
            heap_free(h);
            return 1;
        }
        if (out.executed_at.nanoseconds != expect_t[i] ||
            out.sequence_number != expect_s[i]) {
            fprintf(stderr, "order mismatch at %d\n", i);
            heap_free(h);
            return 1;
        }
    }
    if (heap_pop(h, &out)) {
        heap_free(h);
        return 1;
    }
    heap_free(h);
    return 0;
}
