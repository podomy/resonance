# Speed: what is slow, what is inherent, and what to do

The suite is slow and that is observable fact, not feeling. A typical
scenario waits 8 s before doing anything, holds partitions 10 s at a
time, waits out 90 s split windows, and carries deadlines up to
600 s. Thirty-three scenarios times that is a long night. This file
separates the slowness you can fix from the slowness you cannot, and
says what to do about each.

## Two clocks

There are two clocks in every run, and they answer to different
owners.

**The harness clock** is everything the test code does: fixed holds,
poll intervals, query throttles, spawning processes, tearing down
namespaces. You own this clock completely. Every second on it is
fixable.

**The SUT clock** is everything inside the binary: gossip probe
intervals, suspicion timeouts, sync batching, leader election
timeouts, journal catch-up. The binary's authors own this clock. The
harness cannot make the binary converge faster than the binary's own
timers allow. Any attempt to hurry it from outside either does
nothing or measures a different system than production.

Most wall time is the SUT clock plus fixed holds stacked on top of
it. That is the honest baseline: the floor for any scenario is how
long the real binaries take to notice, agree, and settle, and no
harness change moves that floor.

## Fixable now: harness time

**Run scenarios in parallel.** This is the largest available win.
Today the suite is serial because run-global names collide: network
namespaces (`resonance_<i>`), TUN devices (`tun<i>`), the state
directory, cgroup paths. Parameterize those names per run and each
scenario gets its own sandbox. Wall time for the suite drops toward
the slowest single scenario instead of the sum. This is the same
change that unblocks Weld coexistence, so it pays twice.

**Batch process spawning.** Starting one node shells out roughly
eight times (make directory, copy certs, copy gossip key, write
config, and so on). Multiply by nodes times scenarios. One shell
invocation per node, or direct file writes instead of shell, removes
seconds per scenario with zero semantic change.

**Build once, share everything.** The Concord binary and the test
image already build once per run. Keep it that way in the new
runner. Never rebuild per scenario.

**Tier the suite.** Not every change needs the 600 s scenarios. Run
the fast core (`two`, `three`, `partition`, `workload`, unit tests)
per pull request and the full matrix nightly. The DEADLINE spread
(90 s versus 300-600 s) already tells you which files belong in
which tier.

**Advance on events, not on clocks (new runner only).** Today the
mesh-settle wait burns the full `HOLD_UP` even when the mesh has
been stable for seconds: `seen3` gates `hold()`, but `hold()` still
waits out all 8 s. Advancing when the meshed signal persists across
consecutive reads keeps the invariant (mesh before fault) and skips
the idle tail. Do not retrofit this into `check/`; frozen scenarios
keep their timings.

## The SUT-clock problem, honestly

Your worry is correct: the binary has its own timers, and they
dominate. Memberlist-style gossip takes seconds to mark a node down
and seconds more to agree it is back. Syncing a 100-workload backlog
takes as long as it takes. Killing a leader costs an election
timeout before a successor assigns. None of this responds to harness
pressure. Pumping packets faster does not make the binary think
faster; polling logs harder does not make it converge sooner.

There are exactly two ways to move the SUT clock, and both live in
the system under test, not in Resonance:

1. **Test hooks in the binary.** The `CONCORD_CLOCK_OFFSET` hook
   (`sim/sim.c`, `sim/sim.h`) proves the pattern: the binary reads an
   environment variable that production never sets. A binary could
   offer shortened gossip intervals or sync batch sizes under an
   explicit test profile. No scenario uses the existing hook today,
   which tells you the team already treats SUT-clock control with
   suspicion.
2. **Test-only configuration.** Shorter timeouts via config the
   harness writes at spawn time. Same mechanism as option 1 with a
   different transport.

The warning matters more than the mechanism: a binary on fast timers
is a different system than the production binary. Mechanism tests
(does reunion restore one mesh, does a tombstone win) stay valid on
fast clocks. Timing tests (does failover complete within the
election budget, does suspicion tolerate exactly this lag) do not.
Keep every timing-sensitive verdict at production clocks, and mark
any fast-clock run as testing logic only. If a fast run passes and
production clocks fail, the fast run told you nothing.

## What to do Monday morning

1. Instrument one run first: log timestamps at spawn done, first
   `peers:N`, every phase transition, and verdict. Expect 60-80
   percent of wall time to be fixed holds plus SUT settle. That
   single log tells you the parallelization plus event-advance
   covers nearly all of the fixable time.
2. Parameterize the run-global names. Unblocks parallelism and Weld.
3. Tier the suite into per-PR and nightly.
4. Only then consider SUT test hooks, owned by the Concord and Weld
   teams, with fast-clock runs clearly labeled logic-only.

Do these in order. Each step pays for itself before the next begins,
and none of them changes what any frozen scenario asserts.
