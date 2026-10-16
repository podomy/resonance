#define _GNU_SOURCE
#include "img/img.h"
#include "shared/context.h"
#include "sim/sim.h"
#include "tun/tun.h"
#include "world/world.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/bpf.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
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

// img smoke: mesh three nodes, push the test image into
// every registry from inside its own namespace, read back
// each catalog plus tag list, submit the image on node 0,
// watch for a running instance event. Proves the supply
// chain end to end: build, push, query, pull, start.
// Child logs stream live; the verdict carries the
// analysis.

// drain_tun pumps one packet.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd) {
    tun_pump_fd(map, fd, grid, radio, nodes);
}

// drain_log prints one child line. Sets seen3 on peers:3.
static void drain_log(int i, int fd, int* seen3) {
    char buf[2048];
    ssize_t n;

    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = '\0';
    printf("\033[%dm[node %d]\033[0m %s", 36 + i, i, buf);
    if (strstr(buf, "\"peers\":3") != NULL)
        *seen3 = 1;
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

// bpf_progs prints the device program ids attached at
// path plus its concord child if present. Answers whether
// an ancestor carries a device verdict above the
// container's own program.
static void bpf_progs(const char* wid) {
    const char* dirs[3];
    char sub[256];
    int i;

    dirs[0] = "/sys/fs/cgroup";
    dirs[1] = "/sys/fs/cgroup/concord";
    snprintf(sub, sizeof(sub),
             "/sys/fs/cgroup/concord/%s", wid);
    dirs[2] = sub;
    for (i = 0; i < 3; i++) {
        int fd;
        union bpf_attr attr;
        __u32 ids[8];
        int r;

        fd = open(dirs[i], O_RDONLY | O_DIRECTORY);
        if (fd < 0) {
            printf("[bpf] %s: no dir\n", dirs[i]);
            continue;
        }
        memset(&attr, 0, sizeof(attr));
        attr.query.target_fd = fd;
        attr.query.attach_type = BPF_CGROUP_DEVICE;
        attr.query.prog_cnt = 8;
        attr.query.prog_ids = (__u64)(uintptr_t)ids;
        r = syscall(__NR_bpf, BPF_PROG_QUERY, &attr,
                    sizeof(attr));
        if (r != 0) {
            printf("[bpf] %s: query errno %d\n", dirs[i],
                   errno);
        } else {
            __u32 k;

            printf("[bpf] %s: %u progs", dirs[i],
                   attr.query.prog_cnt);
            for (k = 0; k < attr.query.prog_cnt; k++)
                printf(" %u", ids[k]);
            printf("\n");
        }
        close(fd);
    }
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
// repo and its tags list the tag. Prints what it sees.
static int stocked(void) {
    char buf[4096];
    int i;

    for (i = 0; i < N; i++) {
        if (!img_catalog(i, buf, sizeof(buf)))
            return (0);
        printf("[seed node %d] catalog %s\n", i, buf);
        if (strstr(buf, "concord-test") == NULL)
            return (0);
        if (!img_tags(i, "concord-test", buf,
                      sizeof(buf)))
            return (0);
        printf("[seed node %d] tags %s\n", i, buf);
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
    char wid[64], shortid[16];
    int i, phase, seen3, done, alive;
    time_t t0, tcheck, start;

    // Unbuffered so piped logs stream live.
    setvbuf(stdout, NULL, _IONBF, 0);
    setenv("IMG_DEBUG", "1", 1);
    if (access("./bin/concord", X_OK) != 0) {
        printf("resonance: skip no ./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("resonance: skip (%s)\n",
               strerror(errno));
        return (0);
    }
    if (!sim_nodes_add(&ctx, &map, fds, N))
        return (1);
    if (!sim_addrs_up(N))
        return (1);
    if (!sim_spawn_concord_debug(pids, logfds, N))
        return (1);

    for (i = 0; i < N; i++) {
        p[i].fd = fds[i];
        p[i].events = POLLIN;
        p[N + i].fd = logfds[i];
        p[N + i].events = POLLIN;
    }
    printf("resonance: 3 nodes ready - img smoke\n");
    printf("resonance: daemons %d %d %d\n", (int)pids[0],
           (int)pids[1], (int)pids[2]);

    phase = 0;
    seen3 = 0;
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
                    drain_tun(&map, &ctx.grid, &ctx.radio,
                              &ctx.nodes, fds[i]);
                if (p[N + i].revents & POLLIN)
                    drain_log(i, logfds[i], &seen3);
            }
        }
        if (phase == 0 && seen3 &&
            hold(&t0, HOLD_UP)) {
            // Mesh is up. Report cgroup membership for
            // placement forensics, then push into every
            // registry from inside its own namespace.
            if (system("for f in "
                       "/sys/fs/cgroup/resonance/node*/"
                       "cgroup.procs /sys/fs/cgroup/"
                       "resonance/node*/daemon/"
                       "cgroup.procs; do echo \"== $f\"; "
                       "for p in $(cat $f 2>/dev/null); "
                       "do tr '\\0' ' ' < /proc/$p/cmdline"
                       " 2>/dev/null; echo; done; done") !=
                0) {
                // Best effort forensics.
            }
            for (i = 0; i < N; i++) {
                if (!img_push(i, "concord-test",
                              "latest", IMG_ROOTFS)) {
                    printf("resonance: push node %d "
                           "failed\n",
                           i);
                    break;
                }
            }
            if (i < N)
                break;
            printf("resonance: pushed all nodes\n");
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
                printf("resonance: workload %s on node 0\n",
                       wid);
                phase = 2;
                t0 = 0;
                tcheck = 0;
            } else {
                printf("resonance: submit failed\n");
                break;
            }
        } else if (phase == 2 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Submitted and assigned. Wait for a running
            // instance anywhere. Backstop reports a
            // container that never starts.
            tcheck = time(NULL);
            bpf_progs(wid);
            if (system("echo -n 'cgwatch '; cat "
                       "/sys/fs/cgroup/resonance/node*/"
                       "cgroup.procs 2>/dev/null | tr "
                       "'\\n' ' '; echo") != 0) {
                // Best effort watch.
            }
            if (!assigned_anywhere(wid))
                continue;
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
    }
    // Post-mortem: daemons are dead, so anyone left in a
    // node cgroup outlived teardown and explains a
    // non-empty creation target.
    if (system("for f in "
               "/sys/fs/cgroup/resonance/node*/"
               "cgroup.procs; do echo \"== $f\"; "
               "for p in $(cat $f 2>/dev/null); "
               "do tr '\\0' ' ' < /proc/$p/cmdline"
               " 2>/dev/null; echo; done; done") != 0) {
        // Best effort forensics.
    }
    context_free(&ctx);
    sim_netns_teardown(N);
    if (system("rm -rf " STATEDIR) != 0) {
        // Best effort cleanup, teardown already ran.
    }

    if (done) {
        printf("resonance: supply chain works, instance "
               "running\n");
        return (0);
    }
    printf("resonance: FAIL phase=%d workload %s\n",
           phase, shortid);
    return (1);
}
