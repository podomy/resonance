#define _GNU_SOURCE
#include "driver/concord.h"
#include "sim/sim.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Per-node home: statedir plus /node<i>.
static constexpr int DIR_LEN = 160;

// Journal and store paths under the home.
static constexpr int FILE_LEN = 224;

// Shell lines with statedir plus verbs.
static constexpr int CMD_LEN = 512;

// Netns path: prefix plus index.
static constexpr int NS_LEN = 128;

// One CLI output dump, as in check/.
static constexpr int OUT_LEN = 16384;

// One CLI output line, as in check/.
static constexpr int LINE_LEN = 1024;

// UUID text plus NUL.
static constexpr int ID_LEN = 64;

// PID_LEN holds a pid string plus NUL.
static constexpr int PID_LEN = 32;

// BIN_LEN caps a bin execl path copy.
static constexpr int BIN_LEN = 256;

// RES_LEN caps resolved submit flags
// holding <int>%host expansions.
static constexpr int RES_LEN = 512;

// OPT_LEN caps the tmpfs size option.
static constexpr int OPT_LEN = 64;

// MEM_LINE caps one meminfo line.
static constexpr int MEM_LINE = 256;

// NUM_LEN caps one resolved MB number.
static constexpr int NUM_LEN = 32;

// PCT_MAX caps the <int> in <int>%host:
// larger percents fail as malformed.
static constexpr int PCT_MAX = 10000;

// BIN_DEFAULT is the daemon path when
// bin is null, matching reexec_net.
static const char BIN_DEFAULT[] =
    "./bin/concord";

// g_bin_override is the test binary
// override, null means default.
// Process-global, not thread-safe:
// single-threaded harness only.
// Caller keeps the string alive.
static const char* g_bin_override =
    nullptr;

// JUNK is the exact journal garbage:
// CORRUPT plus newline, 8 bytes.
static const char JUNK[] = "CORRUPT\n";

// NIL_ID is the unassigned marker the
// journal writes for empty assignments.
static const char NIL_ID[] =
    "00000000-0000-0000-0000-000000000000";

// CG_CTL enables controllers on each new
// cgroup level, mirroring sim.
static const char CG_CTL[] =
    "+cpu +memory +pids";

// mark_cloexec sets FD_CLOEXEC on fd,
// mirroring sim cloexec_fd: forked
// shells and daemons never inherit
// the log pipe.
static void mark_cloexec(int fd) {
    int f;

    f = fcntl(fd, F_GETFD);
    if (f < 0)
        return;
    (void)fcntl(fd, F_SETFD,
                f | FD_CLOEXEC);
}

// KILL_WAIT_S bounds the TERM wait
// before KILL, mirroring run's 5 s
// teardown escalation.
static constexpr int KILL_WAIT_S = 5;

// CLI_TIMEOUT_S bounds one CLI shell
// plus journal grep: a hung binary
// counts as failure, never a stall.
// 10 s class, matching weld waits.
static constexpr int CLI_TIMEOUT_S = 10;

// CLI_POLL_MS steps CLI waits, as in
// weld pub plus run poll.
static constexpr int CLI_POLL_MS = 100;

// SLEEP_NS is the 100 ms reap poll
// slice, as in run teardown.
static constexpr int SLEEP_NS = 100000000;

// mono_s returns CLOCK_MONOTONIC
// seconds for bounded waits, so wall
// jumps never stretch them. Matches
// weld mono_s.
static time_t mono_s(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC,
                      &ts) != 0)
        return (0);
    return (ts.tv_sec);
}

// reap_bounded waits up to timeout_s
// for pid, then SIGKILLs and reaps.
// Monotonic clock, so wall jumps
// never stretch it. Final reap after
// KILL blocks with EINTR retry, as in
// run teardown: KILL implies exit.
static void reap_bounded(pid_t pid,
                         int timeout_s) {
    time_t end;
    struct timespec nap;

    if (pid <= 0)
        return;
    end = mono_s() + timeout_s;
    for (;;) {
        pid_t w;

        w = waitpid(pid, nullptr,
                    WNOHANG);
        if (w == pid)
            return;
        if (w < 0 && errno == ECHILD)
            return;
        if (w < 0 && errno != EINTR)
            break;
        if (mono_s() >= end)
            break;
        nap.tv_sec = 0;
        nap.tv_nsec = SLEEP_NS;
        nanosleep(&nap, nullptr);
    }
    kill(pid, SIGKILL);
    while (waitpid(pid, nullptr, 0) <
           0 &&
           errno == EINTR) {
    }
}

// shell_status runs cmd via /bin/sh
// with a bounded wait. True only on
// exit 0; timeout kills and is false.
// Diverges from check/ system: driver
// never stalls on a hung shell.
static bool shell_status(const char* cmd,
                         int timeout_s) {
    pid_t pid;
    time_t end;
    int st;

    pid = fork();
    if (pid < 0)
        return (false);
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c",
              cmd, (char*)nullptr);
        _exit(127);
    }
    end = mono_s() + timeout_s;
    for (;;) {
        pid_t w;
        struct timespec nap;

        w = waitpid(pid, &st,
                    WNOHANG);
        if (w == pid)
            break;
        if (w < 0 && errno != EINTR) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (false);
        }
        if (mono_s() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (false);
        }
        nap.tv_sec = 0;
        nap.tv_nsec = SLEEP_NS;
        nanosleep(&nap, nullptr);
    }
    if (!WIFEXITED(st))
        return (false);
    return (WEXITSTATUS(st) == 0);
}

// quiet_status runs cmd via /bin/sh
// with child output to /dev/null and
// a bounded wait. True only on exit
// 0; timeout kills and is false. Like
// shell_status but discards output so
// exit-only callers stay quiet, as
// exec update drains its pipe.
static bool quiet_status(const char* cmd,
                         int timeout_s) {
    pid_t pid;
    time_t end;
    int st;

    pid = fork();
    if (pid < 0)
        return (false);
    if (pid == 0) {
        int fd;

        fd = open("/dev/null", O_WRONLY);
        if (fd < 0)
            _exit(127);
        if (dup2(fd, 1) < 0)
            _exit(127);
        if (dup2(fd, 2) < 0)
            _exit(127);
        if (fd > 2)
            close(fd);
        execl("/bin/sh", "sh", "-c",
              cmd, (char*)nullptr);
        _exit(127);
    }
    end = mono_s() + timeout_s;
    for (;;) {
        pid_t w;
        struct timespec nap;

        w = waitpid(pid, &st,
                    WNOHANG);
        if (w == pid)
            break;
        if (w < 0 && errno != EINTR) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (false);
        }
        if (mono_s() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (false);
        }
        nap.tv_sec = 0;
        nap.tv_nsec = SLEEP_NS;
        nanosleep(&nap, nullptr);
    }
    if (!WIFEXITED(st))
        return (false);
    return (WEXITSTATUS(st) == 0);
}

// capture_bounded runs cmd via /bin/sh
// and stores stdout into buf. True on
// reap, even on nonzero exit: callers
// scan output as check/ does. Timeout
// kills and is false. Truncates past
// n-1 bytes, as check/ fread caps at
// OUT_LEN. Pipe ends go cloexec, as in
// sim.
static bool capture_bounded(
    const char* cmd, char* buf, size_t n,
    int timeout_s) {
    int pipedes[2];
    pid_t pid;
    time_t end;
    size_t len;
    int st;

    if (n == 0)
        return (false);
    if (pipe(pipedes) != 0)
        return (false);
    mark_cloexec(pipedes[0]);
    mark_cloexec(pipedes[1]);
    pid = fork();
    if (pid < 0) {
        close(pipedes[0]);
        close(pipedes[1]);
        return (false);
    }
    if (pid == 0) {
        if (dup2(pipedes[1], 1) < 0)
            _exit(127);
        close(pipedes[0]);
        close(pipedes[1]);
        execl("/bin/sh", "sh", "-c",
              cmd, (char*)nullptr);
        _exit(127);
    }
    close(pipedes[1]);
    len = 0;
    end = mono_s() + timeout_s;
    for (;;) {
        pid_t w;
        struct pollfd pfd;
        ssize_t k;
        struct timespec nap;

        w = waitpid(pid, &st,
                    WNOHANG);
        if (w == pid)
            break;
        if (w < 0 && errno != EINTR) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            close(pipedes[0]);
            return (false);
        }
        if (mono_s() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            close(pipedes[0]);
            return (false);
        }
        pfd.fd = pipedes[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1,
                 CLI_POLL_MS) <= 0) {
            nap.tv_sec = 0;
            nap.tv_nsec = SLEEP_NS;
            nanosleep(&nap, nullptr);
            continue;
        }
        if (!(pfd.revents & POLLIN))
            continue;
        if (len >= n - 1) {
            char drop[256];
            k = read(pipedes[0], drop,
                     sizeof(drop));
            if (k < 0 &&
                errno == EINTR)
                continue;
            continue;
        }
        k = read(pipedes[0], buf + len,
                 n - len - 1);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            continue;
        len += (size_t)k;
    }
    for (;;) {
        struct pollfd pfd;
        ssize_t k;

        pfd.fd = pipedes[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1,
                 CLI_POLL_MS) <= 0)
            break;
        if (!(pfd.revents & POLLIN))
            break;
        if (len >= n - 1)
            break;
        k = read(pipedes[0], buf + len,
                 n - len - 1);
        if (k <= 0)
            break;
        len += (size_t)k;
    }
    close(pipedes[0]);
    buf[len] = '\0';
    return (true);
}

