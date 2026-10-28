# Layer 2: process lifecycle

Process lifecycle starts real binaries inside the namespaces that
Layer 1 built, and restarts them with precise meanings. The mesh
moves packets; this layer moves processes.

## Starting a node

`spawn_one` in `sim/sim.c` does the work for one node. In plain
words, it prepares an isolated home for the node and then forks:

1. Make a per-node config home (`STATEDIR/node<i>`).
2. Copy the shared CA certificates into it, so the node can prove
   who it is.
3. Copy the shared gossip key into it, so the node joins this fleet
   and no other.
4. Write the node's config: a fresh id, its memberlist and advertise
   addresses on the underlay.
5. Fork. In the child, enter the node's network namespace (except
   node 0, which lives in the host namespace), point config home at
   the prepared directory, redirect output into a log pipe, and exec
   the binary.
6. In the parent, place the child in its per-node cgroup (so each
   node's containers get their own subtree) and keep the pid and the
   log pipe.

Variants on top: `sim_spawn_concord` (plain), `sim_spawn_concord_skew`
(one node with `CONCORD_CLOCK_OFFSET` set, a test time hook the
binary provides), `sim_spawn_concord_anchor` (one node started with
`--anchor` and every config pointed at it), and
`sim_spawn_concord_debug` (verbose logging for diagnosis only).

Note that seeding shells out several times per node (mkdir, cp,
config printf). That cost repeats for every node of every scenario
and is one of the known speedups in `docs/speed.md`.

## Restarting a node: four meanings

"Restart" sounds like one thing but the scenarios pin four different
meanings. Each meaning tests a different durability promise, so the
future driver interface must keep them separate instead of offering
one restart function.

1. **Fresh identity, wiped directory.** `sim_restart_concord`: delete
   the node's whole home and start it as a stranger. Used by
   `check/concord_restart.c:95-102`, and while partitioned or
   mid-sync by `concord_killisolated.c`, `concord_killmidsync.c`,
   `concord_scale_churn.c`, and `concord_spam.c`.
2. **Same directory preserved.** `restart_preserve` in
   `check/concord_cursor_resume.c:152-198`: SIGKILL the process but
   keep identity, journal, cursor store, and pins. Only the process
   dies. Proves cursors resume instead of re-pulling everything.
3. **Same directory, key surgery.** `restart_node` in
   `check/concord_pinalarm.c:159-229` (delete the Noise key, or run
   `node rotate-key` for a generation bump) and `restart_rotated` in
   `check/concord_rotate.c:136-189`. Proves first-key-pins and
   rotation behavior.
4. **Same identity, wiped state.** `restart_wiped` in
   `check/concord_reinstall.c:153-215`: keep the id and key, delete
   journal plus kv store, rejoin empty. Proves peers fall back past
   stale cursors and the node re-pulls full history.

Two more cases kill and never restart: the elected leader in
`check/concord_failover.c:298-305` (a successor must take over
scheduling) and the workload owner in `check/concord_orphan.c:374-376`
(a survivor must reassign its work at a bumped epoch).

## What a Weld driver must provide

The same verbs with different bodies: start a weld node per netns,
start ROS or DDS peers where the scenario needs them, kill and
restart each kind with stated durability (weld nodes keep their
stores the way Concord keeps its journal; ROS peers are usually
stateless and restart fresh). The mesh, the faults, and the runner
never learn these details. They call `spawn`, `kill`, and `restart`
with a durability argument and read back what the driver reports.

## A hook that exists but sleeps

`sim_spawn_concord_skew` sets `CONCORD_CLOCK_OFFSET` for one node,
shifting that node's clock. No scenario uses it today. It is the
only SUT-clock control the harness has, and `docs/speed.md` explains
why touching SUT clocks is delicate.
