#define _GNU_SOURCE
#include "run.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Poll wait per iteration, matching check/.
static constexpr int POLL_MS = 100;

// One TUN packet or log chunk per read.
static constexpr int CHUNK_LEN = 2048;

// Holds rm -rf plus the stored statedir.
static constexpr int CMD_LEN = 192;

// Holds one TUN name (<prefix><base>).
static constexpr int TUN_LEN = 64;

// TEARDOWN_KILL_S bounds the SIGTERM wait
// before SIGKILL. Teardown may run past
// DEADLINE by at most this per child.
static constexpr int TEARDOWN_KILL_S = 5;

// SLEEP_NS is the 100 ms waitpid poll
// slice in teardown_wait.
static constexpr int SLEEP_NS = 100'000'000;

// STAT_LEN caps one /proc stat read;
// the line stays far below this.
static constexpr int STAT_LEN = 1024;

// IO_LEN caps one /proc io read; the
// file stays far below this.
static constexpr int IO_LEN = 1024;

// PROC_LEN holds /proc/<pid>/stat or
// /proc/<pid>/io for any pid text.
static constexpr int PROC_LEN = 48;

// PAGE_FALLBACK is the rss page size
// when sysconf fails; 4 KB is the
// common Linux page.
static constexpr int PAGE_FALLBACK = 4096;

// Fixed arrays hold any valid n; poll
// vector holds TUN plus log halves.
static_assert(sizeof(((Run *)nullptr)->fds) /
              sizeof(int) == TUN_MAP_MAX);
static_assert(sizeof(((Run *)nullptr)->logfds) /
              sizeof(int) == TUN_MAP_MAX);
static_assert(sizeof(((Run *)nullptr)->pids) /
              sizeof(pid_t) == TUN_MAP_MAX);
static_assert(sizeof(((Run *)nullptr)->p) /
              sizeof(struct pollfd) ==
              2 * TUN_MAP_MAX);
static_assert(RUN_SAMPLE_CAP > 0);
static_assert(RUN_SAMPLE_GAP_MS > 0);
static_assert(TUN_MAP_MAX > 0);

// now_ms reports monotonic ms for the
// sampler gate. Zero on clock failure,
// which reads as due once, then the
// stored stamp gates the rest.
static unsigned long long now_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC,
                      &ts) != 0) {
        return (0);
    }
    return ((unsigned long long)ts.tv_sec *
                1000ULL +
            (unsigned long long)ts.tv_nsec /
                1000000ULL);
}

