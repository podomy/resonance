CC ?= cc
CFLAGS += -std=c11 -Wall -Wextra -Werror -O2
export PATH := /usr/local/go/bin:$(PATH)

all: bin/resonance

bin:
	mkdir -p $@

bin/resonance: main.c sim/sim.c heap/heap.c rng/rng.c shared/context.c node/node.c world/world.c udp/udp.c math/math.c tun/tun.c | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/heap_test: check/heap_test.c heap/heap.c | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/node_test: check/node_test.c node/node.c | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/world_test: check/world_test.c world/world.c | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/determinism: check/determinism.c heap/heap.c rng/rng.c shared/context.c node/node.c world/world.c udp/udp.c | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/mcast_test: check/mcast_test.c heap/heap.c rng/rng.c shared/context.c node/node.c world/world.c udp/udp.c | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/tun_netns: check/tun_netns.c tun/tun.c math/math.c world/world.c node/node.c | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/tun_drop: check/tun_drop.c tun/tun.c math/math.c world/world.c node/node.c | bin
	$(CC) $(CFLAGS) -o $@ $^

SIM_SRCS = sim/sim.c tun/tun.c math/math.c world/world.c node/node.c shared/context.c heap/heap.c rng/rng.c

bin/cable: check/cable.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/mcast: check/mcast.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_two: check/concord_two.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_partition: check/concord_partition.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_three: check/concord_three.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_scale: check/concord_scale.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_scale_churn: check/concord_scale_churn.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_workload: check/concord_workload.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_bulk: check/concord_bulk.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_spam: check/concord_spam.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_restart: check/concord_restart.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_mobile: check/concord_mobile.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_divergent: check/concord_divergent.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_killmidsync: check/concord_killmidsync.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_stop: check/concord_stop.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_minority_stop: check/concord_minority_stop.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_flap: check/concord_flap.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_killisolated: check/concord_killisolated.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_splitbrain: check/concord_splitbrain.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_duel: check/concord_duel.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_failover: check/concord_failover.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

check: bin/heap_test bin/node_test bin/world_test bin/determinism bin/mcast_test bin/tun_netns bin/tun_drop
	./bin/heap_test
	./bin/node_test
	./bin/world_test
	./bin/determinism
	./bin/mcast_test
	./bin/tun_netns
	./bin/tun_drop

check-full: check bin/concord_two bin/concord_partition bin/concord_three bin/concord_scale bin/concord_scale_churn bin/concord_workload bin/concord_bulk bin/concord_spam bin/concord_restart bin/concord_mobile bin/concord_divergent bin/concord_killmidsync bin/concord_stop bin/concord_minority_stop bin/concord_flap bin/concord_killisolated bin/concord_splitbrain bin/concord_duel bin/concord_failover
	$(MAKE) concord
	sudo ./bin/concord_two
	sudo ./bin/concord_partition
	sudo ./bin/concord_three
	sudo ./bin/concord_scale
	sudo ./bin/concord_scale_churn
	sudo ./bin/concord_workload
	sudo ./bin/concord_bulk
	sudo ./bin/concord_spam
	sudo ./bin/concord_restart
	sudo ./bin/concord_mobile
	sudo ./bin/concord_divergent
	sudo ./bin/concord_killmidsync
	sudo ./bin/concord_stop
	sudo ./bin/concord_minority_stop
	sudo ./bin/concord_flap
	sudo ./bin/concord_killisolated
	sudo ./bin/concord_splitbrain
	sudo ./bin/concord_duel
	sudo ./bin/concord_failover

concord: | bin
	git -C deps/concord pull --ff-only || git clone https://github.com/podomy/concord.git deps/concord
	cd deps/concord && go build -o $(CURDIR)/bin/concord .

log: bin/resonance
	[ -x ./bin/concord ] || $(MAKE) concord
	sudo ./bin/resonance 2>&1 | tee current.log

clean:
	rm -rf bin

.PHONY: all check check-full clean concord log
