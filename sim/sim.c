#define _GNU_SOURCE
#include "sim.h"
#include "../node/node.h"
#include "../shared/context.h"
#include "../tun/tun.h"
#include "../world/world.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Path and command buffer sizes.
static constexpr int PATH_LEN = 192;
static constexpr int BIG_LEN = 256;
static constexpr int NAME_LEN = 96;
static constexpr int SMALL_CMD = 320;
static constexpr int CMD_LEN = 1024;

// RUN_ID_MAX bounds checked run ids: 100+154 is
// the last sane underlay third octet.
static constexpr int RUN_ID_MAX = 154;

// RUN_ID_MIN is the first allocator id: 0
// stays the historic default run.
static constexpr int RUN_ID_MIN = 1;

// SPAWN_KILL_S bounds the SIGTERM wait
// in spawn_unwind before SIGKILL,
// mirroring run teardown_wait.
static constexpr int SPAWN_KILL_S = 5;

// UNDER_BASE is the underlay third
// octet base; run id shifts it.
static constexpr int UNDER_BASE = 100;

// GOSSIP_LEN is the shared gossip key
// size; stat and head must match.
static constexpr int GOSSIP_LEN = 32;

// SLEEP_NS is the 100 ms waitpid poll
// slice in spawn_wait.
static constexpr int SLEEP_NS = 100'000'000;

// Worst run 154 strings fit SimNet;
// UNDER_BASE plus max id fits octet;
// worst TUN index stays 5 digits.
static_assert(sizeof(((SimNet *)nullptr)->statedir) >=
              sizeof("/var/tmp/resonance-154"));
static_assert(sizeof(((SimNet *)nullptr)->ns_prefix) >=
              sizeof("resonance154_"));
static_assert(sizeof(((SimNet *)nullptr)->tun_prefix) >=
              sizeof("tun"));
static_assert(sizeof(((SimNet *)nullptr)->cgroot) >=
              sizeof("/sys/fs/cgroup/resonance-154"));
static_assert(UNDER_BASE + RUN_ID_MAX == 254);
static_assert((long)RUN_ID_MAX *
                  (long)TUN_MAP_MAX +
              (long)(TUN_MAP_MAX - 1) ==
              15499L);
static_assert(NAME_LEN >= IFNAMSIZ);
static_assert(PATH_LEN >=
              sizeof("/var/tmp/resonance-154/node99"));
static_assert(PATH_LEN >=
              sizeof("/var/run/netns/resonance154_99"));

// RUN_LOCK_PATH guards the run counter with
// flock. RUN_COUNTER_PATH holds the last
// handed id as decimal text.
#define RUN_LOCK_PATH "/var/tmp/resonance-runs.lock"
#define RUN_COUNTER_PATH "/var/tmp/resonance-runs.counter"

// run executes cmd via system.
static int run(const char* cmd) {
    return (system(cmd) == 0);
}

// write_file stores s into path. Returns 1 on success.
static int write_file(const char* path, const char* s) {
    FILE* fp;

    fp = fopen(path, "we");
    if (fp == nullptr)
        return (0);
    if (fputs(s, fp) == EOF) {
        fclose(fp);
        return (0);
    }
    return (fclose(fp) == 0);
}

