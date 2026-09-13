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
#define ROUNDS 3
#define ISOLATE_SECS 12
#define NWL 3
#define MAXW 1024
#define IMAGE "docker.io/library/nginx:alpine"

// concord_spam asserts three beating rounds converge:
// each round submits three workloads, stops the
// previous three, faults two nodes at once (isolate
// one, SIGKILL+restart another), heals, and verifies
// the live triple present and every stopped workload
// absent on all nodes.
// Same underlay 192.168.100.1/2/3 as concord_three.

// drain_tun pumps one packet, or discards if the node
// is currently isolated.
static void drain_tun(TunMap* map, MediumGrid* grid,
                      RadioParams* radio, NodeList* nodes,
                      int fd, int i, int* drop) {
    char junk[2048];
    ssize_t n;

    if (drop[i]) {
        n = read(fd, junk, sizeof(junk));
        (void)n;
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

// stop_workload runs workload stop against node.
// Returns 1 if the CLI reports Stopped.
static int stop_workload(int node, const char* id) {
    FILE* fp;
    char cmd[256];
    char buf[1024];

    snprintf(cmd, sizeof(cmd),
             "XDG_CONFIG_HOME=/tmp/resonance/node%d "
             "./bin/concord workload stop %s 2>/dev/null",
             node, id);
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (0);
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return (0);
    }
    pclose(fp);
    // Output is "Stopped workload <uuid>".
    return (strstr(buf, "Stopped") != NULL);
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

// mesh3 polls node list on every node every 2s,
// reports 1 when all see 3 alive.
static int mesh3(time_t* tcheck) {
    int i;

    if (*tcheck != 0 && time(NULL) - *tcheck < 2)
        return (0);
    *tcheck = time(NULL);
    for (i = 0; i < N; i++) {
        if (nodes_alive(i) != 3)
            return (0);
    }
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

// fetch_lists stores each node's workload list into
// bufs: one CLI round trip per node whatever the
// workload count.
static void fetch_lists(char bufs[N][16384]) {
    int i;

    for (i = 0; i < N; i++) {
        FILE* fp;
        char cmd[256];
        size_t len;
        size_t n;

        snprintf(cmd, sizeof(cmd),
                 "XDG_CONFIG_HOME=/tmp/resonance/node%d "
                 "./bin/concord workload list 2>/dev/null",
                 i);
        fp = popen(cmd, "r");
        if (fp == NULL) {
            bufs[i][0] = '\0';
            continue;
        }
        len = 0;
        while ((n = fread(bufs[i] + len, 1,
                          sizeof(bufs[i]) - len - 1,
                          fp)) > 0) {
            len += n;
            if (len >= sizeof(bufs[i]) - 1)
                break;
        }
        pclose(fp);
        bufs[i][len] = '\0';
    }
}

// live_present reports 1 when all NWL workloads ending
// at nw-1 list on every node.
static int live_present(char shorts[MAXW][16], int nw) {
    char bufs[N][16384];
    int i, k;

    fetch_lists(bufs);
    for (k = nw - NWL; k < nw; k++) {
        for (i = 0; i < N; i++) {
            if (strstr(bufs[i], shorts[k]) == NULL)
                return (0);
        }
    }
    return (1);
}

// stopped_absent reports 1 when all workloads before
// nw-NWL list on no node. Note this is not the
// negation of live_present, which is true when any
// single node lacks a workload.
static int stopped_absent(char shorts[MAXW][16], int nw) {
    char bufs[N][16384];
    int i, k;

    fetch_lists(bufs);
    for (k = 0; k < nw - NWL; k++) {
        for (i = 0; i < N; i++) {
            if (strstr(bufs[i], shorts[k]) != NULL)
                return (0);
        }
    }
    return (1);
}

// verify_round reports 1 when the live triple is
// present everywhere and every stopped workload is
// absent everywhere. On failure it describes the
// first offender in why.
static int verify_round(char shorts[MAXW][16], int nw,
                        char* why, size_t wcap) {
    char bufs[N][16384];
    int i, k;

    fetch_lists(bufs);
    for (k = nw - NWL; k < nw; k++) {
        for (i = 0; i < N; i++) {
            if (strstr(bufs[i], shorts[k]) == NULL) {
                snprintf(why, wcap,
                         "live %s missing on node %d",
                         shorts[k], i);
                return (0);
            }
        }
    }
    for (k = 0; k < nw - NWL; k++) {
        for (i = 0; i < N; i++) {
            if (strstr(bufs[i], shorts[k]) != NULL) {
                snprintf(why, wcap,
                         "stopped %s present on node %d",
                         shorts[k], i);
                return (0);
            }
        }
    }
    return (1);
}

// submit_triple submits NWL workloads back to back on
// node, storing ids. Returns 1 if all landed.
static int submit_triple(int node, char wids[MAXW][64],
                         char shorts[MAXW][16], int nw) {
    int k;

    for (k = 0; k < NWL; k++) {
        if (!submit_workload(node, wids[nw + k],
                             sizeof(wids[nw + k])))
            return (0);
        memcpy(shorts[nw + k], wids[nw + k], 8);
        shorts[nw + k][8] = '\0';
    }
    return (1);
}

// stop_triple stops the NWL workloads ending at nw-1
// on node. Returns 1 if all report Stopped.
static int stop_triple(int node, char wids[MAXW][64],
                       int nw) {
    int k;

    for (k = nw - NWL; k < nw; k++) {
        if (!stop_workload(node, wids[k]))
            return (0);
    }
    return (1);
}

// preserve_journals copies journals and configs
// aside for post-mortem instead of wiping them.
static void preserve_journals(int round) {
    char d[128];
    char cmd[1024];

    snprintf(d, sizeof(d),
             "/tmp/resonance-fail/spam-%ld-r%d",
             (long)time(NULL), round);
    snprintf(cmd, sizeof(cmd),
             "mkdir -p %s && "
             "for i in 0 1 2; do "
             "mkdir -p %s/node$i/concord && "
             "cp /tmp/resonance/node$i/concord/"
             "journal.jsonl %s/node$i/concord/ "
             "2>/dev/null; "
             "cp /tmp/resonance/node$i/concord/"
             "config.json %s/node$i/concord/ "
             "2>/dev/null; done",
             d, d, d, d);
    system(cmd);
    fprintf(stderr,
            "concord_spam: journals preserved at %s\n", d);
}

int main(void) {
    Context ctx;
    TunMap map;
    struct pollfd p[2 * N];
    int fds[N], logfds[N];
    pid_t pids[N];
    int drop[N];
    char wids[MAXW][64], shorts[MAXW][16];
    char why[256];
    int i, k, nw, sub, iso, dead, seen3, alive, done;
    time_t t0, tmesh, tcheck, start, dl;

    if (access("./bin/concord", X_OK) != 0) {
        printf("concord_spam: skip no ./bin/concord\n");
        return (0);
    }
    memset(&map, 0, sizeof(map));
    if (!context_init(&ctx, 32, 1))
        return (1);
    if (!sim_netns_setup(N))
        return (1);
    if (!sim_tuns_open(fds, N)) {
        printf("concord_spam: skip (%s)\n",
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
        drop[i] = 0;
    }

    k = 0;
    nw = 0;
    sub = 0;
    iso = 0;
    dead = 0;
    seen3 = 0;
    done = 0;
    why[0] = '\0';
    t0 = 0;
    tmesh = 0;
    tcheck = 0;
    dl = 0;
    start = time(NULL);
    while (k < ROUNDS && time(NULL) - start < DEADLINE) {
        if (poll(p, 2 * N, 100) > 0) {
            for (i = 0; i < N; i++) {
                if (p[i].revents & POLLIN)
                    drain_tun(&map, &ctx.grid, &ctx.radio,
                              &ctx.nodes, fds[i], i, drop);
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
        if (sub == 0) {
            // Mesh up, submit the first triple.
            if (seen3 && hold(&t0, HOLD_UP)) {
                if (nw + NWL > MAXW ||
                    !submit_triple(0, wids, shorts, nw)) {
                    break;
                }
                nw += NWL;
                sub = 1;
                dl = time(NULL) + 90;
            }
        } else if (sub == 1 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Live triple present everywhere: stop
            // the previous one, if any.
            tcheck = time(NULL);
            if (live_present(shorts, nw)) {
                if (nw > NWL &&
                    !stop_triple(0, wids, nw - NWL)) {
                    break;
                }
                sub = 2;
                dl = time(NULL) + 120;
            } else if (time(NULL) >= dl) {
                break;
            }
        } else if (sub == 2 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Previous triple stopped everywhere:
            // isolate one node and SIGKILL+restart
            // another, at the same time.
            tcheck = time(NULL);
            if (nw <= NWL || stopped_absent(shorts, nw)) {
                iso = k % N;
                dead = (k + 1) % N;
                drop[iso] = 1;
                kill(pids[dead], SIGKILL);
                waitpid(pids[dead], NULL, 0);
                close(logfds[dead]);
                if (!sim_restart_concord(pids, logfds,
                                         dead)) {
                    break;
                }
                p[N + dead].fd = logfds[dead];
                p[N + dead].events = POLLIN;
                sub = 3;
                dl = time(NULL) + ISOLATE_SECS;
            } else if (time(NULL) >= dl) {
                break;
            }
        } else if (sub == 3) {
            // Isolation over: restore, heal both.
            if (time(NULL) >= dl) {
                drop[iso] = 0;
                sub = 4;
                tmesh = 0;
                dl = time(NULL) + 150;
            }
        } else if (sub == 4) {
            // Healed: full mesh back.
            if (mesh3(&tmesh)) {
                sub = 5;
                dl = time(NULL) + 150;
            } else if (time(NULL) >= dl) {
                break;
            }
        } else if (sub == 5 &&
                   (tcheck == 0 ||
                    time(NULL) - tcheck >= 2)) {
            // Verify: live triple present, stopped
            // triples absent.
            tcheck = time(NULL);
            if (verify_round(shorts, nw, why,
                             sizeof(why))) {
                k++;
                if (nw + NWL > MAXW) {
                    break;
                }
                if (!submit_triple(0, wids, shorts, nw)) {
                    break;
                }
                nw += NWL;
                sub = 1;
                dl = time(NULL) + 90;
            } else if (time(NULL) >= dl) {
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
    if (k < ROUNDS || !alive)
        preserve_journals(k);
    if (system("rm -rf /tmp/resonance") != 0) {
        // Best effort cleanup, teardown already ran.
    }

    done = (k >= ROUNDS && alive);
    if (!done) {
        fprintf(stderr,
                "concord_spam: fail round %d sub %d %s\n",
                k, sub, why);
        return (1);
    }
    return (0);
}
