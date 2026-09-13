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
#define DEADLINE 150
#define IMAGE "docker.io/library/nginx:alpine"
#define NIL_UUID "00000000-0000-0000-0000-000000000000"

// concord_failover asserts a new leader takes over
// scheduling after the leader is SIGKILLed: a workload
// submitted after the death gets assigned and converges
// on both survivors.
// Same underlay 192.168.100.1/2/3 as concord_three.

// drain_tun pumps one packet, or discards if the node
// is dead (leader after SIGKILL).
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd, int i, int dead) {
    char junk[2048];
    ssize_t n;

    if (i == dead) {
        n = read(fd, junk, sizeof(junk));
        (void)n;
        return;
    }
    tun_pump_fd(map, fd, grid, radio, nodes);
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

// read_assignment stores the latest SegmentID recorded
// for wid in node i's journal. Returns 1 on success.
static int read_assignment(int node, const char* wid,
                           char* out, size_t n) {
    FILE* fp;
    char cmd[512];
    char buf[128];
    char seg[64];

    snprintf(cmd, sizeof(cmd),
             "grep '%s' /tmp/resonance/node%d/concord/"
             "journal.jsonl 2>/dev/null | grep -o "
             "'\"SegmentID\":\"[^\"]*\"' | tail -1",
             wid, node);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (0);
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return (0);
    }
    pclose(fp);
    // Output is "SegmentID":"<uuid>".
    if (sscanf(buf, "\"SegmentID\":\"%63[^\"]\"", seg) != 1)
        return (0);
    if (strlen(seg) + 1 > n)
        return (0);
    strcpy(out, seg);
    return (1);
}

// elect_leader returns the node with the lowest UUID
// string from the spawned configs: the node Concord
// necessarily elected.
static int elect_leader(void) {
    char best_id[64];
    int i, best, have;

    best = 0;
    have = 0;
    best_id[0] = '\0';
    for (i = 0; i < N; i++) {
        FILE* fp;
        char cmd[256];
        char buf[128];
        char id[64];

        snprintf(cmd, sizeof(cmd),
                 "grep -o '\"id\":\"[^\"]*\"' "
                 "/tmp/resonance/node%d/concord/"
                 "config.json 2>/dev/null",
                 i);
        fp = popen(cmd, "r");
        if (fp == NULL)
            continue;
        if (fgets(buf, sizeof(buf), fp) == NULL) {
            pclose(fp);
            continue;
        }
        pclose(fp);
        if (sscanf(buf, "\"id\":\"%63[^\"]\"", id) != 1)
            continue;
        if (!have || strcmp(id, best_id) < 0) {
            strcpy(best_id, id);
            best = i;
            have = 1;
        }
    }
    return (best);
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
    int s0, s1, leader;
    int has0, has1;
    char wid[64], shortid[16];
    char seg0[64], seg1[64];
    int i, phase, seen3, alive;
    time_t t0, tcheck, start;

    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_failover: skip no ./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_failover: skip (%s)\n",
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

    leader = -1;
    s0 = -1;
    s1 = -1;
    phase = 0;
    seen3 = 0;
    has0 = 0;
    has1 = 0;
    seg0[0] = '\0';
    seg1[0] = '\0';
    t0 = 0;
    tcheck = 0;
    wid[0] = '\0';
    shortid[0] = '\0';
    start = time(NULL);
    while (time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * N, 100) > 0) {
            for (i = 0; i < N; i++) {
                int dead;

                dead = (phase >= 1) ? leader : -1;
                if (p[i].revents & POLLIN)
                    drain_tun(&map, &ctx.grid, &ctx.radio,
                              &ctx.nodes, fds[i], i, dead);
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
            // SIGKILL the leader outright.
            leader = elect_leader();
            s0 = (leader + 1) % N;
            s1 = (leader + 2) % N;
            kill(pids[leader], SIGKILL);
            waitpid(pids[leader], NULL, 0);
            close(logfds[leader]);
            logfds[leader] = -1;
            p[N + leader].fd = -1;
            phase = 1;
            t0 = 0;
            tcheck = 0;
        } else if (phase == 1 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Survivors re-meshed: submit where the dead
            // leader never saw it.
            tcheck = time(NULL);
            if (nodes_alive(s0) == 2 &&
                nodes_alive(s1) == 2) {
                if (!submit_workload(s0, wid,
                                     sizeof(wid)))
                    break;
                memcpy(shortid, wid, 8);
                shortid[8] = '\0';
                phase = 2;
                tcheck = 0;
            }
        } else if (phase == 2 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // A successor took over iff the fresh
            // workload gets a non-nil assignment that
            // both survivors agree on.
            int agree;
            tcheck = time(NULL);
            has0 = workload_present(s0, shortid);
            has1 = workload_present(s1, shortid);
            agree = read_assignment(s0, wid, seg0,
                                    sizeof(seg0)) &&
                    read_assignment(s1, wid, seg1,
                                    sizeof(seg1)) &&
                    strcmp(seg0, NIL_UUID) != 0 &&
                    strcmp(seg0, seg1) == 0;
            if (agree && has0 && has1)
                break;
        }
        alive = 0;
        for (i = 0; i < N; i++) {
            if (i != leader &&
                waitpid(pids[i], NULL, WNOHANG) == 0)
                alive = 1;
        }
        if (!alive)
            break;
    }

    for (i = 0; i < N; i++) {
        if (i == leader)
            continue;
        kill(pids[i], SIGTERM);
        waitpid(pids[i], NULL, 0);
        close(fds[i]);
        close(logfds[i]);
    }
    if (leader >= 0)
        close(fds[leader]);
    context_free(&ctx);
    sim_netns_teardown(N);
    if (system("rm -rf /tmp/resonance") != 0) {
        // Best effort cleanup, teardown already ran.
    }

    if (has0 && has1 && seg0[0] != '\0' &&
        strcmp(seg0, seg1) == 0 &&
        strcmp(seg0, NIL_UUID) != 0)
        return (0);
    fprintf(stderr,
            "concord_failover: fail phase=%d leader=%d "
            "%s=%s/%s on %d/%d\n",
            phase, leader, shortid, seg0, seg1, has0,
            has1);
    return (1);
}
