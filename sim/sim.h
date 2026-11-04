#ifndef SIM_H
#define SIM_H

#include "../shared/context.h"
#include "../tun/tun.h"
#include <stdint.h>
#include <sys/types.h>

// STATEDIR holds per-node homes. It must sit on a
// dev-capable filesystem: container device nodes are
// created inside bundles below it, and a nodev mount
// (the /tmp tmpfs) refuses every device open.
#define STATEDIR "/var/tmp/resonance"

// SimNet holds per-run names. Parallel runs use
// disjoint values; run 0 is the historic default.
// Value type: copy it freely. _net functions
// borrow it through const SimNet* for the call
// only and never retain the pointer.
typedef struct {
    // statedir holds per-node homes
    // below it (node<i>, gossip.key).
    char statedir[128];
    // ns_prefix prefixes each netns
    // name (<prefix><i>).
    char ns_prefix[64];
    // tun_prefix prefixes each TUN
    // name (<prefix><base+i>).
    char tun_prefix[32];
    // tun_base is the first TUN index;
    // node i uses tun<base+i>.
    int tun_base;
    // under holds the first 3 octets
    // of the /24 underlay (node i is
    // .<i+1>).
    uint8_t under[3];
    // cgroot roots per-node cgroups
    // (<root>/node<i>/daemon).
    char cgroot[128];
    // 0 = node 0 lives in the host
    // netns (legacy default, frozen
    // check/ behavior); 1 = every node
    // including 0 lives in its own
    // netns (parallel runs, executor
    // path).
    int isolate_all;
} SimNet;

// sim_default_net returns the historic globals:
// STATEDIR, resonance_, tun base 0, 192.168.100/24,
// /sys/fs/cgroup/resonance, isolate_all 0.
// Callers opt into 1 explicitly.
SimNet sim_default_net(void);

// sim_net_valid checks a net for safe use:
// statedir, ns prefix, tun prefix, and
// cgroot hold only [A-Za-z0-9/_.-] (they
// reach system()), every tun name fits
// IFNAMSIZ-1, and under is a sane /24
// clear of the 10.0.0.0/16 overlay.
bool sim_net_valid(const SimNet* net);

// sim_net_try_for_run is the checked
// constructor new code must use. It writes the
// net for run_id and returns true, or returns
// false for run_id below 0 or above 154, which
// would wrap the octet or alias tun names.
// Result isolate_all is 0; callers opt
// into 1 explicitly.
bool sim_net_try_for_run(int run_id, SimNet* out);

// sim_run_acquire hands out run ids 1..154
// for concurrent processes. It locks
// /var/tmp/resonance-runs.lock with flock,
// sweeps stale counter.tmp.* temps, reads,
// bumps, and writes
// /var/tmp/resonance-runs.counter, then
// unlocks. Missing or empty reads as 0;
// over-long lines reject as corrupt; an
// I/O error fails instead of handing id
// 1. Cur is range-checked before cur+1,
// so LONG_MAX never overflows. Publish is
// temp plus rename at 0600 with a tmp
// fsync plus best-effort /var/tmp fsync:
// a crash before the dir fsync may lose
// the rename. Full (at 154) fails loudly
// with false plus stderr. Ids feed
// sim_net_try_for_run. Monotonic: no
// reuse, so wipe the counter file to reset
// after runs end. flock frees on close or
// crash, so stale locks are impossible.
bool sim_run_acquire(int* out_id);

// sim_netns_setup_net creates one netns per extra
// node using the net prefix.
bool sim_netns_setup_net(const SimNet* net, int n);

// sim_netns_setup_all creates namespaces for
// 0..n-1. Isolated path for parallel runs:
// node 0 gets its own netns, so each run
// owns its route table and port space. New
// code uses this; frozen check/ keeps the
// host-node-0 setup_net default.
bool sim_netns_setup_all(const SimNet* net, int n);

// sim_netns_teardown_net deletes netns 0..n-1
// and the net cgroup subtree. Best effort:
// always true for non-null net, even for
// absurd n; false only for nullptr net.
// Invalid nets are a true no-op, so names
// outside [A-Za-z0-9/_.-] never reach
// system() and a bad-input unwind leaves
// the host untouched.
// Covers host (1..) and isolated (0..); the
// extra del on host runs fails silently.
// Drops the legacy net_namespace_nodeb
// name for the default net only; other
// nets skip it, so distinct runs share
// nothing.
bool sim_netns_teardown_net(const SimNet* net,
                            int n);

// sim_tuns_open_net opens tun<base> in host and
// tun<base+1>.. in their netns. Presets
// fds[0..n-1] to -1 at entry; on false the
// opened prefix stays open and the direct
// caller must close it, as run unwind does.
bool sim_tuns_open_net(const SimNet* net, int* fds,
                       int n);

// sim_tuns_open_all opens every TUN inside
// its own netns, unlike open_net which keeps
// tun<base> in the host. Pairs with
// setup_all; node 0 has no host device.
// Presets fds[0..n-1] to -1 at entry; on
// false the opened prefix stays open and
// the direct caller must close it.
bool sim_tuns_open_all(const SimNet* net, int* fds,
                       int n);

