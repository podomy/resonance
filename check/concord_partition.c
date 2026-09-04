#define _GNU_SOURCE
#include "../shared/context.h"
#include "../sim/sim.h"
#include "../tun/tun.h"
#include "../world/world.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define HOLD_UP 8
#define HOLD_DOWN 10
#define DEADLINE 90

// concord_partition asserts mesh, drop, reunion.
// Same underlay 192.168.100.1/2 as concord_two.

// has reports needle in hay.
static int has(const char* hay, const char* needle) {
    return (strstr(hay, needle) != NULL);
}

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

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * SIM_NODES];
    int fds[SIM_NODES], logfds[SIM_NODES];
    pid_t pids[SIM_NODES];
    int i, drop, seen2, lost, restored, ok;
    time_t t0, start;

    if (access("./concord", X_OK) != 0) {
        printf("concord_partition: skip no ./concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup())
        return (1);
    if (!sim_tuns_open(fds)) {
        printf("concord_partition: skip (%s)\n",
               strerror(errno));
        return (0);
    }
    if (!sim_nodes_add(&ctx, &map, fds))
        return (1);
    if (!sim_addrs_up())
        return (1);
    if (!sim_spawn_concord(pids, logfds))
        return (1);

    for (i = 0; i < SIM_NODES; i++) {
        p[i].fd = fds[i];
        p[i].events = POLLIN;
        p[SIM_NODES + i].fd = logfds[i];
        p[SIM_NODES + i].events = POLLIN;
    }

    drop = 0;
    seen2 = 0;
    lost = 0;
    restored = 0;
    t0 = 0;
    start = time(NULL);
    while (time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * SIM_NODES, 100) > 0) {
            for (i = 0; i < SIM_NODES; i++) {
                if (p[i].revents & POLLIN)
                    drain_tun(&map, &ctx.grid, &ctx.radio,
                              &ctx.nodes, fds[i], drop);
                if (!(p[SIM_NODES + i].revents & POLLIN))
                    continue;
                char buf[2048];
                ssize_t n;

                n = read(logfds[i], buf, sizeof(buf) - 1);
                if (n <= 0)
                    continue;
                buf[n] = '\0';
                // Just look for strings peers:2 and
                // peer.lost.
                if (has(buf, "\"peers\":2"))
                    seen2 = 1;
                if (has(buf, "peer.lost"))
                    lost = 1;
            }
        }
        if (!drop && seen2) {
            if (t0 == 0)
                t0 = time(NULL);
            else if (time(NULL) - t0 >= HOLD_UP) {
                drop = 1;
                t0 = time(NULL);
            }
        } else if (drop && !restored) {
            if (time(NULL) - t0 >= HOLD_DOWN) {
                drop = 0;
                restored = 1;
                seen2 = 0;
                t0 = time(NULL);
            }
        } else if (restored && seen2)
            break;
        ok = 0;
        for (i = 0; i < SIM_NODES; i++) {
            if (waitpid(pids[i], NULL, WNOHANG) == 0)
                ok = 1;
        }
        if (!ok)
            break;
    }

    for (i = 0; i < SIM_NODES; i++) {
        kill(pids[i], SIGTERM);
        waitpid(pids[i], NULL, 0);
        close(fds[i]);
        close(logfds[i]);
    }
    context_free(&ctx);
    system("ip netns del net_namespace_nodeb");
    system("rm -rf /tmp/resonance");

    if (!restored || !lost || !seen2) {
        fprintf(stderr,
                "concord_partition: fail restored=%d "
                "lost=%d seen2=%d\n",
                restored, lost, seen2);
        return (1);
    }
    return (0);
}
