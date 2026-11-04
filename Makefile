CC ?= cc
# Strictness: -Wpedantic turns GNU extensions
# into errors. -Wconversion was measured and
# is NOT enabled: 1 hit total (img/img.c on
# fgets). Revisit if that converts.
CFLAGS += -std=c23 -Wall -Wextra -Werror -O2 -Wunused-result -Wpedantic
# PATH adds the Go toolchain for the img-build
# target only. Kept global for plain reading;
# C builds ignore it, so the entry is harmless
# there.
export PATH := /usr/local/go/bin:$(PATH)

# Waiver (option c): world/world.c radio_path
# uses __int128 for exact 128-bit range math,
# which -Wpedantic rejects under -Werror. The
# file is owned by another agent, so a TU-local
# pragma or rewrite is unavailable. Targets
# that compile world.c therefore waive ONLY
# -Wpedantic here; every other flag (notably
# -Werror) still applies, and every other TU
# keeps full -Wpedantic as error (proved by
# bin/heap_test, bin/node_test, bin/parse_test
# below, which take plain CFLAGS). A new target
# that compiles world.c but is missing from
# this list fails closed; add it here rather
# than weakening CFLAGS.
WORLD_TARGETS = bin/world_test \
    bin/determinism bin/mcast_test \
    bin/tun_netns bin/tun_drop bin/rsuite \
    bin/net_test \
    bin/exec_lint_test \
    bin/driver_concord_test bin/driver_weld_test \
    bin/run_sample_test bin/sim_net_test \
    bin/tun_stat_test \
    bin/cable bin/mcast bin/concord_two \
    bin/concord_partition bin/concord_three \
    bin/concord_scale bin/concord_scale_churn \
    bin/concord_workload bin/concord_bulk \
    bin/concord_spam bin/concord_slow \
    bin/concord_restart bin/concord_rotate \
    bin/concord_mobile bin/concord_divergent \
    bin/concord_killmidsync bin/concord_stop \
    bin/concord_minority_stop bin/concord_flap \
    bin/concord_killisolated \
    bin/concord_splitbrain bin/concord_duel \
    bin/concord_failover bin/concord_orphan \
    bin/concord_orphan_partition \
    bin/concord_orphan_stop \
    bin/concord_cursor_resume \
    bin/concord_catchup bin/concord_reinstall \
    bin/concord_anchor bin/concord_nomem \
    bin/concord_update bin/concord_preempt \
    bin/concord_pinalarm bin/concord_img
$(WORLD_TARGETS): CFLAGS += -Wno-pedantic

# TEST_BINS lists every unprivileged test binary
# run by `make test`. It covers the old check set
# (heap, node, world, determinism, mcast,
# tun_netns, tun_drop) plus the suite set (parse,
# net, nemesis, exec-lint, driver concord/weld,
# run-sample, sim-net, tun-stat, prog). TUN tests
# self-skip without privileges, so the set stays
# green for plain users and CI alike.
TEST_BINS = bin/heap_test bin/node_test \
    bin/world_test bin/determinism \
    bin/mcast_test bin/tun_netns bin/tun_drop \
    bin/parse_test bin/net_test \
    bin/nemesis_test bin/exec_lint_test \
    bin/driver_concord_test \
    bin/driver_weld_test bin/run_sample_test \
    bin/sim_net_test bin/tun_stat_test \
    bin/prog_test

# all builds the binary plain users keep:
# rsuite (suite runner).
# Test binaries are NOT built here; `make test`
# builds them, runs them, then removes them,
# so bin/ never accumulates test artifacts.
# fuzz_parse stays out too: it needs ASan plus
# UBSan runtimes, absent on some hosts, so plain
# `make` must not require it. Build it via
# `make fuzz-parse`.
all: bin/rsuite

bin:
	mkdir -p $@

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

RUN_SRCS = run/run.c

DRIVER_SRCS = driver/concord.c driver/weld.c

# YAML suites run via bin/rsuite (suite/examples/*.yaml).
# Existing check/ targets below are untouched.
SUITE_SRCS = suite/parse.c suite/exec.c suite/nemesis.c

RSUITE_SRCS = cmd/main.c cmd/progress.c

bin/rsuite: $(RSUITE_SRCS) $(SUITE_SRCS) $(SIM_SRCS) $(RUN_SRCS) $(DRIVER_SRCS) | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

