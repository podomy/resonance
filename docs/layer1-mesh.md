# Layer 1: mesh transport

The mesh transport answers one question: how do unmodified binaries
talk to each other through a fake physical world. The answer is real
packets through real TUN devices, with a simulated radio deciding
which packets arrive.

Nothing in this layer knows which binary runs at the other end of a
TUN. Concord gossip, DDS discovery, ROS traffic, and plain `ping`
all look the same here: IPv4 packets to forward or drop.

## The setup sequence

Every scenario runs the same preamble, in this order
(see `check/concord_two.c:39-54` for the reference shape):

1. `context_init(&ctx, 32, 1)`. Build the Layer 0 core. The arguments
   (heap room for 32 events, seed 1) are identical in all 33 Concord
   scenarios.
2. `sim_netns_setup(n)`. Create one network namespace per extra node.
   Node 0 stays in the host namespace so there is always a reference
   point.
3. `sim_tuns_open(fds, n)`. Open `tun0` in the host namespace and
   `tun1..` inside their namespaces. If this fails (no permission, no
   TUN support), the scenario skips instead of failing.
4. `sim_nodes_add(&ctx, &map, fds, n)`. Register one simulated node
   per TUN file descriptor, spaced along the x axis, and remember
   which descriptor belongs to which node and which IP.
5. `sim_addrs_up(n)`. Assign underlay addresses `192.168.100.1/2/3`
   and bring the links up, including multicast routes. The underlay
   is the harness's own wiring and is kept disjoint from the overlay
   (`10.0.0.0/16`) that the binaries use among themselves.

The underlay/overlay split is worth understanding. The binaries form
their own mesh on overlay addresses and never see the underlay. The
harness moves underlay packets and applies radio physics to them.
That is what lets the harness partition, slow, or move nodes without
the binary cooperating.

## How one packet travels

`tun_pump_fd` in `tun/tun.c` does the whole job for one packet:

1. Read one packet from the source TUN.
2. Ignore anything that is not IPv4.
3. If it is multicast (`224.0.0.0/4`), offer it to every other node,
   checking the radio separately for each pair.
4. If it is overlay broadcast (`10.0.0.255`), same treatment.
5. Otherwise look up the destination IP in the map and offer it to
   exactly that node.
6. "Offer" means: ask `radio_path` whether the two node positions can
   hear each other. If yes, write the packet to the destination TUN.
   If no, drop it silently.

`tun_pump_fd_nomcast` is the same function with multicast dropped
instead of broadcast. Only `check/concord_anchor.c:34` uses it, to
prove discovery works through configured anchors while mDNS is blind.

## Proof without any binary

Four scenarios use this layer with no system under test at all:

- `check/cable.c`: two nodes, `ping` must succeed through the pump.
- `check/tun_netns.c`: same across namespaces, `ping` must succeed.
- `check/tun_drop.c`: one node placed ten meters away, past the
  2.5 m radio range, `ping` must fail. The pump runs fine; physics
  drops every packet.
- `check/mcast.c`: multicast listener and sender through the pump.

If you are new to the codebase, read `check/cable.c` first. It is the
shortest complete trip through this layer.

## The one wart: global names

Today the namespace names (`resonance_<i>`), device names (`tun<i>`),
and state directory (`STATEDIR` in `sim/sim.h`) are fixed globals.
That is why the full suite must run one scenario at a time: two runs
would grab the same names. Parameterizing these names per run is the
first refactoring step, and it unblocks parallel runs and Weld
coexistence at once. Nothing about the packet path changes.
