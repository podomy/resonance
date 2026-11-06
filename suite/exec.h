#ifndef SUITE_EXEC_H
#define SUITE_EXEC_H

#include <stddef.h>

// Schema notes: one condition per
// phase in v1 (chain phases for
// AND). A stop or workload read with
// no earlier submit fails lint. x_m
// holds meters; its grammar is the
// float_of decimals (see exec.c).
// Unknown needs verbs fail lint;
// runtime still skips unknown needs
// it cannot know (forward-compat).
// Each kill step runs; only the
// extra stay-dead mark is a no-op.

// Workload names: submit takes an
// optional as: NAME (default
// submitted when absent). Every
// workload ref (present, absent,
// assigned, agree, assigned_on,
// epoch, image, stop, update) takes
// a name, default submitted. Names
// hold [A-Za-z0-9_.-]+, max 63
// chars. A bare submit (no as:)
// while any workload is open fails,
// at lint and at run time; a second
// submit must carry a fresh as:
// name. Duplicate names fail lint.
// Counted submit needs as: plus
// count: 1..100, and mints members
// BASE-0..BASE-N-1; the base name in
// present or absent means the whole
// set (all members on all on-nodes),
// and the same whole-set reading
// holds for the other workload
// conditions. Counts outside 1..100
// fail lint loudly.

// Submit takes an optional rate:
// msgs/sec, 1..1000000. It paces
// multi-issue steps only (counted
// submits, replay lines) via sleeps
// between issues; single-issue steps
// accept it and issue once. Pub
// takes the same advisory rate. Time
// to present is coarse: the run
// polls workload lists about once
// per 2 s, so present.<name>.ms
// metrics round up to that rhythm.

// Endpoint model: weld node, ROS
// peer, concord node. Dispatch on
// capability, not SUT kind: pub via
// weld_pub (weld) or ros_pub_once
// (ros), concord has no pub verb;
// obs via weld/ros logs, concord
// via lists, never logs; relay via
// weld_relay_start only; submit via
// concord_submit, list via
// concord_list_has. Table kEpCap in
// exec.c names the verbs; adding a
// kind is a row, not new branches.
// Bare N means the file default
// (weld in weld files, concord in
// concord files); weld:N is an
// alias for bare in weld files;
// ros:N names the ROS peer;
// concord:N names concord. Old keys
// keep working: from: N, from:
// ros:N, on: N, echo: {by, node},
// ros_pub, pub all mean the same.
// New pairs allowed: from in
// weld/ros, on in weld/ros (weld
// dual logs legacy, ros log only).
// Bad pairs fail loudly naming the
// missing verb, e.g. concord cannot
// publish: no pub verb, ros cannot
// relay: no relay verb, concord
// cannot observe: no log verb.
// Concord pairs only where verbs
// exist: submit plus list.