// cloexec_fd marks fd close-on-exec, so
// forked shells and daemons never inherit
// TUNs, pipes, or the allocator lock
// and temp.
static void cloexec_fd(int fd) {
    int f;

    f = fcntl(fd, F_GETFD);
    if (f < 0)
        return;
    (void)fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

// ns_name_net writes <prefix><i> into buf.
// False on truncation.
static bool ns_name_net(const SimNet* net,
                        char* buf, size_t n,
                        int i) {
    int m;

    m = snprintf(buf, n, "%s%d",
                 net->ns_prefix, i);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// ns_path_net writes /var/run/netns/<prefix><i>.
// False on truncation.
static bool ns_path_net(const SimNet* net,
                        char* buf, size_t n,
                        int i) {
    int m;

    m = snprintf(buf, n,
                 "/var/run/netns/%s%d",
                 net->ns_prefix, i);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// tun_name_net writes <tunprefix><base+i>.
// Long math avoids int overflow. False on
// truncation.
static bool tun_name_net(const SimNet* net,
                         char* buf, size_t n,
                         int i) {
    long v;
    int m;

    v = (long)net->tun_base + (long)i;
    m = snprintf(buf, n, "%s%ld",
                 net->tun_prefix, v);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// node_dir_net writes <statedir>/node<i>.
// False on truncation.
static bool node_dir_net(const SimNet* net,
                         char* buf, size_t n,
                         int i) {
    int m;

    m = snprintf(buf, n, "%s/node%d",
                 net->statedir, i);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// gossip_path_net writes <statedir>/gossip.key.
// False on truncation.
static bool gossip_path_net(const SimNet* net,
                            char* buf, size_t n) {
    int m;

    m = snprintf(buf, n, "%s/gossip.key",
                 net->statedir);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// cg_node_net writes <cgroot>/node<i>.
// False on truncation.
static bool cg_node_net(const SimNet* net,
                        char* buf, size_t n,
                        int i) {
    int m;

    m = snprintf(buf, n, "%s/node%d",
                 net->cgroot, i);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// cgroup_place_net moves pid into
// <cgroot>/node<i>/daemon, creating the subtree as
// needed. libcontainer derives container cgroups
// from the parent of the daemon's own cgroup, so
// the daemon sits one level deep and the per-node
// parent stays empty for containers. Returns 1 on
// success.
static bool cgroup_place_net(const SimNet* net, int i,
                             pid_t pid) {
    char sub[PATH_LEN];
    char leaf[BIG_LEN];
    char target[SMALL_CMD];
    char ctl[BIG_LEN];
    char spid[32];
    int m;

    if (net == nullptr)
        return (false);
    if (mkdir(net->cgroot, 0755) != 0 &&
        errno != EEXIST)
        return (false);
    m = snprintf(ctl, sizeof(ctl),
                 "%s/cgroup.subtree_control",
                 net->cgroot);
    if (m < 0 || m >= (int)sizeof(ctl))
        return (false);
    if (!write_file(ctl, "+cpu +memory +pids"))
        return (false);
    if (!cg_node_net(net, sub, sizeof(sub),
                     i))
        return (false);
    if (mkdir(sub, 0755) != 0 && errno != EEXIST)
        return (false);
    m = snprintf(target, sizeof(target),
                 "%s/cgroup.subtree_control",
                 sub);
    if (m < 0 || m >= (int)sizeof(target))
        return (false);
    if (!write_file(target, "+cpu +memory +pids"))
        return (false);
    m = snprintf(leaf, sizeof(leaf),
                 "%s/daemon", sub);
    if (m < 0 || m >= (int)sizeof(leaf))
        return (false);
    if (mkdir(leaf, 0755) != 0 && errno != EEXIST)
        return (false);
    m = snprintf(target, sizeof(target),
                 "%s/cgroup.procs", leaf);
    if (m < 0 || m >= (int)sizeof(target))
        return (false);
    m = snprintf(spid, sizeof(spid), "%d",
                 (int)pid);
    if (m < 0 || m >= (int)sizeof(spid))
        return (false);
    return (write_file(target, spid));
}

// cgroup_enter_net moves pid into its per-node
// cgroup. Each daemon's containers derive cgroups
// from the daemon's own; sharing one cgroup refuses
// creation as non-empty. Best effort: warns and
// returns 1 when unavailable so mesh-only runs
// never depend on it.
static bool cgroup_enter_net(const SimNet* net, int i,
                             pid_t pid) {
    if (!cgroup_place_net(net, i, pid))
        printf("sim: no per-node cgroup, containers "
               "may collide\n");
    return (true);
}

// open_in_ns opens a tun in another netns. Every
// setns hop is checked: any failure, including a
// failed restore to the old netns, returns -1 so
// the caller never proceeds in the wrong netns.
// Callers treat -1 as fatal and abort setup.
// A failed restore also shouts on stderr and
// closes the fresh fd, so the stranded netns
// never leaks silently. Both ns fds are
// CLOEXEC at source, so forked children never
// inherit them.
static int open_in_ns(const char* ns, const char* ifname,
                      int* fd) {
    int oldfd, nsfd;

    oldfd = open("/proc/self/ns/net",
                 O_RDONLY | O_CLOEXEC);
    nsfd = open(ns, O_RDONLY | O_CLOEXEC);
    if (oldfd < 0 || nsfd < 0) {
        if (oldfd >= 0)
            close(oldfd);
        if (nsfd >= 0)
            close(nsfd);
        return (-1);
    }
    if (setns(nsfd, CLONE_NEWNET) != 0) {
        close(oldfd);
        close(nsfd);
        return (-1);
    }
    if (!open_tun_file(ifname, fd)) {
        if (setns(oldfd, CLONE_NEWNET) != 0) {
            fprintf(stderr, "sim: lost netns "
                    "restore\n");
            close(oldfd);
            close(nsfd);
            return (-1);
        }
        close(oldfd);
        close(nsfd);
        return (-1);
    }
    if (setns(oldfd, CLONE_NEWNET) != 0) {
        fprintf(stderr, "sim: lost netns "
                "restore\n");
        close(*fd);
        *fd = -1;
        close(oldfd);
        close(nsfd);
        return (-1);
    }
    close(oldfd);
    close(nsfd);
    return (0);
}

// sim_default_net returns the historic globals:
// STATEDIR, resonance_, tun base 0, 192.168.100/24,
// /sys/fs/cgroup/resonance, isolate_all 0.
// Callers opt into 1 explicitly.
SimNet sim_default_net(void) {
    SimNet net;

    // memcpy: literals fit by the asserts
    // above, so no snprintf to check.
    memcpy(net.statedir, STATEDIR,
           sizeof(STATEDIR));
    memcpy(net.ns_prefix, "resonance_",
           sizeof("resonance_"));
    memcpy(net.tun_prefix, "tun",
           sizeof("tun"));
    net.tun_base = 0;
    net.under[0] = 192;
    net.under[1] = 168;
    net.under[2] = (uint8_t)UNDER_BASE;
    memcpy(net.cgroot,
           "/sys/fs/cgroup/resonance",
           sizeof("/sys/fs/cgroup/"
                  "resonance"));
    net.isolate_all = 0;
    return (net);
}

// sim_net_for_run writes the net for run
// id. File-local: out-of-range ids alias
// silently, so callers must use
// sim_net_try_for_run instead. Run 0 is the
// default. Higher ids shift the tun base by
// TUN_MAP_MAX, the subnet third octet by the
// id, and suffix the statedir, ns prefix, and
// cgroup root. Names, addrs, dirs, and cgroups
// are disjoint, but node 0 still lives in the
// host netns, so two runs with a host node
// cannot run concurrently: both claim the host
// 224.0.0.0/4 route and both bind port 7946.
// isolate_all is 0; callers opt into 1
// explicitly. False on truncation, which the
// asserts above rule out through run 154.
static bool sim_net_for_run(int run_id,
                            SimNet* out) {
    SimNet net;
    int m;

    net = sim_default_net();
    net.isolate_all = 0;
    if (run_id <= 0) {
        *out = net;
        return (true);
    }
    m = snprintf(net.statedir,
                 sizeof(net.statedir),
                 "%s-%d", STATEDIR, run_id);
    if (m < 0 ||
        m >= (int)sizeof(net.statedir))
        return (false);
    m = snprintf(net.ns_prefix,
                 sizeof(net.ns_prefix),
                 "resonance%d_", run_id);
    if (m < 0 ||
        m >= (int)sizeof(net.ns_prefix))
        return (false);
    // Long math first: the cast back may wrap
    // for absurd ids, but never overflows.
    net.tun_base =
        (int)((long)run_id * (long)TUN_MAP_MAX);
    net.under[2] =
        (uint8_t)((long)UNDER_BASE + (long)run_id);
    m = snprintf(net.cgroot, sizeof(net.cgroot),
                 "%s-%d",
                 "/sys/fs/cgroup/resonance",
                 run_id);
    if (m < 0 ||
        m >= (int)sizeof(net.cgroot))
        return (false);
    *out = net;
    return (true);
}

// field_ok reports true when s is non-empty,
// NUL-terminated inside cap, and holds only
// [A-Za-z0-9/_.-]. Those fields reach system(),
// so anything else returns false.
static bool field_ok(const char* s, size_t cap) {
    size_t n;
    size_t k;

    n = strnlen(s, cap);
    if (n == 0 || n == cap)
        return (false);
    for (k = 0; k < n; k++) {
        char ch = s[k];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '/' || ch == '_' ||
            ch == '.' || ch == '-')
            continue;
        return (false);
    }
    return (true);
}

// under_ok reports true for a sane underlay /24:
// unicast, not loopback, and clear of the
// 10.0.0.0/16 overlay.
static bool under_ok(const uint8_t u[3]) {
    if (u[0] == 0 || u[0] == 127)
        return (false);
    if (u[0] >= 224)
        return (false);
    if (u[0] == 10 && u[1] == 0)
        return (false);
    return (true);
}

// tun_fit reports true when every tun name the
// net can mint (<prefix><base+i> for i below
// TUN_MAP_MAX) fits IFNAMSIZ-1, and the base is
// not negative.
static bool tun_fit(const SimNet* net) {
    char name[64];
    long v;
    int n;
    int i;

    if (net->tun_base < 0)
        return (false);
    for (i = 0; i < TUN_MAP_MAX; i++) {
        v = (long)net->tun_base + (long)i;
        n = snprintf(name, sizeof(name),
                     "%s%ld", net->tun_prefix,
                     v);
        if (n < 0 || n >= IFNAMSIZ)
            return (false);
    }
    return (true);
}

// sim_net_valid checks a net for safe use:
// statedir, ns prefix, tun prefix, and
// cgroot hold only [A-Za-z0-9/_.-] (they
// reach system()), every tun name fits
// IFNAMSIZ-1, and under is a sane /24
// clear of the 10.0.0.0/16 overlay.
bool sim_net_valid(const SimNet* net) {
    if (net == nullptr)
        return (false);
    if (!field_ok(net->statedir,
                  sizeof(net->statedir)))
        return (false);
    if (!field_ok(net->ns_prefix,
                  sizeof(net->ns_prefix)))
        return (false);
    if (!field_ok(net->tun_prefix,
                  sizeof(net->tun_prefix)))
        return (false);
    if (!field_ok(net->cgroot,
                  sizeof(net->cgroot)))
        return (false);
    if (!tun_fit(net))
        return (false);
    if (!under_ok(net->under))
        return (false);
    return (true);
}

// sim_net_try_for_run is the checked constructor
// new code must use. It writes the net for run_id
// and returns true, or returns false for run_id
// below 0 or above RUN_ID_MAX, which would wrap
// the octet or alias tun names. Result
// isolate_all is 0; callers opt into 1
// explicitly.
bool sim_net_try_for_run(int run_id, SimNet* out) {
    SimNet net;

    if (out == nullptr)
        return (false);
    if (run_id < 0 || run_id > RUN_ID_MAX)
        return (false);
    if (!sim_net_for_run(run_id, &net))
        return (false);
    net.isolate_all = 0;
    if (!sim_net_valid(&net))
        return (false);
    *out = net;
    return (true);
}

// counter_tmp_sweep deletes stale acquire
// temps (counter.tmp.*). Best effort:
// ignores all errors. Call while holding
// the lock, so no live temp is removed.
static void counter_tmp_sweep(void) {
    DIR* d;

    d = opendir("/var/tmp");
    if (d == nullptr)
        return;
    for (;;) {
        struct dirent* e;
        char path[160];
        int k;

        e = readdir(d);
        if (e == nullptr)
            break;
        if (strncmp(e->d_name,
                    "resonance-runs."
                    "counter.tmp.",
                    27) != 0)
            continue;
        k = snprintf(path, sizeof(path),
                     "/var/tmp/%s",
                     e->d_name);
        if (k < 0 ||
            k >= (int)sizeof(path))
            continue;
        unlink(path);
    }
    closedir(d);
}

// dir_fsync fsyncs dir best effort, so a
// fresh rename lasts past a crash.
// Ignores errors: safety stays limited
// to the tmp fsync plus this attempt;
// a crash before this may lose rename.
static void dir_fsync(const char* dir) {
    int fd;

    fd = open(dir,
              O_RDONLY | O_DIRECTORY |
                  O_CLOEXEC);
    if (fd < 0)
        return;
    fsync(fd);
    close(fd);
}

// counter_parse reads the last handed id
// from buf. Strict decimal plus trailing
// blank only; false on garbage, so a
// corrupt file never aliases live runs.
static bool counter_parse(const char* buf,
                          long* out) {
    char* end;
    long v;

    if (buf == nullptr || out == nullptr)
        return (false);
    errno = 0;
    v = strtol(buf, &end, 10);
    if (end == buf)
        return (false);
    while (*end == ' ' || *end == '\t' ||
           *end == '\n' || *end == '\r')
        end++;
    if (*end != '\0')
        return (false);
    if (errno != 0)
        return (false);
    *out = v;
    return (true);
}

// sim_run_acquire hands out run ids 1..154
// for concurrent processes. It locks
// RUN_LOCK_PATH with flock, sweeps stale
// counter.tmp.* temps, reads, bumps, and
// writes RUN_COUNTER_PATH, then unlocks.
// Missing or empty reads as 0; over-long
// lines reject as corrupt; an I/O error
// fails instead of handing id 1. Cur is
// range-checked before cur+1, so LONG_MAX
// never overflows. Publish is temp plus
// rename at 0600 with a tmp fsync plus a
// best-effort /var/tmp fsync: a crash
// before the dir fsync may still lose the
// rename. Full (at 154) fails loudly with
// false plus stderr. Ids feed
// sim_net_try_for_run. Monotonic: no
// reuse, so wipe the counter file to reset
// after runs end. flock frees on close or
// crash, so stale locks are impossible.
bool sim_run_acquire(int* out_id) {
    char buf[32];
    char wbuf[32];
    char tmp[64];
    long cur;
    long nxt;
    int lockfd;
    int tmpfd;
    int m;
    FILE* fp;

    if (out_id == nullptr)
        return (false);
    lockfd = open(RUN_LOCK_PATH,
                  O_RDWR | O_CREAT, 0600);
    if (lockfd < 0)
        return (false);
    cloexec_fd(lockfd);
    if (fchmod(lockfd, 0600) != 0) {
        // Best effort: old perms stay.
    }
    if (flock(lockfd, LOCK_EX) != 0) {
        close(lockfd);
        return (false);
    }
    counter_tmp_sweep();
    cur = 0;
    fp = fopen(RUN_COUNTER_PATH, "re");
    if (fp != nullptr) {
        bool blank;
        bool trunc;
        bool ioerr;
        int c;

        blank = fgets(buf, sizeof(buf),
                      fp) == nullptr;
        ioerr = ferror(fp) != 0;
        trunc = false;
        if (!blank &&
            strchr(buf, '\n') ==
                nullptr) {
            c = fgetc(fp);
            if (c != EOF)
                trunc = true;
        }
        fclose(fp);
        if (ioerr) {
            fprintf(stderr, "sim: run "
                    "counter read error\n");
            close(lockfd);
            return (false);
        }
        if (trunc) {
            fprintf(stderr, "sim: run "
                    "counter corrupt\n");
            close(lockfd);
            return (false);
        }
        if (!blank &&
            !counter_parse(buf, &cur)) {
            fprintf(stderr, "sim: run "
                    "counter corrupt\n");
            close(lockfd);
            return (false);
        }
    }
    if (cur < RUN_ID_MIN - 1 ||
        cur >= RUN_ID_MAX) {
        fprintf(stderr, "sim: run ids "
                "exhausted (max %d)\n",
                RUN_ID_MAX);
        close(lockfd);
        return (false);
    }
    nxt = cur + 1;
    m = snprintf(tmp, sizeof(tmp),
                 "%s.tmp.%d",
                 RUN_COUNTER_PATH,
                 (int)getpid());
    if (m < 0 || m >= (int)sizeof(tmp)) {
        close(lockfd);
        return (false);
    }
    tmpfd = open(tmp,
                 O_WRONLY | O_CREAT | O_TRUNC,
                 0600);
    if (tmpfd < 0) {
        close(lockfd);
        return (false);
    }
    cloexec_fd(tmpfd);
    m = snprintf(wbuf, sizeof(wbuf), "%ld\n",
                 nxt);
    if (m < 0 || m >= (int)sizeof(wbuf)) {
        close(tmpfd);
        unlink(tmp);
        close(lockfd);
        return (false);
    }
    if (write(tmpfd, wbuf, (size_t)m) !=
        (ssize_t)m) {
        close(tmpfd);
        unlink(tmp);
        close(lockfd);
        return (false);
    }
    if (fsync(tmpfd) != 0) {
        close(tmpfd);
        unlink(tmp);
        close(lockfd);
        return (false);
    }
    if (close(tmpfd) != 0) {
        unlink(tmp);
        close(lockfd);
        return (false);
    }
    if (rename(tmp, RUN_COUNTER_PATH) != 0) {
        unlink(tmp);
        close(lockfd);
        return (false);
    }
    dir_fsync("/var/tmp");
    close(lockfd);
    *out_id = (int)nxt;
    return (true);
}

// netns_add_one creates netns <prefix><i>.
// Deletes first for idempotent retry.
// False on truncation or add failure.
static bool netns_add_one(const SimNet* net,
                          int i) {
    char name[PATH_LEN];
    char cmd[SMALL_CMD];
    int m;

    if (!ns_name_net(net, name,
                     sizeof(name), i))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip netns del %s 2>/dev/null",
                 name);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    run(cmd);
    m = snprintf(cmd, sizeof(cmd),
                 "ip netns add %s", name);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    return (run(cmd));
}

// netns_del_one deletes netns <prefix><i>.
// Best effort: always true, even on
// truncation (then nothing runs).
static bool netns_del_one(const SimNet* net,
                          int i) {
    char name[PATH_LEN];
    char cmd[SMALL_CMD];
    int m;

    if (!ns_name_net(net, name,
                     sizeof(name), i))
        return (true);
    m = snprintf(cmd, sizeof(cmd),
                 "ip netns del %s 2>/dev/null",
                 name);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (true);
    run(cmd);
    return (true);
}

// sim_netns_setup_net creates one netns per extra
// node using the net prefix. Invalid nets
// fail before touching the host.
bool sim_netns_setup_net(const SimNet* net, int n) {
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    for (i = 1; i < n; i++) {
        if (!netns_add_one(net, i))
            return (false);
    }
    return (true);
}

// sim_netns_setup_all creates namespaces for
// 0..n-1. Isolated path for parallel runs:
// node 0 leaves the host, so each run owns
// its route table and port space. New code
// uses this; frozen check/ keeps the
// host-node-0 setup_net default.
bool sim_netns_setup_all(const SimNet* net,
                         int n) {
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    for (i = 0; i < n; i++) {
        if (!netns_add_one(net, i))
            return (false);
    }
    return (true);
}

// is_default_net reports true for the
// historic run 0 net. Teardown drops the
// legacy net_namespace_nodeb name only
// then, so distinct isolated runs share
// nothing but the allocator file.
static bool is_default_net(const SimNet* net) {
    SimNet d;

    if (net == nullptr)
        return (false);
    d = sim_default_net();
    if (strcmp(net->statedir, d.statedir) !=
        0)
        return (false);
    if (strcmp(net->ns_prefix, d.ns_prefix) !=
        0)
        return (false);
    if (strcmp(net->tun_prefix,
               d.tun_prefix) != 0)
        return (false);
    if (net->tun_base != d.tun_base)
        return (false);
    if (net->under[0] != d.under[0])
        return (false);
    if (net->under[1] != d.under[1])
        return (false);
    if (net->under[2] != d.under[2])
        return (false);
    if (strcmp(net->cgroot, d.cgroot) != 0)
        return (false);
    return (true);
}

// sim_netns_teardown_net deletes netns 0..n-1
// and the net cgroup subtree. Best effort:
// always true for non-null net, even for
// absurd n; false only for nullptr net.
// Invalid nets are a true no-op: their names
// never reach system(), so a bad-input
// unwind leaves the host untouched.
// Missing names just fail silently.
// Covers host (1..) and isolated (0..); the
// extra del on host runs fails silently.
// Drops the legacy net_namespace_nodeb
// name for the default net only, left
// from before per-run prefixes.
bool sim_netns_teardown_net(const SimNet* net, int n) {
    char name[PATH_LEN];
    char cmd[SMALL_CMD];
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (true);
    for (i = 0; i < n; i++)
        netns_del_one(net, i);
    if (is_default_net(net))
        run("ip netns del net_namespace_nodeb "
            "2>/dev/null");
    // Drop per-node cgroups; rmdir only removes empty
    // dirs, so strays fail silently here.
    // Truncated names never run, still true.
    for (i = 0; i < n; i++) {
        int m;

        if (!cg_node_net(net, name,
                         sizeof(name), i))
            continue;
        m = snprintf(cmd, sizeof(cmd),
                     "rmdir %s 2>/dev/null",
                     name);
        if (m < 0 || m >= (int)sizeof(cmd))
            continue;
        run(cmd);
    }
    {
        int m;

        m = snprintf(cmd, sizeof(cmd),
                     "rmdir %s 2>/dev/null",
                     net->cgroot);
        if (m < 0 || m >= (int)sizeof(cmd))
            return (true);
        run(cmd);
    }
    return (true);
}

// tun_open_one_ns opens tun<base+i> in netns
// <prefix><i>. True on success; fds[i] is
// -1 plus cloexec on the way out.
static bool tun_open_one_ns(const SimNet* net,
                            int* fds, int i) {
    char tun[NAME_LEN];
    char path[PATH_LEN];

    if (!tun_name_net(net, tun, sizeof(tun),
                      i))
        return (false);
    if (!ns_path_net(net, path,
                     sizeof(path), i))
        return (false);
    if (open_in_ns(path, tun, &fds[i]) != 0) {
        fds[i] = -1;
        return (false);
    }
    cloexec_fd(fds[i]);
    return (true);
}

// sim_tuns_open_net opens tun<base> in host and
// tun<base+1>.. in their netns. Invalid nets
// fail before opening anything. Presets
// fds[0..n-1] to -1 at entry; on false the
// opened prefix stays open and the direct
// caller must close it, as run unwind does.
bool sim_tuns_open_net(const SimNet* net, int* fds,
                       int n) {
    char tun[NAME_LEN];
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    if (fds == nullptr)
        return (false);
    for (i = 0; i < n; i++)
        fds[i] = -1;
    if (!tun_name_net(net, tun, sizeof(tun),
                      0))
        return (false);
    if (!open_tun_file(tun, &fds[0])) {
        printf("skip (%s)\n", strerror(errno));
        fds[0] = -1;
        return (false);
    }
    cloexec_fd(fds[0]);
    for (i = 1; i < n; i++) {
        if (!tun_open_one_ns(net, fds, i))
            return (false);
    }
    return (true);
}

// sim_tuns_open_all opens every TUN inside
// its own netns, unlike open_net which keeps
// tun<base> in the host. Pairs with
// setup_all; node 0 has no host device.
// Presets fds[0..n-1] to -1 at entry; on
// false the opened prefix stays open and
// the direct caller must close it.
bool sim_tuns_open_all(const SimNet* net, int* fds,
                       int n) {
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    if (fds == nullptr)
        return (false);
    for (i = 0; i < n; i++)
        fds[i] = -1;
    for (i = 0; i < n; i++) {
        if (!tun_open_one_ns(net, fds, i))
            return (false);
    }
    return (true);
}

// sim_nodes_add_net registers simulated nodes.
// Underlay net under/24 stays disjoint from
// overlay 10.0.0.0/16.
bool sim_nodes_add_net(const SimNet* net, Context* ctx,
                       TunMap* map, int* fds, int n) {
    uint8_t ip[4];
    uint64_t id;
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (ctx == nullptr || map == nullptr)
        return (false);
    if (fds == nullptr)
        return (false);
    if (n < 1 || n > TUN_MAP_MAX || n > 254)
        return (false);
    ip[0] = net->under[0];
    ip[1] = net->under[1];
    ip[2] = net->under[2];
    for (i = 0; i < n; i++) {
        int64_t x = (int64_t)i * 1'000'000'000LL;

        ip[3] = (uint8_t)(i + 1);
        if (!context_add_node(ctx, x, 0, 0, 0,
                              &id))
            return (false);
        if (!tun_map_add(map, fds[i], ip, id))
            return (false);
    }
    return (true);
}

// addr_up_one_ns brings lo and tun<i> up in
// netns <prefix><i> with addr a.b.c.(i+1)
// plus the multicast route. True when every
// step runs. False on any truncation, so a
// short name never configures the wrong
// link.
static bool addr_up_one_ns(const SimNet* net,
                           int i, unsigned a,
                           unsigned b,
                           unsigned c) {
    char name[PATH_LEN];
    char tun[NAME_LEN];
    char cmd[CMD_LEN];
    int m;

    if (!ns_name_net(net, name,
                     sizeof(name), i))
        return (false);
    if (!tun_name_net(net, tun, sizeof(tun),
                      i))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip link set "
                 "lo up",
                 name);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip link set "
                 "lo multicast on",
                 name);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip addr add "
                 "%u.%u.%u.%d/24 dev %s",
                 name, a, b, c, i + 1, tun);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip link set "
                 "%s up",
                 name, tun);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip link set "
                 "%s multicast on",
                 name, tun);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip route add "
                 "224.0.0.0/4 dev %s",
                 name, tun);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    return (true);
}