// teardown_wait SIGTERMs pid, waits up to
// TEARDOWN_KILL_S, then SIGKILLs and reaps.
// EINTR retries; bounded past DEADLINE by
// TEARDOWN_KILL_S plus the final reap.
static void teardown_wait(pid_t pid) {
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

        w = waitpid(pid, nullptr, WNOHANG);
        if (w == pid)
            return;
        if (w < 0 && errno == ECHILD)
            return;
        if (time(nullptr) - t0 >=
            TEARDOWN_KILL_S)
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

// host_down_net deletes the host tun<base>
// device plus its addr and multicast route
// after a failed host setup, so a retry
// finds no linger. Best effort: missing
// devices fail silently. No-op for
// isolated nets, which own no host device,
// and for invalid nets, which never
// touched the host.
static void host_down_net(const SimNet* net) {
    char tun[TUN_LEN];
    char cmd[CMD_LEN];
    unsigned a, b, c;
    long v;
    int m;

    if (net == nullptr)
        return;
    if (net->isolate_all != 0)
        return;
    if (!sim_net_valid(net))
        return;
    v = (long)net->tun_base;
    m = snprintf(tun, sizeof(tun),
                 "%s%ld", net->tun_prefix,
                 v);
    if (m < 0 || m >= (int)sizeof(tun))
        return;
    a = net->under[0];
    b = net->under[1];
    c = net->under[2];
    m = snprintf(cmd, sizeof(cmd),
                 "ip route del 224.0.0.0/4 "
                 "dev %s 2>/dev/null",
                 tun);
    if (m >= 0 && m < (int)sizeof(cmd))
        (void)system(cmd);
    m = snprintf(cmd, sizeof(cmd),
                 "ip addr del %u.%u.%u.1/24 "
                 "dev %s 2>/dev/null",
                 a, b, c, tun);
    if (m >= 0 && m < (int)sizeof(cmd))
        (void)system(cmd);
    m = snprintf(cmd, sizeof(cmd),
                 "ip link del %s 2>/dev/null",
                 tun);
    if (m >= 0 && m < (int)sizeof(cmd))
        (void)system(cmd);
}

// setup_unwind closes fresh TUNs, deletes
// the host tun plus its addr and route on
// the host path, frees ctx plus the sample
// store, and deletes the netns after setup
// failure. Invalid nets skip host deletes,
// so a bad-input unwind leaves the host
// untouched.
static void setup_unwind(Run* r,
                         const SimNet* net, int n) {
    int i;

    for (i = 0; i < n; i++) {
        if (r->fds[i] >= 0) {
            close(r->fds[i]);
            r->fds[i] = -1;
        }
    }
    host_down_net(net);
    context_free(&r->ctx);
    free(r->shist);
    r->shist = nullptr;
    sim_netns_teardown_net(net, n);
}

// setup_init stores n/dead/net, zeroes map,
// presets fds/logfds/pids, clears sampler
// store plus peaks plus throttle, clears
// done, and inits ctx. False on ctx fail.
static bool setup_init(Run* r,
                       const SimNet* net, int n) {
    int i;

    r->n = n;
    r->dead = -1;
    r->done = false;
    r->net = *net;
    r->map = (TunMap){};
    for (i = 0; i < n; i++) {
        r->fds[i] = -1;
        r->logfds[i] = -1;
        r->pids[i] = -1;
    }
    for (i = 0; i < TUN_MAP_MAX; i++) {
        r->shead[i] = 0;
        r->scount[i] = 0;
        r->sdropped[i] = 0;
        r->speak[i] = 0;
        r->sutime[i] = 0;
        r->sstime[i] = 0;
    }
    r->slast_ms = 0;
    r->shist = calloc(
        (size_t)TUN_MAP_MAX *
            (size_t)RUN_SAMPLE_CAP,
        sizeof(RunSample));
    if (r->shist == nullptr) {
        return (false);
    }
    if (!context_init(&r->ctx, 32, 1)) {
        free(r->shist);
        r->shist = nullptr;
        return (false);
    }
    return (true);
}

// setup_poll publishes fds into the TUN half
// and parks the log half at -1 until spawn.
static void setup_poll(Run* r) {
    int i;
    int n;

    n = r->n;
    for (i = 0; i < n; i++) {
        r->p[i].fd = r->fds[i];
        r->p[i].events = POLLIN;
        r->p[i].revents = 0;
        r->p[n + i].fd = -1;
        r->p[n + i].events = POLLIN;
        r->p[n + i].revents = 0;
    }
}

// stat_fields parses utime/stime/rss
// from text past the comm paren. Tokens
// after paren count 1..22; 12/13 are
// utime/stime ticks, 22 is rss pages.
// False on short or bad numbers.
static bool stat_fields(const char *s,
                        unsigned long long *ut,
                        unsigned long long *st,
                        long *rss) {
    int tok;
    const char *p;

    if (s == nullptr || ut == nullptr ||
        st == nullptr || rss == nullptr)
        return (false);
    tok = 0;
    p = s;
    while (*p != '\0' && tok < 22) {
        char *e;
        unsigned long long v;
        long w;

        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        tok++;
        if (tok != 12 && tok != 13 &&
            tok != 22) {
            while (*p != '\0' &&
                   *p != ' ' && *p != '\t')
                p++;
            continue;
        }
        e = nullptr;
        if (tok == 22) {
            w = strtol(p, &e, 10);
            if (e == p)
                return (false);
            *rss = w;
            p = e;
            continue;
        }
        v = strtoull(p, &e, 10);
        if (e == p)
            return (false);
        if (tok == 12)
            *ut = v;
        else
            *st = v;
        p = e;
    }
    if (tok < 22)
        return (false);
    return (true);
}

// io_fields fills rchar/wchar bytes
// from /proc io text; missing keys
// stay 0, bad numbers skip silently.
static void io_fields(const char *s,
                      unsigned long long *rc,
                      unsigned long long *wc) {
    const char *p;
    const char *q;
    char *e;

    if (s == nullptr || rc == nullptr ||
        wc == nullptr)
        return;
    p = strstr(s, "rchar:");
    if (p != nullptr) {
        e = nullptr;
        *rc = strtoull(p + 6, &e, 10);
        if (e == p + 6)
            *rc = 0;
    }
    q = strstr(s, "wchar:");
    if (q != nullptr) {
        e = nullptr;
        *wc = strtoull(q + 6, &e, 10);
        if (e == q + 6)
            *wc = 0;
    }
}

// sample_one snapshots pids[i] into the
// store; dead, bad, missing, full, or
// parse fails skip the append silently,
// never fail. Past full, peak plus cpu
// plus latest still refresh and the
// pass counts as dropped, so kept
// slots never move.
static void sample_one(Run *r, int i,
                       unsigned long long now) {
    char path[PROC_LEN];
    char buf[STAT_LEN];
    char iob[IO_LEN];
    unsigned long long ut;
    unsigned long long st;
    unsigned long long rb;
    unsigned long long rc;
    unsigned long long wc;
    RunSample s;
    long pages;
    long ps;
    pid_t pid;
    int fd;
    int m;
    int h;

    if (r == nullptr)
        return;
    if (i < 0 || i >= r->n)
        return;
    if (i >= TUN_MAP_MAX)
        return;
    if (i == r->dead)
        return;
    pid = r->pids[i];
    if (pid <= 0)
        return;
    m = snprintf(path, sizeof(path),
                 "/proc/%ld/stat",
                 (long)pid);
    if (m < 0 ||
        m >= (int)sizeof(path))
        return;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    {
        ssize_t k;

        k = read(fd, buf,
                 sizeof(buf) - 1);
        close(fd);
        if (k <= 0)
            return;
        buf[k] = '\0';
    }
    {
        char *e;

        e = strrchr(buf, ')');
        if (e == nullptr)
            return;
        ut = 0;
        st = 0;
        pages = 0;
        if (!stat_fields(e + 1, &ut,
                         &st, &pages))
            return;
    }
    ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0)
        ps = PAGE_FALLBACK;
    rb = 0;
    if (pages > 0)
        rb = (unsigned long long)pages *
             (unsigned long long)ps;
    rc = 0;
    wc = 0;
    m = snprintf(path, sizeof(path),
                 "/proc/%ld/io",
                 (long)pid);
    if (m >= 0 &&
        m < (int)sizeof(path)) {
        int f;

        f = open(path,
                 O_RDONLY | O_CLOEXEC);
        if (f >= 0) {
            ssize_t k;

            k = read(f, iob,
                     sizeof(iob) - 1);
            close(f);
            if (k > 0) {
                iob[k] = '\0';
                io_fields(iob, &rc,
                          &wc);
            }
        }
    }
    s.pid = pid;
    s.utime = ut;
    s.stime = st;
    s.rss_bytes = rb;
    s.rchar = rc;
    s.wchar = wc;
    s.at = (time_t)(now / 1000ULL);
    s.valid = true;
    if (r->shist == nullptr) {
        r->sdropped[i]++;
    } else if (r->scount[i] <
               RUN_SAMPLE_CAP) {
        h = r->shead[i];
        if (h < 0 ||
            h >= RUN_SAMPLE_CAP) {
            h = r->scount[i];
        }
        r->shist[i][h] = s;
        r->shead[i] = h + 1;
        r->scount[i]++;
    } else {
        r->shist[i][r->scount[i] - 1] = s;
        r->sdropped[i]++;
    }
    if (rb > r->speak[i])
        r->speak[i] = rb;
    r->sutime[i] = ut;
    r->sstime[i] = st;
}

