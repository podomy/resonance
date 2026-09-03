#define _GNU_SOURCE
#include "node/node.h"
#include "shared/context.h"
#include "sim/sim.h"
#include "tun/tun.h"
#include "world/world.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// pump_until forwards tun traffic and logs.
// Partition demo: after peers 2, drop forwarding to trigger
// suspect/failed/peer.lost, then restore and expect peer.seen again.
static void pump_until(TunMap *map, MediumGrid *grid,
                       RadioParams *radio, NodeList *nodes,
                       int fds[SIM_NODES], pid_t pids[SIM_NODES],
                       int *logfds) {
    struct pollfd p[2 * SIM_NODES];
    int i, status;
    int alive;
    int elapsed = 0;
    int drop = 0;
    int seen2 = 0;

    for (i = 0; i < SIM_NODES; i++) {
        p[i].fd = fds[i];
        p[i].events = POLLIN;
    }
    for (i = SIM_NODES; i < 2 * SIM_NODES; i++) {
        p[i].fd = logfds[i - SIM_NODES];
        p[i].events = POLLIN;
    }

    do {
        if (poll(p, 2 * SIM_NODES, 100) > 0) {
            for (i = 0; i < SIM_NODES; i++) {
                if (p[i].revents & POLLIN) {
                    if (!drop)
                        tun_pump_fd(map, fds[i], grid, radio,
                                    nodes);
                    else {
                        char tmp[2048];
                        read(fds[i], tmp, sizeof(tmp));
                    }
                }
            }
            for (i = 0; i < SIM_NODES; i++) {
                if (!(p[SIM_NODES + i].revents & POLLIN))
                    continue;
                char buf[2048];
                ssize_t n;
                n = read(logfds[i], buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    printf("\033[%dm[node %d]\033[0m %s",
                           36 + i, i, buf);
                    if (strstr(buf, "\"peers\":2") != NULL ||
                        strstr(buf, "\"peers\": 2") != NULL)
                        seen2 = 1;
                }
            }
        }
        // Partition after first peers 2 seen.
        if (seen2 && !drop && elapsed > 8000) {
            printf("resonance: partition start (drop)\n");
            drop = 1;
            elapsed = 0;
            seen2 = 0;
        } else if (drop && elapsed > 10000) {
            printf("resonance: partition end (restore)\n");
            drop = 0;
            elapsed = 0;
        }
        alive = 0;
        for (i = 0; i < SIM_NODES; i++) {
            if (waitpid(pids[i], &status, WNOHANG) == 0)
                alive = 1;
        }
        elapsed += 100;
    } while (alive);
}

int main(void) {
    Context ctx;
    TunMap map = {0};
    int fds[SIM_NODES];
    int logfds[SIM_NODES];
    pid_t pids[SIM_NODES];
    int i;

    if (!context_init(&ctx, 32, 1)) {
        fprintf(stderr, "context_init failed\n");
        return (1);
    }
    if (!sim_netns_setup())
        return (1);
    if (!sim_tuns_open(fds))
        return (0);
    if (!sim_nodes_add(&ctx, &map, fds))
        return (1);
    if (!sim_addrs_up())
        return (1);
    if (!sim_spawn_concord(pids, logfds))
        return (1);

    printf("resonance: %zu nodes, %zu tuns ready\n",
           ctx.nodes.len, map.n);
    printf("resonance: partition demo - peers 2, drop, reunion\n");
    pump_until(&map, &ctx.grid, &ctx.radio, &ctx.nodes, fds,
               pids, logfds);

    for (i = 0; i < SIM_NODES; i++) {
        kill(pids[i], SIGTERM);
        waitpid(pids[i], NULL, 0);
        close(fds[i]);
        close(logfds[i]);
    }
    context_free(&ctx);
    system("ip netns del net_namespace_nodeb");
    system("rm -rf /tmp/resonance");
    return (0);
}
