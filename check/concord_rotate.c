#define _GNU_SOURCE
#include "../shared/context.h"
#include "../sim/sim.h"
#include "../tun/tun.h"
#include "../world/world.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define N 3
#define ROT_NODE 2
#define HOLD_UP 8
#define DEADLINE 240
#define IMAGE "docker.io/library/nginx:alpine"

// concord_rotate asserts mesh, rotate one node's
// Noise key in place, re-mesh with re-pin at
// generation 1, then workload convergence on
// all nodes. Same underlay 192.168.100.1/2/3
// as concord_three.

// drain_tun pumps one packet.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd) {
    tun_pump_fd(map, fd, grid, radio, nodes);
}

// drain scans one child log. Sets seen3 on
// peers:3, rot on peer key rotated.
static void drain(int fd, int* seen3, int* rot) {
    char buf[2048];
    ssize_t n;

    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = '\0';
    if (strstr(buf, "\"peers\":3") != NULL)
        *seen3 = 1;
    if (strstr(buf, "peer key rotated") != NULL)
        *rot = 1;
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

// ask_lists refreshes has[] from every node every 2s.
static void ask_lists(int* has, const char* shortid,
                      time_t* tcheck) {
    int i;

    if (*tcheck != 0 && time(NULL) - *tcheck < 2)
        return;
    *tcheck = time(NULL);
    for (i = 0; i < N; i++)
        has[i] = workload_present(i, shortid);
}

// restart_rotated kills node i, runs rotate-key,
// and forks it again with the same dir. Identity,
// journal, and bumped generation persist; only
// the Noise key is fresh. Stores new pid and
// logfd. Returns 1 on success.
static int restart_rotated(pid_t* pids,
                           int* logfds, int i) {
    char dir[64];
    char cmd[256];
    char path[64];
    int pipedes[2];
    pid_t pid;
    int nsfd;

    kill(pids[i], SIGTERM);
    waitpid(pids[i], NULL, 0);
    close(logfds[i]);
    snprintf(dir, sizeof(dir),
             "/tmp/resonance/node%d", i);
    snprintf(cmd, sizeof(cmd),
             "XDG_CONFIG_HOME=%s ./bin/concord "
             "node rotate-key >/dev/null 2>&1",
             dir);
    if (system(cmd) != 0)
        return (0);
    if (pipe(pipedes) < 0)
        return (0);
    pid = fork();
    if (pid < 0) {
        close(pipedes[0]);
        close(pipedes[1]);
        return (0);
    }
    if (pid == 0) {
        setenv("XDG_CONFIG_HOME", dir, 1);
        if (i > 0) {
            snprintf(path, sizeof(path),
                     "/var/run/netns/"
                     "resonance_%d",
                     i);
            nsfd = open(path, O_RDONLY);
            if (nsfd >= 0) {
                setns(nsfd, CLONE_NEWNET);
                close(nsfd);
            }
        }
        dup2(pipedes[1], 1);
        dup2(pipedes[1], 2);
        close(pipedes[0]);
        close(pipedes[1]);
        execl("./bin/concord", "concord",
              (char*)NULL);
        _exit(127);
    }
    close(pipedes[1]);
    pids[i] = pid;
    logfds[i] = pipedes[0];
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
    int i, phase, seen3, rot, done, alive;
    time_t t0, tcheck, start;

    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_rotate: skip no "
               "./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_rotate: skip (%s)\n",
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
    rot = 0;
    done = 0;
    t0 = 0;
    tcheck = 0;
    wid[0] = '\0';
    shortid[0] = '\0';
    start = time(NULL);
    while (time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * N, 100) > 0) {
            for (i = 0; i < N; i++) {
                if (p[i].revents & POLLIN)
                    drain_tun(&map, &ctx.grid,
                              &ctx.radio, &ctx.nodes,
                              fds[i]);
                if (p[N + i].revents & POLLIN)
                    drain(logfds[i], &seen3, &rot);
            }
        }
        if (phase == 0 && seen3 &&
            hold(&t0, HOLD_UP)) {
            // Mesh is up. Rotate node 2 in
            // place, then re-mesh.
            if (!restart_rotated(pids, logfds,
                                 ROT_NODE))
                break;
            p[N + ROT_NODE].fd = logfds[ROT_NODE];
            phase = 1;
            seen3 = 0;
            rot = 0;
            t0 = 0;
            tcheck = 0;
        } else if (phase == 1 && seen3 &&
                   hold(&t0, HOLD_UP)) {
            // Re-meshed. Submit on node 0, wait
            // for all to list it.
            if (submit_workload(0, wid, sizeof(wid))) {
                memcpy(shortid, wid, 8);
                shortid[8] = '\0';
                phase = 2;
                tcheck = 0;
            } else {
                break;
            }
        } else if (phase == 2) {
            ask_lists(has, shortid, &tcheck);
            if (has[0] && has[1] && has[2]) {
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

    if (!done || !rot) {
        fprintf(stderr,
                "concord_rotate: fail phase=%d rot=%d "
                "workload %s on %d %d %d\n",
                phase, rot, shortid, has[0], has[1],
                has[2]);
        return (1);
    }
    return (0);
}