// sim_addrs_up_net assigns underlay IPs and brings
// links up. net under/24 stays disjoint from
// overlay 10.0.0.0/16.
bool sim_addrs_up_net(const SimNet* net, int n) {
    char tun0[NAME_LEN];
    char cmd[CMD_LEN];
    unsigned a, b, c;
    int i;
    int m;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (n < 1 || n > 254)
        return (false);
    a = net->under[0];
    b = net->under[1];
    c = net->under[2];
    if (!tun_name_net(net, tun0, sizeof(tun0),
                      0))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip addr add %u.%u.%u.1/24 dev "
                 "%s",
                 a, b, c, tun0);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip link set %s up", tun0);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip link set %s multicast on",
                 tun0);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "ip route add 224.0.0.0/4 dev "
                 "%s",
                 tun0);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    for (i = 1; i < n; i++) {
        if (!addr_up_one_ns(net, i, a, b,
                            c))
            return (false);
    }
    return (true);
}

// sim_addrs_up_all assigns 192.168.<under>
// .1..n plus multicast routes all inside
// their netns. No host tun, no host addr,
// no host 224.0.0.0/4 route. Pairs with
// setup_all for parallel runs.
bool sim_addrs_up_all(const SimNet* net, int n) {
    unsigned a, b, c;
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (n < 1 || n > 254)
        return (false);
    a = net->under[0];
    b = net->under[1];
    c = net->under[2];
    for (i = 0; i < n; i++) {
        if (!addr_up_one_ns(net, i, a, b,
                            c))
            return (false);
    }
    return (true);
}

