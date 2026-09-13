#define _GNU_SOURCE
#include "../node/node.h"
#include "../shared/context.h"
#include "../sim/sim.h"
#include "../tun/tun.h"
#include "../world/world.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define N 3
#define MOVER 2
#define X_HOME ((int64_t)2 * 1000000000LL)
#define X_AWAY ((int64_t)7 * 1000000000LL)
#define HOLD_UP 8
#define HOLD_DOWN 10
#define DEADLINE 120

// concord_mobile asserts 3-node mesh, drive node 2 out
// of radio range, drive it back, mesh again. No fd is
// ever dropped; radio_path does the partitioning.

// has reports needle in hay.
static int has(const char* hay, const char* needle) {
    return (strstr(hay, needle) != NULL);
}

// move_node sets the x position of the node behind fd.
static void move_node(TunMap* map, NodeList* nodes, int fd,
                      int64_t x) {
    size_t i;

    for (i = 0; i < map->n; i++) {
        if (map->slot[i].fd == fd) {
            Node* node;

            node =
                nodelist_find(nodes, map->slot[i].node_id);
            if (node != NULL)
                node->x_nm = x;
            return;
        }
    }
}

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    int i, away, seen3, lost, back;
    time_t t0, start;

    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_mobile: skip no ./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_mobile: skip (%s)\n",
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

    away = 0;
    seen3 = 0;
    lost = 0;
    back = 0;
    t0 = 0;
    start = time(NULL);
    while (time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * N, 100) > 0) {
            for (i = 0; i < N; i++) {
                if (p[i].revents & POLLIN)
                    tun_pump_fd(&map, fds[i], &ctx.grid,
                                &ctx.radio, &ctx.nodes);
                if (!(p[N + i].revents & POLLIN))
                    continue;
                char buf[2048];
                ssize_t n;

                // Just look for strings peers:3 and
                // peer.lost.
                n = read(logfds[i], buf, sizeof(buf) - 1);
                if (n <= 0)
                    continue;
                buf[n] = '\0';
                if (has(buf, "\"peers\":3"))
                    seen3 = 1;
                if (has(buf, "peer.lost"))
                    lost = 1;
            }
        }
        if (!away && !back && seen3) {
            if (t0 == 0)
                t0 = time(NULL);
            else if (time(NULL) - t0 >= HOLD_UP) {
                move_node(&map, &ctx.nodes, fds[MOVER],
                          X_AWAY);
                away = 1;
                seen3 = 0;
                t0 = time(NULL);
            }
        } else if (away && lost &&
                   time(NULL) - t0 >= HOLD_DOWN) {
            move_node(&map, &ctx.nodes, fds[MOVER], X_HOME);
            away = 0;
            back = 1;
            seen3 = 0;
            t0 = time(NULL);
        } else if (back && seen3)
            break;
        int ok = 0;
        for (i = 0; i < N; i++) {
            if (waitpid(pids[i], NULL, WNOHANG) == 0)
                ok = 1;
        }
        if (!ok)
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
    if (system("rm -rf /tmp/resonance") != 0) {
        // Best effort cleanup, teardown already ran.
    }

    if (!back || !lost || !seen3) {
        fprintf(stderr,
                "concord_mobile: fail back=%d lost=%d "
                "seen3=%d\n",
                back, lost, seen3);
        return (1);
    }
    return (0);
}