// kill_reap_close sends sig, reaps, and
// closes the logfd. Restart paths call it
// before reforking; concord_kill owns
// its own kill so it can report.
// Bounded by KILL_WAIT_S then KILL, as
// in run teardown: diverges from
// check/ blocking reap on purpose, so
// a TERM hang never stalls.
// Swallowed kill errors stay benign
// here: the slot is reaped and reforked
// right after, as in weld restart.
static void kill_reap_close(Run* r, int i,
                            int sig) {
    if (r->pids[i] > 0) {
        kill(r->pids[i], sig);
        reap_bounded(r->pids[i],
                     KILL_WAIT_S);
        r->pids[i] = -1;
    }
    if (r->logfds[i] >= 0) {
        close(r->logfds[i]);
        r->logfds[i] = -1;
    }
}

// write_file stores s into path. True on
// success.
static bool write_file(const char* path,
                       const char* s) {
    FILE* fp;

    fp = fopen(path, "w");
    if (fp == nullptr)
        return (false);
    if (fputs(s, fp) == EOF) {
        fclose(fp);
        return (false);
    }
    return (fclose(fp) == 0);
}

// cg_node writes <cgroot>/node<i>. False
// on truncation.
static bool cg_node(const SimNet* net,
                    char* buf, size_t n,
                    int i) {
    int m;

    if (net == nullptr)
        return (false);
    m = snprintf(buf, n, "%s/node%d",
                 net->cgroot, i);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// cgroup_place moves pid into
// <cgroot>/node<i>/daemon, creating the
// subtree as needed. Mirrors sim's place:
// the daemon sits one level deep so the
// per-node parent stays empty for
// containers. False on any failure.
static bool cgroup_place(const SimNet* net,
                         int i, pid_t pid) {
    char sub[FILE_LEN];
    char leaf[FILE_LEN];
    char target[FILE_LEN];
    char spid[PID_LEN];
    int m;

    if (net == nullptr)
        return (false);
    if (mkdir(net->cgroot, 0755) != 0 &&
        errno != EEXIST)
        return (false);
    m = snprintf(target, sizeof(target),
                 "%s/cgroup.subtree_control",
                 net->cgroot);
    if (m < 0 ||
        m >= (int)sizeof(target))
        return (false);
    if (!write_file(target, CG_CTL))
        return (false);
    if (!cg_node(net, sub, sizeof(sub), i))
        return (false);
    if (mkdir(sub, 0755) != 0 &&
        errno != EEXIST)
        return (false);
    m = snprintf(target, sizeof(target),
                 "%s/cgroup.subtree_control",
                 sub);
    if (m < 0 ||
        m >= (int)sizeof(target))
        return (false);
    if (!write_file(target, CG_CTL))
        return (false);
    m = snprintf(leaf, sizeof(leaf),
                 "%s/daemon", sub);
    if (m < 0 || m >= (int)sizeof(leaf))
        return (false);
    if (mkdir(leaf, 0755) != 0 &&
        errno != EEXIST)
        return (false);
    m = snprintf(target, sizeof(target),
                 "%s/cgroup.procs", leaf);
    if (m < 0 ||
        m >= (int)sizeof(target))
        return (false);
    m = snprintf(spid, sizeof(spid), "%d",
                 (int)pid);
    if (m < 0 || m >= (int)sizeof(spid))
        return (false);
    return (write_file(target, spid));
}

// cgroup_enter moves pid into its per-node
// cgroup. Best effort only: warns and
// still succeeds when unavailable, so a
// restart never fails here. check/
// originals vary; sim warns the same way.
static bool cgroup_enter(const SimNet* net,
                         int i, pid_t pid) {
    if (!cgroup_place(net, i, pid))
        printf("concord: no per-node "
               "cgroup, containers may "
               "collide\n");
    return (true);
}

// flags_ok reports true when s holds only
// workload-run extras: [A-Za-z0-9_=., /-].
// Anything else might be a shell
// metacharacter on the popen line, so the
// caller must reject it.
static bool flags_ok(const char* s) {
    size_t k;

    if (s == nullptr)
        return (false);
    for (k = 0; s[k] != '\0'; k++) {
        char ch = s[k];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '=' ||
            ch == '.' || ch == ',' ||
            ch == ' ' || ch == '/' ||
            ch == '-')
            continue;
        return (false);
    }
    return (true);
}

// image_ok reports true when s holds
// only image refs: alnum plus _ . : /
// plus @ + , -. Anything else might
// be a shell metachar on the popen
// line, so the caller must reject
// it. Union of suite exec plus task
// charset; both stay shell-safe.
static bool image_ok(const char* s) {
    size_t k;

    if (s == nullptr || s[0] == '\0')
        return (false);
    for (k = 0; s[k] != '\0'; k++) {
        char ch = s[k];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '.' ||
            ch == ':' || ch == '/' ||
            ch == '@' || ch == '+' ||
            ch == ',' || ch == '-')
            continue;
        return (false);
    }
    return (true);
}

