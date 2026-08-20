#include "../heap/heap.h"
#include <assert.h>
#include <stdint.h>

/** Event with only heap-order fields set. */
static Event ev(uint64_t t, uint64_t seq) {
    return (Event){.executed_at.nanoseconds = t,
                   .sequence_number = seq};
}

int main(void) {
    MinHeap* h = heap_create(1);
    assert(h != NULL);
    assert(heap_push(h, ev(10, 2)));
    assert(heap_push(h, ev(5, 100)));
    assert(heap_push(h, ev(10, 1)));

    const uint64_t want[][2] = {
        {5, 100},
        {10, 1},
        {10, 2},
    };
    Event out;
    for (int i = 0; i < 3; i++) {
        assert(heap_pop(h, &out));
        assert(out.executed_at.nanoseconds == want[i][0]);
        assert(out.sequence_number == want[i][1]);
    }
    assert(!heap_pop(h, &out));
    heap_free(h);
    return 0;
}