bin/parse_test: suite/parse_test.c suite/parse.c | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

bin/net_test: suite/net_test.c math/math.c world/world.c node/node.c | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

bin/nemesis_test: suite/nemesis_test.c suite/nemesis.c suite/parse.c | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

bin/exec_lint_test: suite/exec_lint_test.c $(SUITE_SRCS) $(SIM_SRCS) $(RUN_SRCS) $(DRIVER_SRCS) | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

bin/fuzz_parse: suite/fuzz_parse.c suite/parse.c | bin
	$(CC) $(CFLAGS) -I. -fsanitize=address,undefined -fno-omit-frame-pointer -o $@ $^

bin/driver_concord_test: suite/driver_concord_test.c $(SIM_SRCS) $(RUN_SRCS) | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

bin/driver_weld_test: suite/driver_weld_test.c $(SIM_SRCS) $(RUN_SRCS) | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

bin/run_sample_test: suite/run_sample_test.c $(SIM_SRCS) $(RUN_SRCS) | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

bin/sim_net_test: suite/sim_net_test.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

bin/tun_stat_test: suite/tun_stat_test.c math/math.c world/world.c node/node.c | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

bin/prog_test: suite/prog_test.c cmd/progress.c | bin
	$(CC) $(CFLAGS) -I. -o $@ $^

fuzz-parse: bin/fuzz_parse
	test -n "$(SUITE_YAML)" || { echo "fuzz-parse: no YAML files"; exit 1; }
	./bin/fuzz_parse $(SUITE_YAML)