// Benchmark runs sustained load by
// reusing the single submit or pub
// path in a paced loop, one issue
// per pace tick. Weld pub family
// takes from: (weld/ros, see above),
// text:, optional topic:, on:
// (weld/ros, same lone-list rule as
// observed), rate: (msgs/sec,
// required 1..1000000) or ramp:
// (adaptive, see below), count:
// and/or duration_s:, plus optional
// warmup:, optional echo:, optional
// payload_bytes:. Concord submit
// family takes node: (concord, submit
// cap), image:, optional flags:, as:
// (required base), rate: or ramp:,
// count: and/or duration_s:, plus
// warmup:. Count bounds are weld
// 1..1000, concord 1..100 (each
// issue mints a workload).
// duration_s: is 1..3600; without
// ramp, at least one of count: or
// duration_s: is required; both
// means stop at the first bound.
// warmup: defaults 0 and must sit
// below count when count is present;
// duration-only runs clamp it to
// issued. Rate times duration_s must
// stay under weld 1000, concord 100,
// so the run stays bounded: rate
// 1..1e6 by duration 1..3600 fits
// ULL, proof beside bench_want, and
// the deadline (start plus dur_ns)
// plus the 10 s grace all fit ULL.
// Pacing asks nanosleep 1e9/rate ns
// between issues; get is coarse:
// hrtimer jitter plus per-issue pub
// (ms) or submit (tens of ms) cost
// dominates, so high rates fall
// short. Duration loops break at
// the deadline and say so loudly
// in overrun (1 short, 0 whole)
// beside target (wanted non-warmup
// issues). Grace dwell pumps the
// run at the 100 ms poll rhythm.
// Weld payload grammar is fixed:
// bench#seq#send_ns#body where seq
// is the zero based per-step index,
// send_ns is sender CLOCK_MONOTONIC
// ns, body is user text or x fill.
// Full prefix match only: user text
// may hold tag like text, matching
// anchors on the full prefix, never
// on content. Old two-field tags
// bench#N# plus text fail lint
// loudly, never misparse.
// Concord matches the full
// workload id in lists, so 8-char
// prefixes cannot collide; OUT_LEN
// truncation may still miss, and
// the grace dwell covers it.
// Publish stamps use the monotonic
// clock per issue; observed stamps
// land when the id first shows in
// the log (weld dual or ros only,
// see above) or list (concord,
// any node). Warmup issues send but
// skip stats and loss both. Stats
// sort ascending then nearest rank
// ceil(pct*n/100): p50/p99/max in
// ms over seen ones, sub-ms ticks
// read 0. Each id counts once (0
// when none seen); clock-zero pubs
// count as loss. Stats emit metric
// events only, via the same
// metric_u64 print site as
// phase.<i>.ms with the same JSON
// escape, so TAP/JSON bytes never
// move: bench.<ph>.<k>.count
// (non-warmup issued),
// bench.<ph>.<k>.loss (never seen
// in a 10 s grace dwell that pumps
// the run),
// bench.<ph>.<k>.target (wanted
// non-warmup),
// bench.<ph>.<k>.overrun (1 when
// issued fell short),
// bench.<ph>.<k>.p50.ms,
// bench.<ph>.<k>.p99.ms,
// bench.<ph>.<k>.max.ms over seen
// ones (0 when none seen),
// bench.<ph>.<k>.rate_ach (actual
// msgs per sec, integer, total
// issued times 1e9 over issue loop
// ns, zero dwell reads total),
// bench.<ph>.<k>.lat per seen id
// (value ms, at most 2000 points
// per step then stop plus a
// bench.<ph>.<k>.truncated mark,
// mirroring flow.truncated),
// bench.<ph>.<k>.hist.<b> bucket
// counts under fixed edges 1 2 5
// 10 20 50 100 200 500 ms (10
// buckets 0-1 1-2 2-5 5-10 10-20
// 20-50 50-100 100-200 200-500
// 500 plus, cap 20). Loss
// never fails the step; the phase
// until/sustain still gates pass.
// Echo roundtrip: benchmark gains
// optional echo: {by: weld|ros,
// node: endpoint} (relay weld-only).
// Absent is one-way mode. Reply
// topic is always <topic>/pong
// where topic is explicit or run
// derived. Run starts a pong relay
// on echo node for the step via
// weld_relay_start then stops it;
// pacing measures RTT as reply
// minus send. By picks weld dual or
// ros only, same rule as on above;
// concord by fails (no log verb).
// Relay node must relay (weld only).
// Relay needs relay.echo in needs.
// ROS driven publish: benchmark
// from: accepts weld:N or ros:N
// (ROS pub on node N peer via
// ros_pub_once). New step ros_pub:
// {from: (weld/ros, both mean ROS
// on that node), topic:, text? or
// payload_bytes?, rate:, count: or
// duration_s:} fires ROS pubs
// outside benches.
// Payload sizes: payload_bytes:
// 0..65536, 0 or absent means use
// text. Body is N x bytes when set,
// deterministic, else user text.
// Audit: tag (bench#seq#send_ns#,
// below 64) plus body fits kBenchPay
// 66000; file needle buffer 132000
// holds full body, so end to end
// still matches; weld pub argv fits
// (ARG_MAX 2 MB, per-string 128 KB,
// so 64 KB ok, 1 MB needs chunking);
// ROS yaml holds tag plus 65536
// (driver ROS_YAML_LEN), so both
// sides sweep the same sizes;
// TUN fragments at IP level
// (MTU 1500, pump reads 2048),
// so large still forwards; log pipes
// chunk 2048 but file_has overlaps,
// so still finds; relay pump uses
// getline (no cap). 64 KB covers
// point clouds; 1 MB images need
// chunking plus larger argv and DDS
// checks. Size sweeps are repeated
// phases with different
// payload_bytes, no sweep syntax,
// see size sweep example. Ramps are
// adaptive: ramp: block only (not
// mode: ramp, to keep the top level
// clean and mirror echo: precedent).
// ramp: takes start_rate:,
// step_rate:, step_duration_s:,
// max_rate:, max_steps: 1..10,
// max_duration_s:, plus loss_pct:
// 0..100 and/or p99_ms: 0..3600000
// (at least one stop required).
// Top rate/count/duration/warmup
// are forbidden with ramp. Per-step
// want is rate times step_duration,
// capped like non-ramp (weld 1000,
// concord 100); concord total
// (steps times max rate times step
// dur) stays below 128 by lint.
// Executor runs the ladder itself:
// measure step, decide up or stop,
// emit bench.<ph>.<k>.ramp.<i>.*
// with the same summary leaves
// (count/loss/target/overrun/p50/
// p99/max/rate_ach, no lat/hist to
// keep logs bounded) plus final
// bench.<ph>.<k>.ceiling.rate
// numeric and ceiling.limited_by as
// a marker event (text would break
// numeric plots). Limited by is one
// of loss, p99, max_rate, max_steps,
// max_duration. Bounds prove end:
// steps 1..10, time 1..3600 s, rate
// 1..1e6 rising by at least 1, each
// step bounded by step_duration plus
// grace, so no infinite loop (see
// run_ramp_weld proof). Old
// multi-phase ladders still work;
// other ramp* keys fail lint with a
// ladder pointer. ROS publish needs
// ros.pub in needs.
// Submit takes an optional id:
// string. The concord submit path
// cannot set IDs (the CLI mints
// UUIDs), so a submit with id: ends
// the run as SUITE_SKIP with reason
// fixed ids unsupported, never faked.
// True duel shape awaits driver
// support.