// statedir_ok reports true when s is
// a shell-safe statedir: non-empty,
// only [A-Za-z0-9/_.-], as in sim
// field_ok. Statedir reaches system
// plus popen shells, so reject else.
static bool statedir_ok(const char* s) {
    size_t k;

    if (s == nullptr || s[0] == '\0')
        return (false);
    for (k = 0; s[k] != '\0'; k++) {
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

// wid_ok reports true when s is a
// workload id: non-empty hex plus -
// only. Wid reaches journal greps in
// a shell, so reject metachars.
static bool wid_ok(const char* s) {
    size_t k;

    if (s == nullptr || s[0] == '\0')
        return (false);
    for (k = 0; s[k] != '\0'; k++) {
        char ch = s[k];

        if ((ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f') ||
            (ch >= 'A' && ch <= 'F') ||
            ch == '-')
            continue;
        return (false);
    }
    return (true);
}

// bin_path resolves null to the test
// override or the default daemon path.
// Explicit bins win over the override.
static const char* bin_path(
    const char* bin) {
    if (bin != nullptr)
        return (bin);
    if (g_bin_override != nullptr)
        return (g_bin_override);
    return (BIN_DEFAULT);
}

// bin_ok reports true for null (default)
// or a short non-empty execl path. The
// path never reaches a shell, so only
// length gates it, never a charset.
static bool bin_ok(const char* bin) {
    if (bin == nullptr)
        return (true);
    if (bin[0] == '\0')
        return (false);
    return (strlen(bin) <
            (size_t)BIN_LEN);
}

// concord_set_bin overrides the daemon
// binary for CLI plus reexec paths.
// Null resets to ./bin/concord.
// Process-global test config, not
// thread-safe: single-threaded harness
// only. Caller keeps the string alive.
void concord_set_bin(const char* bin) {
    g_bin_override = bin;
}

// host_avail_mb stores MemAvailable MB
// from /proc/meminfo. False when the
// line is missing, as in check/
// concord_preempt.c. Read-only probe,
// never writes.
static bool host_avail_mb(
    unsigned long long* out) {
    FILE* fp;
    char line[MEM_LINE];
    unsigned long long kb;

    if (out == nullptr)
        return (false);
    fp = fopen("/proc/meminfo", "r");
    if (fp == nullptr)
        return (false);
    while (fgets(line, sizeof(line),
                 fp) != nullptr) {
        if (sscanf(line,
                   "MemAvailable: %llu kB",
                   &kb) == 1) {
            fclose(fp);
            *out = kb / 1024ULL;
            return (*out > 0);
        }
    }
    fclose(fp);
    return (false);
}

// pct_delim reports a valid char after
// %host: end, space, or comma. Anything
// else is malformed, never silent.
static bool pct_delim(char ch) {
    return (ch == '\0' || ch == ' ' ||
            ch == ',');
}

// resolve_host_percent expands each
// <int>%host token in flags into MB:
// MemAvailable times int over 100. True
// with out holding digits only. False
// on any bare or bad percent, overflow,
// or truncation: malformed fails loud.
static bool resolve_host_percent(
    const char* flags, char* out, size_t n,
    unsigned long long free_mb) {
    size_t i;
    size_t o;

    if (flags == nullptr)
        return (false);
    if (out == nullptr)
        return (false);
    if (n == 0)
        return (false);
    if (free_mb > 1844674407370955ULL)
        return (false);
    i = 0;
    o = 0;
    while (flags[i] != '\0') {
        char ch;
        size_t j;
        size_t k;
        unsigned long long val;
        unsigned long long mb;
        char num[NUM_LEN];
        int m;

        ch = flags[i];
        if (ch < '0' || ch > '9') {
            if (ch == '%')
                return (false);
            if (o + 1 >= n)
                return (false);
            out[o++] = ch;
            i++;
            continue;
        }
        j = i;
        val = 0;
        while (flags[j] >= '0' &&
               flags[j] <= '9') {
            unsigned d;

            d = (unsigned)(flags[j] -
                           '0');
            if (val >
                (unsigned long long)PCT_MAX)
                return (false);
            val = val * 10ULL +
                  (unsigned long long)d;
            j++;
        }
        if (val >
            (unsigned long long)PCT_MAX)
            return (false);
        if (flags[j] != '%') {
            for (; i < j; i++) {
                if (o + 1 >= n)
                    return (false);
                out[o++] = flags[i];
            }
            continue;
        }
        if (strncmp(&flags[j + 1],
                    "host", 4) != 0)
            return (false);
        if (!pct_delim(flags[j + 5]))
            return (false);
        mb = free_mb * val / 100ULL;
        m = snprintf(num, sizeof(num),
                     "%llu", mb);
        if (m < 0 ||
            m >= (int)sizeof(num))
            return (false);
        for (k = 0; num[k] != '\0';
             k++) {
            if (o + 1 >= n)
                return (false);
            out[o++] = num[k];
        }
        i = j + 5;
    }
    if (o >= n)
        return (false);
    out[o] = '\0';
    return (true);
}

// mount_cap mounts a tmpfs cap over dir
// in a fresh mount namespace. Zero mb
// is a no-op success. False means the
// caller must _exit, never run uncapped
// by accident. Needs CAP_SYS_ADMIN or
// an allowing userns, else it fails.
static bool mount_cap(
    const char* dir,
    unsigned long long mb) {
    char opt[OPT_LEN];
    int m;

    if (mb == 0)
        return (true);
    if (dir == nullptr)
        return (false);
    if (unshare(CLONE_NEWNS) != 0)
        return (false);
    m = snprintf(opt, sizeof(opt),
                 "size=%llum", mb);
    if (m < 0 || m >= (int)sizeof(opt))
        return (false);
    if (mount("tmpfs", dir, "tmpfs", 0,
              opt) != 0)
        return (false);
    return (true);
}

// reexec_ex reforks concord for node i
// like reexec_net but with binary bin
// plus disk cap disk_mb. Null bin means
// the default. Zero disk_mb means
// uncapped. Always plain: variants do
// not survive a restart. The child
// exits on setenv, mount, netns, or dup
// fail, never in the wrong ns uncapped.
static bool reexec_ex(
    const SimNet* net, pid_t* pids,
    int* logfds, int i, const char* bin,
    unsigned long long disk_mb) {
    char dir[DIR_LEN];
    char path[NS_LEN];
    const char* exe;
    int pipedes[2];
    pid_t c;
    int nsfd;
    int m;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (pids == nullptr)
        return (false);
    if (logfds == nullptr)
        return (false);
    if (!bin_ok(bin))
        return (false);
    exe = bin_path(bin);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 net->statedir, i);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    if (mkdir(dir, 0700) != 0 &&
        errno != EEXIST)
        return (false);
    if (i > 0 || net->isolate_all) {
        m = snprintf(path, sizeof(path),
                     "/var/run/netns/%s%d",
                     net->ns_prefix, i);
        if (m < 0 ||
            m >= (int)sizeof(path))
            return (false);
    }
    if (pipe(pipedes) < 0)
        return (false);
    mark_cloexec(pipedes[0]);
    mark_cloexec(pipedes[1]);
    c = fork();
    if (c < 0) {
        close(pipedes[0]);
        close(pipedes[1]);
        return (false);
    }
    if (c == 0) {
        if (setenv("XDG_CONFIG_HOME",
                   dir, 1) != 0)
            _exit(127);
        if (!mount_cap(dir, disk_mb))
            _exit(127);
        if (i > 0 || net->isolate_all) {
            nsfd = open(path, O_RDONLY);
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
        execl(exe, "concord",
              (char*)nullptr);
        _exit(127);
    }
    close(pipedes[1]);
    pids[i] = c;
    logfds[i] = pipedes[0];
    cgroup_enter(net, i, c);
    return (true);
}

// spawn_one_ex forks one concord like
// sim spawn_one_net but with binary bin
// plus disk cap and no cert seeding:
// the uncapped default path seeds via
// sim instead. Offset sets
// CONCORD_CLOCK_OFFSET; anchor_arg is
// one extra daemon argv entry. Bin
// never reaches a shell: execl only.
static bool spawn_one_ex(
    const SimNet* net, pid_t* pid,
    int* logfd, int i,
    const char* clock_offset,
    const char* anchor_arg,
    const char* bin,
    unsigned long long disk_mb) {
    char dir[DIR_LEN];
    char path[NS_LEN];
    const char* exe;
    int pipedes[2];
    int m;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (pid == nullptr)
        return (false);
    if (logfd == nullptr)
        return (false);
    if (!bin_ok(bin))
        return (false);
    exe = bin_path(bin);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 net->statedir, i);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    if (mkdir(net->statedir, 0700) != 0 &&
        errno != EEXIST)
        return (false);
    if (mkdir(dir, 0700) != 0 &&
        errno != EEXIST)
        return (false);
    if (i > 0 || net->isolate_all) {
        m = snprintf(path, sizeof(path),
                     "/var/run/netns/%s%d",
                     net->ns_prefix, i);
        if (m < 0 ||
            m >= (int)sizeof(path))
            return (false);
    }
    if (pipe(pipedes) < 0)
        return (false);
    mark_cloexec(pipedes[0]);
    mark_cloexec(pipedes[1]);
    *pid = fork();
    if (*pid < 0) {
        close(pipedes[0]);
        close(pipedes[1]);
        return (false);
    }
    if (*pid == 0) {
        if (setenv("XDG_CONFIG_HOME",
                   dir, 1) != 0)
            _exit(127);
        if (clock_offset != nullptr &&
            setenv("CONCORD_CLOCK_OFFSET",
                   clock_offset,
                   1) != 0)
            _exit(127);
        if (!mount_cap(dir, disk_mb))
            _exit(127);
        if (i > 0 || net->isolate_all) {
            int nsfd;

            nsfd = open(path, O_RDONLY);
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
        if (anchor_arg != nullptr) {
            execl(exe, "concord",
                  anchor_arg,
                  (char*)nullptr);
        } else {
            execl(exe, "concord",
                  (char*)nullptr);
        }
        _exit(127);
    }
    close(pipedes[1]);
    *logfd = pipedes[0];
    cgroup_enter(net, i, *pid);
    return (true);
}

// reexec_net reforks concord for node i in
// its netns, XDG home under net statedir.
// Same child body as restart_preserve in
// check/concord_cursor_resume.c. Always
// plain: skew, anchor, and debug spawn
// variants do not survive a restart. Node
// 0 enters <prefix>0 when isolate_all.
// The child exits on netns open, setns,
// or setenv fail, never in the host
// netns with a wrong home. Net must pass
// sim_net_valid: statedir plus prefix
// reach the shell and the netns path.
// Dup failures exit: the parent would
// else read silence. Pipe ends go
// cloexec right after pipe, as in sim.
// The parent then places the child in
// its per-node cgroup, best effort: a
// warn, never a restart failure.
static bool reexec_net(const SimNet* net,
                       pid_t* pids, int* logfds,
                       int i) {
    char dir[DIR_LEN];
    char path[NS_LEN];
    const char* exe;
    int pipedes[2];
    pid_t c;
    int nsfd;
    int m;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    exe = bin_path(nullptr);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 net->statedir, i);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    if (i > 0 || net->isolate_all) {
        m = snprintf(path, sizeof(path),
                     "/var/run/netns/%s%d",
                     net->ns_prefix, i);
        if (m < 0 ||
            m >= (int)sizeof(path))
            return (false);
    }
    if (pipe(pipedes) < 0)
        return (false);
    mark_cloexec(pipedes[0]);
    mark_cloexec(pipedes[1]);
    c = fork();
    if (c < 0) {
        close(pipedes[0]);
        close(pipedes[1]);
        return (false);
    }
    if (c == 0) {
        if (setenv("XDG_CONFIG_HOME", dir,
                   1) != 0)
            _exit(127);
        if (i > 0 || net->isolate_all) {
            nsfd = open(path, O_RDONLY);
            if (nsfd < 0)
                _exit(127);
            if (setns(nsfd, CLONE_NEWNET) !=
                0) {
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
        execl(exe, "concord",
              (char*)nullptr);
        _exit(127);
    }
    close(pipedes[1]);
    pids[i] = c;
    logfds[i] = pipedes[0];
    cgroup_enter(net, i, c);
    return (true);
}

// restart_fresh SIGKILLs node i and reforks
// it wiped: sim mints a new identity.
// Statedir gate keeps the wipe shell
// safe, as in the shell restarts below.
static bool restart_fresh(Run* r, int i) {
    if (!statedir_ok(r->net.statedir))
        return (false);
    kill_reap_close(r, i, SIGKILL);
    return (sim_restart_concord_net(
        &r->net, r->pids, r->logfds, i));
}

// restart_same SIGKILLs node i and reforks
// in place: dir, id, and stores persist.
// Statedir gate matches the other
// restarts; reexec_net checks the net
// again anyway.
static bool restart_same(Run* r, int i) {
    if (!statedir_ok(r->net.statedir))
        return (false);
    kill_reap_close(r, i, SIGKILL);
    return (reexec_net(&r->net, r->pids,
                       r->logfds, i));
}

// restart_rotated SIGTERMs node i, bumps
// its Noise generation, and reforks in
// place. Ports restart_rotated in check/.
// TERM, not KILL: the daemon flushes the
// journal before exit, so rotate sees a
// clean store. Reap plus rotate shell
// bound by KILL_WAIT_S plus
// CLI_TIMEOUT_S: diverges from check/
// blocking wait on purpose.
static bool restart_rotated(Run* r, int i) {
    char dir[DIR_LEN];
    char cmd[CMD_LEN];
    const char* exe;
    int m;

    if (!statedir_ok(r->net.statedir))
        return (false);
    kill_reap_close(r, i, SIGTERM);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 r->net.statedir, i);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    exe = bin_path(nullptr);
    m = snprintf(cmd, sizeof(cmd),
                 "XDG_CONFIG_HOME=%s "
                 "%s node rotate-key "
                 ">/dev/null 2>&1",
                 dir, exe);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!shell_status(cmd,
                      CLI_TIMEOUT_S))
        return (false);
    return (reexec_net(&r->net, r->pids,
                       r->logfds, i));
}

// restart_keywipe SIGKILLs node i, deletes
// only its Noise secret, and reforks: a
// fresh key at the same generation. This
// is the pinalarm first-restart half. Rm
// runs via bounded shell with a gated
// statedir; weld uses fork plus argv rm
// and needs no gate.
static bool restart_keywipe(Run* r,
                            int i) {
    char dir[DIR_LEN];
    char cmd[CMD_LEN];
    int m;

    if (!statedir_ok(r->net.statedir))
        return (false);
    kill_reap_close(r, i, SIGKILL);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 r->net.statedir, i);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "rm -f %s/concord/noise/"
                 "secret.key",
                 dir);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!shell_status(cmd,
                      CLI_TIMEOUT_S))
        return (false);
    return (reexec_net(&r->net, r->pids,
                       r->logfds, i));
}

