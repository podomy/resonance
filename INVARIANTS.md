# Invariants

Each scenario in `check/` names the Concord behavior it
pins down. The behavior is stated as an invariant: a
property that must hold no matter how the fault lands.
Scenarios are the enforcement; this file is the index.

## Mesh convergence

Every connected segment agrees on who is alive, and
reunion restores one mesh.

- `concord_two`: two nodes mesh.
- `concord_three`: 3-node mesh survives isolate/reunion.
- `concord_partition`: 2-node mesh survives
  drop/reunion.
- `concord_restart`: a SIGKILLED node restarted fresh
  rejoins the mesh.
- `concord_mobile`: a node driven out of radio range
  and back re-meshes without any fd ever dropping.
- `concord_flap`: mesh returns to `peers:3` after every
  one of three isolate/restore cycles.
- `concord_scale`: 5-node mesh plus workload converge
  after isolate/reunion.
- `concord_scale_churn`: a SIGKILLED node restarted
  fresh resyncs full journals and rejoins the 5-node
  mesh.

## Workload propagation

The journal is a grow-only set, merge is union, so
every submitted workload reaches every connected node.

- `concord_workload`: submitted on an isolated node,
  shows up on the other two after reunion.
- `concord_bulk`: five submitted in isolation all
  arrive.
- `concord_divergent`: one submitted per side, all
  arrive.
- `concord_splitbrain`: three isolated nodes, one each,
  all three everywhere after heal.
- `concord_slow`: converges over a laggy link; suspect
  flapping is tolerated.
- `concord_spam`: three beating rounds of submit, stop,
  isolate, and SIGKILL converge every time.
- `concord_killmidsync`: a workload survives SIGKILL of
  its node mid-sync.
- `concord_killisolated`: a node killed mid-partition
  and restarted fresh converges after reunion.

## Scheduling liveness

Exactly one leader assigns, and death promotes a
successor.

- `concord_failover`: after the leader is SIGKILLED, a
  new leader assigns a fresh workload on the survivors.

## Deterministic conflicts

One workload ID with several spec copies resolves the
same way on every node, regardless of arrival order.

- `concord_duel`: two split leaders assign the same ID;
  after reunion all nodes agree on one assignee.

## Tombstone dominance

A stop beats any live spec copy, with or without quorum,
at any epoch.

- `concord_stop`: stopped on one side of a partition,
  stays stopped everywhere after reunion.
- `concord_minority_stop`: stopped on the lone side,
  stays stopped without quorum behind it.
- `concord_orphan_stop`: stopped on an isolated owner
  beats the pair side reassignment at a higher epoch.

## Epoch reassignment

A spec pinned to a dead node moves to a live node with
a bumped epoch; reunion keeps the max epoch copy.

- `concord_orphan`: SIGKILL the owner, a survivor
  reassigns at epoch 1, survivors converge on one copy.
- `concord_orphan_partition`: split three ways, every
  non-owner side orphans independently, reunion keeps
  the max epoch copy.

## Key pinning

First valid key per node ID sticks; rotation bumps
generation and re-pins.

- `concord_rotate`: rotate one node's Noise key in
  place, peers re-pin at generation 1, mesh and workload
  converge after.

## Sync durability

Cursors remember what each node already pulled from each
peer, persist across restarts, and unknown cursors fall
back to the beginning instead of sticking. The serving
index finds the position without re-reading the file,
and stale entries heal by overwrite on first use.

- `concord_cursor_resume`: SIGKILL a node, restart it
  with the same dir. Identity, journal, cursor store,
  and pins persist; only the process dies. Re-mesh,
  then workload convergence on all nodes.
- `concord_catchup`: a 100-workload backlog built on an
  isolated node drains onto all nodes after reunion.
- `concord_reinstall`: a node stopped, wiped of journal
  plus kv store, and restarted under the same id and
  Noise key rejoins cleanly. Peers fall back past stale
  cursors, the node re-pulls full history, workload
  converges everywhere.

## Discovery

mDNS carries LAN discovery; provisioned rendezvous
anchors carry it where multicast cannot go. Either path
alone must mesh the fleet.

- `concord_anchor`: node 0 serves as anchor, all nodes
  carry it in config, multicast dropped from boot.
  Mesh plus workload convergence on the anchor path
  alone.

## Out of scope

There is no Byzantine outsider scenario, by
construction rather than omission. The fleet is
single-tenant, every node is operator-provisioned with
the same CA and gossip key, and a stranger can neither
join, gossip, nor handshake. See the threat model in
`deps/concord/docs/noise.md`. Faults in scope are
own-node corruption, loss, and partition, which the
rules above absorb.
