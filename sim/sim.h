#ifndef SIM_H
#define SIM_H

#include "../node/node.h"
#include "../shared/context.h"
#include "../tun/tun.h"
#include "../world/world.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#define SIM_NODES 2

// sim_netns_setup creates net_namespace_nodeb.
bool sim_netns_setup(void);

// sim_tuns_open opens tun0 and tun1 in netns.
bool sim_tuns_open(int fds[SIM_NODES]);

// sim_nodes_add registers simulated nodes.
// Underlay 192.168.100.0/24 is disjoint from overlay 10.0.0.0/16.
bool sim_nodes_add(Context *ctx, TunMap *map,
                   int fds[SIM_NODES]);

// sim_addrs_up assigns underlay IPs and brings links up.
bool sim_addrs_up(void);

// sim_spawn_concord forks concord nodes with isolated XDG_CONFIG_HOME.
bool sim_spawn_concord(pid_t pids[SIM_NODES],
                       int *logfds);

#endif
