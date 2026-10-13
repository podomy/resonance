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
#define HOLD_EMPTY 30
#define DEADLINE 300
#define IMAGE "docker.io/library/nginx:alpine"
#define HUGE_MEM "999999999999"
#define NIL_UUID "00000000-0000-0000-0000-000000000000"

// concord_nomem asserts a workload whose memory request
// fits no node stays unassigned on every node while a
// plain workload assigns and converges. Refusal must
// never strand healthy scheduling. Same underlay
// 192.168.100.1/2/3 as concord_three.

// drain_tun pumps one packet.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd) {
    tun_pump_fd(map, fd, grid, radio, nodes);
}

// drain_log scans one child line. Sets seen3 on peers:3.
static void drain_log(int i, int fd, int* seen3) {
    char buf[2048];
    ssize_t n;

    (void)i;
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = '\0';
    if (strstr(buf, "\"peers\":3") != NULL)
        *seen3 = 1;
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
// stores the id into out. mem is a --memory value or
// NULL for none. Returns 1 on success.
static int submit_workload(int node, char* out, size_t n,
                           const char* mem) {
    FILE* fp;
    char cmd[512];
    char buf[1024];
    char id[64];

    if (mem != NULL) {
        snprintf(cmd, sizeof(cmd),
                 "XDG_CONFIG_HOME=/tmp/resonance/node%d "
                 "./bin/concord workload run --memory %s "
                 IMAGE " 2>/dev/null",
                 node, mem);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "XDG_CONFIG_HOME=/tmp/resonance/node%d "
                 "./bin/concord workload run " IMAGE
                 " 2>/dev/null",
                 node);
    }
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

// assigned_anywhere reports 1 if any journal holds a
// non-nil AssignedNodeID for wid.
static int assigned_anywhere(const char* wid) {
    char cmd[512];
    int rc;

    snprintf(cmd, sizeof(cmd),
             "grep -h '%s' /tmp/resonance/node*/concord/"
             "journal.jsonl 2>/dev/null | grep -o "
             "'\"AssignedNodeID\":\"[^\"]*\"' | "
             "grep -v '%s' | grep -q .",
             wid, NIL_UUID);
    rc = system(cmd);
    return (rc == 0);
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

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    int hasa[N], hasb[N];
    char wa[64], wb[64], sa[16], sb[16];
    int i, phase, seen3, done, alive, aleak;
    time_t t0, tchecka, tcheckb, start;

    // Unbuffered so piped logs stream live.
    setvbuf(stdout, NULL, _IONBF, 0);
    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_nomem: skip no "
               "./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_nomem: skip (%s)\n",
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
        hasa[i] = 0;
        hasb[i] = 0;
    }
    phase = 0;
    seen3 = 0;
    done = 0;
    aleak = 0;
    t0 = 0;
    tchecka = 0;
    tcheckb = 0;
    wa[0] = '\0';
    wb[0] = '\0';
    sa[0] = '\0';
    sb[0] = '\0';
    start = time(NULL);
    while (time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * N, 100) > 0) {
            for (i = 0; i < N; i++) {
                if (p[i].revents & POLLIN)
                    drain_tun(&map, &ctx.grid, &ctx.radio,
                              &ctx.nodes, fds[i]);
                if (p[N + i].revents & POLLIN)
                    drain_log(i, logfds[i], &seen3);
            }
        }
        if (phase == 0 && seen3 &&
            hold(&t0, HOLD_UP)) {
            // Mesh is up. Submit A with an unfit
            // memory request, then plain B.
            if (!submit_workload(0, wa, sizeof(wa),
                                 HUGE_MEM))
                break;
            memcpy(sa, wa, 8);
            sa[8] = '\0';

            if (!submit_workload(0, wb, sizeof(wb),
                                 NULL))
                break;
            memcpy(sb, wb, 8);
            sb[8] = '\0';
            phase = 1;
            tchecka = 0;
            tcheckb = 0;
        } else if (phase == 1 &&
                   (tchecka == 0 ||
                    time(NULL) - tchecka >= 2 ||
                    tcheckb == 0 ||
                    time(NULL) - tcheckb >= 2)) {
            // Wait for both specs everywhere plus B
            // assigned. A must stay unassigned.
            ask_lists(hasa, sa, &tchecka);
            ask_lists(hasb, sb, &tcheckb);
            if (!(hasa[0] && hasa[1] && hasa[2]))
                continue;
            if (assigned_anywhere(wa)) {
                aleak = 1;
                break;
            }
            if (!(hasb[0] && hasb[1] && hasb[2]))
                continue;
            if (!assigned_anywhere(wb))
                continue;
            phase = 2;
            t0 = 0;
            tcheckb = 0;
        } else if (phase == 2 &&
                   (tcheckb == 0 ||
                    time(NULL) - tcheckb >= 2)) {
            // B converged? A still empty? Hold the
            // empty watch to the end.
            ask_lists(hasb, sb, &tcheckb);
            if (assigned_anywhere(wa)) {
                aleak = 1;
                break;
            }
            if (hasb[0] && hasb[1] && hasb[2] &&
                hold(&t0, HOLD_EMPTY)) {
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

    if (!done || aleak) {
        fprintf(stderr,
                "concord_nomem: fail phase=%d aleak=%d A "
                "%s %d %d %d B %s %d %d %d\n",
                phase, aleak, sa, hasa[0], hasa[1],
                hasa[2], sb, hasb[0], hasb[1], hasb[2]);
        return (1);
    }
    return (0);
}