// restart_wiped SIGKILLs node i, deletes
// journal plus kv store, verifies both are
// gone, and reforks under the same id.
// Ports restart_wiped in check/. Same
// bounded rm note as keywipe above.
static bool restart_wiped(Run* r, int i) {
    char dir[DIR_LEN];
    char cmd[CMD_LEN];
    char jpath[FILE_LEN];
    char kpath[FILE_LEN];
    struct stat st;
    int m;

    if (!statedir_ok(r->net.statedir))
        return (false);
    kill_reap_close(r, i, SIGKILL);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 r->net.statedir, i);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    m = snprintf(jpath, sizeof(jpath),
                 "%s/concord/journal.jsonl",
                 dir);
    if (m < 0 || m >= (int)sizeof(jpath))
        return (false);
    m = snprintf(kpath, sizeof(kpath),
                 "%s/concord/bbolt.db", dir);
    if (m < 0 || m >= (int)sizeof(kpath))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "rm -f %s %s", jpath, kpath);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!shell_status(cmd,
                      CLI_TIMEOUT_S))
        return (false);
    if (stat(jpath, &st) == 0)
        return (false);
    if (stat(kpath, &st) == 0)
        return (false);
    return (reexec_net(&r->net, r->pids,
                       r->logfds, i));
}

// first_line runs cmd and stores its first
// line into buf. False on empty output
// or timeout. Bounded by CLI_TIMEOUT_S:
// diverges from check/ blocking pclose
// on purpose. Throttle callers via
// run_throttle as in mesh_count.
static bool first_line(const char* cmd,
                       char* buf, size_t n) {
    if (!capture_bounded(cmd, buf, n,
                         CLI_TIMEOUT_S))
        return (false);
    return (buf[0] != '\0');
}

// concord_have_binary reports the
// resolved binary executable: false
// means skip, not fail.
bool concord_have_binary(void) {
    return (access(bin_path(nullptr),
                   X_OK) == 0);
}

// concord_spawn forks one concord per node
// into r pids/logfds, then run_rearm each.
// Skew uses opt_node plus offset string;
// anchor uses opt_node; rest ignore both.
// Skew plus anchor reject opt_node
// outside [0,n): skew would miss,
// anchor would point nowhere.
// Net must pass sim_net_valid: statedir
// plus prefixes reach shells. Defense
// in depth; sim spawn checks again.
bool concord_spawn(Run* r, ConcordSpawn v,
                   int opt_node,
                   const char* opt_arg) {
    bool ok;
    int i;

    if (r == nullptr)
        return (false);
    if (!sim_net_valid(&r->net))
        return (false);
    switch (v) {
    case CONCORD_SPAWN_PLAIN:
        ok = sim_spawn_concord_net(
            &r->net, r->pids, r->logfds,
            r->n);
        break;
    case CONCORD_SPAWN_SKEW:
        if (opt_arg == nullptr)
            return (false);
        if (opt_node < 0 || opt_node >= r->n)
            return (false);
        ok = sim_spawn_concord_skew_net(
            &r->net, r->pids, r->logfds,
            r->n, opt_node, opt_arg);
        break;
    case CONCORD_SPAWN_ANCHOR:
        if (opt_node < 0 || opt_node >= r->n)
            return (false);
        ok = sim_spawn_concord_anchor_net(
            &r->net, r->pids, r->logfds,
            r->n, opt_node);
        break;
    case CONCORD_SPAWN_DEBUG:
        ok = sim_spawn_concord_debug_net(
            &r->net, r->pids, r->logfds,
            r->n);
        break;
    default:
        return (false);
    }
    if (!ok)
        return (false);
    for (i = 0; i < r->n; i++)
        run_rearm(r, i);
    return (true);
}

// GOSSIP_LEN is the shared gossip key
// size; stat and head must match.
static constexpr int GOSSIP_LEN = 32;

// seed_gossip creates the shared gossip
// key once per cluster. Reuses it when
// present. Mirrors sim ensure. Single
// owner: one setup per statedir.
static bool seed_gossip(
    const SimNet* net) {
    char path[DIR_LEN];
    char cmd[CMD_LEN * 2];
    struct stat st;
    int m;

    if (mkdir(net->statedir, 0700) != 0 &&
        errno != EEXIST)
        return (false);
    m = snprintf(path, sizeof(path),
                 "%s/gossip.key",
                 net->statedir);
    if (m < 0 || m >= (int)sizeof(path))
        return (false);
    if (stat(path, &st) == 0 &&
        st.st_size == GOSSIP_LEN)
        return (true);
    m = snprintf(cmd, sizeof(cmd),
                 "head -c 32 /dev/urandom "
                 "> %s && chmod 600 %s",
                 path, path);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    return (shell_status(cmd,
                         CLI_TIMEOUT_S));
}

// concord_seed_home seeds node home:
// dir, CA copy, gossip key copy, plain
// config.json. Skew offset plus anchor
// args stay spawn-time via env plus
// argv; seed writes plain form only.
// Mirrors sim spawn_one_net seeding.
bool concord_seed_home(const SimNet* net,
                       int node) {
    char dir[DIR_LEN];
    char certs[FILE_LEN];
    char gossip[DIR_LEN];
    char cmd[CMD_LEN * 2];
    unsigned a, b, c;
    int m;

    if (net == nullptr)
        return (false);
    if (!sim_net_valid(net))
        return (false);
    if (node < 0 ||
        node >= TUN_MAP_MAX)
        return (false);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 net->statedir, node);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    if (mkdir(net->statedir, 0700) != 0 &&
        errno != EEXIST)
        return (false);
    if (mkdir(dir, 0700) != 0 &&
        errno != EEXIST)
        return (false);
    m = snprintf(certs, sizeof(certs),
                 "%s/concord/certs", dir);
    if (m < 0 || m >= (int)sizeof(certs))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "mkdir -p %s", certs);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!shell_status(cmd,
                      CLI_TIMEOUT_S))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "cp certs/ca.crt %s/ "
                 "2>/dev/null", certs);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!shell_status(cmd,
                      CLI_TIMEOUT_S))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "cp certs/ca.key %s/ "
                 "2>/dev/null", certs);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!shell_status(cmd,
                      CLI_TIMEOUT_S))
        return (false);
    if (!seed_gossip(net))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "mkdir -p %s/concord/"
                 "memberservice", dir);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!shell_status(cmd,
                      CLI_TIMEOUT_S))
        return (false);
    m = snprintf(gossip, sizeof(gossip),
                 "%s/gossip.key",
                 net->statedir);
    if (m < 0 ||
        m >= (int)sizeof(gossip))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "cp %s %s/concord/"
                 "memberservice/secret.key"
                 " && chmod 600 %s/concord/"
                 "memberservice/secret.key",
                 gossip, dir, dir);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!shell_status(cmd,
                      CLI_TIMEOUT_S))
        return (false);
    a = net->under[0];
    b = net->under[1];
    c = net->under[2];
    m = snprintf(cmd, sizeof(cmd),
                 "mkdir -p %s/concord && "
                 "uuid=$(cat /proc/sys/"
                 "kernel/random/uuid) && "
                 "printf '{\"id\":\"%%s\","
                 "\"memberlist_address\":"
                 "\"0.0.0.0:7946\","
                 "\"advertise_address\":"
                 "\"%u.%u.%u.%d\"}' "
                 "\"$uuid\" > %s/concord/"
                 "config.json",
                 dir, a, b, c, node + 1,
                 dir);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    return (shell_status(cmd,
                         CLI_TIMEOUT_S));
}

// seed_anchor rewrites node config with
// an anchors entry for under.octet:7946.
// Spawn-time step for anchor variants;
// seed_home writes plain form only.
static bool seed_anchor(const SimNet* net,
                        int node,
                        int octet) {
    char dir[DIR_LEN];
    char cmd[CMD_LEN * 2];
    unsigned a, b, c;
    int m;

    if (net == nullptr)
        return (false);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 net->statedir, node);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    a = net->under[0];
    b = net->under[1];
    c = net->under[2];
    m = snprintf(cmd, sizeof(cmd),
                 "mkdir -p %s/concord && "
                 "uuid=$(cat /proc/sys/"
                 "kernel/random/uuid) && "
                 "printf '{\"id\":\"%%s\","
                 "\"memberlist_address\":"
                 "\"0.0.0.0:7946\","
                 "\"advertise_address\":"
                 "\"%u.%u.%u.%d\","
                 "\"anchors\":[{\"address\":"
                 "\"%u.%u.%u.%d:7946\"}]}' "
                 "\"$uuid\" > %s/concord/"
                 "config.json",
                 dir, a, b, c, node + 1,
                 a, b, c, octet, dir);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    return (shell_status(cmd,
                         CLI_TIMEOUT_S));
}

// restore_env puts back saved XDG plus
// clock offset. Best effort: ignores
// setenv errors. Frees the saves.
static void restore_env(char* sx,
                        bool have_x,
                        char* sc,
                        bool have_c) {
    if (have_x && sx != nullptr)
        setenv("XDG_CONFIG_HOME", sx, 1);
    else
        unsetenv("XDG_CONFIG_HOME");
    if (have_c && sc != nullptr)
        setenv("CONCORD_CLOCK_OFFSET",
               sc, 1);
    else
        unsetenv("CONCORD_CLOCK_OFFSET");
    free(sx);
    free(sc);
}