// Memory percent: flags are free
// form, and --memory 60%host passes
// lint. A token <int>%host resolves
// in the driver to MemAvailable
// times int over 100 in MB via
// /proc/meminfo, as in check/
// concord_preempt.c. Bare or bad
// percent fails loudly in the
// driver, never reaches the shell.

// Mixed versions: top-level bins:
// maps a node index to a binary
// path (node below nodes, path a
// non-empty string). Concord boots
// the default binary, then
// preserve-restarts each listed node
// into its binary: the driver has no
// per-node spawn verb, so bins: is a
// spawn plus per-node restart, and
// each listed node restarts once at
// startup. Weld passes the per-node
// bin straight to its per-node
// spawn. Missing bins binaries skip
// the run like a missing default.

// Spawn skew is end to end: variant
// skew with node plus offset lints,
// and the run maps it to the
// concord skew spawn (see skew.yaml).

// Boot disk cap: top-level
// disk_cap_mb: 0..65536 (MB,
// concord-only, default 0 uncapped)
// flows into concord_spawn_ex at
// boot on the plain and bins paths
// alike. Named disk_cap_mb (not
// disk:) so the key cannot read as a
// device or collide with the
// diskfill step family; the _mb
// suffix mirrors timeout_s. Zero
// means uncapped, matching the
// driver. Restarts never carry a cap
// (they stay 0): remounting live
// state would hide prior seeds and
// boot fresh, against preserve
// semantics.

