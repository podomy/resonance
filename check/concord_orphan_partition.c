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
#define DEADLINE 300
#define IMAGE "docker.io/library/nginx:alpine"
#define NIL_UUID "00000000-0000-0000-000000000000"

// concord_orphan_partition asserts a workload assigned
// before a three-way split is orphaned on every
// non-owner side, and all nodes converge on the max
// epoch copy after reunion. Same underlay
// 192.168.100.1/2/3 as concord_three.

// drain_tun pumps one packet, or discards if isolated.
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

// copies stores node i's sorted unique assignment
// pairs for wid, one per line. Journal lines carry
// both fields adjacently. Returns 1 on success.
static int copies(int node, const char* wid, char* out,
                  size_t n) {
    FILE* fp;
    char cmd[512];
    size_t len;
    size_t r;

    snprintf(cmd, sizeof(cmd),
             "grep '%s' /tmp/resonance/node%d/concord/"
             "journal.jsonl 2>/dev/null | grep -o "
             "'\"AssignedNodeID\":\"[^\"]*\","
             "\"AssignmentEpoch\":[0-9][0-9]*' | "
             "sort -u",
             wid, node);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (0);
    len = 0;
    while ((r = fread(out + len, 1, n - len - 1,
                      fp)) > 0) {
        len += r;
        if (len >= n - 1)
            break;
    }
    pclose(fp);
    out[len] = '\0';
    return (1);
}

// set_max_epoch returns the highest AssignmentEpoch in
// a copies set, or -1 when the set holds no pairs.
static long set_max_epoch(const char* set) {
    const char* t;
    long best;
    long e;

    best = -1;
    t = set;
    while ((t = strstr(t, "\"AssignmentEpoch\":")) !=
           NULL) {
        t += strlen("\"AssignmentEpoch\":");
        if (sscanf(t, "%ld", &e) != 1)
            break;
        if (e > best)
            best = e;
    }
    return (best);
}

// author_assigned reports 1 when node i's journal holds
// a self-authored non-nil assignment for wid: this lone
// side acted independently. Synced copies keep their
// author, so reunion cannot fake this. Any epoch counts;
// the first post-split tick may name a soon-dead peer
// before the side settles on itself.
static int author_assigned(int node, const char* wid) {
    char cmd[768];
    char self[64];
    int rc;

    if (!node_id(node, self, sizeof(self)))
        return (0);
    snprintf(cmd, sizeof(cmd),
             "grep '%s' /tmp/resonance/node%d/concord/"
             "journal.jsonl 2>/dev/null | grep "
             "'\"node_id\":\"%s\"' | grep -o "
             "'\"AssignedNodeID\":\"[^\"]*\"' | "
             "grep -v '%s' | grep -q .",
             wid, node, self, NIL_UUID);
    rc = system(cmd);
    return (rc == 0);
}

// sides_authored reports 1 when every non-owner side
// authored an assignment for wid.
static int sides_authored(int owner, const char* wid) {
    int i;

    for (i = 0; i < N; i++) {
        if (i == owner)
            continue;
        if (!author_assigned(i, wid))
            return (0);
    }
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
    char sets[N][1024];
    int i, phase, seen3, done, alive, owner;
    int agree, epochok;
    time_t t0, tcheck, start;

    // Unbuffered so piped logs stream live.
    setvbuf(stdout, NULL, _IONBF, 0);
    if (access("./bin/concord", X_OK) != 0) {
    printf("concord_orphan_partition: skip no "
           "./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_orphan_partition: skip (%s)\n",
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
        sets[i][0] = '\0';
    }
    phase = 0;
    seen3 = 0;
    done = 0;
    owner = -1;
    agree = 0;
    epochok = 0;
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
                              fds[i], phase == 2);
                if (p[N + i].revents & POLLIN)
                    drain_log(i, logfds[i], &seen3);
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
            // first assignment, then split all
            // three apart.
            ask_lists(has, shortid, &tcheck);
            if (!(has[0] && has[1] && has[2]))
                continue;
            if (!assigned_anywhere(wid))
                continue;
            owner = owner_index(wid);
            if (owner < 0)
                continue;
            phase = 2;
            seen3 = 0;
            t0 = 0;
            tcheck = 0;
        } else if (phase == 2 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Heal once every non-owner side authored
            // an assignment. The blind timer fails
            // instead: without both sides the verdict
            // is weak.
            tcheck = time(NULL);
            if (sides_authored(owner, wid)) {
                phase = 3;
                seen3 = 0;
                t0 = 0;
                tcheck = 0;
            } else if (hold(&t0, HOLD_SPLIT)) {
                break;
            }
        } else if (phase == 3 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Healed. Wait for re-mesh plus identical
            // assignment sets everywhere peaking above
            // epoch 0. Same inputs project to one copy
            // under the deterministic view rule; the peak
            // value itself is timing-dependent.
            tcheck = time(NULL);
            agree = 1;
            for (i = 0; i < N; i++) {
                if (!copies(i, wid, sets[i],
                            sizeof(sets[i])))
                    agree = 0;
            }
            for (i = 0; i < N; i++) {
                if (sets[i][0] == '\0' ||
                    strcmp(sets[i], sets[0]) != 0)
                    agree = 0;
            }
            epochok = agree &&
                      set_max_epoch(sets[0]) >= 1;
            if (seen3 && agree && epochok) {
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
        if (!copies(i, wid, sets[i],
                    sizeof(sets[i])))
            sets[i][0] = '\0';
    }
    for (i = 0; i < N; i++) {
        kill(pids[i], SIGTERM);
        waitpid(pids[i], NULL, 0);
        close(fds[i]);
        close(logfds[i]);
    }
    context_free(&ctx);
    sim_netns_teardown(N);
    if (!done) {
        // Dump journals before cleanup: post-mortem
        // epoch evidence.
        if (system("cat "
                   "/tmp/resonance/node*/concord/"
                   "journal.jsonl 2>/dev/null") != 0) {
            // Best effort debug dump.
        }
    }
    if (system("rm -rf /tmp/resonance") != 0) {
        // Best effort cleanup, teardown already ran.
    }

    if (!done) {
        fprintf(stderr,
                "concord_orphan_partition: fail phase=%d "
                "owner=%d maxepochs=%ld/%ld/%ld "
                "agree=%d epochok=%d workload %s on "
                "%d %d %d\n",
                phase, owner, set_max_epoch(sets[0]),
                set_max_epoch(sets[1]),
                set_max_epoch(sets[2]), agree,
                epochok, shortid, has[0], has[1],
                has[2]);
        return (1);
    }
    return (0);
}