// ensure_gossip_key_net creates the shared gossip
// key once per cluster. Reuses it when present.
// Single owner: one setup per statedir at a time;
// parallel setups on one statedir race on the
// key file.
static bool ensure_gossip_key_net(
    const SimNet* net) {
    struct stat st;
    char path[PATH_LEN];
    char cmd[CMD_LEN];

    int m;

    if (net == nullptr)
        return (false);
    if (mkdir(net->statedir, 0700) != 0 &&
        errno != EEXIST)
        return (false);
    if (!gossip_path_net(net, path,
                         sizeof(path)))
        return (false);
    if (stat(path, &st) == 0 &&
        st.st_size == GOSSIP_LEN)
        return (true);
    m = snprintf(cmd, sizeof(cmd),
                 "head -c %d /dev/urandom > %s "
                 "&& chmod 600 %s",
                 GOSSIP_LEN, path, path);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    return (true);
}

// spawn_fail closes both pipe ends and reports
// failure, so no failed spawn leaks fds.
static bool spawn_fail(int pipedes[2]) {
    close(pipedes[0]);
    close(pipedes[1]);
    return (false);
}

// spawn_wait SIGTERMs pid, waits up to
// SPAWN_KILL_S, then SIGKILLs and reaps.
// EINTR retries, mirroring run teardown.
// Returns only after reap or ECHILD, so
// the caller may then mark -1 safely.
static void spawn_wait(pid_t pid) {
    struct timespec ts;
    time_t t0;

    if (pid <= 0)
        return;
    kill(pid, SIGTERM);
    t0 = time(nullptr);
    if (t0 == (time_t)-1) {
        kill(pid, SIGKILL);
        while (waitpid(pid, nullptr, 0) <
                   0 &&
               errno == EINTR) {
        }
        return;
    }
    for (;;) {
        int w;

        w = waitpid(pid, nullptr,
                    WNOHANG);
        if (w == pid)
            return;
        if (w < 0 && errno == ECHILD)
            return;
        if (time(nullptr) - t0 >=
            SPAWN_KILL_S)
            break;
        ts.tv_sec = 0;
        ts.tv_nsec = SLEEP_NS;
        nanosleep(&ts, nullptr);
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, nullptr, 0) < 0 &&
           errno == EINTR) {
    }
}

