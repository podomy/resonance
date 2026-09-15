<h1 align="center">Resonance</h1>

<p align="center">
  <a href="https://github.com/podomy/resonance/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/podomy/resonance/ci.yml?label=linux" alt="Linux"></a>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/podomy/resonance" alt="License"></a>
</p>

Resonance is a high-fidelity simulator and regression harness for
[Concord](https://github.com/podomy/concord): it runs real Concord
binaries in isolated network namespaces over a simulated radio mesh,
and asserts cluster convergence across partition, fault, and scale
scenarios. It was primarily made for Concord to create a flywheel of
software and simulation.

### Build and test

```sh
make                    # build ./bin/resonance
make concord            # fetch Concord from GitHub and rebuild if new
make check              # fast checks, no Concord
sudo make check-full    # all checks, needs ./bin/concord
make log                # run demo, tee output to current.log
make clean              # remove binaries and test artifacts
```

### Contributing

Discuss your change with the engineering team at [contact@podomy.com](mailto:contact@podomy.com) before opening a PR in order not to waste anybody's effort or time.

Announcements and engineering updates on distributed systems, consensus algorithms, and robotic fleet coordination are shared via our newsletter at [podomy.com](https://podomy.com).

- [Commit message format](./COMMITS)
- [Contributor license agreement](./CLA)
- [Contributing](./CONTRIBUTING)

### License

Resonance is distributed under the GNU Affero General Public License v3.0 or
later. See [LICENSE](./LICENSE).
