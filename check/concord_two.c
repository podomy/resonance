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
#include <unistd.h>

// concord_two asserts two-node mesh.
// Underlay 192.168.100.1/2 is disjoint from
// overlay 10.0.0.0/16. Both peer.seen and peers 2 within
// deadline, then SIGTERM.

// has reports needle in hay.
static int has(const char* hay, const char* needle) {
    return (strstr(hay, needle) != NULL);
}

int main(void) {
    if (access("./concord", X_OK) != 0) {
        printf("concord_two: skip no ./concord\n");
        return (0);
    }

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
    if (!sim_tuns_open(fds)) {
        printf("concord_two: skip (%s)\n", strerror(errno));
        return (0);
    }
    if (!sim_nodes_add(&ctx, &map, fds))
        return (1);
    if (!sim_addrs_up())
        return (1);
    if (!sim_spawn_concord(pids, logfds))
        return (1);

    struct pollfd p[2 * SIM_NODES];
    for (i = 0; i < SIM_NODES; i++) {
        p[i].fd = fds[i];
        p[i].events = POLLIN;
    }
    for (i = SIM_NODES; i < 2 * SIM_NODES; i++) {
        p[i].fd = logfds[i - SIM_NODES];
        p[i].events = POLLIN;
    }

    int seen[SIM_NODES] = {0};
    int peers2[SIM_NODES] = {0};
    int deadline_ms = 45000;
    int elapsed = 0;

    while (elapsed < deadline_ms) {
        if (poll(p, 2 * SIM_NODES, 100) > 0) {
            for (i = 0; i < SIM_NODES; i++) {
                if (p[i].revents & POLLIN)
                    tun_pump_fd(&map, fds[i], &ctx.grid,
                                &ctx.radio, &ctx.nodes);
            }
            // Take logs from each concord pipe into buf and
            // check.
            for (i = 0; i < SIM_NODES; i++) {
                if (!(p[SIM_NODES + i].revents & POLLIN))
                    continue;
                char buf[2048];
                ssize_t n =
                    read(logfds[i], buf, sizeof(buf) - 1);
                if (n <= 0)
                    continue;
                buf[n] = '\0';
                // Just look for strings "peer.seen" and
                // "peers:2".
                if (has(buf, "peer.seen"))
                    seen[i] = 1;
                if (has(buf, "\"peers\":2") ||
                    has(buf, "\"peers\": 2"))
                    peers2[i] = 1;
            }
        }
        int ok = 1;
        for (i = 0; i < SIM_NODES; i++) {
            if (!seen[i] || !peers2[i])
                ok = 0;
        }
        if (ok)
            break;
        // Check children still alive.
        int alive = 0;
        for (i = 0; i < SIM_NODES; i++) {
            if (waitpid(pids[i], NULL, WNOHANG) == 0)
                alive = 1;
        }
        if (!alive)
            break;
        elapsed += 100;
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

    int ok = 1;
    for (i = 0; i < SIM_NODES; i++) {
        if (!seen[i] || !peers2[i])
            ok = 0;
    }
    if (!ok) {
        fprintf(
            stderr,
            "concord_two: timeout peers %d/%d seen %d/%d\n",
            peers2[0] + peers2[1], SIM_NODES,
            seen[0] + seen[1], SIM_NODES);
        return (1);
    }
    return (0);
}
