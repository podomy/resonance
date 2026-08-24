CC ?= cc
CFLAGS += -std=c11 -Wall -Wextra -Werror -O2

PROG =	resonance
SRCS =	main.c \
	heap/heap.c \
	rng/rng.c \
	shared/context.c \
	node/node.c \
	world/world.c \
	udp/udp.c

HEAP_TEST =	check/heap_test
NODE_TEST =	check/node_test
WORLD_TEST =	check/world_test
DET_TEST =	check/determinism
MCAST_TEST =	check/mcast_test

TESTS =	${HEAP_TEST} ${NODE_TEST} ${WORLD_TEST} ${DET_TEST} \
	${MCAST_TEST}

.PHONY: all clean check

all: ${PROG}

${PROG}: ${SRCS}
	${CC} ${CFLAGS} -o $@ ${SRCS}

${HEAP_TEST}: check/heap_test.c heap/heap.c
	${CC} ${CFLAGS} -o $@ check/heap_test.c heap/heap.c

${NODE_TEST}: check/node_test.c node/node.c
	${CC} ${CFLAGS} -o $@ check/node_test.c node/node.c

${WORLD_TEST}: check/world_test.c world/world.c
	${CC} ${CFLAGS} -o $@ check/world_test.c world/world.c

LIBSRCS =	heap/heap.c rng/rng.c shared/context.c \
		node/node.c world/world.c udp/udp.c

${DET_TEST}: check/determinism.c ${LIBSRCS}
	${CC} ${CFLAGS} -o $@ check/determinism.c ${LIBSRCS}

${MCAST_TEST}: check/mcast_test.c ${LIBSRCS}
	${CC} ${CFLAGS} -o $@ check/mcast_test.c ${LIBSRCS}

check: ${TESTS}
	./${HEAP_TEST}
	./${NODE_TEST}
	./${WORLD_TEST}
	./${DET_TEST}
	./${MCAST_TEST}

clean:
	rm -f ${PROG} ${TESTS}
