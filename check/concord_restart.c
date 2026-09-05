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
#define KILL 2
#define HOLD_UP 5
#define DEADLINE 150

// concord_restart asserts mesh, SIGKILL one node,
// restart it fresh, mesh again.
// Same underlay 192.168.100.1/2/3 as concord_three.

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    int i, seen3, killed, back;
    time_t t0, start;

    if (access("./concord", X_OK) != 0) {
        printf("concord_restart: skip no ./concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_restart: skip (%s)\n",
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

    seen3 = 0;
    killed = 0;
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

                // Just look for the string peers:3.
                n = read(logfds[i], buf, sizeof(buf) - 1);
                if (n <= 0)
                    continue;
                buf[n] = '\0';
                if (strstr(buf, "\"peers\":3") != NULL) {
                    if (!killed)
                        seen3 = 1;
                    else
                        back = 1;
                }
            }
        }
        if (seen3 && !killed) {
            if (t0 == 0)
                t0 = time(NULL);
            else if (time(NULL) - t0 >= HOLD_UP) {
                kill(pids[KILL], SIGKILL);
                waitpid(pids[KILL], NULL, 0);
                close(logfds[KILL]);
                if (!sim_restart_concord(pids, logfds,
                                         KILL))
                    break;
                p[N + KILL].fd = logfds[KILL];
                p[N + KILL].events = POLLIN;
                killed = 1;
            }
        } else if (killed && back)
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
    system("rm -rf /tmp/resonance");

    if (!seen3 || !killed || !back) {
        fprintf(stderr,
                "concord_restart: fail seen3=%d killed=%d "
                "back=%d\n",
                seen3, killed, back);
        return (1);
    }
    return (0);
}