// sample_pass snapshots each live pid;
// force skips the 250 ms gate for
// the teardown final. Missing procs
// skip silently, never fail.
static void sample_pass(Run *r,
                        bool force) {
    unsigned long long now;
    int i;

    if (r == nullptr)
        return;
    if (r->n < 1 || r->n > TUN_MAP_MAX)
        return;
    now = now_ms();
    if (!force) {
        if (r->slast_ms != 0 &&
            now - r->slast_ms <
                (unsigned long long)
                    RUN_SAMPLE_GAP_MS)
            return;
    }
    r->slast_ms = now;
    for (i = 0; i < r->n; i++)
        sample_one(r, i, now);
}

// poll_once waits ms, discards dead TUN,
// pumps or drops each ready TUN, and hands
// each ready log chunk to log.
static int poll_once(Run* r, RunDrop drop,
                     RunLog log, void* ud,
                     int ms) {
    char buf[CHUNK_LEN];
    int hit;
    int i;

    if (r == nullptr)
        return (-1);
    if (ms < 0)
        ms = 0;
    hit = poll(r->p, (nfds_t)(2 * r->n),
               ms);
    if (hit > 0) {
        for (i = 0; i < r->n; i++) {
            bool d;

            if (!(r->p[i].revents &
                  POLLIN))
                continue;
            if (i == r->dead) {
                ssize_t k;

                k = read(r->fds[i], buf,
                         sizeof(buf));
                (void)k;
                continue;
            }
            d = false;
            if (drop != nullptr)
                d = drop(ud, i);
            if (d) {
                ssize_t k;

                k = read(r->fds[i], buf,
                         sizeof(buf));
                (void)k;
                continue;
            }
            tun_pump_fd(&r->map,
                        r->fds[i],
                        &r->ctx.grid,
                        &r->ctx.radio,
                        &r->ctx.nodes);
        }
        for (i = 0; i < r->n; i++) {
            ssize_t k;

            if (!(r->p[r->n + i].revents &
                  POLLIN))
                continue;
            if (r->logfds[i] < 0)
                continue;
            k = read(r->logfds[i], buf,
                     sizeof(buf) - 1);
            if (k <= 0)
                continue;
            buf[k] = '\0';
            if (log != nullptr)
                log(ud, i, buf,
                    (size_t)k);
        }
    }
    sample_pass(r, false);
    return (hit);
}

