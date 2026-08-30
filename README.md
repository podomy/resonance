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