// concord_spawn_bin seeds each home then
// forks via sim_spawn_cmd plus run_rearm.
// Variant plus opt mirror concord_spawn.
// Null bin means the default. Env split:
// XDG home plus skew offset ride the
// parent env into the fork; argv carries
// --anchor or --log-level=debug. No
// chdir, so relative bins keep working.
bool concord_spawn_bin(Run* r, int variant,
                       int opt_node,
                       const char* opt_arg,
                       const char* bin) {
    const char* exe;
    char* sx;
    char* sc;
    char* ox;
    char* oc;
    bool have_x;
    bool have_c;
    int i;

    if (r == nullptr)
        return (false);
    if (!sim_net_valid(&r->net))
        return (false);
    if (r->n < 1 || r->n > TUN_MAP_MAX)
        return (false);
    if (variant != (int)CONCORD_SPAWN_PLAIN &&
        variant != (int)CONCORD_SPAWN_SKEW &&
        variant !=
            (int)CONCORD_SPAWN_ANCHOR &&
        variant != (int)CONCORD_SPAWN_DEBUG)
        return (false);
    if (variant == (int)CONCORD_SPAWN_SKEW) {
        if (opt_arg == nullptr)
            return (false);
        if (opt_node < 0 ||
            opt_node >= r->n)
            return (false);
    }
    if (variant ==
        (int)CONCORD_SPAWN_ANCHOR) {
        if (opt_node < 0 ||
            opt_node >= r->n)
            return (false);
    }
    if (!bin_ok(bin))
        return (false);
    exe = bin_path(bin);
    for (i = 0; i < r->n; i++) {
        r->pids[i] = -1;
        r->logfds[i] = -1;
    }
    for (i = 0; i < r->n; i++) {
        if (!concord_seed_home(&r->net,
                               i))
            return (false);
    }
    if (variant ==
        (int)CONCORD_SPAWN_ANCHOR) {
        for (i = 0; i < r->n; i++) {
            if (!seed_anchor(&r->net, i,
                             opt_node + 1))
                return (false);
        }
    }
    ox = getenv("XDG_CONFIG_HOME");
    oc = getenv("CONCORD_CLOCK_OFFSET");
    have_x = ox != nullptr;
    have_c = oc != nullptr;
    sx = have_x ? strdup(ox) : nullptr;
    sc = have_c ? strdup(oc) : nullptr;
    if ((have_x && sx == nullptr) ||
        (have_c && sc == nullptr)) {
        free(sx);
        free(sc);
        return (false);
    }
    for (i = 0; i < r->n; i++) {
        char dir[DIR_LEN];
        char* plain[2];
        char* extra[3];
        char** argv;
        char* arg;
        SimCmd scmd;
        int m;

        m = snprintf(dir, sizeof(dir),
                     "%s/node%d",
                     r->net.statedir, i);
        if (m < 0 ||
            m >= (int)sizeof(dir)) {
            int k;

            for (k = 0; k < i; k++)
                kill_reap_close(r, k,
                                SIGKILL);
            restore_env(sx, have_x, sc,
                        have_c);
            return (false);
        }
        if (setenv("XDG_CONFIG_HOME",
                   dir, 1) != 0) {
            int k;

            for (k = 0; k < i; k++)
                kill_reap_close(r, k,
                                SIGKILL);
            restore_env(sx, have_x, sc,
                        have_c);
            return (false);
        }
        if (variant ==
                (int)CONCORD_SPAWN_SKEW &&
            i == opt_node) {
            if (setenv("CONCORD_"
                       "CLOCK_OFFSET",
                       opt_arg, 1) != 0) {
                int k;

                for (k = 0; k < i; k++)
                    kill_reap_close(
                        r, k, SIGKILL);
                restore_env(sx, have_x,
                            sc, have_c);
                return (false);
            }
        } else {
            unsetenv("CONCORD_"
                     "CLOCK_OFFSET");
        }
        arg = nullptr;
        if (variant ==
                (int)CONCORD_SPAWN_ANCHOR &&
            i == opt_node)
            arg = "--anchor";
        if (variant ==
            (int)CONCORD_SPAWN_DEBUG)
            arg = "--log-level=debug";
        if (arg == nullptr) {
            plain[0] = "concord";
            plain[1] = nullptr;
            argv = plain;
        } else {
            extra[0] = "concord";
            extra[1] = arg;
            extra[2] = nullptr;
            argv = extra;
        }
        scmd.bin = exe;
        scmd.argv = argv;
        scmd.workdir = nullptr;
        if (!sim_spawn_cmd(&r->net,
                           &r->pids[i],
                           &r->logfds[i],
                           i, &scmd)) {
            int k;

            for (k = 0; k < i; k++)
                kill_reap_close(r, k,
                                SIGKILL);
            restore_env(sx, have_x, sc,
                        have_c);
            return (false);
        }
    }
    restore_env(sx, have_x, sc, have_c);
    for (i = 0; i < r->n; i++)
        run_rearm(r, i);
    return (true);
}

// concord_kill sends sig, reaps, closes the
// logfd, and notes dead: stay-dead, never
// restarted. Teardown then skips the pid.
// True when already dead: idempotent, as
// in weld_kill. A non-ESRCH kill failure
// is false, as in weld_kill: the slot
// keeps its pid, so a later kill never
// hits a reused pid. Reap bound by
// KILL_WAIT_S then KILL: diverges from
// check/ blocking reap on purpose.
bool concord_kill(Run* r, int i, int sig) {
    if (r == nullptr)
        return (false);
    if (i < 0 || i >= r->n)
        return (false);
    if (r->pids[i] <= 0) {
        if (r->logfds[i] >= 0) {
            close(r->logfds[i]);
            r->logfds[i] = -1;
        }
        run_note_dead(r, i);
        return (true);
    }
    if (kill(r->pids[i], sig) != 0 &&
        errno != ESRCH)
        return (false);
    reap_bounded(r->pids[i],
                 KILL_WAIT_S);
    if (r->logfds[i] >= 0)
        close(r->logfds[i]);
    r->pids[i] = -1;
    r->logfds[i] = -1;
    run_note_dead(r, i);
    return (true);
}

// concord_restart kills node i and reforks
// it with durability d, then run_rearm.
// Fresh wipes the dir for a new identity.
// Refork is always plain: skew, anchor,
// and debug variants do not survive it.
// Net must pass sim_net_valid: restart
// shells interpolate statedir. Same
// defense note as spawn.
bool concord_restart(Run* r, int i,
                     ConcordDurability d) {
    bool ok;

    if (r == nullptr)
        return (false);
    if (i < 0 || i >= r->n)
        return (false);
    if (!sim_net_valid(&r->net))
        return (false);
    switch (d) {
    case CONCORD_FRESH:
        ok = restart_fresh(r, i);
        break;
    case CONCORD_PRESERVE:
        ok = restart_same(r, i);
        break;
    case CONCORD_REKEY_ROTATE:
        ok = restart_rotated(r, i);
        break;
    case CONCORD_KEY_WIPE:
        ok = restart_keywipe(r, i);
        break;
    case CONCORD_WIPE_STATE:
        ok = restart_wiped(r, i);
        break;
    default:
        return (false);
    }
    if (!ok)
        return (false);
    run_rearm(r, i);
    return (true);
}

// concord_submit runs workload run with
// image plus extra flags against node,
// parsing Submitted workload <uuid> into
// out. Null flags means no extra flags.
// Image outside alnum plus _ . : / @ + ,
// - fails; flags outside
// [A-Za-z0-9_=., /-] fail: both reach
// a shell. Statedir must pass
// statedir_ok for the same reason.
// Bounded by CLI_TIMEOUT_S via
// first_line.
bool concord_submit(const char* statedir,
                    int node, const char* image,
                    const char* flags, char* out,
                    size_t n) {
    char cmd[CMD_LEN];
    char buf[LINE_LEN];
    char res[RES_LEN];
    char id[ID_LEN];
    const char* use;
    const char* exe;
    int m;

    if (statedir == nullptr)
        return (false);
    if (!statedir_ok(statedir))
        return (false);
    if (image == nullptr || out == nullptr)
        return (false);
    if (!image_ok(image))
        return (false);
    if (flags == nullptr)
        flags = "";
    use = flags;
    if (strchr(flags, '%') != nullptr) {
        unsigned long long free_mb;

        if (!host_avail_mb(&free_mb))
            return (false);
        if (!resolve_host_percent(flags,
                                  res,
                                  sizeof(res),
                                  free_mb))
            return (false);
        use = res;
    }
    if (!flags_ok(use))
        return (false);
    exe = bin_path(nullptr);
    m = snprintf(cmd, sizeof(cmd),
                 "XDG_CONFIG_HOME=%s/node%d "
                 "%s workload run "
                 "%s %s 2>/dev/null",
                 statedir, node, exe,
                 image, use);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!first_line(cmd, buf, sizeof(buf)))
        return (false);
    if (sscanf(buf, "%*s %*s %63s", id) !=
        1)
        return (false);
    if (strlen(id) + 1 > n)
        return (false);
    strcpy(out, id);
    return (true);
}

// concord_stop runs workload stop on
// node and wants the Stopped line.
// Statedir plus wid reach a shell:
// gates reject metachars, so ungated
// exec inputs now fail fast. Node
// must be non-negative. Bounded by
// CLI_TIMEOUT_S via first_line, first
// line only, as exec fgets one line.
bool concord_stop(const char* statedir,
                  int node, const char* wid) {
    char cmd[CMD_LEN];
    char buf[LINE_LEN];
    const char* exe;
    char* nl;
    int m;

    if (!statedir_ok(statedir))
        return (false);
    if (!wid_ok(wid))
        return (false);
    if (node < 0)
        return (false);
    exe = bin_path(nullptr);
    m = snprintf(cmd, sizeof(cmd),
                 "XDG_CONFIG_HOME=%s/node%d "
                 "%s workload stop "
                 "%s 2>/dev/null",
                 statedir, node, exe, wid);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    if (!first_line(cmd, buf, sizeof(buf)))
        return (false);
    nl = strchr(buf, '\n');
    if (nl != nullptr)
        *nl = '\0';
    return (strstr(buf, "Stopped") !=
            nullptr);
}