// run_setup_host builds ctx, netns, TUNs,
// nodes, and addrs for n nodes. Spawn stays
// driver side: after RUN_OK the driver
// spawns into pids/logfds, then calls
// run_rearm per node. TUN failure is a
// skip; the rest is a fail. Partial setup
// unwinds what was made, incl. the host
// tun plus its addr and route.
// Intentional host parity: node 0 stays in
// the host netns for check/ parity and
// single runs debuggable from the host.
// Requires net isolate_all 0; nonzero
// fails with RUN_FAIL and no side effects.
RunResult run_setup_host(Run* r,
                         const SimNet* net,
                         int n) {
    int e;

    if (r == nullptr || net == nullptr)
        return (RUN_FAIL);
    if (n < 1 || n > TUN_MAP_MAX)
        return (RUN_FAIL);
    if (net->isolate_all != 0)
        return (RUN_FAIL);
    if (!setup_init(r, net, n))
        return (RUN_FAIL);
    if (!sim_netns_setup_net(net, n)) {
        setup_unwind(r, net, n);
        return (RUN_FAIL);
    }
    if (!sim_tuns_open_net(net, r->fds, n)) {
        e = errno;
        setup_unwind(r, net, n);
        errno = e;
        return (RUN_SKIP);
    }
    if (!sim_nodes_add_net(net, &r->ctx, &r->map,
                           r->fds, n)) {
        setup_unwind(r, net, n);
        return (RUN_FAIL);
    }
    if (!sim_addrs_up_net(net, n)) {
        setup_unwind(r, net, n);
        return (RUN_FAIL);
    }
    setup_poll(r);
    return (RUN_OK);
}

// run_setup_isolated mirrors run_setup_host on
// the _all path: netns 0..n-1, every TUN
// inside its netns, no host tun0/addr/route.
// Same verdicts, skip errno, and unwind.
// Isolated runs with distinct nets share
// only the allocator file. Requires net
// isolate_all nonzero; 0 fails with RUN_FAIL
// and no side effects. Keeps explicit four
// steps: a single-bool sim helper would
// lose the TUN skip versus fail split,
// the skip errno, and the heavy unwind,
// so the duplication stays here on purpose.
RunResult run_setup_isolated(Run* r,
                             const SimNet* net,
                             int n) {
    int e;

    if (r == nullptr || net == nullptr)
        return (RUN_FAIL);
    if (n < 1 || n > TUN_MAP_MAX)
        return (RUN_FAIL);
    if (net->isolate_all == 0)
        return (RUN_FAIL);
    if (!setup_init(r, net, n))
        return (RUN_FAIL);
    if (!sim_netns_setup_all(net, n)) {
        setup_unwind(r, net, n);
        return (RUN_FAIL);
    }
    if (!sim_tuns_open_all(net, r->fds, n)) {
        e = errno;
        setup_unwind(r, net, n);
        errno = e;
        return (RUN_SKIP);
    }
    if (!sim_nodes_add_net(net, &r->ctx, &r->map,
                           r->fds, n)) {
        setup_unwind(r, net, n);
        return (RUN_FAIL);
    }
    if (!sim_addrs_up_all(net, n)) {
        setup_unwind(r, net, n);
        return (RUN_FAIL);
    }
    setup_poll(r);
    return (RUN_OK);
}

