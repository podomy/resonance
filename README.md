<h1 align="center">Resonance</h1>

<p align="center">
  <a href="https://github.com/podomy/resonance/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/podomy/resonance/ci.yml?label=linux" alt="Linux"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/podomy/resonance" alt="License"></a>
</p>

Resonance is a physical-world simulation environment for distributed systems.
It models autonomous nodes -- drones, rovers, satellites, edge vehicles --
moving through physical 2D space and realistically simulates the underlying
network mesh as nodes move in and out of range.

Resonance is designed for deterministic, reproducible experimentation.
Mobility, radio propagation, and packet delivery are driven by an explicit
discrete-event model with a seeded RNG, so the same scenario yields the same
outcome on every run. The simulation does not hide the physical layer behind
an ideal link abstraction; it computes reachability and delay from positions
and the medium that lies between them.

Resonance complements Concord. Concord coordinates a fleet when the network
is degraded or partitioned; Resonance simulates the world that causes those
partitions -- terrain, distance, and interference -- before the fleet is
deployed.

### Build and test

```sh
make          # build ./resonance
make check    # build and run all checks
make clean    # remove binaries and test artifacts
```

Compiler is `cc` with `-std=c11 -Wall -Wextra -Werror -O2`.
No external dependencies.

### Project structure

- `world/` -- 2D medium grid and radio propagation (`MediumGrid`, `Material`, `RadioParams`, `radio_path`)
- `node/` -- node identity, position, velocity, and indexed storage (`Node`, `NodeList`)
- `shared/` -- discrete-event context, event heap, and deterministic scheduling (`Context`)
- `heap/` -- min-heap backing the event queue
- `rng/` -- seeded deterministic RNG
- `udp/` -- UDP bindings and delivery through the simulated medium
- `tun/` -- TUN device integration and `TunMap` forwarding
- `math/` -- fixed-point and geometric helpers
- `check/` -- verification programs run by `make check`

### Checks

| Test | What it covers |
|------|----------------|
| `check/heap_test` | Min-heap ordering and invariants |
| `check/node_test` | `NodeList` indexing and swap-remove |
| `check/world_test` | Grid initialization and radio reachability |
| `check/determinism` | End-to-end determinism under a fixed seed |
| `check/mcast_test` | Multicast delivery through the medium |
| `check/tun_netns` | TUN forwarding across network namespaces (needs `CAP_NET_ADMIN`) |
| `check/tun_drop` | TUN drop on unreachable path (needs `CAP_NET_ADMIN`) |

The last two tests gracefully skip when the runner lacks permission to
create TUN devices or network namespaces.

### Contributing

Discuss your change with the community before opening a PR

[dev@podomy.com](mailto:dev@podomy.com)

[Archive of the past messages can be found here.](https://archive.podomy.com)

You must subscribe to receive responses.

- [Commit message format](./COMMITS)
- [Contributor license agreement](./CLA)
- [Contributing](./CONTRIBUTING)

### License

Resonance is distributed under the GNU Affero General Public License v3.0 or
later. See [LICENSE](./LICENSE).