// spawn_unwind SIGTERMs, reaps, and closes
// nodes 0..upto-1 after a partial spawn
// failure, so a failed spawn never orphans
// children. Each wait loops on EINTR and
// escalates to SIGKILL after SPAWN_KILL_S,
// mirroring run teardown. Slots mark -1
// only after the reap, never after an
// interrupted wait.
static void spawn_unwind(pid_t* pids,
                         int* logfds, int upto) {
    int k;

    for (k = 0; k < upto; k++) {
        if (pids[k] > 0) {
            spawn_wait(pids[k]);
            pids[k] = -1;
        }
        if (logfds[k] >= 0) {
            close(logfds[k]);
            logfds[k] = -1;
        }
    }
}

// spawn_one_net seeds node i and forks it. Stores
// pid and logfd. Parent config root must exist.
// Invalid nets fail before pipe(). clock_offset
// is the CONCORD_CLOCK_OFFSET value for this
// node, or nullptr for true time. anchor_arg
// is an extra daemon argv entry (e.g.
// "--anchor") or nullptr. anchor_octet adds an
// anchors entry for under.anchor_octet:7946 to
// every node config when nonzero. isolate_all
// nonzero also enters node 0 into its netns;
// 0 keeps the host default. The pipe is
// cloexec, so shells never inherit it. The
// child exits (127) instead of running in the
// host netns when its netns open or setns
// fails, and on dup failure. Every failure
// after pipe() closes both pipe ends.
static bool spawn_one_net(const SimNet* net,
                          pid_t* pid, int* logfd,
                          int i,
                          const char* clock_offset,
                          const char* anchor_arg,
                          int anchor_octet) {
    char dir[PATH_LEN];
    char certs[BIG_LEN];
    char cmd[CMD_LEN];
    char path[PATH_LEN];
    unsigned a, b, c;
    int pipedes[2];
    int m;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (pipe(pipedes) < 0)
        return (false);
    cloexec_fd(pipedes[0]);
    cloexec_fd(pipedes[1]);

    // Per-node XDG_CONFIG_HOME.
    if (!node_dir_net(net, dir, sizeof(dir),
                      i))
        return (spawn_fail(pipedes));
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return (spawn_fail(pipedes));

    // Seed CA from repo certs/ into
    // $XDG_CONFIG_HOME/concord/certs.
    // Concord requires ca.crt/ca.key.
    m = snprintf(certs, sizeof(certs),
                 "%s/concord/certs", dir);
    if (m < 0 || m >= (int)sizeof(certs))
        return (spawn_fail(pipedes));
    m = snprintf(cmd, sizeof(cmd),
                 "mkdir -p %s", certs);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (spawn_fail(pipedes));
    if (!run(cmd))
        return (spawn_fail(pipedes));

    m = snprintf(cmd, sizeof(cmd),
                 "cp certs/ca.crt %s/ 2>/dev/null",
                 certs);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (spawn_fail(pipedes));
    if (!run(cmd))
        return (spawn_fail(pipedes));

    m = snprintf(cmd, sizeof(cmd),
                 "cp certs/ca.key %s/ 2>/dev/null",
                 certs);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (spawn_fail(pipedes));
    if (!run(cmd))
        return (spawn_fail(pipedes));

    // Seed shared gossip key into
    // $XDG_CONFIG_HOME/concord/memberservice.
    // Concord requires identical secret.key
    // on every node.
    if (!ensure_gossip_key_net(net))
        return (spawn_fail(pipedes));
    m = snprintf(cmd, sizeof(cmd),
                 "mkdir -p %s/concord/"
                 "memberservice",
                 dir);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (spawn_fail(pipedes));
    if (!run(cmd))
        return (spawn_fail(pipedes));
    if (!gossip_path_net(net, path,
                         sizeof(path)))
        return (spawn_fail(pipedes));
    m = snprintf(cmd, sizeof(cmd),
                 "cp %s %s/concord/memberservice/"
                 "secret.key && chmod 600 "
                 "%s/concord/memberservice/"
                 "secret.key",
                 path, dir, dir);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (spawn_fail(pipedes));
    if (!run(cmd))
        return (spawn_fail(pipedes));

    a = net->under[0];
    b = net->under[1];
    c = net->under[2];
    if (anchor_octet != 0) {
        m = snprintf(cmd, sizeof(cmd),
                     "mkdir -p %s/concord && uuid="
                     "$(cat /proc/sys/kernel/"
                     "random/uuid) && printf "
                     "'{\"id\":\"%%s\","
                     "\"memberlist_address\":"
                     "\"0.0.0.0:7946\","
                     "\"advertise_address\":"
                     "\"%u.%u.%u.%d\","
                     "\"anchors\":[{\"address\":"
                     "\"%u.%u.%u.%d:7946\"}]}' "
                     "\"$uuid\" > "
                     "%s/concord/config.json",
                     dir, a, b, c, i + 1, a, b,
                     c, anchor_octet, dir);
    } else {
        m = snprintf(cmd, sizeof(cmd),
                     "mkdir -p %s/concord && uuid="
                     "$(cat /proc/sys/kernel/"
                     "random/uuid) && printf "
                     "'{\"id\":\"%%s\","
                     "\"memberlist_address\":"
                     "\"0.0.0.0:7946\","
                     "\"advertise_address\":"
                     "\"%u.%u.%u.%d\"}' "
                     "\"$uuid\" > "
                     "%s/concord/config.json",
                     dir, a, b, c, i + 1, dir);
    }
    if (m < 0 || m >= (int)sizeof(cmd))
        return (spawn_fail(pipedes));
    if (!run(cmd))
        return (spawn_fail(pipedes));

    *pid = fork();

    if (*pid < 0)
        return (spawn_fail(pipedes));
    if (*pid == 0) {
        setenv("XDG_CONFIG_HOME", dir, 1);
        if (clock_offset != nullptr) {
            setenv("CONCORD_CLOCK_OFFSET",
                   clock_offset, 1);
        }
        if (i > 0 || net->isolate_all) {
            int nsfd;

            if (!ns_path_net(net, path,
                             sizeof(path), i))
                _exit(127);
            nsfd = open(path, O_RDONLY |
                               O_CLOEXEC);
            if (nsfd < 0)
                _exit(127);
            if (setns(nsfd, CLONE_NEWNET) != 0) {
                close(nsfd);
                _exit(127);
            }
            close(nsfd);
        }

        // Pipe stdout, stderr to write end.
        // A failed dup exits: the parent
        // would otherwise read silence.
        if (dup2(pipedes[1], 1) < 0)
            _exit(127);
        if (dup2(pipedes[1], 2) < 0)
            _exit(127);

        // Close pipe from fd table.
        close(pipedes[0]);
        close(pipedes[1]);

        if (anchor_arg != nullptr) {
            execl("./bin/concord", "concord",
                  anchor_arg, (char*)nullptr);
        } else {
            execl("./bin/concord", "concord",
                  (char*)nullptr);
        }
        _exit(127);
    }

    close(pipedes[1]);
    cgroup_enter_net(net, i, *pid);
    *logfd = pipedes[0];
    return (true);
}

