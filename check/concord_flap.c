#define _GNU_SOURCE
#include "../shared/context.h"
#include "../sim/sim.h"
#include "../tun/tun.h"
#include "../world/world.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define N 3
#define ISOLATE 2
#define FLAPS 3
#define HOLD_UP 8
#define HOLD_DOWN 10
#define DEADLINE 240

// concord_flap asserts three isolate/restore cycles of
// node 2, with peers:3 after every reunion.
// Same underlay 192.168.100.1/2/3 as concord_three.

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

// hold arms t0 on first call, fires secs later.
static int hold(time_t* t0, int secs) {
    if (*t0 == 0) {
        *t0 = time(NULL);
        return (0);
    }
    return (time(NULL) - *t0 >= secs);
}

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    int i, phase, seen3, flaps, alive;
    time_t t0, start;

    if (access("./concord", X_OK) != 0) {
        printf("concord_flap: skip no ./concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_flap: skip (%s)\n",
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

    phase = 0;
    seen3 = 0;
    flaps = 0;
    t0 = 0;
    start = time(NULL);
    while (time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * N, 100) > 0) {
            for (i = 0; i < N; i++) {
                if (p[i].revents & POLLIN)
                    drain_tun(&map, &ctx.grid, &ctx.radio,
                              &ctx.nodes, fds[i], i,
                              phase == 1);
                if (!(p[N + i].revents & POLLIN))
                    continue;
                char buf[2048];
                ssize_t n;

                // Just look for the string peers:3.
                n = read(logfds[i], buf, sizeof(buf) - 1);
                if (n <= 0)
                    continue;
                buf[n] = '\0';
                if (strstr(buf, "\"peers\":3") != NULL)
                    seen3 = 1;
            }
        }
        if (phase == 0 && seen3 && hold(&t0, HOLD_UP)) {
            phase = 1;
            seen3 = 0;
            t0 = 0;
        } else if (phase == 1 && hold(&t0, HOLD_DOWN)) {
            phase = 2;
            seen3 = 0;
            t0 = 0;
        } else if (phase == 2 && seen3) {
            flaps++;
            if (flaps >= FLAPS)
                break;
            phase = 0;
            seen3 = 0;
            t0 = 0;
        }
        alive = 0;
        for (i = 0; i < N; i++) {
            if (waitpid(pids[i], NULL, WNOHANG) == 0)
                alive = 1;
        }
        if (!alive)
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

    if (flaps < FLAPS) {
        fprintf(stderr,
                "concord_flap: fail flaps=%d phase=%d\n",
                flaps, phase);
        return (1);
    }
    return (0);
}
