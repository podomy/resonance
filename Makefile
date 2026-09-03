CC ?= cc
CFLAGS += -std=c11 -Wall -Wextra -Werror -O2
export PATH := /usr/local/go/bin:$(PATH)

all: resonance

resonance: main.c sim/sim.c heap/heap.c rng/rng.c shared/context.c node/node.c world/world.c udp/udp.c math/math.c tun/tun.c
	$(CC) $(CFLAGS) -o $@ $^

check/heap_test: check/heap_test.c heap/heap.c
	$(CC) $(CFLAGS) -o $@ $^

check/node_test: check/node_test.c node/node.c
	$(CC) $(CFLAGS) -o $@ $^

check/world_test: check/world_test.c world/world.c
	$(CC) $(CFLAGS) -o $@ $^

check/determinism: check/determinism.c heap/heap.c rng/rng.c shared/context.c node/node.c world/world.c udp/udp.c
	$(CC) $(CFLAGS) -o $@ $^

check/mcast_test: check/mcast_test.c heap/heap.c rng/rng.c shared/context.c node/node.c world/world.c udp/udp.c
	$(CC) $(CFLAGS) -o $@ $^

check/tun_netns: check/tun_netns.c tun/tun.c math/math.c world/world.c node/node.c
	$(CC) $(CFLAGS) -o $@ $^

check/tun_drop: check/tun_drop.c tun/tun.c math/math.c world/world.c node/node.c
	$(CC) $(CFLAGS) -o $@ $^

SIM_SRCS = sim/sim.c tun/tun.c math/math.c world/world.c node/node.c shared/context.c heap/heap.c rng/rng.c

check/cable: check/cable.c $(SIM_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

check/mcast: check/mcast.c $(SIM_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

check/concord_two: check/concord_two.c $(SIM_SRCS)
	$(CC) $(CFLAGS) -o $@ $^

check: check/heap_test check/node_test check/world_test check/determinism check/mcast_test check/tun_netns check/tun_drop
	./check/heap_test
	./check/node_test
	./check/world_test
	./check/determinism
	./check/mcast_test
	./check/tun_netns
	./check/tun_drop

check-full: check check/concord_two
	$(MAKE) concord
	sudo ./check/concord_two

concord:
	git -C deps/concord pull --ff-only || git clone https://github.com/podomy/concord.git deps/concord
	cd deps/concord && go build -o $(CURDIR)/concord .

clean:
	rm -f resonance check/heap_test check/node_test check/world_test check/determinism check/mcast_test check/tun_netns check/tun_drop check/cable check/mcast check/concord_two

.PHONY: all check check-full clean concord