// Net faults: drop takes a mapping
// with nodes: (all or a list) plus
// loss: 0..1, delay_ms:,
// jitter_ms:, kbps: (0 means
// uncapped), dup_pct: 0..100, and
// dir: out, in, or both (default
// out, the legacy egress shape).
// Ranges outside fail lint loudly.
// Out shapes the listed fds; in
// shapes every other fd (sender-side
// approximation, also shaping third
// party traffic); both shapes all
// fds. TunQos semantics mirror
// tun/tun.h: loss is deterministic
// from the seed, delay is never
// slept by the pump, kbps is a token
// bucket, dup resends a copy. A bare
// drop (all, a list, or a mapping
// with nodes: plus no fault keys)
// keeps the legacy full-drop shape:
// dir: out (the default) marks the
// listed fds; in arms directed tun
// edges from every other fd into
// each listed fd, dropping unicast
// into the listed nodes only; both
// arms both (marks plus ingress
// edges; egress edges stay redundant
// under the marks). dir: alone never
// arms QoS shaping. Multicast and
// broadcast fan-out ignore directed
// edges per the tun layer, so an in
// drop never blinds discovery. Slow
// is {nodes, secs:
// 1..3600}: deferred drain via
// TunQos slow_secs. Heal clears all
// pump faults at once (every qos
// slot, every directed edge) plus
// the legacy drop marks in its scope
// (listed nodes, or all for heal:
// all); fault keys never appear on
// heal. Multicast: off is a phase
// key (on is the default): that
// phase pumps via
// tun_pump_fd_nomcast, so anchor
// discovery alone must converge.
// Pump mode is per-phase only.

// Replay is weld-only: replay:
// {file, topic?, from?} publishes a
// text log, one payload per line, in
// order from node from: (default 0)
// on topic: (default the run topic).
// Lint checks shape only; files may
// appear at run time, so an
// unreadable file fails the run
// loudly. Empty lines are skipped.
// Concord rejects replay at lint:
// no equivalent verb exists. Disk
// steps are concord-only: diskfill:
// {node, mb: 0..65536} (0 removes
// the filler), diskreadonly: {node,
// state: on|off}, journalcorrupt:
// {node}. They call
// concord_disk_fill, _disk_readonly,
// and _journal_corrupt exactly. The
// image supply step is concord-only
// too: push: {repo?, tag?, rootfs?,
// entrypoint?}, all optional, default
// concord-test, latest, img/rootfs,
// /probe. It tars the rootfs and PUTs
// it as repo:tag into every node's
// registry via img_push_net, so a
// later submit can pull locally.

// Query conditions (concord-only):
// agree: {workload?} holds when all
// nodes report the same non-nil
// AssignedNodeID. assigned_on:
// {workload?, node} holds when that
// node reports non-nil. epoch:
// {workload?, node, eq|gte?} reads
// AssignmentEpoch; with no cmp it
// holds when any epoch reads back.
// Update is {workload?, image}: the
// run shells workload update
// --image <img> <wid> on the submit
// node, as in check/concord_update.c.
// image: {workload?, value, on}
// holds when every on-node journal
// reports that image for every set
// member. Journal reads follow the
// check/ grep shapes (AssignedNodeID
// tail-1, AssignmentEpoch tail-1,
// Image tail-1); unreadable journals
// read false, never error.

// Oracle table (source, SUT, on,
// when to use):
// text: weld log grep, weld-only,
// on all or list, content present
// or absent checks.
// marker: node log grep, either
// SUT, on all, list, or any, raw
// log substring checks.
// observed: weld dual logs or ros
// only, weld SUT only, on one
// scalar endpoint (bare N or weld:N
// dual legacy, ros:N ros only,
// concord:N fails, no log verb),
// pub/sub round trips.
// peers: full-mesh verdict marker,
// concord-only, on all or list,
// mesh convergence counts.
// mesh_count: CLI alive count on
// concord, live children on weld,
// any SUT, no on, fixed liveness.
// agree: journal AssignedNodeID
// unanimity, concord-only, no on,
// scheduler convergence.
// assigned_on: per-node journal
// AssignedNodeID, concord-only, one
// node, placement checks.
// epoch: per-node AssignmentEpoch,
// concord-only, one node plus an
// optional eq or gte literal, epoch
// drift or resume checks.
// image: per-node journal Image,
// concord-only, on all or list,
// update convergence checks.

// Progress fractions: the executor
// prints sub-phase lines shaped as
// suite: frac <phase> <num> <den>
// with plain non-negative ints,
// num <= den, den > 0. Sources are
// real events only: after each do
// step (done steps of total steps),
// on until node-set transitions at
// the 2 s query rhythm (nodes now
// meeting of nodes in scope), and
// once per second on hold or
// sustain dwells (elapsed of hold_s
// or timeout_s). Unknown shapes
// print nothing. Consumers ignore
// malformed frac lines, never fail.

