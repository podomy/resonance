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
#define DEADLINE 300
#define IMAGE "docker.io/library/nginx:alpine"
#define NEW_IMAGE "docker.io/library/redis:alpine"
#define NIL_UUID "00000000-0000-0000-0000-000000000000"

// concord_update asserts workload update moves a live
// workload to a new image keeping its identity: same ID
// and assignment converge on all nodes at epoch 1 with
// the new image. Same underlay 192.168.100.1/2/3 as
// concord_three.

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

// update_workload runs workload update against node.
// Returns 1 if the CLI exits zero.
static int update_workload(int node, const char* wid,
                           const char* image) {
    FILE* fp;
    char cmd[512];
    char buf[1024];
    int rc;

    snprintf(cmd, sizeof(cmd),
             "XDG_CONFIG_HOME=/tmp/resonance/node%d "
             "./bin/concord workload update --image %s "
             "%s 2>&1",
             node, image, wid);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (0);
    while (fgets(buf, sizeof(buf), fp) != NULL)
        continue;
    rc = pclose(fp);
    return (rc == 0);
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

// read_image stores the latest Image recorded for wid
// in node i's journal. Returns 1 on success.
static int read_image(int node, const char* wid,
                      char* out, size_t n) {
    FILE* fp;
    char cmd[512];
    char buf[256];
    char img[192];

    snprintf(cmd, sizeof(cmd),
             "grep '%s' /tmp/resonance/node%d/concord/"
             "journal.jsonl 2>/dev/null | grep -o "
             "'\"Image\":\"[^\"]*\"' | tail -1",
             wid, node);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (0);
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return (0);
    }
    pclose(fp);
    // Output is "Image":"<ref>".
    if (sscanf(buf, "\"Image\":\"%191[^\"]\"", img) != 1)
        return (0);
    if (strlen(img) + 1 > n)
        return (0);
    strcpy(out, img);
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

// read_state stores every node's latest assignment,
// epoch, and image for wid. Missing reads stay nil,
// zero, and empty.
static void read_state(const char* wid, char segs[N][64],
                       unsigned long long epochs[N],
                       char imgs[N][192]) {
    int i;

    for (i = 0; i < N; i++) {
        if (!read_assignment(i, wid, segs[i],
                             sizeof(segs[i])))
            strcpy(segs[i], NIL_UUID);
        if (!read_epoch(i, wid, &epochs[i]))
            epochs[i] = 0;
        if (!read_image(i, wid, imgs[i],
                        sizeof(imgs[i])))
            imgs[i][0] = '\0';
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
    char imgs[N][192];
    char firstseg[64];
    unsigned long long epochs[N];
    int i, phase, seen3, done, alive;
    int agree;
    time_t t0, tcheck, start;

    // Unbuffered so piped logs stream live.
    setvbuf(stdout, NULL, _IONBF, 0);
    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_update: skip no "
               "./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_update: skip (%s)\n",
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
        imgs[i][0] = '\0';
    }
    firstseg[0] = '\0';
    phase = 0;
    seen3 = 0;
    done = 0;
    agree = 0;
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
                              &ctx.nodes, fds[i]);
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
            // Spec everywhere plus first assignment:
            // record the assignee, then bump image.
            ask_lists(has, shortid, &tcheck);
            if (!(has[0] && has[1] && has[2]))
                continue;
            if (!assigned_anywhere(wid))
                continue;
            if (!read_assignment(0, wid, firstseg,
                                 sizeof(firstseg)))
                continue;
            if (strcmp(firstseg, NIL_UUID) == 0)
                continue;
            if (!update_workload(0, wid, NEW_IMAGE))
                break;
            phase = 2;
            tcheck = 0;
        } else if (phase == 2 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Same ID and assignee everywhere, epoch 1,
            // new image on all nodes.
            tcheck = time(NULL);
            read_state(wid, segs, epochs, imgs);
            agree = 1;
            for (i = 0; i < N; i++) {
                if (strcmp(segs[i], firstseg) != 0)
                    agree = 0;
                if (epochs[i] != 1)
                    agree = 0;
                if (strcmp(imgs[i], NEW_IMAGE) != 0)
                    agree = 0;
            }
            if (agree) {
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

    if (!done)
        read_state(wid, segs, epochs, imgs);
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
                "concord_update: fail phase=%d "
                "segs=%s/%s/%s "
                "epochs=%llu/%llu/%llu agree=%d "
                "workload %s on %d %d %d\n",
                phase, segs[0], segs[1], segs[2],
                epochs[0], epochs[1], epochs[2], agree,
                shortid, has[0], has[1], has[2]);
        return (1);
    }
    return (0);
}
