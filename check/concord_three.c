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

#define N 3
#define ISOLATE 2
#define HOLD_UP 8
#define HOLD_DOWN 10
#define DEADLINE 90

// concord_three asserts 3-node mesh, isolate 2, reunion.
// Underlay 192.168.100.1/2/3, overlay 10.0.0.0/16.

// has reports needle in hay.
static int has(const char* hay, const char* needle) {
    return (strstr(hay, needle) != NULL);
}

// drain_tun pumps one packet, or discards if isolated.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd, int i, int drop) {
    char junk[2048];

    if (drop && i == ISOLATE) {
        read(fd, junk, sizeof(junk));
        return;
    }
    tun_pump_fd(map, fd, grid, radio, nodes);
}

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    int i, drop, seen3, lost, restored;
    time_t t0, start;

    if (access("./concord", X_OK) != 0) {
        printf("concord_three: skip no ./concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_three: skip (%s)\n",
               strerror(errno));
        return (0);
    }
    if (!sim_nodes_add(&ctx, &map, fds, N))
        return (1);
    if (!sim_addrs_up(N))
        return (1);
    if (!sim_spawn_concord(pids, logfds, N))
        return (1);

    for (i = 0; i < N; i++) {
        p[i].fd = fds[i];
        p[i].events = POLLIN;
        p[N + i].fd = logfds[i];
        p[N + i].events = POLLIN;
    }

    drop = 0;
    seen3 = 0;
    lost = 0;
    restored = 0;
    t0 = 0;
    start = time(NULL);
    while (time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * N, 100) > 0) {
            for (i = 0; i < N; i++) {
                if (p[i].revents & POLLIN)
                    drain_tun(&map, &ctx.grid, &ctx.radio,
                              &ctx.nodes, fds[i], i, drop);
                if (!(p[N + i].revents & POLLIN))
                    continue;
                char buf[2048];
                ssize_t n;

                n = read(logfds[i], buf, sizeof(buf) - 1);
                if (n <= 0)
                    continue;
                buf[n] = '\0';
                // Just look for strings peers:3 and
                // peer.lost.
                if (has(buf, "\"peers\":3"))
                    seen3 = 1;
                if (has(buf, "peer.lost"))
                    lost = 1;
            }
        }
        if (!drop && seen3) {
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
                seen3 = 0;
                t0 = time(NULL);
            }
        } else if (restored && seen3)
            break;
    }

    for (i = 0; i < N; i++) {
        kill(pids[i], SIGTERM);
        waitpid(pids[i], NULL, 0);
        close(fds[i]);
        close(logfds[i]);
    }
    context_free(&ctx);
    sim_netns_teardown(N);
    system("rm -rf /tmp/resonance");

    if (!restored || !lost || !seen3) {
        fprintf(stderr,
                "concord_three: fail restored=%d lost=%d "
                "seen3=%d\n",
                restored, lost, seen3);
        return (1);
    }
    return (0);
}