// Timeline events: after every phase
// begin and done, every applied step,
// every first marker observation in
// a phase, every condition outcome,
// every metric, and every verdict,
// the run prints one flushed line:
// suite: ev {"t":MS,"k":K,"ph":I,
// "msg":TEXT,"d1":A,"d2":B} where MS
// is ms since file start (post-lint,
// pre-setup instant), K is one of
// phase, step, marker, metric,
// verdict, or cond, I is the phase
// index, TEXT is JSON-escaped at the
// single print site (so malformed
// output is impossible by
// construction), and A/B are i64.
// Conventions: phase msg is begin
// NAME or done NAME (A is 0, then
// elapsed ms); step msg names the
// verb (A is the step index); marker
// msg is the marker text (A is nodes
// matched); cond msg is the cond
// text (A is 1 met, 0 unmet);
// metric msg is key=value text (A
// repeats the value when integral;
// text keys like paths carry A 0):
// phase.<i>.ms phase durations,
// present.<name>.ms per-workload
// time to present, maxrss.mb peak
// RSS via getrusage in MB; verdict
// msg is pass NAME, fail WHY, or
// skip WHY (A is 0, 1, 2). Lint
// emits no events. Older suite:
// prints stay byte-identical.
// Flow plus resource keys repeat
// about every 250 ms during until and
// sustain polls (same keys as the
// teardown dump, which stays the
// last point; the manifest below is
// teardown-only), so series gain
// shape.
//
// Metric units (no new YAML keys, no
// schema change; additive keys only,
// old logs still render):
// phase.<i>.ms is wall ms of phase i.
// present.<name>.ms is first sight
// minus submit in ms; same-ms reads
// 0, like bench sub-ms ticks read 0.
// maxrss.mb is peak RSS in MB, from
// ru_maxrss (KB on Linux) over 1024.
// flow.A.to.B.pkts counts packets,
// .bytes counts bytes, .radio_drops
// and .policy_drops count drops; all
// four are cumulative totals.
// flow.A.to.B.<leaf>.delta counts the
// same leaf since the prior sample on
// the shared 250 ms tick the sampler
// already runs on. First sample diffs
// from a zero baseline, so delta
// equals current. Cells saturate at
// max (tun sadd), so they never wrap:
// a read below stored means a fresh
// map (new run in this process), and
// diffs from zero instead.
// res.nodeN.samples counts samples.
// res.nodeN.samples_dropped counts
// passes past a full store: kept
// slots never move, drops only
// count.
// res.nodeN.peakrss.mb is MB, same
// scale as maxrss.mb.
// res.nodeN.cpu.utime and .stime stay
// scheduler ticks, byte-identical.
// res.nodeN.cpu.user.secs and .sys.secs
// are decimal seconds (two digits)
// from ticks over HZ, read once per
// process from sysconf(_SC_CLK_TCK)
// and cached; d1 mirrors the total
// centiseconds. Lookup failure keeps
// ticks only, with no .secs lines.
// res.nodeN.rss.bytes, .rchar and
// .wchar stay bytes: exact, not MB.
// bench.<ph>.<k>.count, .loss,
// .target and .overrun count issues;
// .p50.ms, .p99.ms and .max.ms are
// whole ms over seen ones;
// .rate_ach is msgs per sec, integer;
// .lat is ms per seen id, at most
// 2000 per step then .truncated;
// .hist.<b> counts bucket b under
// edges 1 2 5 10 20 50 100 200 500,
// 10 buckets, cap 20.
// bench.<ph>.<k>.ramp.<i>.* repeats
// the summary leaves per ramp step
// i (count/loss/target/overrun/p50/
// p99/max/rate_ach, no lat/hist);
// bench.<ph>.<k>.ceiling.rate is the
// top passing rate, numeric; ceiling
// limited by rides a marker event,
// never a text metric (plots stay
// numeric).
// profile.node<i>.ok is 0/1 status.
// profile.node<i>.path names
// profile/<scope>/node<i>/snapshot.txt
// relative to the log dir, teardown
// only, and only when that file
// exists, so links never die; values
// reject absolute paths and .., so
// they cannot escape the log dir.
// forensics.dir names the aside dir
// (suite-<secs>), teardown only, and
// only when it landed; the report
// pairs it with the statedir-fail
// sibling holding journals.