// concord_update runs workload update
// --image on node for wid. Image plus
// wid plus statedir reach a shell:
// gates reject metachars. True only
// on exit 0 via quiet_status, output
// drained as exec drains its pipe.
// Bounded by CLI_TIMEOUT_S.
bool concord_update(const char* statedir,
                    int node, const char* wid,
                    const char* image) {
    char cmd[CMD_LEN * 2];
    const char* exe;
    int m;

    if (!statedir_ok(statedir))
        return (false);
    if (!wid_ok(wid))
        return (false);
    if (!image_ok(image))
        return (false);
    if (node < 0)
        return (false);
    exe = bin_path(nullptr);
    m = snprintf(cmd, sizeof(cmd),
                 "XDG_CONFIG_HOME=%s/node%d "
                 "%s workload update "
                 "--image %s %s 2>&1",
                 statedir, node, exe,
                 image, wid);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    return (quiet_status(cmd,
                         CLI_TIMEOUT_S));
}

// concord_list_has reports shortid
// found in node workload list.
// 8-char prefix may collide: unique
// verdicts need the full id. Output
// caps at OUT_LEN bytes as in check/:
// longer lists truncate and may miss.
// Bounded by CLI_TIMEOUT_S: diverges
// from check/ blocking popen on
// purpose. Throttle callers via
// run_throttle, as in mesh_count.
// Statedir must pass statedir_ok: it
// reaches a shell. Shortid never
// reaches the shell: strstr only.
bool concord_list_all(const char* statedir,
                      int node, char* out,
                      size_t n) {
    char cmd[CMD_LEN];
    const char* exe;
    int m;

    if (statedir == nullptr)
        return (false);
    if (!statedir_ok(statedir))
        return (false);
    if (out == nullptr || n == 0)
        return (false);
    exe = bin_path(nullptr);
    m = snprintf(cmd, sizeof(cmd),
                 "XDG_CONFIG_HOME=%s/node%d "
                 "%s workload list "
                 "2>/dev/null",
                 statedir, node, exe);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    return (capture_bounded(cmd, out, n,
                            CLI_TIMEOUT_S));
}

// concord_list_has reports shortid
// found in node workload list.
bool concord_list_has(const char* statedir,
                      int node,
                      const char* shortid) {
    char buf[OUT_LEN];

    if (shortid == nullptr)
        return (false);
    if (!concord_list_all(statedir,
                          node, buf,
                          sizeof(buf)))
        return (false);
    return (strstr(buf, shortid) !=
            nullptr);
}

// concord_assigned_anywhere reports a
// non-nil AssignedNodeID for wid in any
// node journal under statedir. Wid must
// pass wid_ok and statedir must pass
// statedir_ok: both reach a grep shell.
// Bounded by CLI_TIMEOUT_S: a hung
// grep counts as false, never a stall.
// Journal scan races writers: daemons
// append during the grep, so a fresh
// entry may miss. Callers poll and
// retry, and quiesce restarts during
// the scan: a wipe deletes the file
// under the reader.
bool concord_assigned_anywhere(
    const char* statedir, const char* wid) {
    char cmd[CMD_LEN];
    int m;

    if (statedir == nullptr)
        return (false);
    if (!statedir_ok(statedir))
        return (false);
    if (wid == nullptr)
        return (false);
    if (!wid_ok(wid))
        return (false);
    m = snprintf(cmd, sizeof(cmd),
                 "grep -h '%s' %s/node*/concord/"
                 "journal.jsonl 2>/dev/null | "
                 "grep -o '\"AssignedNodeID\":"
                 "\"[^\"]*\"' | grep -v '%s' | "
                 "grep -q .",
                 wid, statedir, NIL_ID);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    return (shell_status(cmd,
                         CLI_TIMEOUT_S));
}

// concord_mesh_count counts alive members
// in node list output, or -1 on error.
// Counts strict " alive " with spaces, as
// in check/concord_splitbrain.c: bare
// alive overcounts. Output caps at
// OUT_LEN bytes as in check/: longer
// output truncates and undercounts.
// Bounded by CLI_TIMEOUT_S: diverges
// from check/ blocking popen.
// Throttle callers via run_throttle to
// about one query per 2 s: each call
// shells node list. Statedir must pass
// statedir_ok: it reaches a shell.
int concord_mesh_count(const char* statedir,
                       int node) {
    char cmd[CMD_LEN];
    char buf[OUT_LEN];
    const char* exe;
    const char* s;
    int alive;
    int m;

    if (statedir == nullptr)
        return (-1);
    if (!statedir_ok(statedir))
        return (-1);
    exe = bin_path(nullptr);
    m = snprintf(cmd, sizeof(cmd),
                 "XDG_CONFIG_HOME=%s/node%d "
                 "%s node list "
                 "2>/dev/null",
                 statedir, node, exe);
    if (m < 0 || m >= (int)sizeof(cmd))
        return (-1);
    if (!capture_bounded(cmd, buf,
                         sizeof(buf),
                         CLI_TIMEOUT_S))
        return (-1);
    alive = 0;
    s = buf;
    while ((s = strstr(s, " alive ")) !=
           nullptr) {
        alive++;
        s++;
    }
    return (alive);
}

// concord_spawn_ex forks like
// concord_spawn but with binary bin
// plus disk cap disk_mb. Variant,
// opt_node, and opt_arg mirror
// concord_spawn exactly. Null bin means
// the default; zero disk_mb means
// uncapped and default bin delegates to
// concord_spawn for exact seeding.
// Custom or capped paths fork direct
// with no sim cert seeding and boot
// fresh under the cap. Bin never
// reaches a shell: execl only.
bool concord_spawn_ex(Run* r, int variant,
                      int opt_node,
                      const char* opt_arg,
                      const char* bin,
                      unsigned long long disk_mb) {
    const char* exe;
    int i;

    if (r == nullptr)
        return (false);
    if (!sim_net_valid(&r->net))
        return (false);
    if (r->n < 1 ||
        r->n > TUN_MAP_MAX)
        return (false);
    if (variant !=
            (int)CONCORD_SPAWN_PLAIN &&
        variant !=
            (int)CONCORD_SPAWN_SKEW &&
        variant !=
            (int)CONCORD_SPAWN_ANCHOR &&
        variant !=
            (int)CONCORD_SPAWN_DEBUG)
        return (false);
    if (variant ==
        (int)CONCORD_SPAWN_SKEW) {
        if (opt_arg == nullptr)
            return (false);
        if (opt_node < 0 ||
            opt_node >= r->n)
            return (false);
    }
    if (variant ==
        (int)CONCORD_SPAWN_ANCHOR) {
        if (opt_node < 0 ||
            opt_node >= r->n)
            return (false);
    }
    if (!bin_ok(bin))
        return (false);
    exe = bin_path(bin);
    if (disk_mb == 0 &&
        strcmp(exe, BIN_DEFAULT) == 0)
        return (concord_spawn(
            r, (ConcordSpawn)variant,
            opt_node, opt_arg));
    for (i = 0; i < r->n; i++) {
        const char* off;
        const char* arg;

        off = nullptr;
        arg = nullptr;
        if (variant ==
                (int)CONCORD_SPAWN_SKEW &&
            i == opt_node)
            off = opt_arg;
        if (variant ==
                (int)CONCORD_SPAWN_ANCHOR &&
            i == opt_node)
            arg = "--anchor";
        if (variant ==
            (int)CONCORD_SPAWN_DEBUG)
            arg = "--log-level=debug";
        if (!spawn_one_ex(
                &r->net, &r->pids[i],
                &r->logfds[i], i, off, arg,
                bin, disk_mb)) {
            int k;

            for (k = 0; k < i; k++)
                kill_reap_close(r, k,
                                SIGKILL);
            return (false);
        }
    }
    for (i = 0; i < r->n; i++)
        run_rearm(r, i);
    return (true);
}

