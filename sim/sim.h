#ifndef SIM_H
#define SIM_H

#include "../shared/context.h"
#include "../tun/tun.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

// STATEDIR holds per-node homes. It must sit on a
// dev-capable filesystem: container device nodes are
// created inside bundles below it, and a nodev mount
// (the /tmp tmpfs) refuses every device open.
#define STATEDIR "/var/tmp/resonance"

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
// set it.
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
// the old child and closed its logfd.
bool sim_restart_concord(pid_t* pids, int* logfds, int i);

#endif