// sim_nodes_add_net registers simulated nodes.
// Underlay net under/24 stays disjoint from
// overlay 10.0.0.0/16.
bool sim_nodes_add_net(const SimNet* net,
                       Context* ctx, TunMap* map,
                       int* fds, int n);

// sim_addrs_up_net assigns underlay IPs and brings
// links up.
bool sim_addrs_up_net(const SimNet* net, int n);

// sim_addrs_up_all assigns 192.168.<under>
// .1..n plus multicast routes all inside
// their netns. No host tun, no host addr,
// no host 224.0.0.0/4 route. Pairs with
// setup_all for parallel runs.
bool sim_addrs_up_all(const SimNet* net, int n);

// sim_spawn_concord_net forks concord nodes with
// isolated XDG_CONFIG_HOME under net statedir.
bool sim_spawn_concord_net(const SimNet* net,
                           pid_t* pids,
                           int* logfds, int n);

// sim_spawn_concord_skew_net forks like net spawn
// but runs node skew_node with CONCORD_CLOCK_OFFSET
// set to offset seconds. Production must never
// set it. Returns false when skew_node is out of
// range or offset is nullptr.
bool sim_spawn_concord_skew_net(
    const SimNet* net, pid_t* pids, int* logfds,
    int n, int skew_node, const char* offset);

// sim_spawn_concord_anchor_net forks like net spawn
// but runs node anchor_node with --anchor and points
// every node config at it via anchors.
bool sim_spawn_concord_anchor_net(
    const SimNet* net, pid_t* pids, int* logfds,
    int n, int anchor_node);

// sim_spawn_concord_debug_net forks like net spawn
// but runs every node with debug logging.
// Voluminous: diagnosis only.
bool sim_spawn_concord_debug_net(
    const SimNet* net, pid_t* pids, int* logfds,
    int n);

// sim_restart_concord_net wipes node i for a fresh
// identity and forks it again. Refuses when
// pids[i] is live or logfds[i] is open: the
// caller must have killed, reaped, and
// closed first, so a restart never orphans.
// Returns false for negative or huge i.
// Shares the spawn path, so the child exits
// rather than run in the host netns
// without its own netns.
bool sim_restart_concord_net(const SimNet* net,
                             pid_t* pids,
                             int* logfds, int i);

// SimCmd is one generic child: binary
// plus argv plus optional workdir. No
// SUT knowledge: no certs, no keys, no
// config, no workload strings.
typedef struct {
    // bin is the executable path.
    const char* bin;
    // argv is the exec argv with
    // argv[0] set (e.g. "concord").
    char* const* argv;
    // workdir is the child chdir target,
    // or nullptr for no chdir. Caller
    // keeps relative bins working by
    // passing nullptr.
    const char* workdir;
} SimCmd;

// sim_spawn_cmd forks bin plus argv with
// stdout plus stderr into a log pipe.
// Enters <ns_prefix><node> for node above
// 0, plus node 0 when isolate_all. Home
// is untouched: the caller prepares it.
// Cgroup placement is best effort like
// spawn_one_net. Child _exit(127) on
// open, setns, chdir, dup2, or exec
// failure. No SUT knowledge.
bool sim_spawn_cmd(const SimNet* net,
                   pid_t* pid, int* logfd,
                   int node,
                   const SimCmd* cmd);

// Legacy wrappers below use the default net and
// stay frozen for check/. New code must use the
// _net forms above with an explicit SimNet.

// sim_netns_setup creates one netns per extra node.
bool sim_netns_setup(int n);

// sim_netns_teardown deletes those netns.
bool sim_netns_teardown(int n);

// sim_tuns_open opens tun0 in host, tun1.. in netns.
bool sim_tuns_open(int* fds, int n);

// sim_nodes_add registers simulated nodes.
// Underlay 192.168.100.0/24 is disjoint from
// overlay 10.0.0.0/16.
bool sim_nodes_add(Context* ctx, TunMap* map, int* fds,
                   int n);

// sim_addrs_up assigns underlay IPs and brings links up.
bool sim_addrs_up(int n);

// sim_spawn_concord forks concord nodes with isolated
// XDG_CONFIG_HOME.
bool sim_spawn_concord(pid_t* pids, int* logfds, int n);

// sim_spawn_concord_skew forks like sim_spawn_concord
// but runs node skew_node with CONCORD_CLOCK_OFFSET
// set to offset seconds (e.g. "300"). The offset env
// is the Concord test time hook; production must never
// set it. Returns false when skew_node is out of
// range or offset is nullptr.
bool sim_spawn_concord_skew(pid_t* pids, int* logfds, int n,
                            int skew_node,
                            const char* offset);

// sim_spawn_concord_anchor forks like sim_spawn_concord
// but runs node anchor_node with --anchor and points
// every node config at it via anchors.
bool sim_spawn_concord_anchor(pid_t* pids, int* logfds,
                              int n, int anchor_node);

// sim_spawn_concord_debug forks like sim_spawn_concord
// but runs every node with debug logging. Voluminous:
// diagnosis only, never the suite default.
bool sim_spawn_concord_debug(pid_t* pids, int* logfds,
                             int n);

// sim_restart_concord wipes node i for a fresh identity
// and forks it again. Caller must have killed and reaped
// the old child and closed its logfd. Returns false
// for negative i.
bool sim_restart_concord(pid_t* pids, int* logfds, int i);

#endif