// Probe registry (declarative
// wiring, exec.c file-static).
// Send maps to the existing issue
// path, observe to the existing
// read path; scrape adds one curl
// in the observe netns, no SUT
// driver change. Relay stays
// orthogonal transport, never a
// probe entry.
// Send: weld-pub via weld_pub,
// ros-pub via ros_pub_once,
// workload-submit via
// concord_submit. Observe:
// log-contains via weld log grep,
// ros-log-contains via ROS log
// grep, journal-has via journal
// field grep, workload-running
// via concord_list_has,
// prometheus-scrape via curl in
// the observe netns. Needs:
// weld-pub wants weld.pub,
// ros-pub wants ros.pub,
// workload-submit wants
// workload.submit, log-contains
// wants weld.log, ros-log-contains
// wants ros.log, journal-has wants
// journal.has, workload-running
// wants workload.list,
// prometheus-scrape wants
// metrics.scrape. Old needs
// keep meaning; new verbs only
// add names.
// Desugar mapping (old sugar to
// the same entries): from N or
// weld:N plus text or payload_bytes
// to send weld-pub; from ros:N to
// send ros-pub; node plus image
// plus as plus flags to send
// workload-submit; on N or weld:N
// to expect log-contains; on ros:N
// to expect ros-log-contains;
// concord list reads to expect
// workload-running; journal reads
// to expect journal-has. Pub,
// ros_pub, submit, observed, and
// present or absent map the same
// way. Old keys keep working; new
// send plus expect blocks name the
// entries directly.
// Placeholder grammar: templates
// hold {{seq}}, {{send_ns}},
// {{body}}, {{wid}}. Seq is the
// zero based per-step index.
// Send_ns is sender monotonic ns
// stamped per issue; clock zero
// reads as loss. Body is user text
// or N x fill when payload_bytes
// is set. Wid is the submitted
// workload id, workload-submit
// only. Unknown names fail lint
// naming the allowed list. Wid
// outside submit fails loudly.
// Match rules: log plus ros logs
// match the full payload prefix,
// never content; old two-field
// tags fail loudly. Journal match
// is a field plus value map; list
// match is the state running.
// Scrape match is metric plus op
// plus value plus port plus path:
// metric is the base name, labels
// sum; op is eq, ne, lt, le, gt,
// or ge; value is numeric; port
// is required 1..65535; path
// defaults to /metrics. Scrape
// gates log stats: threshold miss
// reads as 100 percent loss.
// Pairing choice: generic send or
// expect with any via lints clean,
// even nonsense. A nonsense pair
// measures 100 percent loss
// honestly through the same stats,
// never a lint error. Old sugar
// keeps its loud pair failures so
// old files lint identically.

// SuiteVerdict names the file outcome:
// pass, fail, or skip with a reason.
typedef enum {
    SUITE_PASS,
    SUITE_FAIL,
    SUITE_SKIP,
} SuiteVerdict;

// SuiteOpts tunes one run file attempt.
// run_id 0 self-acquires via the sim
// allocator. Nullptr bins mean the
// defaults ./bin/concord and
// ./bin/weld.
typedef struct {
    int run_id;
    const char *weld_bin;
    const char *concord_bin;
} SuiteOpts;

// suite_run_file validates then runs
// one suite file. why carries the skip
// reason or first-failure text. The
// executor path is always fully
// isolated (no host footprint); C
// scenarios keep the host default.
SuiteVerdict suite_run_file(
    const char *path,
    const SuiteOpts *opts, char *why,
    size_t why_n);

// suite_lint_file validates one suite
// file only. True means the file would
// run. why carries file:line errors.
bool suite_lint_file(const char *path,
                     char *why,
                     size_t why_n);

#endif
