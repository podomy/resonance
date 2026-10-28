# Layer 3: fault control

Faults are how scenarios earn their keep. Every fault in Resonance
works from outside the binary. The binary never cooperates, never
gets a test hook called, never knows it is being tested. There are
five mechanisms, and every scenario is built from them.

## 1. Packet drop (partition)

The workhorse. Instead of pumping a TUN file descriptor, read its
packet and throw it away:

```c
n = read(fd, junk, sizeof(junk));   /* drop */
tun_pump_fd(map, fd, grid, radio, nodes);  /* forward */
```

`check/concord_partition.c:28-41` shows the shape. Seventeen files
use it with different predicates: one node isolated by index (the
common `i == ISOLATE` form), all descriptors dropped (full cut in
`partition`, three-way split in `splitbrain` and `orphan_partition`),
per-node arrays with a rotating victim (`spam`), an elected victim
(`duel`), or a dead leader whose packets blackhole forever
(`failover`). Reunion is simply resuming the pump. Nothing on the
binary side changes state; the world just goes quiet and comes back.

## 2. Multicast blinding

Pump everything except IPv4 multicast. mDNS discovery goes blind
while unicast gossip, sync, and Noise keep flowing. Only
`check/concord_anchor.c:34` uses it, to prove that provisioned
rendezvous anchors carry discovery alone. If you need the one-line
version: this fault unplugs the LAN's shouting channel and checks the
phone book still works.

## 3. Moving nodes (radio truth)

Change a node's simulated position and let `radio_path` do the
partitioning. `move_node` in `check/concord_mobile.c:36-51` drives
node 2 from 2 m (`X_HOME`) to 7 m (`X_AWAY`), past the 2.5 m range,
and back, without any file descriptor ever being dropped. The static
version is `check/tun_drop.c:33-42`: one node parked ten meters out
so physics drops everything while the pump runs normally. Prefer
moves over drops when the property under test is about radio reality
(range, mobility, reunion without reconnects) rather than about
packet loss.

## 4. Lag batching

`pump_slow` in `check/concord_slow.c:32-46` drains a node's packets
only once every `SLOW_SECS` (2 s). Nothing is lost; everything is
late. Only `concord_slow` uses it. It proves convergence tolerates a
laggy link with flapping suspicion, and it is the mechanism a future
slow-subscriber scenario (for example Weld backpressure) would reuse.

## 5. Killing processes

Final cleanup everywhere uses SIGTERM. Faults use SIGKILL, except the
key-rotation restarts, which use SIGTERM because the node cooperates
by rotating first. The kill matrix (who dies, when, restarted which
way) is in `docs/layer2-lifecycle.md`. From this layer's view a kill
is just another partition with a process attached: the node's packets
stop because nobody reads them, and restart reattaches it.

## Timers that schedule faults

Three timer flavors appear in every scenario, all built on the same
`hold()` helper (arms on first call, fires N seconds later):

- `HOLD_UP` (usually 8 s): mesh must be up and settled before the
  fault lands. Never skip it; every reunion verdict assumes a meshed
  start.
- `HOLD_DOWN` (usually 10 s) and friends (`HOLD_SPLIT` 90 s,
  `ISOLATE_SECS` 12 s): how long the fault lasts.
- `DEADLINE` (90 to 600 s): the backstop. If the verdict is not
  reached by then, the scenario fails instead of hanging forever.

## The rule

Faults arrive through pump predicates, node positions, and signals
only. If a fault needs the binary's cooperation, it is not a fault,
it is a feature request. File it against the system under test.
