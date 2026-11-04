#ifndef CONCORD_H
#define CONCORD_H

#include "run/run.h"
#include <stddef.h>
#include <sys/types.h>

// ConcordDurability names the five
// restart durabilities the driver
// implements: fresh identity, preserved
// dir, rotated key, wiped key, wiped
// state. Docs group the two key cases
// as one meaning. Stay-dead is
// concord_kill with no restart.
typedef enum : int {
    CONCORD_FRESH = 0,
    CONCORD_PRESERVE,
    CONCORD_REKEY_ROTATE,
    CONCORD_KEY_WIPE,
    CONCORD_WIPE_STATE,
} ConcordDurability;

// ConcordSpawn picks the sim spawn body:
// plain, clock-skewed, anchored, debug.
typedef enum : int {
    CONCORD_SPAWN_PLAIN = 0,
    CONCORD_SPAWN_SKEW,
    CONCORD_SPAWN_ANCHOR,
    CONCORD_SPAWN_DEBUG,
} ConcordSpawn;

// concord_have_binary reports ./bin/concord
// executable: false means skip, not fail.
bool concord_have_binary(void);

// concord_spawn forks one concord per node
// into r pids/logfds, then run_rearm each.
// Skew uses opt_node plus offset string;
// anchor uses opt_node; rest ignore both.
// Skew plus anchor reject opt_node
// outside [0,n): skew would miss,
// anchor would point nowhere.
// Net must pass sim_net_valid, defense
// in depth; sim spawn checks again.
// Placement honors isolate_all.
bool concord_spawn(Run* r, ConcordSpawn v,
                   int opt_node,
                   const char* opt_arg);

// concord_kill sends sig, reaps, closes the
// logfd, and notes dead: stay-dead, never
// restarted. Teardown then skips the pid.
// True when already dead: idempotent, as
// in weld_kill. Non-ESRCH kill failure
// is false, as in weld_kill. Reap bound
// by 5 s then KILL, as in run teardown:
// diverges from check/ block on purpose.
bool concord_kill(Run* r, int i, int sig);

// concord_restart kills node i and reforks
// it with durability d, then run_rearm.
// Fresh wipes the dir for a new identity.
// Refork is always plain: skew, anchor,
// and debug variants do not survive it.
// Node 0 enters its netns when the net
// is isolated. Net must pass
// sim_net_valid; restart shells use the
// statedir. Rotate uses TERM so the
// daemon flushes before exit.
bool concord_restart(Run* r, int i,
                     ConcordDurability d);

// concord_submit runs workload run with
// image plus extra flags against node,
// parsing Submitted workload <uuid> into
// out. Null flags means no extra flags.
// Image allows alnum plus _ . : / @ + ,
// - only; flags allow [A-Za-z0-9_=., /-]
// only: both reach a shell.
// Charset-safe but flag-injectable by
// design: the gate stops shell breaks,
// not extra CLI verbs, so callers own
// flags. Statedir allows [A-Za-z0-9/_.-]
// only. Bounded by 10 s: a hung CLI is
// false, never a stall.
// Percent grammar: a token <int>%host
// (as in --memory 60%host) resolves to
// MemAvailable times int over 100 in
// MB via /proc/meminfo, as in check/
// concord_preempt.c 60/120. Bare or bad
// percent fails loudly, never passes
// through to the shell.
bool concord_submit(const char* statedir,
                    int node, const char* image,
                    const char* flags, char* out,
                    size_t n);

// concord_stop runs workload stop on
// node and wants the Stopped line.
// Statedir plus wid reach a shell:
// gates reject metachars. Node must
// be non-negative. Bounded by 10 s
// via first line: diverges from exec
// plus check/ blocking popen.
bool concord_stop(const char* statedir,
                  int node, const char* wid);

// concord_update runs workload update
// --image on node for wid. Image plus
// wid plus statedir reach a shell:
// gates reject metachars. True only
// on exit 0. Bounded by 10 s with
// output drained: diverges from exec
// plus check/ blocking popen.
bool concord_update(const char* statedir,
                    int node, const char* wid,
                    const char* image);

// concord_list_has reports shortid
// found in node workload list.
// 8-char prefix may collide: unique
// verdicts need the full id. Output
// caps at 16384 bytes as in check/:
// longer lists truncate and may miss.
// Bounded by 10 s: diverges from
// check/ blocking popen on purpose.
// Throttle callers via run_throttle,
// as in mesh_count. Shortid never
// reaches the shell: strstr only.
bool concord_list_has(const char* statedir,
                      int node,
                      const char* shortid);

// concord_assigned_anywhere reports a
// non-nil AssignedNodeID for wid in any
// node journal under statedir. Wid
// allows hex plus - only; statedir
// allows [A-Za-z0-9/_.-] only: both
// reach a grep shell. Bounded by 10 s:
// a hung grep is false. Scan races
// writers: callers poll and retry, and
// quiesce restarts during the scan.
bool concord_assigned_anywhere(
    const char* statedir, const char* wid);