// sim_spawn_cmd forks bin plus argv with
// stdout plus stderr into a log pipe.
// Enters <ns_prefix><node> for node above
// 0, plus node 0 when isolate_all. Home
// is untouched: the caller prepares it.
// Cgroup placement is best effort like
// spawn_one_net. Loud false on bad net,
// pid, logfd, node, or cmd: bad nets,
// null or empty bins, null argv, null
// argv[0], or empty workdir never fork.
// Child _exit(127) on chdir, ns open,
// setns, dup2, or exec failure. Pipe is
// cloexec, as in spawn_one_net.
bool sim_spawn_cmd(const SimNet* net,
                   pid_t* pid, int* logfd,
                   int node,
                   const SimCmd* cmd) {
    char nspath[PATH_LEN];
    int pipedes[2];
    bool need_ns;

    if (net == nullptr) {
        fprintf(stderr, "sim: null net\n");
        return (false);
    }
    if (!sim_net_valid(net)) {
        fprintf(stderr, "sim: bad net\n");
        return (false);
    }
    if (pid == nullptr ||
        logfd == nullptr) {
        fprintf(stderr, "sim: null slot\n");
        return (false);
    }
    if (node < 0 || node >= TUN_MAP_MAX) {
        fprintf(stderr, "sim: bad node\n");
        return (false);
    }
    if (cmd == nullptr) {
        fprintf(stderr, "sim: null cmd\n");
        return (false);
    }
    if (cmd->bin == nullptr ||
        cmd->bin[0] == '\0') {
        fprintf(stderr, "sim: bad bin\n");
        return (false);
    }
    if (cmd->argv == nullptr ||
        cmd->argv[0] == nullptr) {
        fprintf(stderr, "sim: bad argv\n");
        return (false);
    }
    if (cmd->workdir != nullptr &&
        cmd->workdir[0] == '\0') {
        fprintf(stderr, "sim: bad dir\n");
        return (false);
    }
    need_ns = node > 0 ||
        net->isolate_all;
    if (need_ns &&
        !ns_path_net(net, nspath,
                     sizeof(nspath),
                     node)) {
        fprintf(stderr, "sim: bad ns\n");
        return (false);
    }
    if (pipe(pipedes) < 0) {
        fprintf(stderr, "sim: no pipe\n");
        return (false);
    }
    cloexec_fd(pipedes[0]);
    cloexec_fd(pipedes[1]);
    *pid = fork();
    if (*pid < 0)
        return (spawn_fail(pipedes));
    if (*pid == 0) {
        if (cmd->workdir != nullptr &&
            chdir(cmd->workdir) != 0)
            _exit(127);
        if (need_ns) {
            int nsfd;

            nsfd = open(nspath,
                        O_RDONLY |
                            O_CLOEXEC);
            if (nsfd < 0)
                _exit(127);
            if (setns(nsfd,
                      CLONE_NEWNET) != 0) {
                close(nsfd);
                _exit(127);
            }
            close(nsfd);
        }
        if (dup2(pipedes[1], 1) < 0)
            _exit(127);
        if (dup2(pipedes[1], 2) < 0)
            _exit(127);
        close(pipedes[0]);
        close(pipedes[1]);
        execv(cmd->bin, cmd->argv);
        _exit(127);
    }
    close(pipedes[1]);
    cgroup_enter_net(net, node, *pid);
    *logfd = pipedes[0];
    return (true);
}

