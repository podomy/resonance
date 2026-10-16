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
#define HOLD_EVICT 90
#define DEADLINE 300
#define IMAGE "docker.io/library/nginx:alpine"
#define NIL_UUID "00000000-0000-0000-0000-000000000000"

// concord_preempt asserts guaranteed workloads displace
// best-effort ones: a guaranteed arrival that fits
// nowhere evicts a best-effort workload, written as an
// unassignment at epoch 1 or above. Sizes derive from
// host MemAvailable, the gossip free baseline. Same
// underlay 192.168.100.1/2/3 as concord_three.

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
             "XDG_CONFIG_HOME=" STATEDIR "/node%d "
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
// NULL, qos a --qos value or NULL. Returns 1 on success.
static int submit_workload(int node, char* out, size_t n,
                           const char* mem,
                           const char* qos) {
    FILE* fp;
    char cmd[512];
    char buf[1024];
    char id[64];
    char memflag[128];
    char qosflag[128];

    memflag[0] = '\0';
    qosflag[0] = '\0';
    if (mem != NULL) {
        snprintf(memflag, sizeof(memflag),
                 "--memory %s ", mem);
    }
    if (qos != NULL) {
        snprintf(qosflag, sizeof(qosflag),
                 "--qos %s ", qos);
    }
    snprintf(cmd, sizeof(cmd),
             "XDG_CONFIG_HOME=" STATEDIR "/node%d "
             "./bin/concord workload run %s%s" IMAGE
             " 2>/dev/null",
             node, memflag, qosflag);
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
             "grep '%s' " STATEDIR "/node%d/concord/"
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
             "grep '%s' " STATEDIR "/node%d/concord/"
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
             "grep -h '%s' " STATEDIR "/node*/concord/"
             "journal.jsonl 2>/dev/null | grep -o "
             "'\"AssignedNodeID\":\"[^\"]*\"' | "
             "grep -v '%s' | grep -q .",
             wid, NIL_UUID);
    rc = system(cmd);
    return (rc == 0);
}

// preempted reports 1 if any journal holds wid
// unassigned at epoch 1 or above: a displacement
// fired somewhere and synced.
static int preempted(const char* wid) {
    char seg[64];
    unsigned long long e;
    int i;

    for (i = 0; i < N; i++) {
        if (!read_assignment(i, wid, seg, sizeof(seg)))
            continue;
        if (strcmp(seg, NIL_UUID) != 0)
            continue;
        if (!read_epoch(i, wid, &e))
            continue;
        if (e >= 1)
            return (1);
    }
    return (0);
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

// host_avail_mb stores host MemAvailable megabytes, the
// same baseline the gossip sampler reports free from.
// Returns 1 on success.
static int host_avail_mb(unsigned long long* out) {
    FILE* fp;
    char line[256];
    unsigned long long kb;

    fp = fopen("/proc/meminfo", "r");
    if (fp == NULL)
        return (0);
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "MemAvailable: %llu kB",
                   &kb) == 1) {
            fclose(fp);
            *out = kb / 1024ULL;
            return (*out > 0);
        }
    }
    fclose(fp);
    return (0);
}

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    int has[N];
    char wbe[64], wg[64], sbe[16], sg[16];
    char memb[32], memg[32];
    unsigned long long free, bem, gm;
    int i, phase, seen3, done, alive, evicted, gfit;
    time_t t0, tcheck, start;

    // Unbuffered so piped logs stream live.
    setvbuf(stdout, NULL, _IONBF, 0);
    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_preempt: skip no "
               "./bin/concord\n");
        return (0);
    }
    if (!host_avail_mb(&free)) {
        printf("concord_preempt: skip no meminfo\n");
        return (0);
    }
    bem = free * 60 / 100;
    gm = free * 120 / 100;
    if (bem == 0 || gm == 0) {
        printf("concord_preempt: skip tiny host\n");
        return (0);
    }
    snprintf(memb, sizeof(memb), "%llu", bem);
    snprintf(memg, sizeof(memg), "%llu", gm);
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_preempt: skip (%s)\n",
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
    done = 0;
    evicted = 0;
    gfit = 0;
    t0 = 0;
    tcheck = 0;
    wbe[0] = '\0';
    wg[0] = '\0';
    sbe[0] = '\0';
    sg[0] = '\0';
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
            // Mesh is up. Submit best-effort BE at
            // 40% of free, wait for assignment.
            if (submit_workload(0, wbe, sizeof(wbe),
                                memb, "besteffort")) {
                memcpy(sbe, wbe, 8);
                sbe[8] = '\0';

                phase = 1;
                tcheck = 0;
            } else {
                break;
            }
        } else if (phase == 1 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // BE everywhere and assigned: submit
            // guaranteed G at 120% of free. G fits
            // nowhere, so BE must give way.
            ask_lists(has, sbe, &tcheck);
            if (!(has[0] && has[1] && has[2]))
                continue;
            if (!assigned_anywhere(wbe))
                continue;
            if (submit_workload(0, wg, sizeof(wg), memg,
                                "guaranteed")) {
                memcpy(sg, wg, 8);
                sg[8] = '\0';

                phase = 2;
                t0 = 0;
                tcheck = 0;
            } else {
                break;
            }
        } else if (phase == 2 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Watch for BE displacement. Backstop
            // reports a scheduler that never yields.
            tcheck = time(NULL);
            if (preempted(wbe)) {
                evicted = 1;
                gfit = assigned_anywhere(wg);
                done = 1;
                break;
            }
            if (hold(&t0, HOLD_EVICT))
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

    for (i = 0; i < N; i++) {
        kill(pids[i], SIGTERM);
        waitpid(pids[i], NULL, 0);
        close(fds[i]);
        close(logfds[i]);
    }
    context_free(&ctx);
    sim_netns_teardown(N);
    if (system("rm -rf " STATEDIR) != 0) {
        // Best effort cleanup, teardown already ran.
    }

    if (!done) {
        fprintf(stderr,
                "concord_preempt: fail phase=%d evicted=%d "
                "gfit=%d BE %s G %s\n",
                phase, evicted, gfit, sbe, sg);
        return (1);
    }
    return (0);
}
