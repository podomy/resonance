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
#define HOLD_DOWN 10
#define DEADLINE 150
#define IMAGE "docker.io/library/nginx:alpine"

// concord_workload asserts a workload submitted on an
// isolated node shows up on the other two after reunion.
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

// workload_present runs workload list against node dir
// and reports 1 if shortid shows up.
static int workload_present(int node, const char* shortid) {
    FILE* fp;
    char cmd[256];
    char buf[16384];
    size_t len;
    size_t n;

    snprintf(cmd, sizeof(cmd),
             "XDG_CONFIG_HOME=/tmp/resonance/node%d "
             "./concord workload list 2>/dev/null",
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

// submit_workload runs workload run against the isolated
// node and stores the id into out. Returns 1 on success.
static int submit_workload(char* out, size_t n) {
    FILE* fp;
    char buf[1024];
    char id[64];

    fp = popen("XDG_CONFIG_HOME=/tmp/resonance/node2 "
               "./concord workload run " IMAGE
               " 2>/dev/null",
               "r");
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

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    int has[N];
    char wid[64], shortid[16];
    int i, drop, seen3, restored, ok;
    time_t t0, tcheck, start;

    if (access("./concord", X_OK) != 0) {
        printf("concord_workload: skip no ./concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_workload: skip (%s)\n",
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
        has[i] = 0;
    }

    drop = 0;
    seen3 = 0;
    restored = 0;
    t0 = 0;
    tcheck = 0;
    wid[0] = '\0';
    shortid[0] = '\0';
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

                // Just look for the string peers:3.
                n = read(logfds[i], buf, sizeof(buf) - 1);
                if (n <= 0)
                    continue;
                buf[n] = '\0';
                if (strstr(buf, "\"peers\":3") != NULL)
                    seen3 = 1;
            }
        }
        if (!drop && !restored && seen3) {
            if (t0 == 0)
                t0 = time(NULL);
            else if (time(NULL) - t0 >= HOLD_UP) {
                drop = 1;
                seen3 = 0;
                t0 = time(NULL);
                if (submit_workload(wid, sizeof(wid))) {
                    memcpy(shortid, wid, 8);
                    shortid[8] = '\0';
                } else {
                    break;
                }
            }
        } else if (drop && time(NULL) - t0 >= HOLD_DOWN) {
            drop = 0;
            restored = 1;
            seen3 = 0;
            t0 = time(NULL);
            tcheck = 0;
        } else if (restored && shortid[0] != '\0') {
            // Ask nodes 0 and 1 directly every 2s.
            if (tcheck == 0 || time(NULL) - tcheck >= 2) {
                tcheck = time(NULL);
                has[0] = workload_present(0, shortid);
                has[1] = workload_present(1, shortid);
                if (has[0] && has[1])
                    break;
            }
        }
        ok = 0;
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

    if (!has[0] || !has[1]) {
        fprintf(stderr,
                "concord_workload: fail workload %s on "
                "0=%d 1=%d\n",
                shortid, has[0], has[1]);
        return (1);
    }
    return (0);
}