// run_poll waits 100 ms, pumps or drops each
// ready TUN per drop, and hands each ready log
// chunk to log. Ticks the sampler at most
// once per 100 ms. Returns what poll returned.
// Drop and pump errors stay silent.
int run_poll(Run* r, RunDrop drop, RunLog log,
             void* ud) {
    return (poll_once(r, drop, log, ud,
                      POLL_MS));
}

// run_poll_ms waits at most ms, then acts
// exactly like run_poll. Lets paced loops
// drain while waiting without overshooting
// their period. ms below 0 reads as 0.
int run_poll_ms(Run* r, RunDrop drop, RunLog log,
                void* ud, int ms) {
    return (poll_once(r, drop, log, ud,
                      ms));
}

// run_hold arms t0 on first call, fires secs
// later. Same semantics as the hold copies in
// check/.
bool run_hold(time_t* t0, int secs) {
    if (t0 == nullptr)
        return (false);
    if (*t0 == 0) {
        *t0 = time(nullptr);
        return (false);
    }
    return (time(nullptr) - *t0 >= secs);
}

// run_throttle reports true at most once per
// secs: the 2 s CLI query rhythm. The first
// call always fires.
bool run_throttle(time_t* t, int secs) {
    if (t == nullptr)
        return (false);
    if (*t != 0 && time(nullptr) - *t < secs)
        return (false);
    *t = time(nullptr);
    return (true);
}

// run_alive reports any non-skipped child still
// running. skip names one dead-by-fault index
// to ignore, or -1 for none. Slots observed
// reaped (or already gone) clear to -1, so
// teardown never kills a reused pid. Other
// wait errors assume alive.
bool run_alive(Run* r, int skip) {
    int i;

    if (r == nullptr)
        return (false);
    for (i = 0; i < r->n; i++) {
        pid_t pid;
        int w;

        if (i == skip)
            continue;
        pid = r->pids[i];
        if (pid <= 0)
            continue;
        w = waitpid(pid, nullptr, WNOHANG);
        if (w == 0)
            return (true);
        if (w > 0 || errno == ECHILD) {
            r->pids[i] = -1;
            continue;
        }
        return (true);
    }
    return (false);
}

// run_note_dead records i as dead by fault so
// teardown skips it and poll quarantines its
// TUN. Only one slot holds; a second note
// keeps the first. The driver kills, reaps,
// and closes the logfd first, as the failover
// kill site does; a still-open logfd closes
// here, so future noters never leak.
void run_note_dead(Run* r, int i) {
    if (r == nullptr)
        return;
    if (i < 0 || i >= r->n)
        return;
    // One stay-dead slot; keep the first.
    if (r->dead >= 0)
        return;
    r->dead = i;
    if (r->logfds[i] >= 0) {
        close(r->logfds[i]);
        r->logfds[i] = -1;
    }
    r->p[r->n + i].fd = -1;
}

// run_rearm publishes logfds[i] into the log
// poll half after driver spawn or restart. A
// matching dead mark clears: it lives again.
void run_rearm(Run* r, int i) {
    if (r == nullptr)
        return;
    if (i < 0 || i >= r->n)
        return;
    r->p[r->n + i].fd = r->logfds[i];
    r->p[r->n + i].events = POLLIN;
    if (r->dead == i)
        r->dead = -1;
}

