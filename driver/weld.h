#ifndef WELD_H
#define WELD_H

#include "run/run.h"
#include <stddef.h>
#include <sys/types.h>

// Weld driver: spawn, kill, and restart
// of weld echo nodes in scenario netns,
// docker ROS peers beside them, plus
// the send verb. Observed helpers were
// removed: suite exec reads log files
// direct, so no callers remain. Every
// path comes from the caller, never
// globals, so parallel runs with
// distinct domain ids stay apart. The
// weld repo ships no prebuilt binary:
// build it with cd cmd/weld plus
// GOWORK=off go build (see weld
// docs/cli.md), then pass the path as
// bin. Null bin means the default.
// Domain plus topic args are sanity
// checks only; cross-run mixing is
// stopped by the unique per-run
// derivation in the scenario, not by
// validation here. Weld echo nodes
// take no per-node cgroup, unlike
// concord daemons: they are light and
// share the run cgroup.
// Disk faults stay out: weld echo nodes
// hold no state for this purpose, so no
// disk cap, fill, readonly, or journal
// verbs exist here. Concord owns them.
// weld_spawn_node already takes bin per
// node, so no new spawn verbs either.

// WELD_BIN_DEFAULT is used when bin is
// null. It matches the resonance layout,
// not any weld repo path: weld pins no
// prebuilt binary location.
#define WELD_BIN_DEFAULT "./bin/weld"

// WELD_TOPIC_DEFAULT is the weld topic
// default from weld docs/cli.md.
#define WELD_TOPIC_DEFAULT "/chatter"

// WELD_ROS_TYPE is the echo and pub
// type from weld helpers.go RosType.
#define WELD_ROS_TYPE "std_msgs/msg/String"

// WELD_RMW_DEFAULT is the peer RMW from
// weld helpers.go RMW.
#define WELD_RMW_DEFAULT "rmw_cyclonedds_cpp"

// WELD_DISTRO_DEFAULT is the canonical
// gate peer from weld distro.go.
#define WELD_DISTRO_DEFAULT "jazzy"

// Domain bounds from weld docs/cli.md,
// ephemeral slice from helpers.go.
static constexpr int WELD_DOMAIN_MIN = 0;
static constexpr int WELD_DOMAIN_MAX = 101;
static constexpr int WELD_EPHEM_MIN = 60;
static constexpr int WELD_EPHEM_MAX = 69;

// WeldDurability names the two restart
// meanings: wiped home or same home.
typedef enum : int {
    WELD_FRESH = 0,
    WELD_PRESERVE,
} WeldDurability;

// WeldRos holds one docker ROS peer: the
// foreground docker exec pid, its scoped
// pkill keys, and its capture file.
typedef struct {
    pid_t pid;
    char container[64];
    char topic[128];
    char cap[192];
} WeldRos;

// weld_skip_unless_bin reports true when
// the run must skip: bin not executable.
// True means must-skip, false means go.
// Null bin means the default path.
bool weld_skip_unless_bin(const char *bin);

// weld_skip_unless_docker reports true
// when the run must skip: no daemon
// answers docker info. True means
// must-skip, false means go. Bounded
// wait: a hung daemon counts as skip.
bool weld_skip_unless_docker(void);

// weld_spawn_node forks weld echo into r
// pids plus logfds for node i, entering
// <ns_prefix><i> for i above 0, plus
// node 0 when isolate_all, with
// ROS_DOMAIN_ID set, then run_rearm.
// Null bin or topic means defaults.
// Like concord_spawn it rearms; unlike
// it the caller passes domain plus bin
// plus topic per node for the re-exec.
bool weld_spawn_node(Run *r, int i,
                     int domain,
                     const char *bin,
                     const char *topic);

// weld_kill sends sig to node i, reaps
// it, closes its logfd, and notes dead:
// stay-dead, as in concord_kill. True
// when already dead: idempotent.
bool weld_kill(Run *r, int i, int sig);

// weld_restart kills plus reaps plus
// closes node i then reforks it with
// durability d, then run_rearm. Fresh
// wipes the home via fork plus argv rm
// with no shell; preserve keeps it.
// Null bin plus topic mean defaults.
// Like concord_restart it owns the
// kill; unlike it the caller passes
// domain plus bin plus topic again.
// Placement honors isolate_all.
bool weld_restart(Run *r, int i,
                  int domain,
                  const char *bin,
                  const char *topic,
                  WeldDurability d);

// weld_pub runs weld pub once from the
// node netns, bounded by 30 s matching
// the Go harness PubTimeout, and
// reports true only on exit 0 plus
// the sent confirmation. Hung runs
// die by SIGKILL and report false.
// Null bin plus topic mean defaults.
bool weld_pub(const SimNet *net, int node,
              int domain, const char *bin,
              const char *topic,
              const char *payload);

// weld_ros_peer_start launches ros2 topic
// echo in weld-<distro> via foreground
// docker exec argv shaped like the weld
// helper DockerArgs, with the ROS setup
// as -e flags and no shell. Capture
// lands in the per-run statedir, not the
// weld logs volume, so parallel runs
// stay apart. The client stays on the
// host; the peer runs in the container.
// Null distro means the default; null
// topic is rejected, not defaulted.
bool weld_ros_peer_start(Run *r,
                         WeldRos *peer,
                         int i,
                         const char *distro,
                         int domain,
                         const char *topic);