// ensure_parent_net creates the parent config root
// for all nodes. Mode 0700 matches certs.Dir().
// EEXIST is benign for idempotent setup.
static bool ensure_parent_net(const SimNet* net) {
    if (net == nullptr)
        return (false);
    if (mkdir(net->statedir, 0700) != 0 &&
        errno != EEXIST)
        return (false);
    return (true);
}

// sim_spawn_concord_net forks concord nodes with
// isolated XDG_CONFIG_HOME. Slots preset to
// -1; a partial failure unwinds the live
// prefix, so no child is orphaned.
bool sim_spawn_concord_net(const SimNet* net,
                           pid_t* pids,
                           int* logfds, int n) {
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (pids == nullptr || logfds == nullptr)
        return (false);
    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    if (!ensure_parent_net(net))
        return (false);
    for (i = 0; i < n; i++) {
        pids[i] = -1;
        logfds[i] = -1;
    }
    for (i = 0; i < n; i++) {
        if (!spawn_one_net(net, &pids[i],
                           &logfds[i], i, nullptr,
                           nullptr, 0)) {
            spawn_unwind(pids, logfds, i);
            return (false);
        }
    }
    return (true);
}

// sim_spawn_concord_skew_net forks like net spawn
// but runs node skew_node with CONCORD_CLOCK_OFFSET
// set to offset seconds. The offset env is the
// Concord test time hook; production must never
// set it. Returns false when skew_node is out of
// range or offset is nullptr.
bool sim_spawn_concord_skew_net(
    const SimNet* net, pid_t* pids, int* logfds,
    int n, int skew_node, const char* offset) {
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (pids == nullptr || logfds == nullptr)
        return (false);
    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    if (skew_node < 0 || skew_node >= n)
        return (false);
    if (offset == nullptr)
        return (false);
    if (!ensure_parent_net(net))
        return (false);
    for (i = 0; i < n; i++) {
        pids[i] = -1;
        logfds[i] = -1;
    }
    for (i = 0; i < n; i++) {
        const char* off;

        off = (i == skew_node) ? offset : nullptr;
        if (!spawn_one_net(net, &pids[i],
                           &logfds[i], i, off,
                           nullptr, 0)) {
            spawn_unwind(pids, logfds, i);
            return (false);
        }
    }
    return (true);
}

