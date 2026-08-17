#include <stdio.h>
#include <stdint.h>

#define MAX_HANDLERS 5

typedef struct {
    uint64_t nanoseconds;
} Timestamp;

typedef void (*CallbackFunc)(void);

typedef struct {
    Timestamp executed_at;
    CallbackFunc handlers[MAX_HANDLERS];
    int handler_count;
} Event;

void handler_one(void) {
    printf("Running handler one\n");
}

int main() {
    Event event;
    event.executed_at.nanoseconds = 10000ULL;
    event.handlers[0] = handler_one;
    event.handler_count = 1;

    for(int i = 0; i < event.handler_count; i++) {
        event.handlers[i]();
    }

    return 0;
}
