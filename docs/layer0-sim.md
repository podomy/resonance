# Layer 0: simulation core

The simulation core is pure computation. It knows nothing about
processes, network namespaces, TUN devices, Concord, Weld, ROS, or DDS.
Given the same seed it produces the same event stream twice, which is
what makes simulation failures reproducible.

## The pieces

**Event heap** (`heap/heap.h`, `heap/heap.c`). A queue ordered by
(time, sequence number). Two events at the same timestamp run in the
order they were scheduled, so replays are stable. Tested on its own
by `check/heap_test.c`.

**Node list** (`node/node.h`, `node/node.c`). The live nodes, packed in
an array, plus an index from node id to array slot so lookup by id is
cheap and removal is a swap with the last element. Tested by
`check/node_test.c`.

**Radio world** (`world/world.h`, `world/world.c`). A grid of cells,
each cell filled with a material. Air passes radio waves, rock and
water and the rest block them. `radio_path` answers one question:
given two positions and a radio range, can they hear each other, and
with what delay. Positions are integers in nanometers, so there is no
floating point anywhere in the verdict path. Tested by
`check/world_test.c`, which pins exact delay values.

**Random generator** (`rng/rng.h`, `rng/rng.c`). A fixed generator
(xoshiro with a splitmix seeder). The file header says the
implementation is frozen, and it means it: changing one output value
breaks replay of every recorded run. All simulated randomness must go
through `rng_u64` after `rng_seed`.

**In-process UDP** (`udp/udp.h`, `udp/udp.c`). A tiny UDP for tests
that run without real binaries. Delivery goes through the same
`radio_path` check as real packets, then lands in the event heap.
Raw binaries never call this; they use kernel sockets and a TUN.
It exists for `make check` and small experiments. `check/mcast_test.c`
shows the intended use: bind three nodes, move one, assert delivery.

**Context** (`shared/context.h`, `shared/context.c`, `shared/events.h`).
The object that holds everything above: clock, heap, RNG, node list,
grid, radio parameters, UDP binds. `context_init` builds it,
`context_free` releases it, `context_run` drains the heap. The event
callback contract is in `shared/events.h`: the payload is owned by the
engine and freed when the callback returns, so copy anything you keep,
and never destroy the context from inside a handler.

## The determinism contract

`check/determinism.c` pins the two promises this layer makes. Same
seed gives the same random stream, and the same schedule gives the
same trace. If either ever breaks, that test fails, and nothing above
this layer can be trusted until it passes again.

## What does not belong here

Anything that touches the operating system beyond memory: no `fork`,
no `system`, no sockets, no files, no clocks except the simulated
one. If a function needs a binary, a namespace, or a config file, it
lives in a higher layer.