// weld_ros_peer_stop ends the peer:
// TERM plus reap the client, then a
// scoped bounded pkill by topic.
// Pkill -f is regex, so topics stay in
// the fixed charset of slash plus
// alnum plus underscore. Client reap
// bound by KILL_WAIT_S then KILL, as in
// run teardown; pkill is bounded by
// WAIT_TIMEOUT_S and never stalls.
bool weld_ros_peer_stop(WeldRos *peer);

// weld_log_save appends one log chunk
// to <statedir>/weld-<i>.log. Ud
// carries the statedir, so scenarios
// pass it straight to run_poll as the
// RunLog callback. Suite exec reads
// files for observed checks, not
// pipes; empty payload true lives in
// exec file_has, not here.

void weld_log_save(void *ud, int i,
                   const char *buf,
                   size_t len);

// Profiler collection contract: the
// harness never profiles in place;
// it only collects what external
// tools leave behind. Profiles plus
// snapshots land under
// <statedir>/profile/node<i>/, made
// best effort by the helpers below:
// absent dirs plus absent perf never
// fail the run. Inside the binary
// profiling (pprof, trace spans) is
// the SUT job, never the harness.
// Readable stacks need SUT binaries
// with debug symbols; without them
// the same files degrade to raw
// addresses. No check/ profiler
// exists, so parity constrains only
// the bounded waits plus snprintf
// checks shared with the rest of
// this driver. Helpers never reap
// r pids; they reap only the perf
// children they fork.

// WeldProfileRec holds one perf
// record child. pid -1 means idle.
// Start sets it; stop reaps plus
// clears. Never copy a live rec.
typedef struct {
    pid_t pid;
} WeldProfileRec;

// weld_profile_snapshot takes a
// one-shot perf stat -p on node
// into outdir (perf-stat.txt) plus
// a resource snapshot
// (snapshot.txt) from rusage plus
// proc. Outdir is utmost
// <statedir>/profile/node<i>.
// True when snapshot.txt lands;
// absent perf still lands it, so
// true. False on bad args or IO
// setup failure only. Outdir gated
// to shell-safe chars; mkdir plus
// perf run argv only, no shell.
// Stat sleeps 5 s, waits bound 10
// s. False never fails the run:
// log it.
bool weld_profile_snapshot(Run *r,
                           int node,
                           const char *outdir);

// weld_profile_record_start brackets
// a best effort perf record -p on
// node into outdir/perf.data. True
// when the child runs, with rec
// holding its pid. False is a clean
// skip (no perf, no child, busy
// rec, dead child) with why set,
// never a run fail. Start on a live
// rec is busy: stop first, so no
// child ever leaks. Early probe
// spans 500 ms only; the child
// outlives the call until stop.
// Argv only, no shell.
bool weld_profile_record_start(
    Run *r, int node,
    const char *outdir,
    WeldProfileRec *rec, char *why,
    size_t nwhy);

// weld_profile_record_stop ends the
// start above: SIGINT, then a 5 s
// bounded reap, then KILL past the
// bound. Always reaps exactly once
// and clears rec to idle; idle stop
// is true. False on null rec only.
// Null why keeps no reason. Never
// reaps r pids, only the perf child
// from start.
bool weld_profile_record_stop(
    WeldProfileRec *rec, char *why,
    size_t nwhy);

// Relay plus ROS one-shot contract:
// the suite benchmark observe path
// needs a pong relay plus ROS paced
// pub. Both read ROS_DOMAIN_ID for
// the domain, mirroring the weld
// CLI --domain default, so suite
// sets it to the run domain first.
// Unset or bad domain fails loudly:
// the harness never silently uses
// domain 0. SUT-clock note: send_ns
// in bench payloads comes from the
// sender clock; same-host runs share
// it, cross-host runs must handle
// skew outside this driver.

// weld_relay_start forks a pong relay
// for node: a weld echo child on
// topic piped to a pump child that
// republishes every line to
// <topic>/pong via weld_pub, byte
// identical. Streaming pair chosen
// over a second node loop: no
// window loss. No shell, argv only.
// Null topic or bin mean defaults.
// The pump blocks in read until
// stop, so callers stop the relay
// before teardown; leaked relays
// reparent, never reaped. Caller
// keeps bin alive until stop.
// Relay adds a fixed delay near one
// pub fork plus exec: calibrate it
// out with a same-node loopback run
// before reading bench numbers.
// False loudly on any setup fail.
bool weld_relay_start(Run *r, int node,
                      const char *topic,
                      const char *bin);

// weld_relay_stop ends the node relay:
// TERM plus reap of both children,
// bound by KILL_WAIT_S then KILL,
// as in peer stop. Never touches r
// pids. Idle stop is a no-op.
void weld_relay_stop(Run *r, int node);

// ros_pub_once publishes text once
// via ros2 topic pub in weld-<distro>
// through docker exec argv shaped
// like the peer env, with no shell.
// Yaml is {data: text}, as in the
// weld Go harness. Flags found: the
// paced talkers use --rate N (weld
// testbed/live), so one-shot uses
// --times 1, the stock ros2topic
// count flag. True only on exit 0.
// Null topic or empty text fails.
// Null or empty distro means the
// default; unknown distros fail
// closed. One docker exec costs
// order 0.1 s, so repeated one-shots
// top out near 10 msgs/s: native
// --rate pacing needs a held peer
// proc. Single shots may predate
// match, so paced callers repeat
// until seen, as in
// PublishUntilEcho.
bool ros_pub_once(const SimNet *net,
                  int node,
                  const char *topic,
                  const char *text,
                  const char *distro);

#endif