// concord_restart_ex kills node i and
// reforks it with durability plus bin
// plus disk cap. Durability mirrors
// concord_restart; default bin plus
// uncapped delegates there exactly.
// Helper rotate-key stays on the
// default binary so no custom path
// reaches a shell. Capped reforks hide
// prior seeds and boot fresh.
bool concord_restart_ex(
    Run* r, int i, int durability,
    const char* bin,
    unsigned long long disk_mb) {
    char dir[DIR_LEN];
    char cmd[CMD_LEN];
    const char* exe;
    bool ok;
    int m;

    if (r == nullptr)
        return (false);
    if (i < 0 || i >= r->n)
        return (false);
    if (!sim_net_valid(&r->net))
        return (false);
    if (durability <
            (int)CONCORD_FRESH ||
        durability >
            (int)CONCORD_WIPE_STATE)
        return (false);
    if (!bin_ok(bin))
        return (false);
    exe = bin_path(bin);
    if (disk_mb == 0 &&
        strcmp(exe, BIN_DEFAULT) == 0)
        return (concord_restart(
            r, i,
            (ConcordDurability)durability));
    if (!statedir_ok(r->net.statedir))
        return (false);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 r->net.statedir, i);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    switch (durability) {
    case CONCORD_FRESH:
        kill_reap_close(r, i, SIGKILL);
        m = snprintf(cmd, sizeof(cmd),
                     "rm -rf %s", dir);
        if (m < 0 ||
            m >= (int)sizeof(cmd))
            return (false);
        if (!shell_status(cmd,
                          CLI_TIMEOUT_S))
            return (false);
        ok = reexec_ex(&r->net, r->pids,
                       r->logfds, i, bin,
                       disk_mb);
        break;
    case CONCORD_PRESERVE:
        kill_reap_close(r, i, SIGKILL);
        ok = reexec_ex(&r->net, r->pids,
                       r->logfds, i, bin,
                       disk_mb);
        break;
    case CONCORD_REKEY_ROTATE: {
        kill_reap_close(r, i, SIGTERM);
        m = snprintf(cmd, sizeof(cmd),
                     "XDG_CONFIG_HOME=%s "
                     "%s node rotate-key "
                     ">/dev/null 2>&1",
                     dir,
                     bin_path(nullptr));
        if (m < 0 ||
            m >= (int)sizeof(cmd))
            return (false);
        if (!shell_status(cmd,
                          CLI_TIMEOUT_S))
            return (false);
        ok = reexec_ex(&r->net, r->pids,
                       r->logfds, i, bin,
                       disk_mb);
        break;
    }
    case CONCORD_KEY_WIPE:
        kill_reap_close(r, i, SIGKILL);
        m = snprintf(cmd, sizeof(cmd),
                     "rm -f %s/concord/noise/"
                     "secret.key",
                     dir);
        if (m < 0 ||
            m >= (int)sizeof(cmd))
            return (false);
        if (!shell_status(cmd,
                          CLI_TIMEOUT_S))
            return (false);
        ok = reexec_ex(&r->net, r->pids,
                       r->logfds, i, bin,
                       disk_mb);
        break;
    case CONCORD_WIPE_STATE: {
        char jpath[FILE_LEN];
        char kpath[FILE_LEN];
        struct stat st;

        kill_reap_close(r, i, SIGKILL);
        m = snprintf(jpath,
                     sizeof(jpath),
                     "%s/concord/"
                     "journal.jsonl",
                     dir);
        if (m < 0 ||
            m >= (int)sizeof(jpath))
            return (false);
        m = snprintf(kpath,
                     sizeof(kpath),
                     "%s/concord/bbolt.db",
                     dir);
        if (m < 0 ||
            m >= (int)sizeof(kpath))
            return (false);
        m = snprintf(cmd, sizeof(cmd),
                     "rm -f %s %s", jpath,
                     kpath);
        if (m < 0 ||
            m >= (int)sizeof(cmd))
            return (false);
        if (!shell_status(cmd,
                          CLI_TIMEOUT_S))
            return (false);
        if (stat(jpath, &st) == 0)
            return (false);
        if (stat(kpath, &st) == 0)
            return (false);
        ok = reexec_ex(&r->net, r->pids,
                       r->logfds, i, bin,
                       disk_mb);
        break;
    }
    default:
        return (false);
    }
    if (!ok)
        return (false);
    run_rearm(r, i);
    return (true);
}

// concord_disk_fill fallocates the node
// filler to mb MB, best effort on
// ENOSPC. Zero mb removes it. Direct
// file calls, no shell: no charset or
// timeout beyond the fs itself. Cannot
// force a shared filesystem full.
bool concord_disk_fill(Run* r, int node,
                       unsigned long long mb) {
    char dir[DIR_LEN];
    char path[FILE_LEN];
    off_t len;
    int fd;
    int rc;
    int m;

    if (r == nullptr)
        return (false);
    if (node < 0 || node >= r->n)
        return (false);
    if (!sim_net_valid(&r->net))
        return (false);
    if (!statedir_ok(r->net.statedir))
        return (false);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 r->net.statedir, node);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    m = snprintf(path, sizeof(path),
                 "%s/filler.bin", dir);
    if (m < 0 || m >= (int)sizeof(path))
        return (false);
    if (mb == 0) {
        unlink(path);
        return (true);
    }
    if (mb > 8796093022207ULL)
        return (false);
    if (mkdir(dir, 0700) != 0 &&
        errno != EEXIST)
        return (false);
    fd = open(path,
              O_WRONLY | O_CREAT | O_TRUNC,
              0644);
    if (fd < 0)
        return (false);
    len = (off_t)(mb * 1024ULL *
                  1024ULL);
    rc = posix_fallocate(fd, 0, len);
    close(fd);
    if (rc == 0)
        return (true);
    if (rc == ENOSPC)
        return (true);
    return (false);
}

// concord_disk_readonly flips writability
// on the node state tree via chmod -R,
// bounded by 10 s. Approximation, not
// block-device RO: root bypasses it.
// Reversible via false. Gated statedir
// keeps the shell safe.
bool concord_disk_readonly(Run* r, int node,
                           bool ro) {
    char dir[DIR_LEN];
    char cmd[CMD_LEN];
    int m;

    if (r == nullptr)
        return (false);
    if (node < 0 || node >= r->n)
        return (false);
    if (!sim_net_valid(&r->net))
        return (false);
    if (!statedir_ok(r->net.statedir))
        return (false);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 r->net.statedir, node);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    if (ro) {
        m = snprintf(cmd, sizeof(cmd),
                     "chmod -R a-w %s", dir);
    } else {
        m = snprintf(cmd, sizeof(cmd),
                     "chmod -R u+w %s", dir);
    }
    if (m < 0 || m >= (int)sizeof(cmd))
        return (false);
    return (shell_status(cmd,
                         CLI_TIMEOUT_S));
}

// concord_journal_corrupt truncates the
// node journal then writes JUNK: 8
// bytes CORRUPT plus newline. Direct
// file calls, no shell. Next readers
// see invalid JSON.
bool concord_journal_corrupt(Run* r,
                             int node) {
    char dir[DIR_LEN];
    char path[FILE_LEN];
    ssize_t k;
    int fd;
    int m;

    if (r == nullptr)
        return (false);
    if (node < 0 || node >= r->n)
        return (false);
    if (!sim_net_valid(&r->net))
        return (false);
    if (!statedir_ok(r->net.statedir))
        return (false);
    m = snprintf(dir, sizeof(dir),
                 "%s/node%d",
                 r->net.statedir, node);
    if (m < 0 || m >= (int)sizeof(dir))
        return (false);
    m = snprintf(path, sizeof(path),
                 "%s/concord/journal.jsonl",
                 dir);
    if (m < 0 || m >= (int)sizeof(path))
        return (false);
    fd = open(path,
              O_WRONLY | O_CREAT | O_TRUNC,
              0644);
    if (fd < 0)
        return (false);
    k = write(fd, JUNK, sizeof(JUNK) - 1);
    close(fd);
    if (k != (ssize_t)(sizeof(JUNK) - 1))
        return (false);
    return (true);
}

// PROFILE_STAT_S bounds one perf stat
// sleep: a short sample, never a
// stall. Wait bound reuses
// CLI_TIMEOUT_S, as in submit.
static constexpr int PROFILE_STAT_S = 5;

// PROFILE_EARLY_N times
// PROFILE_EARLY_MS polls a fresh perf
// record for a fast exit, so start
// reaps a dead child at once.
static constexpr int PROFILE_EARLY_N = 5;
static constexpr int PROFILE_EARLY_MS =
    100;

// PROFILE_LINE caps one proc line in
// the snapshot; PROFILE_COPY caps one
// whole proc file dump.
static constexpr int PROFILE_LINE = 256;
static constexpr int PROFILE_COPY =
    4096;

// set_why stores s into why. Null why
// or zero nwhy keeps nothing.
// Truncates safely, never fails.
static void set_why(char* why,
                    size_t nwhy,
                    const char* s) {
    int m;

    if (why == nullptr)
        return;
    if (nwhy == 0)
        return;
    if (s == nullptr)
        s = "";
    m = snprintf(why, nwhy, "%s", s);
    if (m < 0)
        why[0] = '\0';
}

// have_perf probes perf --version with
// a bounded wait. True only on exit
// 0. Fixed argv, no shell, so no
// gate needed. Reaps exactly once.
static bool have_perf(void) {
    pid_t pid;
    time_t end;
    int st;

    pid = fork();
    if (pid < 0)
        return (false);
    if (pid == 0) {
        int fd;

        fd = open("/dev/null", O_WRONLY);
        if (fd < 0)
            _exit(127);
        if (dup2(fd, 1) < 0)
            _exit(127);
        if (dup2(fd, 2) < 0)
            _exit(127);
        if (fd > 2)
            close(fd);
        execlp("perf", "perf",
               "--version",
               (char*)nullptr);
        _exit(127);
    }
    end = mono_s() + CLI_TIMEOUT_S;
    for (;;) {
        pid_t w;
        struct timespec nap;

        w = waitpid(pid, &st, WNOHANG);
        if (w == pid)
            break;
        if (w < 0 && errno != EINTR) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (false);
        }
        if (mono_s() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (false);
        }
        nap.tv_sec = 0;
        nap.tv_nsec = SLEEP_NS;
        nanosleep(&nap, nullptr);
    }
    if (!WIFEXITED(st))
        return (false);
    return (WEXITSTATUS(st) == 0);
}

// ensure_dir runs mkdir -p on dir with
// a bounded wait. True only on exit
// 0. Fork plus argv, no shell, so
// metachars never run. Reaps once.
static bool ensure_dir(const char* dir) {
    pid_t pid;
    time_t end;
    int st;

    if (dir == nullptr)
        return (false);
    pid = fork();
    if (pid < 0)
        return (false);
    if (pid == 0) {
        execlp("mkdir", "mkdir", "-p",
               dir, (char*)nullptr);
        _exit(127);
    }
    end = mono_s() + CLI_TIMEOUT_S;
    for (;;) {
        pid_t w;
        struct timespec nap;

        w = waitpid(pid, &st, WNOHANG);
        if (w == pid)
            break;
        if (w < 0 && errno != EINTR) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (false);
        }
        if (mono_s() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (false);
        }
        nap.tv_sec = 0;
        nap.tv_nsec = SLEEP_NS;
        nanosleep(&nap, nullptr);
    }
    if (!WIFEXITED(st))
        return (false);
    return (WEXITSTATUS(st) == 0);
}

