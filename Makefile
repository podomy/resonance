CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2

.PHONY: all clean check

all: resonance

resonance: main.c heap/heap.c rng/rng.c shared/context.c \
		node/node.c
	$(CC) $(CFLAGS) -o $@ $^

check/heap_test: check/heap_test.c heap/heap.c
	$(CC) $(CFLAGS) -o $@ $^

check/determinism: check/determinism.c heap/heap.c rng/rng.c \
		shared/context.c node/node.c
	$(CC) $(CFLAGS) -o $@ $^

check/node_test: check/node_test.c node/node.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f resonance check/heap_test check/determinism \
		check/node_test

check: check/heap_test check/determinism check/node_test
	./check/heap_test
	./check/determinism
	./check/node_test
