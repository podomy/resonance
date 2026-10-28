# Layer 4: observation

Observation is how a scenario decides what the cluster did. There are
three channels: child logs, files on disk, and CLI output. Log
scanning and the polling rhythm are generic and belong in the future
runner. Journal shapes and CLI verbs are per-system and belong in the
driver.

## Channel 1: child log pipes

Every spawned binary has its stdout and stderr redirected into a
pipe. The scenario reads chunks and searches for marker strings.
The two markers that carry nearly every mesh verdict:

- `"peers":N` (sometimes with a space, `"peers": N`; scenarios check
  both). Means this node currently sees a full mesh.
- `peer.seen` / `peer.lost`. Means a peer arrived or vanished.

Specialist markers appear where the property needs them: `pin
mismatch` and `peer key rotated` in the key scenarios
(`concord_pinalarm.c`, `concord_rotate.c`), `peer sync ok` plus a
peer address in `concord_killmidsync.c:178-180` (kill exactly while
syncing). On failure, scenarios dump recent error lines before
cleaning up (`grep '"level":"error"' ... | tail`), so the log that
decided the run is visible. Two scenarios (`img`, `stop`) also tee
child output to files under STATEDIR for post-mortem; the rest read
pipes only.

## Channel 2: files on disk

Each node's home holds a journal (`journal.jsonl`), a config
(`config.json`), and stores. Scenarios read them with shell greps
(they never write, except the key and journal surgery listed below):

- `AssignedNodeID` per workload id: who is scheduled where. The
  `assigned_anywhere` helper (non-nil id in any journal) gates most
  workload scenarios.
- `AssignmentEpoch`: which reassignment generation a copy belongs
  to. Orphan, duel, update, and preempt verdicts compare epochs.
- `Image`: which container image a workload copy carries. Only the
  update scenario reads it.
- Node `id` from `config.json`: maps journal entries back to nodes,
  used to elect victims and leaders (lowest UUID) and to find owners.
- `workload.instance.running`: the image supply chain's end state.
  Only `concord_img` reads it.

The only writes to node state are the durability tests themselves:
deleting the Noise secret key (`pinalarm`), deleting journal plus kv
store (`reinstall`), and running `node rotate-key` (`pinalarm`,
`rotate`). Everything else is read-only.

## Channel 3: CLI verbs

Scenarios drive the system the way an operator would, with the config
home pointed at one node:

- `workload run IMAGE`: submit, parse `Submitted workload <uuid>`.
  Flags extend it: `--memory` for the no-fit and preemption
  scenarios.
- `workload list`: poll every 2 s, match the first 8 uuid characters.
  This is the standard "has it arrived everywhere" check.
- `workload stop <id>`: look for `Stopped`. Tombstone scenarios
  assert the stopped copy wins everywhere after reunion.
- `workload update --image <image> <id>`: only the update scenario.
- `node list`: count `alive` entries for mesh-size verdicts at scale.
- `node rotate-key`: only the key scenarios.
- Image supply (`img_push`, `img_catalog`, `img_tags`): only
  `concord_img`. Push the test image into every registry from inside
  its own namespace, read back catalogs and tags, submit, wait for a
  running instance.

## The declared bypass

`img_push` deliberately avoids the mesh: it talks to each node's
registry over loopback inside that node's own namespace, because the
filesystem is shared across namespaces. No byte crosses the
simulated radio, so pushing never stalls behind a partition. This is
the one sanctioned bypass, and it is documented at the top of
`img/img.h`. Any future bypass (for example seeding ROS bags) gets
the same treatment: explicit, documented, never silent.

## What goes where

Runner: poll rhythm, log scanning loop, query throttling, verdict
plumbing, failure forensics. Driver: journal shapes, CLI verbs,
marker strings, the bypass helpers. When Weld arrives, only driver
code is new: topic echo capture instead of journal grep, `weld pub`
instead of `workload run`, same runner underneath.
