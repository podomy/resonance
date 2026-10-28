# Layer 5: scenario runner

The runner is the loop that holds everything together. Today it is
copied into every scenario file (including the `hold()` helper, which
is byte-identical in 26 files). It is the first new code to write:
one shared loop that future scenarios configure instead of copying.

## The loop, in plain words

Each scenario builds a poll vector with two halves: the first half
watches the TUN file descriptors, the second half watches the child
log pipes. Then it repeats until verdict or deadline:

1. Wait up to 100 ms for anything to happen.
2. For each ready TUN, pump one packet (or drop it, if the fault
   schedule says so).
3. For each ready log pipe, read a chunk and scan for markers.
4. Check the phase machine: does the current phase's exit condition
   hold (meshed, settled, submitted, converged)? If yes, advance.
5. Check the children are still alive. If none are, stop.
6. Check the deadline. If passed, stop and fail.

Queries against the system (workload lists, node lists, journal
greps) are throttled to about one round every 2 s. They shell out,
so running them every 100 ms would drown the machine they measure.

## Phases and holds

A phase machine is just an integer plus exit conditions. Phase 0 is
nearly always "wait for mesh plus settle": the `seen3` flag (full
mesh observed) gates `hold()`, which arms on first call and fires
`HOLD_UP` seconds later. Later phases follow the property: submit,
verify spread, land the fault, verify the wound, heal, verify
reunion. On every transition the scenario resets its timers and
observation flags so stale state never leaks across phases.

## Teardown order

The order is invariant across all scenarios, and the future runner
must keep it:

1. SIGTERM each child, reap it, close its TUN and log descriptors.
   (Dead-by-fault pids are skipped or already reaped; each scenario
   knows its own dead.)
2. Free the simulation context.
3. Delete the network namespaces.
4. Wipe the state directory.
5. Report the verdict.

Failure runs forensics between steps 1 and 4: error lines from logs,
peer-count histograms, journal dumps, or per-round journal
preservation (`concord_spam.c` copies journals aside before wiping).
Pass runs wipe silently. This ordering matters: forensics needs the
files, and the files die at step 4.

## Skips are not failures

A scenario returns success without testing anything when its
preconditions are absent: no `./bin/concord`, no TUN permission, no
test image, tiny host memory. CI reads "skip" as "not applicable
here", which keeps the same suite green on laptops and harness
hosts. The future runner keeps three distinct exits: pass, fail, and
skip with a reason.

## What the shared runner takes as configuration

Looking across all 33 scenarios, a scenario configures exactly these
things and nothing else:

- Node count and which spawn variant to use.
- The fault schedule: which descriptors drop when, which nodes move
  where, who gets killed and restarted how.
- The observation set: which markers, which journal queries, which
  CLI polls.
- The phase machine: ordered phases with exit conditions and holds.
- The verdict: which flags must hold at the end, and what forensics
  to run if they do not.

Everything else (poll vector, pump calls, throttling, liveness,
teardown, skips) is identical and belongs in the library. Porting
one simple scenario (`two` or `partition`) against the new runner is
the proof that the extraction is faithful: same verdicts, no copied
loop.
