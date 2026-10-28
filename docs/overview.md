# Resonance, layer by layer

Resonance runs real binaries in isolated network namespaces and checks
that they converge as a cluster while the network misbehaves. It was
built for Concord, and it is being opened up so Weld can use the same
machinery for ROS and DDS testing.

If you are new, read this file first, then the layer files in order.
Each layer file explains one part of the system: what it is, why it
exists, and which source files implement it.

## The six layers

Every scenario does the same six things. The layers are the same six
things, split apart so each can be reused on its own.

1. **Simulation core** (`docs/layer0-sim.md`). A clock, an event queue,
   a node list, a radio world, a random generator, and in-process UDP.
   No processes, no network, no binaries. Pure data structures with
   deterministic replay.
2. **Mesh transport** (`docs/layer1-mesh.md`). Real packets through real
   TUN devices, gated by the simulated radio. This is how unmodified
   binaries talk to each other through a fake physical world.
3. **Process lifecycle** (`docs/layer2-lifecycle.md`). Starting,
   killing, and restarting real binaries inside network namespaces,
   with four different restart meanings (fresh, preserved, re-keyed,
   wiped).
4. **Fault control** (`docs/layer3-faults.md`). Dropping packets,
   blinding multicast, moving nodes out of radio range, slowing links,
   and killing processes. All faults work from outside the binary.
5. **Observation** (`docs/layer4-observation.md`). Reading child logs,
   journals on disk, and CLI output to decide what the cluster did.
6. **Scenario runner** (`docs/layer5-runner.md`). The poll loop, the
   phase machine, the timers, teardown, and the verdict.

Speed is its own topic (`docs/speed.md`). The short version: some
slowness is the harness and can be fixed, and some slowness is the
binary's own timers and cannot be fixed from outside. That file tells
the two apart.

## How one scenario runs

Here is the whole story in ten steps, using `check/concord_three.c`
as the example. Three nodes mesh, one gets isolated, then it rejoins.

1. Check that `./bin/concord` exists. If not, skip quietly.
2. Build the simulation core: clock at zero, empty node list, empty
   radio grid.
3. Create one network namespace per extra node.
4. Open one TUN device per node (node 0 in the host namespace, the
   rest inside their own namespace).
5. Register the nodes and give them underlay addresses
   (`192.168.100.1/2/3`, a range kept disjoint from the overlay
   `10.0.0.0/16` the binaries use among themselves).
6. Fork one real Concord binary per node, each with its own config
   home, and collect their log output through pipes.
7. Poll in a loop: pump packets between TUNs through the radio check,
   and scan log lines for `"peers":3`.
8. Once meshed and settled, stop pumping one node's packets
   (read and discard instead of forward). That node is partitioned.
9. After a hold time, resume pumping. Watch for `"peers":3` again
   plus the `peer.lost` marker from the partition window.
10. Kill everything, free memory, delete namespaces, wipe state,
    and return pass or fail.

Steps 2 to 5 are Layer 1. Step 6 is Layer 2. Steps 8 and 9 are
Layer 3. Step 7's log scanning is Layer 4, and the loop holding it
all together is Layer 5.

## Two rules for the whole codebase

- `check/` is frozen. Existing scenarios are read-only. New behavior
  is added beside them, never by editing them.
- The mesh is the only channel between nodes. Anything that bypasses
  the mesh (today only the image push in `img/`) must be declared and
  documented, never silent.

## Running the fast checks

```sh
make                    # build ./bin/resonance
make check              # fast checks, no Concord binary needed
sudo make check-full    # all scenarios, needs ./bin/concord
```

`make check` needs no privileges for most binaries. The two TUN tests
skip gracefully without them.
