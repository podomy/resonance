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
#define HOLD_SPLIT 90
#define DEADLINE 240
#define MAX_TRIES 3
#define IMAGE "docker.io/library/nginx:alpine"
#define NIL_UUID "00000000-0000-0000-0000-000000000000"

// concord_duel asserts two scheduler leaders, split
// apart, assign the same workload, and every node
// converges on one assignee after reunion.
// Same underlay 192.168.100.1/2/3 as concord_three.

// drain_tun pumps one packet, or discards if isolated.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd, int drop) {
    char junk[2048];

    if (drop) {
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

// count_assignees reports how many distinct non-nil
// SegmentIDs were ever recorded for wid cluster-wide.
static int count_assignees(const char* wid) {
    FILE* fp;
    char cmd[512];
    char buf[64];
    int k;

    snprintf(cmd, sizeof(cmd),
             "grep -h '%s' /tmp/resonance/node*/concord/"
             "journal.jsonl 2>/dev/null | grep -o "
             "'\"SegmentID\":\"[^\"]*\"' | grep -v '%s' | "
             "sort -u | wc -l",
             wid, NIL_UUID);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (-1);
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return (-1);
    }
    pclose(fp);
    if (sscanf(buf, "%d", &k) != 1)
        return (-1);
    return (k);
}

// assigned_anywhere reports 1 if any journal holds a
// non-nil SegmentID for wid. A split only duels while
// the spec is still unassigned everywhere.
static int assigned_anywhere(const char* wid) {
    char cmd[512];
    int rc;

    snprintf(cmd, sizeof(cmd),
             "grep -h '%s' /tmp/resonance/node*/concord/"
             "journal.jsonl 2>/dev/null | grep -o "
             "'\"SegmentID\":\"[^\"]*\"' | grep -v '%s' | "
             "grep -q .",
             wid, NIL_UUID);
    rc = system(cmd);
    return (rc == 0);
}

// sides_assigned reports 1 when the victim journal
// and at least one pair journal each hold a non-nil
// SegmentID for wid: both sides visibly assigned.
static int sides_assigned(int victim, const char* wid) {
    char seg[64];
    int i;

    if (!read_assignment(victim, wid, seg, sizeof(seg)) ||
        strcmp(seg, NIL_UUID) == 0)
        return (0);
    for (i = 0; i < N; i++) {
        if (i == victim)
            continue;
        if (read_assignment(i, wid, seg, sizeof(seg)) &&
            strcmp(seg, NIL_UUID) != 0)
            return (1);
    }
    return (0);
}

// elect_victim returns the node with the lowest UUID
// string from the spawned configs. That node leads
// every view it belongs to.
static int elect_victim(void) {
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
    int has[N];
    char wid[64], shortid[16];
    char segs[N][64];
    int i, victim, phase, seen3, alive, tries, duel;
    time_t t0, tcheck, start;

    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_duel: skip no ./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_duel: skip (%s)\n",
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
        segs[i][0] = '\0';
    }

    victim = 2;
    phase = 0;
    seen3 = 0;
    tries = 0;
    duel = -1;
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
                              &ctx.nodes, fds[i],
                              phase == 2 && i == victim);
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
            // Submit on node 0, wait for all to list it.
            if (submit_workload(0, wid, sizeof(wid))) {
                memcpy(shortid, wid, 8);
                shortid[8] = '\0';
                tries++;
                phase = 1;
                tcheck = 0;
            } else {
                break;
            }
        } else if (phase == 1) {
            // Split only while the spec is present
            // everywhere and still unassigned. If the
            // scheduler got there first, resubmit fresh
            // and try again.
            ask_lists(has, shortid, &tcheck);
            if (has[0] && has[1] && has[2]) {
                if (!assigned_anywhere(wid)) {
                    victim = elect_victim();
                    phase = 2;
                    t0 = 0;
                } else if (tries >= MAX_TRIES) {
                    break;
                } else if (submit_workload(0, wid,
                                           sizeof(wid))) {
                    memcpy(shortid, wid, 8);
                    shortid[8] = '\0';
                    tries++;
                    for (i = 0; i < N; i++)
                        has[i] = 0;
                    tcheck = 0;
                } else {
                    break;
                }
            }
        } else if (phase == 2 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Restore once both sides visibly assigned;
            // the blind timer made this a lottery. The
            // hold is only a backstop: without real
            // divergence the verdict still fails.
            tcheck = time(NULL);
            if (sides_assigned(victim, wid) ||
                hold(&t0, HOLD_SPLIT)) {
                phase = 3;
                seen3 = 0;
                t0 = 0;
                tcheck = 0;
            }
        } else if (phase == 3 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Read every node's latest assignment.
            int agree;
            tcheck = time(NULL);
            agree = 1;
            for (i = 0; i < N; i++) {
                if (!read_assignment(i, wid, segs[i],
                                     sizeof(segs[i])) ||
                    strcmp(segs[i], NIL_UUID) == 0) {
                    agree = 0;
                    break;
                }
                if (strcmp(segs[i], segs[0]) != 0)
                    agree = 0;
            }
            if (agree)
                break;
        }
        alive = 0;
        for (i = 0; i < N; i++) {
            if (waitpid(pids[i], NULL, WNOHANG) == 0)
                alive = 1;
        }
        if (!alive)
            break;
    }

    duel = count_assignees(wid);
    for (i = 0; i < N; i++) {
        kill(pids[i], SIGTERM);
        waitpid(pids[i], NULL, 0);
        close(fds[i]);
        close(logfds[i]);
    }
    context_free(&ctx);
    sim_netns_teardown(N);
    system("rm -rf /tmp/resonance");

    if (phase != 3 || duel < 2) {
        fprintf(stderr,
                "concord_duel: fail phase=%d duel=%d "
                "%s=%s/%s/%s\n",
                phase, duel, shortid, segs[0], segs[1],
                segs[2]);
        return (1);
    }
    return (0);
}