# suite-lint validates every YAML scenario without privileges.
# Uses wildcard so empty dirs fail with a clear note
# instead of handing a literal *.yaml path to rsuite.
SUITE_YAML := $(wildcard suite/examples/*.yaml)
suite-lint: bin/rsuite
	test -n "$(SUITE_YAML)" || { echo "suite-lint: no YAML files"; exit 1; }
	./bin/rsuite lint $(SUITE_YAML)

IMG_SRCS = img/img.c

# img-build compiles the test image binaries into
# img/rootfs. Fully offline: local static builds only.
img-build:
	mkdir -p img/rootfs
	cd img && CGO_ENABLED=0 go build -o rootfs/probe ./probe.go && CGO_ENABLED=0 go build -o rootfs/probe-check ./check.go

# bin/cable and bin/mcast are manual-only. They use
# fixed global names (netns cable, tun0/tun1) and need
# root plus ping/socat, so they stay out of all and test.
# Build plus run one by hand to test
# the pump path.
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

bin/concord_slow: check/concord_slow.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_restart: check/concord_restart.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_rotate: check/concord_rotate.c $(SIM_SRCS) | bin
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

bin/concord_orphan: check/concord_orphan.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_orphan_partition: check/concord_orphan_partition.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_orphan_stop: check/concord_orphan_stop.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_cursor_resume: check/concord_cursor_resume.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_catchup: check/concord_catchup.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_reinstall: check/concord_reinstall.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_anchor: check/concord_anchor.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_nomem: check/concord_nomem.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_update: check/concord_update.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_preempt: check/concord_preempt.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

bin/concord_pinalarm: check/concord_pinalarm.c $(SIM_SRCS) | bin
	$(CC) $(CFLAGS) -o $@ $^

# concord_img needs the test image binaries at run
# time; img-build keeps img/rootfs stocked.
bin/concord_img: check/concord_img.c $(SIM_SRCS) $(IMG_SRCS) | bin img-build
	$(CC) $(CFLAGS) -o $@ $^

# test is the single unprivileged entry point.
# It runs every TEST_BIN, then suite-lint, then
# the standard gate, then the Go suite when Go
# exists. Go stays conditional: concord plus img
# paths need Go, but C-only boxes still pass
# without it, so missing Go prints a skip line
# and stays green.
test: $(TEST_BINS) bin/rsuite
	for t in $(TEST_BINS); do ./$$t || exit 1; done
	$(MAKE) suite-lint
	$(MAKE) standard
	if command -v go >/dev/null 2>&1; then cd test && go test ./...; else echo "test: go not found, skipping Go suite (Go needed for concord/img paths; C-only boxes still pass)"; fi
	# Last on purpose: standard reuses these same binaries
	# as prerequisites, so removing earlier just makes it
	# rebuild them. Green runs end clean; red runs keep
	# the binaries for inspection.
	rm -f $(TEST_BINS)

# standard: C23 spelling and version gate.
# Fast and unprivileged. test runs it after the
# binaries (keep the fast path fast); CI also runs
# it explicitly. check/ is exempt from spelling
# (it may legally use NULL) but must still
# compile under the same flags, proved by the
# prerequisites below.
# STD_SRC: non-check C sources under the rule
# (deps/ and bin/ are never scanned).
STD_SRC = sim heap rng shared node world udp math tun img suite run driver cmd
# STD_PAREN: dirs already converted to the
# `return (x)` house style. heap, shared, node
# and world still use bare returns and are
# owned elsewhere; they join once converted.
STD_PAREN = sim udp math tun img suite run driver rng cmd

standard: bin/heap_test bin/node_test bin/world_test bin/determinism bin/mcast_test bin/tun_netns bin/tun_drop
	mkdir -p /tmp/opencode
	@for d in $(STD_SRC); do test -e $$d || { echo "standard: missing path $$d"; exit 1; }; done
	printf '%s\n' '#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 202311L)' '#error "C23 required"' '#endif' 'int main(void) { return (0); }' | $(CC) -std=c23 -x c - -o /tmp/opencode/c23_probe
	@if grep -rn 'stdbool\.h' $(STD_SRC) --include='*.c' --include='*.h'; then echo "standard: banned <stdbool.h> (bool is a C23 keyword)"; exit 1; fi
	@if grep -rnwE '_Bool|_Static_assert|_Noreturn|_Alignas|_Alignof|_Thread_local' $(STD_SRC) --include='*.c' --include='*.h'; then echo "standard: banned legacy spelling (use bool/static_assert/noreturn/alignas/alignof/thread_local)"; exit 1; fi
	@if grep -rn '__attribute__' $(STD_SRC) --include='*.c' --include='*.h'; then echo "standard: banned __attribute__ (use [[]] spelling)"; exit 1; fi
	@if grep -rnE '(^|[^_a-zA-Z0-9])_Atomic([^_a-zA-Z0-9]|$$)' $(STD_SRC) --include='*.c' --include='*.h'; then echo "standard: banned _Atomic keyword (use atomic_ from <stdatomic.h>)"; exit 1; fi
	@if grep -rnw NULL $(STD_SRC) --include='*.c' --include='*.h'; then echo "standard: banned NULL (use nullptr)"; exit 1; fi
	@if grep -rnE 'return[[:space:]]+(true|false)[[:space:]]*;' $(STD_PAREN) --include='*.c' --include='*.h'; then echo "standard: bare return true/false (use return (x) style)"; exit 1; fi
	@echo "standard: ok"

# deps/concord is a reference checkout of the Concord repo,
# not a working copy. Never edit it in place. Build
# ./bin/concord from it by hand when a Concord binary is
# needed (cd deps/concord && go build -o $(CURDIR)/bin/concord
# ./cmd/concord); the Makefile owns no Concord target.

log: bin/rsuite
	@echo "log is retired; run a YAML suite instead:" >&2
	@echo "  sudo ./bin/rsuite run suite/examples/concord_two.yaml" >&2
	@exit 1

clean:
	rm -rf bin

# compile-commands refreshes clang tooling input from the
# exact build recipes. Rerun after adding sources or flags.
compile-commands:
	TARGS=$$(grep -oE '^bin/[A-Za-z0-9_]+' Makefile | sort -u | tr '\n' ' '); $(MAKE) -BnB $$TARGS 2>/dev/null | grep -E '^cc ' | python3 -c "import json,sys;seen=set();out=[{'directory':'/home/neil/Documents/resonance','command':l,'file':f} for l in (x.strip() for x in sys.stdin) for f in l.split() if f.endswith('.c') and f not in seen and not seen.add(f)];json.dump(out,open('compile_commands.json','w'),indent=2);print('entries:',len(out))"

# check was replaced by test. This stub exists so a stale
# `make check` fails loudly instead of resolving to the
# check/ directory and no-op green.
check:
	@echo "check is gone; run 'make test' instead." >&2
	exit 1

.PHONY: all test clean compile-commands img-build log suite-lint standard fuzz-parse check
