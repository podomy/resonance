#ifndef RUN_H
#define RUN_H

#include "../sim/sim.h"
#include <poll.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

// RUN_SAMPLE_CAP is the per-node store
// depth; passes append until full,
// then hold: new samples count as
// dropped, never overwrite, so kept
// history is never discarded. Depth
// 4096 holds about 17 min at the
// 250 ms cadence; longer runs keep
// peak plus cpu plus latest live and
// count the rest as dropped.
static constexpr int RUN_SAMPLE_CAP = 4096;

// RUN_SAMPLE_GAP_MS throttles sampler
// passes to at most one per 250 ms on
// the monotonic clock.
static constexpr int RUN_SAMPLE_GAP_MS = 250;

// RunSample is one /proc snapshot:
// utime/stime in clock ticks, rss_bytes
// in bytes (pages times page size),
// rchar/wchar in bytes from /proc io,
// at wall seconds, pid sampled. valid
// marks a filled slot; zeroed is empty.
typedef struct {
    pid_t pid;
    unsigned long long utime;
    unsigned long long stime;
    unsigned long long rss_bytes;
    unsigned long long rchar;
    unsigned long long wchar;
    time_t at;
    bool valid;
} RunSample;

// Run owns one scenario attempt: sim ctx, TUN
// map, child tables, and the poll vector. fds,
// logfds, and pids are fixed arrays so teardown
// stays total. n counts nodes; dead marks the
// one stay-dead index, or -1; only one holds.
typedef struct {
    Context ctx;
    TunMap map;
    SimNet net;
    int fds[TUN_MAP_MAX];
    int logfds[TUN_MAP_MAX];
    pid_t pids[TUN_MAP_MAX];
    struct pollfd p[2 * TUN_MAP_MAX];
    int n;
    int dead;
    // done guards teardown: the first call
    // runs, later calls return at once, so
    // forensics run exactly once.
    bool done;
    // shist is the heap sample store,
    // [node][slot] in append order;
    // null until setup_init (or the
    // test alloc helper) calloc's it.
    // shead is the next slot, scount
    // caps at CAP, sdropped counts
    // passes past full. speak holds
    // peak rss bytes, sutime/sstime
    // the last cpu totals, slast_ms
    // throttles passes to GAP_MS on
    // the monotonic clock.
    RunSample (*shist)[RUN_SAMPLE_CAP];
    int shead[TUN_MAP_MAX];
    int scount[TUN_MAP_MAX];
    int sdropped[TUN_MAP_MAX];
    unsigned long long speak[TUN_MAP_MAX];
    unsigned long long sutime[TUN_MAP_MAX];
    unsigned long long sstime[TUN_MAP_MAX];
    unsigned long long slast_ms;
} Run;

// RunResult is the setup verdict: ok, skip for
// missing preconditions like TUN permission,
// or hard fail.
typedef enum RunResult : int {
    RUN_OK = 0,
    RUN_SKIP,
    RUN_FAIL,
} RunResult;

// RunDrop decides per ready TUN: true drops one
// packet, false pumps it. Null pumps always.
typedef bool (*RunDrop)(void* ud, int i);

// RunLog takes one log chunk per ready pipe.
// buf holds len bytes plus a NUL past len for
// text scans. Null drains silently.
typedef void (*RunLog)(void* ud, int i,
                       const char* buf, size_t len);

// RunForensics runs before the statedir wipe
// on failure, while files still exist.
typedef void (*RunForensics)(void* ud);

// run_setup_host builds ctx, netns, TUNs,
// nodes, and addrs for n nodes. Spawn stays
// driver side: after RUN_OK the driver
// spawns into pids/logfds, then calls
// run_rearm per node. TUN failure is a
// skip; the rest is a fail. Partial setup
// unwinds what was made, incl. the host
// tun device plus its addr and route.
// Intentional host parity: node 0 stays in
// the host netns for check/ parity and
// single runs debuggable from the host.
// Concurrent executor runs use
// run_setup_isolated instead. Pairing:
// requires net isolate_all 0; nonzero
// fails loudly with RUN_FAIL and no side
// effects.
RunResult run_setup_host(Run* r,
                         const SimNet* net,
                         int n);

