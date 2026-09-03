<h1 align="center">Resonance</h1>

<p align="center">
  <a href="https://github.com/podomy/resonance/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/podomy/resonance/ci.yml?label=linux" alt="Linux"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/podomy/resonance" alt="License"></a>
</p>

Resonance simulates autonomous nodes moving in physical 2D space and the
network mesh and interference as they move in and out of range. It was
primarily made for [Concord](https://github.com/podomy/concord) to create a flywheel of software and simulation.

### Build and test

```sh
make                    # build ./resonance
make concord            # fetch Concord from GitHub and rebuild if new
make check              # fast checks, no Concord
sudo make check-full    # all checks, needs ./concord
make clean              # remove binaries and test artifacts
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
