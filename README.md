<h1 align="center">Resonance</h1>

<p align="center">
  <a href="https://github.com/podomy/resonance/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/podomy/resonance/ci.yml?label=linux" alt="Linux"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/podomy/resonance" alt="License"></a>
</p>

Resonance simulates autonomous nodes moving in physical 2D space and the
network mesh as nodes move in and out of range.

Deterministic discrete-event model with seeded RNG. Same scenario, same
outcome. Reachability and delay from positions and medium.

Complements Concord. Concord coordinates fleets through partitions, Resonance
simulates the physical world that creates them.

### Build and test

```sh
make          # build ./resonance
make check    # build and run all checks
make clean    # remove binaries and test artifacts
```

Compiler is `cc` with `-std=c11 -Wall -Wextra -Werror -O2`.

### Project structure

- `world/`: medium grid and radio propagation
- `node/`: node identity, position, velocity
- `shared/`: discrete-event context and scheduling
- `heap/`: min-heap for event queue
- `rng/`: seeded RNG
- `udp/`: UDP delivery through simulated medium
- `tun/`: TUN device and forwarding
- `math/`: helpers
- `check/`: verification programs

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

Last two tests skip without `CAP_NET_ADMIN`.

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