// sim_restart_concord_net wipes node i for a fresh
// identity and forks it again. Refuses when
// pids[i] is live or logfds[i] is open: the
// caller must have killed, reaped, and
// closed first, so a restart never orphans.
// Returns false for negative or huge i.
// Shares the spawn path, so the child exits
// rather than run in the host netns
// without its own netns.
bool sim_restart_concord_net(const SimNet* net,
                             pid_t* pids,
                             int* logfds, int i) {
    char dir[PATH_LEN];
    char cmd[SMALL_CMD];
    int m;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (pids == nullptr || logfds == nullptr)
        return (false);
    if (i < 0 || i >= TUN_MAP_MAX)
        return (false);
    if (pids[i] > 0)
        return (false);
    if (logfds[i] >= 0)
        return (false);
    if (!node_dir_net(net, dir, sizeof(dir),
                      i))
        return (false);
    m = snprintf(cmd, sizeof(cmd), "rm -rf %s",
                 dir);
    // A truncated rm -rf refuses: it could
    // wipe the wrong path.
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!run(cmd))
        return (false);
    return (spawn_one_net(net, &pids[i],
                          &logfds[i], i, nullptr,
                          nullptr, 0));
}

// sim_spawn_concord_anchor_net forks like net spawn
// but runs node anchor_node with --anchor and points
// every node config at it via anchors. Underlay
// anchor_octet:7946 must be the anchor address.
bool sim_spawn_concord_anchor_net(
    const SimNet* net, pid_t* pids, int* logfds,
    int n, int anchor_node) {
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (pids == nullptr || logfds == nullptr)
        return (false);
    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    if (anchor_node < 0 || anchor_node >= n)
        return (false);
    if (!ensure_parent_net(net))
        return (false);
    for (i = 0; i < n; i++) {
        pids[i] = -1;
        logfds[i] = -1;
    }
    for (i = 0; i < n; i++) {
        const char* arg;

        arg = (i == anchor_node) ? "--anchor"
                                 : nullptr;
        if (!spawn_one_net(net, &pids[i],
                           &logfds[i], i, nullptr,
                           arg, anchor_node + 1)) {
            spawn_unwind(pids, logfds, i);
            return (false);
        }
    }
    return (true);
}

// sim_spawn_concord_debug_net forks like net spawn
// but runs every node with debug logging.
// Voluminous: diagnosis only, never suite default.
bool sim_spawn_concord_debug_net(
    const SimNet* net, pid_t* pids, int* logfds,
    int n) {
    int i;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (pids == nullptr || logfds == nullptr)
        return (false);
    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    if (!ensure_parent_net(net))
        return (false);
    for (i = 0; i < n; i++) {
        pids[i] = -1;
        logfds[i] = -1;
    }
    for (i = 0; i < n; i++) {
        if (!spawn_one_net(net, &pids[i],
                           &logfds[i], i, nullptr,
                           "--log-level=debug",
                           0)) {
            spawn_unwind(pids, logfds, i);
            return (false);
        }
    }
    return (true);
}

// Legacy wrappers below use the default net and
// stay frozen for check/. New code must use the
// _net forms above with an explicit SimNet.

// sim_netns_setup creates one netns per extra node.
bool sim_netns_setup(int n) {
    SimNet net = sim_default_net();

    return (sim_netns_setup_net(&net, n));
}

// sim_netns_teardown deletes those netns.
bool sim_netns_teardown(int n) {
    SimNet net = sim_default_net();

    return (sim_netns_teardown_net(&net, n));
}

// sim_tuns_open opens tun0 in host, tun1.. in netns.
bool sim_tuns_open(int* fds, int n) {
    SimNet net = sim_default_net();

    return (sim_tuns_open_net(&net, fds, n));
}

// sim_nodes_add registers simulated nodes.
// Underlay 192.168.100.0/24 is disjoint from
// overlay 10.0.0.0/16.
bool sim_nodes_add(Context* ctx, TunMap* map, int* fds,
                   int n) {
    SimNet net = sim_default_net();

    return (sim_nodes_add_net(&net, ctx, map, fds,
                              n));
}

// sim_addrs_up assigns underlay IPs and brings links up.
bool sim_addrs_up(int n) {
    SimNet net = sim_default_net();

    return (sim_addrs_up_net(&net, n));
}

// sim_spawn_concord forks concord nodes with isolated
// XDG_CONFIG_HOME.
bool sim_spawn_concord(pid_t* pids, int* logfds, int n) {
    SimNet net = sim_default_net();

    return (sim_spawn_concord_net(&net, pids,
                                 logfds, n));
}

// sim_spawn_concord_skew forks like sim_spawn_concord
// but runs node skew_node with CONCORD_CLOCK_OFFSET
// set to offset seconds (e.g. "300"). The offset env
// is the Concord test time hook; production must never
// set it. Returns false when skew_node is out of
// range or offset is nullptr.
bool sim_spawn_concord_skew(pid_t* pids, int* logfds, int n,
                            int skew_node,
                            const char* offset) {
    SimNet net = sim_default_net();

    return (sim_spawn_concord_skew_net(
        &net, pids, logfds, n, skew_node, offset));
}

// sim_restart_concord wipes node i for a fresh identity
// and forks it again. Caller must have killed and reaped
// the old child and closed its logfd. Returns false
// for negative i.
bool sim_restart_concord(pid_t* pids, int* logfds, int i) {
    SimNet net = sim_default_net();

    return (sim_restart_concord_net(&net, pids,
                                   logfds, i));
}

// sim_spawn_concord_anchor forks like sim_spawn_concord
// but runs node anchor_node with --anchor and points
// every node config at it via anchors. Underlay
// 192.168.100.anchor_octet:7946 must be the anchor
// node's address.
bool sim_spawn_concord_anchor(pid_t* pids, int* logfds,
                              int n, int anchor_node) {
    SimNet net = sim_default_net();

    return (sim_spawn_concord_anchor_net(
        &net, pids, logfds, n, anchor_node));
}

// sim_spawn_concord_debug forks like sim_spawn_concord
// but runs every node with debug logging. Voluminous:
// diagnosis only, never the suite default.
bool sim_spawn_concord_debug(pid_t* pids, int* logfds,
                             int n) {
    SimNet net = sim_default_net();

    return (sim_spawn_concord_debug_net(
        &net, pids, logfds, n));
}
