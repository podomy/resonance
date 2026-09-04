#define _GNU_SOURCE
#include "node/node.h"
#include "shared/context.h"
#include "sim/sim.h"
#include "tun/tun.h"
#include "world/world.h"
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HOLD_UP 8
#define HOLD_DOWN 10

// drain_tun pumps one packet, or discards it if drop.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd, int drop) {
    char junk[2048];

    if (!drop) {
        tun_pump_fd(map, fd, grid, radio, nodes);
        return;
    }
    read(fd, junk, sizeof(junk));
}

// drain_log prints one child line. Sets seen2 on peers:2.
static void drain_log(int i, int fd, int* seen2) {
    char buf[2048];
    ssize_t n;

    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = '\0';
    printf("\033[%dm[node %d]\033[0m %s", 36 + i, i, buf);
    if (strstr(buf, "\"peers\":2") != NULL)
        *seen2 = 1;
}

// children_alive is 1 if any child still runs.
static int children_alive(pid_t* pids) {
    int i;

    for (i = 0; i < SIM_NODES; i++) {
        if (waitpid(pids[i], NULL, WNOHANG) == 0)
            return (1);
    }
    return (0);
}

// partition_tick cuts the cable HOLD_UP after mesh-up,
// then restores it HOLD_DOWN later. One cycle only.
static void partition_tick(int* drop, int* seen2,
                           time_t* t0, int* restored) {
    time_t now;

    now = time(NULL);
    if (*restored)
        return;
    if (*drop) {
        if (now - *t0 < HOLD_DOWN)
            return;
        printf("resonance: partition end\n");
        *drop = 0;
        *seen2 = 0;
        *t0 = now;
        *restored = 1;
        return;
    }
    if (!*seen2)
        return;
    if (*t0 == 0) {
        *t0 = now;
        return;
    }
    if (now - *t0 < HOLD_UP)
        return;
    printf("resonance: partition start\n");
    *drop = 1;
    *seen2 = 0;
    *t0 = now;
}

// pump_until forwards tun and logs until children exit.
static void pump_until(TunMap* map, MediumGrid* grid,
                       RadioParams* radio, NodeList* nodes,
                       int* fds, pid_t* pids, int* logfds) {
    struct pollfd p[2 * SIM_NODES];
    int i, drop, seen2, restored, done;
    time_t t0;

    drop = 0;
    seen2 = 0;
    restored = 0;
    done = 0;
    t0 = 0;
    for (i = 0; i < SIM_NODES; i++) {
        p[i].fd = fds[i];
        p[i].events = POLLIN;
        p[SIM_NODES + i].fd = logfds[i];
        p[SIM_NODES + i].events = POLLIN;
    }
    do {
        if (poll(p, 2 * SIM_NODES, 100) > 0) {
            for (i = 0; i < SIM_NODES; i++) {
                if (p[i].revents & POLLIN)
                    drain_tun(map, grid, radio, nodes,
                              fds[i], drop);
                if (p[SIM_NODES + i].revents & POLLIN)
                    drain_log(i, logfds[i], &seen2);
            }
        }
        partition_tick(&drop, &seen2, &t0, &restored);
        if (restored && seen2)
            done = 1;
        if (restored && t0 != 0 &&
            time(NULL) - t0 >= HOLD_UP)
            done = 1;
    } while (!done && children_alive(pids));
}

// reap kills children and closes fds.
static void reap(pid_t* pids, int* fds, int* logfds) {
    int i;

    for (i = 0; i < SIM_NODES; i++) {
        kill(pids[i], SIGTERM);
        waitpid(pids[i], NULL, 0);
        close(fds[i]);
        close(logfds[i]);
    }
}

int main(void) {
    Context ctx;
    TunMap map;
    int fds[SIM_NODES], logfds[SIM_NODES];
    pid_t pids[SIM_NODES];

    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
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
    pump_until(&map, &ctx.grid, &ctx.radio, &ctx.nodes, fds,
               pids, logfds);
    reap(pids, fds, logfds);
    context_free(&ctx);
    system("ip netns del net_namespace_nodeb");
    system("rm -rf /tmp/resonance");
    return (0);
}
