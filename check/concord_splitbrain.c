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
#define HOLD_UP 8
#define HOLD_DOWN 10
#define DEADLINE 240
#define IMAGE "docker.io/library/nginx:alpine"

// concord_splitbrain asserts three isolated nodes, one
// workload each, converge fully on heal: mesh plus all
// three workloads on all three nodes.
// Same underlay 192.168.100.1/2/3 as concord_three.

// drain_tun pumps one packet, or discards all if split.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd, int drop) {
    char junk[2048];
    ssize_t n;

    if (drop) {
        n = read(fd, junk, sizeof(junk));
        (void)n;
        return;
    }
    tun_pump_fd(map, fd, grid, radio, nodes);
}

// nodes_mesh runs node list against node and reports 1
// if three members show alive. State based, not log
// based: a reunion that never shrinks the ID set still
// counts.
static int nodes_mesh(int node) {
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
    alive = 0;
    s = buf;
    while ((s = strstr(s, " alive ")) != NULL) {
        alive++;
        s++;
    }
    return (alive == 3);
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

// hold arms t0 on first call, fires secs later.
static int hold(time_t* t0, int secs) {
    if (*t0 == 0) {
        *t0 = time(NULL);
        return (0);
    }
    return (time(NULL) - *t0 >= secs);
}

// submit_each submits one workload per node, storing
// short ids. Returns 1 if all three landed.
static int submit_each(char wids[N][64], char shorts[N][16]) {
    int i;

    for (i = 0; i < N; i++) {
        if (!submit_workload(i, wids[i], sizeof(wids[i])))
            return (0);
        memcpy(shorts[i], wids[i], 8);
        shorts[i][8] = '\0';
    }
    return (1);
}

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    int has[N][N];
    char wids[N][64], shorts[N][16];
    int i, j, phase, seen3, alive;
    int mesh[N];
    time_t t0, tcheck, tmiss, start;

    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_splitbrain: skip no ./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_splitbrain: skip (%s)\n",
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
        for (j = 0; j < N; j++)
            has[i][j] = 0;
        mesh[i] = 0;
        wids[i][0] = '\0';
        shorts[i][0] = '\0';
    }

    phase = 0;
    seen3 = 0;
    t0 = 0;
    tcheck = 0;
    tmiss = 0;
    start = time(NULL);
    while (time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * N, 100) > 0) {
            for (i = 0; i < N; i++) {
                if (p[i].revents & POLLIN)
                    drain_tun(&map, &ctx.grid, &ctx.radio,
                              &ctx.nodes, fds[i],
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
            // Split everyone apart, one workload each.
            if (!submit_each(wids, shorts))
                break;
            phase = 1;
            t0 = 0;
        } else if (phase == 1 && hold(&t0, HOLD_DOWN)) {
            phase = 2;
            seen3 = 0;
            t0 = 0;
            tcheck = 0;
        } else if (phase == 2 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Ask every node for every workload.
            tcheck = time(NULL);
            for (i = 0; i < N; i++) {
                for (j = 0; j < N; j++)
                    has[i][j] = workload_present(i,
                                                 shorts[j]);
                mesh[i] = nodes_mesh(i);
            }
            if (mesh[0] && mesh[1] && mesh[2] &&
                has[0][0] && has[0][1] &&
                has[0][2] && has[1][0] && has[1][1] &&
                has[1][2] && has[2][0] && has[2][1] &&
                has[2][2])
                break;
            else if (tmiss == 0 ||
                     time(NULL) - tmiss >= 30) {
                // Still waiting: say what is missing.
                tmiss = time(NULL);
                printf("concord_splitbrain: waiting "
                       "mesh=%d%d%d "
                       "has=%d%d%d/%d%d%d/%d%d%d\n",
                       mesh[0], mesh[1], mesh[2],
                       has[0][0], has[0][1],
                       has[0][2], has[1][0], has[1][1],
                       has[1][2], has[2][0], has[2][1],
                       has[2][2]);
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

    if (!(mesh[0] && mesh[1] && mesh[2] && has[0][0] &&
          has[0][1] && has[0][2] && has[1][0] &&
          has[1][1] && has[1][2] && has[2][0] &&
          has[2][1] && has[2][2])) {
        fprintf(stderr,
                "concord_splitbrain: fail phase=%d "
                "mesh=%d%d%d has=%d%d%d/%d%d%d/%d%d%d\n",
                phase, mesh[0], mesh[1], mesh[2],
                has[0][0], has[0][1],
                has[0][2], has[1][0], has[1][1], has[1][2],
                has[2][0], has[2][1], has[2][2]);
        return (1);
    }
    return (0);
}