// concord_mesh_count counts alive members
// in node list output, or -1 on error.
// Counts strict " alive " with spaces, as
// in check/concord_splitbrain.c. Output
// caps at 16384 bytes as in check/:
// longer output truncates and
// undercounts. Bounded by 10 s:
// diverges from check/ block. Throttle
// callers via run_throttle to about one
// query per 2 s: each call shells out.
int concord_mesh_count(const char* statedir,
                       int node);

// concord_spawn_ex forks like
// concord_spawn but with binary bin
// plus disk cap disk_mb. Variant,
// opt_node, and opt_arg mirror
// concord_spawn exactly. Null bin means
// ./bin/concord. Zero disk_mb means
// uncapped. Nonzero disk_mb mounts a
// private tmpfs over the node state dir
// in the child: unshare CLONE_NEWNS,
// then mount tmpfs size=disk_mb m.
// Limits: needs CAP_SYS_ADMIN or a
// userns allowing the mount, else the
// child exits 127; the mount hides
// prior seeds so capped nodes boot
// fresh; the cap covers only that node
// dir, never the shared filesystem.
bool concord_spawn_ex(Run* r, int variant,
                      int opt_node,
                      const char* opt_arg,
                      const char* bin,
                      unsigned long long disk_mb);

// concord_restart_ex kills node i and
// reforks it with durability plus bin
// plus disk cap. Durability mirrors
// concord_restart exactly. Null bin
// means ./bin/concord. Zero disk_mb
// means uncapped. Nonzero disk_mb uses
// the same unshare plus tmpfs mount as
// spawn_ex, with the same limits.
bool concord_restart_ex(Run* r, int i,
                        int durability,
                        const char* bin,
                        unsigned long long disk_mb);

// concord_set_bin overrides the daemon
// binary for CLI plus reexec paths.
// Null resets to ./bin/concord.
// Process-global test config, not
// thread-safe: single-threaded harness
// only. Caller keeps the string alive.
// Daemon spawn via sim keeps its own
// path; use spawn_bin for custom bins.
void concord_set_bin(const char* bin);

// concord_seed_home seeds node home: dir,
// CA copy, gossip key copy, plain
// config.json. Skew offset plus anchor
// args stay spawn-time via env plus
// argv; seed writes the plain form only.
// Mirrors sim spawn_one_net seeding.
bool concord_seed_home(const SimNet* net,
                       int node);

// concord_spawn_bin seeds each home then
// forks via sim_spawn_cmd plus run_rearm.
// Variant plus opt mirror concord_spawn.
// Null bin means the default. Workdir
// stays null so relative bins work.
bool concord_spawn_bin(Run* r, int variant,
                       int opt_node,
                       const char* opt_arg,
                       const char* bin);

// concord_disk_fill fallocates
// <statedir>/node<node>/filler.bin to
// mb MB via posix_fallocate. Zero mb
// removes the filler. ENOSPC is
// best-effort success: the filler did
// its job. Cannot force a shared
// filesystem full: other writers keep
// room, so callers check usage, not
// just this verdict.
bool concord_disk_fill(Run* r, int node,
                       unsigned long long mb);

// concord_disk_readonly flips writability
// on the node state tree via chmod -R:
// true runs chmod a-w, false runs chmod
// u+w, both bounded by 10 s. Documented
// approximation, not block-device RO:
// root bypasses it, and it is fully
// reversible via false.
bool concord_disk_readonly(Run* r, int node,
                           bool ro);

// concord_journal_corrupt truncates
// <statedir>/node<node>/concord/
// journal.jsonl then writes exact bytes
// CORRUPT plus newline: 8 bytes total
// (C O R R U P T LF). Readers see
// invalid JSON on next scan.
bool concord_journal_corrupt(Run* r,
                             int node);

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

// ConcordProfileRec holds one perf
// record child. pid -1 means idle.
// Start sets it; stop reaps plus
// clears. Never copy a live rec.
typedef struct {
    pid_t pid;
} ConcordProfileRec;

// concord_profile_snapshot takes a
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
bool concord_profile_snapshot(Run* r,
                              int node,
                              const char* outdir);

// concord_profile_record_start
// brackets a best effort perf record
// -p on node into outdir/perf.data.
// True when the child runs, with
// rec holding its pid. False is a
// clean skip (no perf, no child,
// busy rec, dead child) with why
// set, never a run fail. Start on
// a live rec is busy: stop first,
// so no child ever leaks. Early
// probe spans 500 ms only; the
// child outlives the call until
// stop. Argv only, no shell.
bool concord_profile_record_start(
    Run* r, int node, const char* outdir,
    ConcordProfileRec* rec, char* why,
    size_t nwhy);

// concord_profile_record_stop ends
// the start above: SIGINT, then a
// 5 s bounded reap, then KILL past
// the bound. Always reaps exactly
// once and clears rec to idle; idle
// stop is true. False on null rec
// only. Null why keeps no reason.
// Never reaps r pids, only the perf
// child from start.
bool concord_profile_record_stop(
    ConcordProfileRec* rec, char* why,
    size_t nwhy);

#endif
