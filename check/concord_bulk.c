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
#define HOLD_UP 8
#define DEADLINE 240
#define NWL 5
#define IMAGE "docker.io/library/nginx:alpine"

// concord_bulk asserts five workloads submitted on an
// isolated node all show up on all nodes after reunion.
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

// submit_workload runs workload run against node and
// stores the id into out. Returns 1 on success.
static int submit_workload(int node, char* out, size_t n) {
    FILE* fp;
    char cmd[256];
    char buf[1024];
    char id[64];

    snprintf(cmd, sizeof(cmd),
             "XDG_CONFIG_HOME=/tmp/resonance/node%d "
             "./bin/concord workload run " IMAGE " 2>/dev/null",
             node);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (0);
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return (0);
    }
    pclose(fp);
    // Output is "Submitted workload <uuid>".
    if (sscanf(buf, "%*s %*s %63s", id) != 1)
        return (0);
    if (strlen(id) + 1 > n)
        return (0);
    strcpy(out, id);
    return (1);
}

// workload_present runs workload list against node and
// reports 1 if shortid shows up.
static int workload_present(int node, const char* shortid) {
    FILE* fp;
    char cmd[256];
    char buf[16384];
    size_t len;
    size_t n;

    snprintf(cmd, sizeof(cmd),
             "XDG_CONFIG_HOME=/tmp/resonance/node%d "
             "./bin/concord workload list 2>/dev/null",
             node);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (0);
    len = 0;
    while ((n = fread(buf + len, 1, sizeof(buf) - len - 1,
                      fp)) > 0) {
        len += n;
        if (len >= sizeof(buf) - 1)
            break;
    }
    pclose(fp);
    buf[len] = '\0';
    return (strstr(buf, shortid) != NULL);
}

// nodes_alive runs node list against node and reports
// how many members show alive.
static int nodes_alive(int node) {
    FILE* fp;
    char cmd[256];
    char buf[16384];
    size_t len;
    size_t n;
    int alive;
    char* s;

    snprintf(cmd, sizeof(cmd),
             "XDG_CONFIG_HOME=/tmp/resonance/node%d "
             "./bin/concord node list 2>/dev/null",
             node);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (-1);
    len = 0;
    while ((n = fread(buf + len, 1, sizeof(buf) - len - 1,
                      fp)) > 0) {
        len += n;
        if (len >= sizeof(buf) - 1)
            break;
    }
    pclose(fp);
    buf[len] = '\0';
    alive = 0;
    s = buf;
    while ((s = strstr(s, "alive")) != NULL) {
        alive++;
        s++;
    }
    return (alive);
}

// mesh3 polls node list on every node every 2s,
// reports 1 when all see 3 alive.
static int mesh3(time_t* tcheck) {
    int i;

    if (*tcheck != 0 && time(NULL) - *tcheck < 2)
        return (0);
    *tcheck = time(NULL);
    for (i = 0; i < N; i++) {
        if (nodes_alive(i) != 3)
            return (0);
    }
    return (1);
}

// hold arms t0 on first call, fires secs later.
static int hold(time_t* t0, int secs) {
    if (*t0 == 0) {
        *t0 = time(NULL);
        return (0);
    }
    return (time(NULL) - *t0 >= secs);
}

// converged reports 1 when every shortid lists on
// every node. Polls at most every 2s.
static int converged(char shorts[NWL][16], time_t* tcheck) {
    int k, i;

    if (*tcheck != 0 && time(NULL) - *tcheck < 2)
        return (0);
    *tcheck = time(NULL);
    for (k = 0; k < NWL; k++) {
        for (i = 0; i < N; i++) {
            if (!workload_present(i, shorts[k]))
                return (0);
        }
    }
    return (1);
}

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    char wids[NWL][64], shorts[NWL][16];
    int i, k, phase, seen3, done, alive, nsub;
    time_t t0, tcheck, tmesh, start;

    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_bulk: skip no ./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_bulk: skip (%s)\n",
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
    done = 0;
    nsub = 0;
    t0 = 0;
    tcheck = 0;
    tmesh = 0;
    for (k = 0; k < NWL; k++) {
        wids[k][0] = '\0';
        shorts[k][0] = '\0';
    }
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
            // Split first, then submit all five on the
            // lone node.
            phase = 1;
        } else if (phase == 1) {
            if (nsub < NWL &&
                submit_workload(ISOLATE, wids[nsub],
                                sizeof(wids[nsub]))) {
                memcpy(shorts[nsub], wids[nsub], 8);
                shorts[nsub][8] = '\0';
                nsub++;
            } else if (nsub < NWL) {
                break;
            } else {
                phase = 2;
                seen3 = 0;
                tcheck = 0;
            }
        } else if (phase == 2) {
            if (converged(shorts, &tcheck) &&
                mesh3(&tmesh)) {
                done = 1;
                break;
            }
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
    if (system("rm -rf /tmp/resonance") != 0) {
        // Best effort cleanup, teardown already ran.
    }

    if (!done) {
        fprintf(stderr,
                "concord_bulk: fail phase=%d submitted=%d\n",
                phase, nsub);
        return (1);
    }
    return (0);
}