// dump_proc appends path into fp up to
// PROFILE_COPY bytes. Best effort: a
// missing file notes, never fails.
static void dump_proc(FILE* fp,
                      const char* path) {
    FILE* in;
    char buf[PROFILE_LINE];
    size_t total;

    if (fp == nullptr)
        return;
    if (path == nullptr)
        return;
    in = fopen(path, "r");
    if (in == nullptr) {
        fprintf(fp, "%s: missing\n",
                path);
        return;
    }
    fprintf(fp, "== %s ==\n", path);
    total = 0;
    while (fgets(buf, sizeof(buf), in) !=
           nullptr) {
        if (total >=
            (size_t)PROFILE_COPY)
            break;
        fputs(buf, fp);
        total += strlen(buf);
    }
    fclose(in);
}

// write_resource stores outdir/
// snapshot.txt: node plus pid, self
// plus children rusage, then proc
// status, stat, and io when the pid
// lives. True on fclose 0.
static bool write_resource(
    const char* outdir, pid_t pid,
    int node) {
    char path[FILE_LEN];
    char proc[FILE_LEN];
    struct rusage self;
    struct rusage kids;
    FILE* fp;
    int m;

    m = snprintf(path, sizeof(path),
                 "%s/snapshot.txt", outdir);
    if (m < 0 ||
        m >= (int)sizeof(path))
        return (false);
    fp = fopen(path, "w");
    if (fp == nullptr)
        return (false);
    fprintf(fp, "node %d pid %d\n", node,
            (int)pid);
    if (getrusage(RUSAGE_SELF, &self) ==
        0)
        fprintf(fp, "self maxrss %ld\n",
                (long)self.ru_maxrss);
    if (getrusage(RUSAGE_CHILDREN,
                  &kids) == 0)
        fprintf(fp, "kids maxrss %ld\n",
                (long)kids.ru_maxrss);
    if (pid <= 0) {
        fprintf(fp, "child: none\n");
        return (fclose(fp) == 0);
    }
    m = snprintf(proc, sizeof(proc),
                 "/proc/%d/status",
                 (int)pid);
    if (m >= 0 && m < (int)sizeof(proc))
        dump_proc(fp, proc);
    m = snprintf(proc, sizeof(proc),
                 "/proc/%d/stat",
                 (int)pid);
    if (m >= 0 && m < (int)sizeof(proc))
        dump_proc(fp, proc);
    m = snprintf(proc, sizeof(proc),
                 "/proc/%d/io", (int)pid);
    if (m >= 0 && m < (int)sizeof(proc))
        dump_proc(fp, proc);
    return (fclose(fp) == 0);
}

// run_perf_stat runs perf stat -p pid
// for PROFILE_STAT_S into outdir/
// perf-stat.txt. True only on exit 0.
// Bounded by CLI_TIMEOUT_S then
// KILL, as in shell_status. Reaps
// exactly once. Failure degrades to
// the resource file, never false up.
static bool run_perf_stat(
    pid_t pid, const char* outdir) {
    char path[FILE_LEN];
    char spid[PID_LEN];
    char secs[PID_LEN];
    pid_t c;
    time_t end;
    int st;
    int m;

    m = snprintf(path, sizeof(path),
                 "%s/perf-stat.txt",
                 outdir);
    if (m < 0 ||
        m >= (int)sizeof(path))
        return (false);
    m = snprintf(spid, sizeof(spid),
                 "%d", (int)pid);
    if (m < 0 ||
        m >= (int)sizeof(spid))
        return (false);
    m = snprintf(secs, sizeof(secs),
                 "%d", PROFILE_STAT_S);
    if (m < 0 ||
        m >= (int)sizeof(secs))
        return (false);
    c = fork();
    if (c < 0)
        return (false);
    if (c == 0) {
        int fd;

        fd = open(path,
                  O_WRONLY | O_CREAT |
                      O_TRUNC,
                  0644);
        if (fd < 0)
            _exit(127);
        if (dup2(fd, 1) < 0)
            _exit(127);
        if (dup2(fd, 2) < 0)
            _exit(127);
        if (fd > 2)
            close(fd);
        execlp("perf", "perf", "stat",
               "-p", spid, "--", "sleep",
               secs, (char*)nullptr);
        _exit(127);
    }
    end = mono_s() + CLI_TIMEOUT_S;
    for (;;) {
        pid_t w;
        struct timespec nap;

        w = waitpid(c, &st, WNOHANG);
        if (w == c)
            break;
        if (w < 0 && errno != EINTR) {
            kill(c, SIGKILL);
            waitpid(c, &st, 0);
            return (false);
        }
        if (mono_s() >= end) {
            kill(c, SIGKILL);
            waitpid(c, &st, 0);
            return (false);
        }
        nap.tv_sec = 0;
        nap.tv_nsec = SLEEP_NS;
        nanosleep(&nap, nullptr);
    }
    if (!WIFEXITED(st))
        return (false);
    return (WEXITSTATUS(st) == 0);
}

// concord_profile_snapshot takes a
// one-shot perf stat -p on node plus
// a resource snapshot into outdir.
// Best effort: absent perf still
// lands snapshot.txt, so true. Never
// reaps r pids, only perf children.
bool concord_profile_snapshot(
    Run* r, int node,
    const char* outdir) {
    pid_t pid;

    if (r == nullptr)
        return (false);
    if (node < 0 || node >= r->n)
        return (false);
    if (outdir == nullptr)
        return (false);
    if (!statedir_ok(outdir))
        return (false);
    pid = r->pids[node];
    (void)ensure_dir(outdir);
    if (!write_resource(outdir, pid,
                        node))
        return (false);
    if (pid <= 0)
        return (true);
    if (kill(pid, 0) != 0 &&
        errno == ESRCH)
        return (true);
    if (!have_perf())
        return (true);
    (void)run_perf_stat(pid, outdir);
    return (true);
}

// concord_profile_record_start
// brackets a best effort perf record
// -g -p on node into outdir/ with
// -o perf.data plus a log. Early perf
// exits reap here and read as skip;
// a live child passes to stop. Never
// touches r pids beyond reading.
bool concord_profile_record_start(
    Run* r, int node,
    const char* outdir,
    ConcordProfileRec* rec, char* why,
    size_t nwhy) {
    char data[FILE_LEN];
    char logp[FILE_LEN];
    char spid[PID_LEN];
    pid_t pid;
    pid_t c;
    int m;
    int k;

    if (r == nullptr)
        return (false);
    if (rec == nullptr)
        return (false);
    if (rec->pid > 0) {
        set_why(why, nwhy, "busy");
        return (false);
    }
    if (node < 0 || node >= r->n)
        return (false);
    if (outdir == nullptr)
        return (false);
    if (!statedir_ok(outdir))
        return (false);
    pid = r->pids[node];
    if (pid <= 0) {
        set_why(why, nwhy, "no child");
        return (false);
    }
    if (kill(pid, 0) != 0 &&
        errno == ESRCH) {
        set_why(why, nwhy,
                "child gone");
        return (false);
    }
    (void)ensure_dir(outdir);
    if (!have_perf()) {
        set_why(why, nwhy, "no perf");
        return (false);
    }
    m = snprintf(data, sizeof(data),
                 "%s/perf.data", outdir);
    if (m < 0 ||
        m >= (int)sizeof(data))
        return (false);
    m = snprintf(logp, sizeof(logp),
                 "%s/perf-record.log",
                 outdir);
    if (m < 0 ||
        m >= (int)sizeof(logp))
        return (false);
    m = snprintf(spid, sizeof(spid),
                 "%d", (int)pid);
    if (m < 0 ||
        m >= (int)sizeof(spid))
        return (false);
    c = fork();
    if (c < 0) {
        set_why(why, nwhy,
                "fork fail");
        return (false);
    }
    if (c == 0) {
        int fd;

        fd = open(logp,
                  O_WRONLY | O_CREAT |
                      O_APPEND,
                  0644);
        if (fd < 0)
            _exit(127);
        if (dup2(fd, 1) < 0)
            _exit(127);
        if (dup2(fd, 2) < 0)
            _exit(127);
        if (fd > 2)
            close(fd);
        execlp("perf", "perf",
               "record", "-g", "-p",
               spid, "-o", data,
               (char*)nullptr);
        _exit(127);
    }
    for (k = 0; k < PROFILE_EARLY_N;
         k++) {
        pid_t w;
        struct timespec nap;
        int st;

        w = waitpid(c, &st, WNOHANG);
        if (w == c) {
            if (WIFEXITED(st) &&
                WEXITSTATUS(st) == 0)
                set_why(why, nwhy,
                        "perf left");
            else
                set_why(why, nwhy,
                        "perf fail");
            return (false);
        }
        if (w < 0 && errno == ECHILD) {
            set_why(why, nwhy,
                    "perf lost");
            return (false);
        }
        if (w < 0 && errno != EINTR) {
            kill(c, SIGKILL);
            waitpid(c, &st, 0);
            set_why(why, nwhy,
                    "perf wait");
            return (false);
        }
        nap.tv_sec = 0;
        nap.tv_nsec =
            PROFILE_EARLY_MS * 1000000L;
        nanosleep(&nap, nullptr);
    }
    rec->pid = c;
    set_why(why, nwhy, "");
    return (true);
}

// concord_profile_record_stop ends the
// start above: SIGINT so perf writes
// perf.data, then a bounded reap by
// KILL_WAIT_S then KILL, as in kill.
// Always reaps once and idles rec.
bool concord_profile_record_stop(
    ConcordProfileRec* rec, char* why,
    size_t nwhy) {
    pid_t pid;

    if (rec == nullptr)
        return (false);
    if (rec->pid <= 0) {
        rec->pid = -1;
        set_why(why, nwhy, "idle");
        return (true);
    }
    pid = rec->pid;
    rec->pid = -1;
    if (pid > 0) {
        kill(pid, SIGINT);
        reap_bounded(pid,
                     KILL_WAIT_S);
    }
    set_why(why, nwhy, "");
    return (true);
}