// run_teardown SIGTERMs and reaps children,
// closes TUNs and logs, runs forensics when
// failed, frees ctx plus the sample store,
// deletes the netns, then wipes the stored
// statedir. Wait loops on EINTR and escalates
// to SIGKILL after 5 s, so teardown may run
// past DEADLINE by at most 5 s per child.
// The dead index keeps its TUN close only.
// Clears every handled pid and fd to -1 and
// sets done, so a second call is a no-op and
// forensics run once: call once after RUN_OK.
// Takes a final sampler pass first, so
// peaks and cpu totals stay readable.
// Revalidates the net before rm -rf and
// refuses on invalid.
void run_teardown(Run* r, bool failed,
                  RunForensics forensics, void* ud) {
    char cmd[CMD_LEN];
    int i;
    int k;

    if (r == nullptr)
        return;
    if (r->done)
        return;
    r->done = true;
    sample_pass(r, true);
    for (i = 0; i < r->n; i++) {
        if (i == r->dead) {
            if (r->fds[i] >= 0) {
                close(r->fds[i]);
                r->fds[i] = -1;
            }
            continue;
        }
        // >0 only: kill(-1) hits every proc.
        if (r->pids[i] > 0) {
            teardown_wait(r->pids[i]);
            r->pids[i] = -1;
        }
        if (r->fds[i] >= 0) {
            close(r->fds[i]);
            r->fds[i] = -1;
        }
        if (r->logfds[i] >= 0) {
            close(r->logfds[i]);
            r->logfds[i] = -1;
        }
    }
    if (failed && forensics != nullptr)
        forensics(ud);
    context_free(&r->ctx);
    free(r->shist);
    r->shist = nullptr;
    sim_netns_teardown_net(&r->net, r->n);
    if (!sim_net_valid(&r->net))
        return;
    k = snprintf(cmd, sizeof(cmd),
                 "rm -rf %s",
                 r->net.statedir);
    if (k < 0 || k >= CMD_LEN)
        return;
    if (system(cmd) != 0) {
        // Best effort cleanup, teardown ran.
    }
}

// run_sample_hist copies back slots ago:
// 0 is latest, 1 prior, up to stored
// count - 1. False past stored count
// or bad args.
bool run_sample_hist(const Run* r, int node,
                     int back, RunSample* out) {
    int c;
    int idx;

    if (r == nullptr || out == nullptr)
        return (false);
    if (node < 0 || node >= r->n)
        return (false);
    if (node >= TUN_MAP_MAX)
        return (false);
    if (back < 0 || back >= RUN_SAMPLE_CAP)
        return (false);
    c = r->scount[node];
    if (c <= 0 || back >= c)
        return (false);
    idx = c - 1 - back;
    *out = r->shist[node][idx];
    if (!out->valid)
        return (false);
    return (true);
}

// run_sample_get copies the latest sample
// for node into out. False on bad node,
// empty history, or nullptr.
bool run_sample_get(const Run* r, int node,
                    RunSample* out) {
    return (run_sample_hist(r, node, 0,
                            out));
}

// run_sample_count reports stored samples
// for node, 0..CAP. Bad node reports 0.
int run_sample_count(const Run* r,
                     int node) {
    int c;

    if (r == nullptr)
        return (0);
    if (node < 0 || node >= r->n)
        return (0);
    if (node >= TUN_MAP_MAX)
        return (0);
    c = r->scount[node];
    if (c < 0)
        return (0);
    if (c > RUN_SAMPLE_CAP)
        return (RUN_SAMPLE_CAP);
    return (c);
}

// run_sample_dropped reports passes past
// full for node. Bad node reports 0.
int run_sample_dropped(const Run* r,
                       int node) {
    int d;

    if (r == nullptr)
        return (0);
    if (node < 0 || node >= r->n)
        return (0);
    if (node >= TUN_MAP_MAX)
        return (0);
    d = r->sdropped[node];
    if (d < 0)
        return (0);
    return (d);
}

// run_sample_peak reports peak rss in
// bytes for node. False when empty or
// bad args.
bool run_sample_peak(const Run* r, int node,
                     unsigned long long* out) {
    if (r == nullptr || out == nullptr)
        return (false);
    if (node < 0 || node >= r->n)
        return (false);
    if (node >= TUN_MAP_MAX)
        return (false);
    if (r->scount[node] <= 0)
        return (false);
    *out = r->speak[node];
    return (true);
}

// run_sample_cpu reports last utime plus
// stime totals in clock ticks. False
// when empty or bad args.
bool run_sample_cpu(const Run* r, int node,
                    unsigned long long* ut,
                    unsigned long long* st) {
    if (r == nullptr || ut == nullptr ||
        st == nullptr)
        return (false);
    if (node < 0 || node >= r->n)
        return (false);
    if (node >= TUN_MAP_MAX)
        return (false);
    if (r->scount[node] <= 0)
        return (false);
    *ut = r->sutime[node];
    *st = r->sstime[node];
    return (true);
}

// run_sample_tick snapshots each live
// child at most once per 250 ms. Best
// effort: missing /proc, parse fails,
// full store, and non-Linux skip
// silently. run_poll calls it, so no
// caller help is needed; call by hand
// only to tick in tests.
void run_sample_tick(Run* r) {
    sample_pass(r, false);
}
