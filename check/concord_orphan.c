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
#define REASSIGN_WAIT 90
#define DEADLINE 300
#define IMAGE "docker.io/library/nginx:alpine"
#define NIL_UUID "00000000-0000-0000-000000000000"

// concord_orphan asserts a workload assigned to a
// node that is then SIGKILLED is reassigned by a
// survivor at epoch 1, and all survivors converge
// on one copy. Same underlay 192.168.100.1/2/3
// as concord_three.

// drain_tun pumps one packet.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd) {
    tun_pump_fd(map, fd, grid, radio, nodes);
}

// drain scans one child log. Sets seen3 on peers:3.
static void drain(int fd, int* seen3) {
    char buf[2048];
    ssize_t n;

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

// read_assignment stores the latest AssignedNodeID
// recorded for wid in node i's journal. Returns 1
// on success.
static int read_assignment(int node, const char* wid,
                           char* out, size_t n) {
    FILE* fp;
    char cmd[512];
    char buf[128];
    char seg[64];

    snprintf(cmd, sizeof(cmd),
             "grep '%s' /tmp/resonance/node%d/concord/"
             "journal.jsonl 2>/dev/null | grep -o "
             "'\"AssignedNodeID\":\"[^\"]*\"' | tail -1",
             wid, node);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (0);
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return (0);
    }
    pclose(fp);
    // Output is "AssignedNodeID":"<uuid>".
    if (sscanf(buf, "\"AssignedNodeID\":\"%63[^\"]\"",
               seg) != 1)
        return (0);
    if (strlen(seg) + 1 > n)
        return (0);
    strcpy(out, seg);
    return (1);
}

// read_epoch stores the latest AssignmentEpoch
// recorded for wid in node i's journal. Returns 1
// on success.
static int read_epoch(int node, const char* wid,
                      unsigned long long* out) {
    FILE* fp;
    char cmd[512];
    char buf[128];
    unsigned long long e;

    snprintf(cmd, sizeof(cmd),
             "grep '%s' /tmp/resonance/node%d/concord/"
             "journal.jsonl 2>/dev/null | grep -o "
             "'\"AssignmentEpoch\":[0-9]*' | tail -1",
             wid, node);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (0);
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return (0);
    }
    pclose(fp);
    // Output is "AssignmentEpoch":<num>.
    if (sscanf(buf, "\"AssignmentEpoch\":%llu", &e) != 1)
        return (0);
    *out = e;
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

// node_id stores node i's own id from its config.
// Returns 1 on success.
static int node_id(int i, char* out, size_t n) {
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
        return (0);
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return (0);
    }
    pclose(fp);
    if (sscanf(buf, "\"id\":\"%63[^\"]\"", id) != 1)
        return (0);
    if (strlen(id) + 1 > n)
        return (0);
    strcpy(out, id);
    return (1);
}