// run_setup_isolated mirrors run_setup_host but
// on the _all path: netns 0..n-1, every TUN
// in its netns, no host tun0/addr/route.
// Same contract: RUN_OK/RUN_SKIP/RUN_FAIL,
// log half at -1 until spawn, errno kept
// on skip, unwind on failure. Use for
// concurrent executor runs; with distinct
// SimNets, live runs share nothing except
// the allocator file. Pairing: requires net
// isolate_all nonzero; 0 fails loudly with
// RUN_FAIL and no side effects.
RunResult run_setup_isolated(Run* r,
                             const SimNet* net,
                             int n);

// run_poll waits 100 ms, pumps or drops each
// ready TUN per drop, and hands each ready log
// chunk to log. Ticks the sampler at most
// once per 100 ms. Returns what poll returned.
// Drop and pump errors stay silent.
int run_poll(Run* r, RunDrop drop, RunLog log,
             void* ud);

// run_poll_ms waits at most ms, then acts
// exactly like run_poll. Lets paced loops
// drain while waiting without overshooting
// their period. ms below 0 reads as 0.
int run_poll_ms(Run* r, RunDrop drop, RunLog log,
                void* ud, int ms);

// run_hold arms t0 on first call, fires secs
// later. Same semantics as the hold copies in
// check/.
bool run_hold(time_t* t0, int secs);

// run_throttle reports true at most once per
// secs: the 2 s CLI query rhythm. The first
// call always fires.
bool run_throttle(time_t* t, int secs);

// run_alive reports any non-skipped child still
// running. skip names one dead-by-fault index
// to ignore, or -1 for none. Slots observed
// reaped clear to -1, so a later kill never
// hits a reused pid.
bool run_alive(Run* r, int skip);

// run_note_dead records i as dead by fault so
// teardown skips it and poll quarantines its
// TUN by discarding reads. Only one slot
// holds; a second note keeps the first. The
// driver kills, reaps, and closes the logfd
// first, as the failover kill site does;
// a still-open logfd closes here, so future
// noters never leak.
void run_note_dead(Run* r, int i);

// run_rearm publishes logfds[i] into the log
// poll half after driver spawn or restart. A
// matching dead mark clears: it lives again.
void run_rearm(Run* r, int i);

// run_teardown SIGTERMs and reaps children,
// closes TUNs and logs, runs forensics when
// failed, frees ctx, deletes the netns, then
// wipes the stored statedir. Wait loops on
// EINTR and escalates to SIGKILL after 5 s,
// so teardown may run past DEADLINE by at
// most 5 s per child. The dead index
// keeps its TUN close only. Clears every
// handled pid and fd to -1 and sets done,
// so a second call is a no-op and forensics
// run once: call once after RUN_OK.
// Takes a final sampler pass first, so
// peaks and cpu totals stay readable.
// Revalidates the net before rm -rf and
// refuses on invalid.
void run_teardown(Run* r, bool failed,
                  RunForensics forensics, void* ud);

// run_sample_get copies the latest sample
// for node into out. False on bad node,
// empty history, or nullptr.
bool run_sample_get(const Run* r, int node,
                    RunSample* out);

// run_sample_hist copies back slots ago:
// 0 is latest, 1 prior, up to stored
// count - 1. False past stored count
// or bad args. Append-only: slots
// never move once stored.
bool run_sample_hist(const Run* r, int node,
                     int back, RunSample* out);

// run_sample_count reports stored samples
// for node, 0..CAP. Bad node reports 0.
int run_sample_count(const Run* r, int node);

// run_sample_dropped reports passes past
// full for node: samples counted but
// not stored. Bad node reports 0.
int run_sample_dropped(const Run* r,
                       int node);

// run_sample_peak reports peak rss in
// bytes for node. False when empty or
// bad args.
bool run_sample_peak(const Run* r, int node,
                     unsigned long long* out);

// run_sample_cpu reports last utime plus
// stime totals in clock ticks. False
// when empty or bad args.
bool run_sample_cpu(const Run* r, int node,
                    unsigned long long* ut,
                    unsigned long long* st);

// run_sample_tick snapshots each live
// child at most once per 250 ms on the
// monotonic clock. Best effort: missing
// /proc, parse fails, full store, and
// non-Linux skip silently. Past full,
// passes still refresh peak plus cpu
// plus latest and count as dropped;
// kept slots never move. run_poll
// calls it, so no caller help is needed;
// call by hand only to tick in tests.
void run_sample_tick(Run* r);

#endif
