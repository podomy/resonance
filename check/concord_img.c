#define _GNU_SOURCE
#include "../img/img.h"
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
#define HOLD_RUN 180
#define DEADLINE 600
#define IMG_REF "localhost:8444/concord-test:latest"
#define IMG_ROOTFS "img/rootfs"
#define NIL_UUID "00000000-0000-0000-0000-000000000000"

// concord_img pins the image supply chain end to end:
// mesh three nodes, push the test image into every
// registry from inside its own namespace, read back
// each catalog plus tag list, submit the image on node
// 0, and wait for a running instance event anywhere.
// Same underlay 192.168.100.1/2/3 as concord_three.
// Bundles live under STATEDIR (see sim/sim.h): a nodev
// mount there refuses every device open, so /tmp is
// out regardless of cleanup convenience.

// drain_tun pumps one packet.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd) {
    tun_pump_fd(map, fd, grid, radio, nodes);
}

// drain_log scans one child line. Sets seen3 on peers:3.
// keep tee: child output mirrored for post-mortem.
static void drain_log(int i, int fd, int* seen3,
                      FILE* keep) {
    char buf[2048];
    ssize_t n;

    (void)i;
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = '\0';
    if (strstr(buf, "\"peers\":3") != NULL)
        *seen3 = 1;
    if (keep != NULL)
        fwrite(buf, 1, (size_t)n, keep);
}

// submit_workload runs workload run against node and
// stores the id into out. Returns 1 on success.
static int submit_workload(int node, char* out, size_t n) {
    FILE* fp;
    char cmd[256];
    char buf[1024];
    char id[64];

    snprintf(cmd, sizeof(cmd),
             "XDG_CONFIG_HOME=" STATEDIR "/node%d "
             "./bin/concord workload run " IMG_REF
             " 2>/dev/null",
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

// running_anywhere reports 1 if any journal holds a
// running instance event for wid.
static int running_anywhere(const char* wid) {
    char cmd[512];
    int rc;

    snprintf(cmd, sizeof(cmd),
             "grep -h '%s' " STATEDIR "/node*/concord/"
             "journal.jsonl 2>/dev/null | grep -q "
             "'workload.instance.running'",
             wid);
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

// stocked reports 1 if every node's catalog lists the
// repo and its tags list the tag.
static int stocked(void) {
    char buf[4096];
    int i;

    for (i = 0; i < N; i++) {
        if (!img_catalog(i, buf, sizeof(buf)))
            return (0);
        if (strstr(buf, "concord-test") == NULL)
            return (0);
        if (!img_tags(i, "concord-test", buf,
                      sizeof(buf)))
            return (0);
        if (strstr(buf, "latest") == NULL)
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
    FILE* keep[N];
    char wid[64], shortid[16];
    int i, phase, seen3, done, alive, assigned;
    time_t t0, tcheck, start;

    // Unbuffered so piped logs stream live.
    setvbuf(stdout, NULL, _IONBF, 0);
    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_img: skip no ./bin/concord\n");
        return (0);
    }
    if (access(IMG_ROOTFS "/probe", X_OK) != 0) {
        printf("concord_img: skip no test image\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_img: skip (%s)\n",
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
        char path[64];

        snprintf(path, sizeof(path),
                 STATEDIR "/check-node%d.log", i);
        keep[i] = fopen(path, "w");
    }

    for (i = 0; i < N; i++) {
        p[i].fd = fds[i];
        p[i].events = POLLIN;
        p[N + i].fd = logfds[i];
        p[N + i].events = POLLIN;
    }

    phase = 0;
    seen3 = 0;
    done = 0;
    t0 = 0;
    tcheck = 0;
    wid[0] = '\0';
    shortid[0] = '\0';
    assigned = 0;
    start = time(NULL);
    while (time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * N, 100) > 0) {
            for (i = 0; i < N; i++) {
                if (p[i].revents & POLLIN)
                    drain_tun(&map, &ctx.grid, &ctx.radio,
                              &ctx.nodes, fds[i]);
                if (p[N + i].revents & POLLIN)
                    drain_log(i, logfds[i], &seen3,
                              keep[i]);
            }
        }
        if (phase == 0 && seen3 &&
            hold(&t0, HOLD_UP)) {
            // Mesh is up. Push into every registry
            // from inside its own namespace.
            for (i = 0; i < N; i++) {
                if (!img_push(i, "concord-test",
                              "latest", IMG_ROOTFS))
                    break;
            }
            if (i < N)
                break;
            phase = 1;
            t0 = 0;
            tcheck = 0;
        } else if (phase == 1 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 5)) {
            // Read back every catalog plus tag list,
            // then submit the image on node 0.
            tcheck = time(NULL);
            if (!stocked())
                continue;
            if (submit_workload(0, wid, sizeof(wid))) {
                memcpy(shortid, wid, 8);
                shortid[8] = '\0';
                phase = 2;
                t0 = 0;
                tcheck = 0;
            } else {
                break;
            }
        } else if (phase == 2 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Submitted and assigned. Wait for a running
            // instance anywhere. Backstop reports a
            // container that never starts.
            tcheck = time(NULL);
            if (!assigned_anywhere(wid))
                continue;
            assigned = 1;
            if (running_anywhere(wid)) {
                done = 1;
                break;
            }
            if (hold(&t0, HOLD_RUN))
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
        if (keep[i] != NULL)
            fclose(keep[i]);
    }
    context_free(&ctx);
    sim_netns_teardown(N);

    if (!done) {
        // Failure forensics before cleanup: the
        // daemon-side errors that decided the run.
        if (system("grep -h '\"level\":\"error\"' "
                   STATEDIR "/check-node*.log "
                   "2>/dev/null | tail -n 15") != 0) {
            // Best effort, logs may be absent.
        }
        fprintf(stderr,
                "concord_img: fail phase=%d assigned=%d "
                "workload %s\n",
                phase, assigned, shortid);
        if (system("rm -rf " STATEDIR) != 0) {
            // Best effort cleanup, teardown ran.
        }
        return (1);
    }
    if (system("rm -rf " STATEDIR) != 0) {
        // Best effort cleanup, teardown already ran.
    }
    return (0);
}