// owner_index returns the node index whose config id
// matches the latest non-nil assignment for wid,
// or -1 when unassigned or unknown.
static int owner_index(const char* wid) {
    char seg[64];
    char id[64];
    int i;

    if (!read_assignment(0, wid, seg, sizeof(seg)))
        return (-1);
    if (strcmp(seg, NIL_UUID) == 0)
        return (-1);
    for (i = 0; i < N; i++) {
        if (!node_id(i, id, sizeof(id)))
            continue;
        if (strcmp(id, seg) == 0)
            return (i);
    }
    return (-1);
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

// read_segs stores every node's latest AssignedNodeID
// and AssignmentEpoch for wid. Missing reads stay
// NIL_UUID and 0.
static void read_segs(const char* wid,
                      char segs[N][64],
                      unsigned long long epochs[N]) {
    int i;

    for (i = 0; i < N; i++) {
        if (!read_assignment(i, wid, segs[i],
                             sizeof(segs[i])))
            strcpy(segs[i], NIL_UUID);
        if (!read_epoch(i, wid, &epochs[i]))
            epochs[i] = 0;
    }
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
    char ownerid[64];
    unsigned long long epochs[N];
    int i, phase, seen3, done, alive, owner;
    int moved, agree, epochok, ref;
    time_t t0, tcheck, start;

    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_orphan: skip no "
               "./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_orphan: skip (%s)\n",
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
        strcpy(segs[i], NIL_UUID);
        epochs[i] = 0;
    }

    phase = 0;
    seen3 = 0;
    done = 0;
    owner = -1;
    moved = 0;
    agree = 0;
    epochok = 0;
    ref = 0;
    t0 = 0;
    tcheck = 0;
    wid[0] = '\0';
    shortid[0] = '\0';
    ownerid[0] = '\0';
    start = time(NULL);
    while (time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * N, 100) > 0) {
            for (i = 0; i < N; i++) {
                if (p[i].revents & POLLIN)
                    drain_tun(&map, &ctx.grid,
                              &ctx.radio, &ctx.nodes,
                              fds[i]);
                if (p[N + i].revents & POLLIN)
                    drain(logfds[i], &seen3);
            }
        }
        if (phase == 0 && seen3 &&
            hold(&t0, HOLD_UP)) {
            // Mesh is up. Submit on node 0, wait
            // for assignment.
            if (submit_workload(0, wid, sizeof(wid))) {
                memcpy(shortid, wid, 8);
                shortid[8] = '\0';
                phase = 1;
                tcheck = 0;
            } else {
                break;
            }
        } else if (phase == 1 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Wait for the spec everywhere plus a
            // first assignment, then SIGKILL the
            // owner. The owner stays dead.
            ask_lists(has, shortid, &tcheck);
            if (!(has[0] && has[1] && has[2]))
                continue;
            if (!assigned_anywhere(wid))
                continue;
            owner = owner_index(wid);
            if (owner < 0)
                continue;
            if (!node_id(owner, ownerid,
                         sizeof(ownerid)))
                break;
            kill(pids[owner], SIGKILL);
            waitpid(pids[owner], NULL, 0);
            phase = 2;
            t0 = 0;
            tcheck = 0;
        } else if (phase == 2 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Watch for a survivor claiming the
            // spec at epoch 1. Backstop reports
            // stuck.
            tcheck = time(NULL);
            read_segs(wid, segs, epochs);
            moved = 0;
            for (i = 0; i < N; i++) {
                if (strcmp(segs[i], NIL_UUID) != 0 &&
                    strcmp(segs[i], ownerid) != 0)
                    moved = 1;
            }
            if (moved) {
                phase = 3;
                tcheck = 0;
            } else if (hold(&t0, REASSIGN_WAIT)) {
                break;
            }
        } else if (phase == 3 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // A survivor claimed it. Wait for one
            // copy on the survivors at epoch 1.
            // The dead owner's frozen journal can
            // never agree; skip it.
            tcheck = time(NULL);
            read_segs(wid, segs, epochs);
            agree = 1;
            epochok = 1;
            ref = (owner == 0) ? 1 : 0;
            for (i = 0; i < N; i++) {
                if (i == owner)
                    continue;
                if (strcmp(segs[i], NIL_UUID) == 0 ||
                    strcmp(segs[i], segs[ref]) != 0)
                    agree = 0;
                if (epochs[i] != 1)
                    epochok = 0;
            }
            if (agree && epochok) {
                done = 1;
                break;
            }
        }
        alive = 0;
        for (i = 0; i < N; i++) {
            if (i == owner && phase >= 2)
                continue;
            if (waitpid(pids[i], NULL, WNOHANG) == 0)
                alive = 1;
        }
        if (!alive)
            break;
    }

    read_segs(wid, segs, epochs);
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
                "concord_orphan: fail phase=%d owner=%d "
                "epochs=%llu/%llu/%llu moved=%d "
                "agree=%d epochok=%d workload %s on "
                "%d %d %d\n",
                phase, owner, epochs[0], epochs[1],
                epochs[2], moved, agree, epochok,
                shortid, has[0], has[1], has[2]);
        return (1);
    }
    return (0);
}
