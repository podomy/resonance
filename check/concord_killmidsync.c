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
#define DEADLINE 180
#define IMAGE "docker.io/library/nginx:alpine"
#define SYNC2 "192.168.100.3:8443"

// concord_killmidsync asserts a workload submitted on an
// isolated node survives a SIGKILL of that node during
// reunion sync and shows up on all nodes after restart.
// Same underlay 192.168.100.1/2/3 as concord_three.

// drain_tun pumps one packet, or discards if isolated.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd, int i, int drop) {
    char junk[2048];
    ssize_t n;

    if (drop && i == ISOLATE) {
        n = read(fd, junk, sizeof(junk));
        (void)n;
        return;
    }
    tun_pump_fd(map, fd, grid, radio, nodes);
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

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    int has[N];
    char wid[64], shortid[16];
    int i, phase, seen3, sync2, ok;
    time_t t0, tcheck, start;

    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_killmidsync: skip no ./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_killmidsync: skip (%s)\n",
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

    phase = 0;
    seen3 = 0;
    sync2 = 0;
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
                              &ctx.nodes, fds[i], i,
                              phase == 1);
                if (!(p[N + i].revents & POLLIN))
                    continue;
                char buf[2048];
                ssize_t n;

                // Just look for strings peers:3 and a
                // completed sync with node 2.
                n = read(logfds[i], buf, sizeof(buf) - 1);
                if (n <= 0)
                    continue;
                buf[n] = '\0';
                if (strstr(buf, "\"peers\":3") != NULL)
                    seen3 = 1;
                if (strstr(buf, "peer sync ok") != NULL &&
                    strstr(buf, SYNC2) != NULL)
                    sync2 = 1;
            }
        }
        if (phase == 0 && seen3 && hold(&t0, HOLD_UP)) {
            seen3 = 0;
            sync2 = 0;
            t0 = time(NULL);
            if (!submit_workload(ISOLATE, wid, sizeof(wid)))
                break;
            memcpy(shortid, wid, 8);
            shortid[8] = '\0';
            phase = 1;
        } else if (phase == 1 && hold(&t0, HOLD_DOWN)) {
            phase = 2;
        } else if (phase == 2 && sync2) {
            // First sync round-trip with node 2 is done;
            // more rounds are due. Kill it mid-reunion.
            kill(pids[ISOLATE], SIGKILL);
            waitpid(pids[ISOLATE], NULL, 0);
            close(logfds[ISOLATE]);
            if (!sim_restart_concord(pids, logfds, ISOLATE))
                break;
            p[N + ISOLATE].fd = logfds[ISOLATE];
            p[N + ISOLATE].events = POLLIN;
            phase = 3;
            seen3 = 0;
        } else if (phase == 3 && shortid[0] != '\0' &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            tcheck = time(NULL);
            for (i = 0; i < N; i++)
                has[i] = workload_present(i, shortid);
            if (has[0] && has[1] && has[2])
                break;
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
    if (system("rm -rf /tmp/resonance") != 0) {
        // Best effort cleanup, teardown already ran.
    }

    if (!has[0] || !has[1] || !has[2]) {
        fprintf(stderr,
                "concord_killmidsync: fail workload %s on "
                "%d %d %d phase=%d\n",
                shortid, has[0], has[1], has[2], phase);
        return (1);
    }
    return (0);
}
