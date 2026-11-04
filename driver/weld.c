#define _GNU_SOURCE
#include "weld.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Small buffers: statedir paths plus
// docker argv vectors, nothing more.
static constexpr int PATH_LEN = 192;
static constexpr int DOM_LEN = 16;
static constexpr int ENV_LEN = 256;
static constexpr int AV_LEN = 48;
static constexpr int READ_LEN = 65536;

// PUB_TIMEOUT_S bounds one weld pub,
// matching helpers.go PubTimeout, so a
// hung binary never stalls the run.
// Wall time, as in the Go harness;
// bounded observe waits below use the
// monotonic clock instead.
static constexpr int PUB_TIMEOUT_S = 30;

// PUB_POLL_MS steps the pub wait, so
// reads plus reaps stay responsive.
static constexpr int PUB_POLL_MS = 100;

// WAIT_TIMEOUT_S bounds the pkill plus
// docker-info waits: hung daemons die
// by SIGKILL instead of stalling. 10 s
// class, matching pub patience. Read
// against the monotonic clock, so wall
// jumps never stretch the wait.
static constexpr int WAIT_TIMEOUT_S = 10;

// KILL_WAIT_S bounds TERM waits before
// KILL, mirroring run's 5 s teardown
// escalation. WAIT covers shells;
// this covers signals.
static constexpr int KILL_WAIT_S = 5;

// SLEEP_NS is the 100 ms reap poll
// slice, as in run teardown.
static constexpr int SLEEP_NS = 100000000;

// mark_cloexec sets FD_CLOEXEC on fd,
// mirroring sim cloexec_fd: forked
// helpers never inherit pipes or the
// capture fd.
static void mark_cloexec(int fd) {
    int f;

    f = fcntl(fd, F_GETFD);
    if (f < 0)
        return;
    (void)fcntl(fd, F_SETFD,
                f | FD_CLOEXEC);
}

// mono_s returns CLOCK_MONOTONIC
// seconds for bounded waits, so wall
// jumps never stretch them. PUB keeps
// wall time for Go parity; these waits
// do not.
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

// node_home writes <statedir>/node<i>.
// False on truncation or null net.
static bool node_home(const SimNet *net,
                      char *buf, size_t n,
                      int i) {
    int m;

    if (net == nullptr)
        return (false);
    m = snprintf(buf, n, "%s/node%d",
                 net->statedir, i);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// weld_cap writes <statedir>/weld-<i>.
// False on truncation.
static bool weld_cap(const char *statedir,
                     char *buf, size_t n,
                     int i) {
    int m;

    if (statedir == nullptr)
        return (false);
    m = snprintf(buf, n, "%s/weld-%d.log",
                 statedir, i);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// ros_cap writes <statedir>/ros-<i>.
// False on truncation.
static bool ros_cap(const char *statedir,
                    char *buf, size_t n,
                    int i) {
    int m;

    if (statedir == nullptr)
        return (false);
    m = snprintf(buf, n, "%s/ros-%d.log",
                 statedir, i);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// ns_path writes /var/run/netns/<pre>i,
// the same path sim spawn enters. False
// on truncation or null net, mirroring
// concord cg_node.
static bool ns_path(const SimNet *net,
                    char *buf, size_t n,
                    int i) {
    int m;

    if (net == nullptr)
        return (false);
    m = snprintf(buf, n,
                 "/var/run/netns/%s%d",
                 net->ns_prefix, i);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// dom_ok reports a weld-legal domain.
// Sanity check only; isolation comes
// from unique derivation, not here.
static bool dom_ok(int domain) {
    return (domain >= WELD_DOMAIN_MIN &&
            domain <= WELD_DOMAIN_MAX);
}

// topic_ok reports a weld-legal topic:
// slash-led, slash plus alnum plus
// underscore only. Sanity check only,
// same as above. Fixed charset keeps
// pkill -f plus file greps scoped to
// the unique per-run derivation.
static bool topic_ok(const char *topic) {
    size_t k;

    if (topic == nullptr)
        return (false);
    if (topic[0] != '/')
        return (false);
    for (k = 0; topic[k] != 0; k++) {
        char ch = topic[k];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '/' || ch == '_')
            continue;
        return (false);
    }
    return (true);
}

// mkdir_ok makes dir, EEXIST benign.
static bool mkdir_ok(const char *dir) {
    if (mkdir(dir, 0700) == 0)
        return (true);
    return (errno == EEXIST);
}

// enter_ns moves to <prefix><i> for i
// above 0, plus node 0 when the net is
// isolated. Node 0 keeps host netns
// only on the legacy path. False when
// the ns path truncates.
static bool enter_ns(const SimNet *net,
                     int i) {
    char path[PATH_LEN];
    int fd;

    if (net == nullptr)
        return (false);
    if (i <= 0 && !net->isolate_all)
        return (true);
    if (!ns_path(net, path,
                 sizeof(path), i))
        return (false);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return (false);
    if (setns(fd, CLONE_NEWNET) != 0) {
        close(fd);
        return (false);
    }
    close(fd);
    return (true);
}

// kill_reap_close sends KILL, reaps,
// and closes the logfd. Restart owns
// the kill, like concord_restart.
// Bounded by KILL_WAIT_S then KILL, as
// in run teardown: diverges from
// check/ blocking reap on purpose;
// weld restart has no TERM flush case.
static void kill_reap_close(Run *r,
                            int i) {
    if (r->pids[i] > 0) {
        kill(r->pids[i], SIGKILL);
        reap_bounded(r->pids[i],
                     KILL_WAIT_S);
        r->pids[i] = -1;
    }
    if (r->logfds[i] >= 0) {
        close(r->logfds[i]);
        r->logfds[i] = -1;
    }
}

// spawn_echo forks bin echo with a log
// pipe into pid plus logfd. Timeout 0
// runs until signal, so one child is a
// standing subscriber. Caller checks
// i below r->n; here i needs >= 0.
// Null bin plus topic mean defaults.
// No per-node cgroup: weld echo nodes
// are light and share the run cgroup,
// unlike concord daemons. Dup failures
// exit, as in sim.
static bool spawn_echo(const SimNet *net,
                       pid_t *pid,
                       int *logfd, int i,
                       int domain,
                       const char *bin,
                       const char *topic) {
    char home[PATH_LEN];
    char dom[DOM_LEN];
    int pipedes[2];
    int m;

    if (net == nullptr)
        return (false);
    if (bin == nullptr)
        bin = WELD_BIN_DEFAULT;
    if (topic == nullptr)
        topic = WELD_TOPIC_DEFAULT;
    if (!dom_ok(domain))
        return (false);
    if (!topic_ok(topic))
        return (false);
    if (i < 0)
        return (false);
    if (!mkdir_ok(net->statedir))
        return (false);
    if (!node_home(net, home,
                   sizeof(home), i))
        return (false);
    if (!mkdir_ok(home))
        return (false);
    if (pipe(pipedes) != 0)
        return (false);
    mark_cloexec(pipedes[0]);
    mark_cloexec(pipedes[1]);
    m = snprintf(dom, sizeof(dom), "%d",
                 domain);
    if (m < 0 || m >= (int)sizeof(dom)) {
        close(pipedes[0]);
        close(pipedes[1]);
        return (false);
    }
    *pid = fork();
    if (*pid < 0) {
        close(pipedes[0]);
        close(pipedes[1]);
        return (false);
    }
    if (*pid == 0) {
        setenv("ROS_DOMAIN_ID", dom, 1);
        if (!enter_ns(net, i))
            _exit(127);
        if (dup2(pipedes[1], 1) < 0)
            _exit(127);
        if (dup2(pipedes[1], 2) < 0)
            _exit(127);
        close(pipedes[0]);
        close(pipedes[1]);
        execl(bin, "weld", "echo",
              "--topic", topic,
              "--domain", dom,
              "--timeout", "0",
              (char *)nullptr);
        _exit(127);
    }
    close(pipedes[1]);
    *logfd = pipedes[0];
    return (true);
}

// wipe_home removes one node home with
// no shell: fork plus exec rm only, so
// metachars in home never run. This is
// the argv path that needs no
// sim_net_valid gate, unlike concord
// bounded shell rm with statedir.
// Bounded by WAIT_TIMEOUT_S then KILL:
// a hung rm counts as failure.
static bool wipe_home(const char *home) {
    pid_t pid;
    time_t end;
    int st;

    pid = fork();
    if (pid < 0)
        return (false);
    if (pid == 0) {
        execlp("rm", "rm", "-rf", home,
               (char *)nullptr);
        _exit(127);
    }
    end = mono_s() + WAIT_TIMEOUT_S;
    for (;;) {
        pid_t w;
        struct timespec nap = {
            0, SLEEP_NS,
        };

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
        nanosleep(&nap, nullptr);
    }
    if (!WIFEXITED(st))
        return (false);
    return (WEXITSTATUS(st) == 0);
}

// distro_py writes the distro python dir
// from weld distro.go DistroEnv. False
// on truncation.
static bool distro_py(const char *distro,
                      char *buf, size_t n) {
    const char *py;
    int m;

    py = "python3.12";
    if (strcmp(distro, "humble") == 0)
        py = "python3.10";
    if (strcmp(distro, "rolling") == 0)
        py = "python3.14";
    m = snprintf(buf, n, "%s", py);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// distro_ok reports a known peer distro.
// Unknown names stay out so the distro
// string never reaches argv raw.
static bool distro_ok(const char *distro) {
    return (strcmp(distro, "humble") ==
                    0 ||
            strcmp(distro, "jazzy") ==
                    0 ||
            strcmp(distro, "rolling") ==
                    0);
}

// ros_pkill best-effort kills peer echo
// procs in the container scoped by the
// peer topic. Pkill -f is regex, so the
// topic stays in the fixed charset of
// slash plus alnum plus underscore.
// Bounded by WAIT_TIMEOUT_S, then
// SIGKILL: a hung docker exec never
// stalls teardown.
static void ros_pkill(WeldRos *peer) {
    char pat[192];
    pid_t pid;
    time_t end;
    int m;

    if (peer == nullptr)
        return;
    if (peer->container[0] == '\0')
        return;
    if (peer->topic[0] == '\0')
        return;
    m = snprintf(pat, sizeof(pat),
                 "topic echo %s",
                 peer->topic);
    if (m < 0 ||
        m >= (int)sizeof(pat))
        return;
    pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        execlp("docker", "docker",
               "exec", peer->container,
               "pkill", "-f", pat,
               (char *)nullptr);
        _exit(127);
    }
    end = mono_s() + WAIT_TIMEOUT_S;
    for (;;) {
        pid_t w;
        struct timespec nap = {
            0, 100 * 1000 * 1000,
        };

        w = waitpid(pid, nullptr,
                    WNOHANG);
        if (w == pid)
            return;
        if (w < 0 && errno != EINTR) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return;
        }
        if (mono_s() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            return;
        }
        nanosleep(&nap, nullptr);
    }
}

// weld_skip_unless_bin reports true when
// the run must skip: bin not executable.
// True means must-skip, false means go.
// Null bin means the default path.
bool weld_skip_unless_bin(const char *bin) {
    if (bin == nullptr)
        bin = WELD_BIN_DEFAULT;
    return (access(bin, X_OK) != 0);
}

// weld_skip_unless_docker reports true
// when no daemon answers docker info.
// True means must-skip, false means go.
// Bounded by WAIT_TIMEOUT_S, then
// SIGKILL: a hung daemon counts as
// must-skip, never a stall.
bool weld_skip_unless_docker(void) {
    pid_t pid;
    int st;
    int dev;
    time_t end;

    pid = fork();
    if (pid < 0)
        return (true);
    if (pid == 0) {
        dev = open("/dev/null", O_WRONLY);
        if (dev < 0)
            _exit(127);
        if (dup2(dev, 1) < 0)
            _exit(127);
        if (dup2(dev, 2) < 0)
            _exit(127);
        close(dev);
        execlp("docker", "docker",
               "info", (char *)nullptr);
        _exit(127);
    }
    end = mono_s() + WAIT_TIMEOUT_S;
    for (;;) {
        pid_t w;
        struct timespec nap = {
            0, 100 * 1000 * 1000,
        };

        w = waitpid(pid, &st,
                    WNOHANG);
        if (w == pid)
            break;
        if (w < 0 && errno != EINTR) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (true);
        }
        if (mono_s() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (true);
        }
        nanosleep(&nap, nullptr);
    }
    if (!WIFEXITED(st))
        return (true);
    return (WEXITSTATUS(st) != 0);
}

// weld_spawn_node forks weld echo into r
// pids plus logfds for node i, then
// run_rearm, like concord_spawn. Null
// bin plus topic mean defaults.
bool weld_spawn_node(Run *r, int i,
                     int domain,
                     const char *bin,
                     const char *topic) {
    bool ok;

    if (r == nullptr)
        return (false);
    if (i < 0 || i >= r->n)
        return (false);
    ok = spawn_echo(&r->net,
                    &r->pids[i],
                    &r->logfds[i], i,
                    domain, bin, topic);
    if (!ok)
        return (false);
    run_rearm(r, i);
    return (true);
}

// weld_kill sends sig to node i, reaps
// it, closes its logfd, and notes dead:
// stay-dead, as in concord_kill. True
// when already dead: idempotent, so a
// second kill never fails. Teardown
// skips a dead slot pid but still
// closes its TUN, so marking stays
// safe. Reap bound by KILL_WAIT_S
// then KILL: diverges from check/
// blocking reap on purpose.
bool weld_kill(Run *r, int i, int sig) {
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

// weld_restart kills plus reaps plus
// closes node i then reforks it with
// durability d, then run_rearm. Fresh
// wipes the home, preserve keeps it.
// Null bin plus topic mean defaults.
bool weld_restart(Run *r, int i,
                  int domain,
                  const char *bin,
                  const char *topic,
                  WeldDurability d) {
    char home[PATH_LEN];
    bool ok;

    if (r == nullptr)
        return (false);
    if (i < 0 || i >= r->n)
        return (false);
    kill_reap_close(r, i);
    if (d == WELD_FRESH) {
        if (!node_home(&r->net, home,
                       sizeof(home), i))
            return (false);
        if (!wipe_home(home))
            return (false);
    }
    ok = spawn_echo(&r->net,
                    &r->pids[i],
                    &r->logfds[i], i,
                    domain, bin, topic);
    if (!ok)
        return (false);
    run_rearm(r, i);
    return (true);
}

// weld_pub runs weld pub once from the
// node netns, bounded by PUB_TIMEOUT_S.
// True needs exit 0 plus the sent line.
// Timeout kills the child and is false.
// Null bin plus topic mean defaults.
// Both stdout and stderr feed the pipe:
// parent misses stderr errors else.
bool weld_pub(const SimNet *net, int node,
              int domain, const char *bin,
              const char *topic,
              const char *payload) {
    char dom[DOM_LEN];
    char buf[READ_LEN];
    size_t len;
    int pipedes[2];
    pid_t pid;
    int st;
    time_t end;
    int m;

    if (net == nullptr)
        return (false);
    if (payload == nullptr)
        return (false);
    if (bin == nullptr)
        bin = WELD_BIN_DEFAULT;
    if (topic == nullptr)
        topic = WELD_TOPIC_DEFAULT;
    if (!dom_ok(domain))
        return (false);
    if (!topic_ok(topic))
        return (false);
    if (node < 0)
        return (false);
    if (pipe(pipedes) != 0)
        return (false);
    mark_cloexec(pipedes[0]);
    mark_cloexec(pipedes[1]);
    m = snprintf(dom, sizeof(dom), "%d",
                 domain);
    if (m < 0 || m >= (int)sizeof(dom)) {
        close(pipedes[0]);
        close(pipedes[1]);
        return (false);
    }
    pid = fork();
    if (pid < 0) {
        close(pipedes[0]);
        close(pipedes[1]);
        return (false);
    }
    if (pid == 0) {
        setenv("ROS_DOMAIN_ID", dom, 1);
        if (!enter_ns(net, node))
            _exit(127);
        if (dup2(pipedes[1], 1) < 0)
            _exit(127);
        if (dup2(pipedes[1], 2) < 0)
            _exit(127);
        close(pipedes[0]);
        close(pipedes[1]);
        execl(bin, "weld", "pub",
              "--topic", topic,
              "--domain", dom,
              "--text", payload,
              (char *)nullptr);
        _exit(127);
    }
    close(pipedes[1]);
    len = 0;
    end = time(nullptr) +
        PUB_TIMEOUT_S;
    for (;;) {
        struct pollfd pfd;
        pid_t w;
        ssize_t k;

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
        if (time(nullptr) >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            close(pipedes[0]);
            return (false);
        }
        pfd.fd = pipedes[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1,
                 PUB_POLL_MS) <= 0)
            continue;
        if (!(pfd.revents & POLLIN))
            continue;
        if (len >= sizeof(buf) - 1) {
            char drop[256];

            k = read(pipedes[0], drop,
                     sizeof(drop));
            if (k < 0 &&
                errno == EINTR)
                continue;
            if (k == 0)
                continue;
            continue;
        }
        k = read(pipedes[0], buf + len,
                 sizeof(buf) - len - 1);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            continue;
        len += (size_t)k;
    }
    for (;;) {
        ssize_t k;

        if (len >= sizeof(buf) - 1)
            break;
        k = read(pipedes[0],
                 buf + len,
                 sizeof(buf) - len - 1);
        if (k <= 0)
            break;
        len += (size_t)k;
    }
    close(pipedes[0]);
    if (!WIFEXITED(st))
        return (false);
    if (WEXITSTATUS(st) != 0)
        return (false);
    buf[len] = '\0';
    return (strstr(buf, "sent:") !=
            nullptr);
}

// weld_ros_peer_start launches ros2 topic
// echo in weld-<distro> through docker
// exec argv shaped like the weld helper
// DockerArgs, with the ROS setup as -e
// flags and no shell. Capture lands in
// the per-run statedir. Null distro
// means the default; null topic is
// rejected, not defaulted: the caller
// must pass the run topic.
bool weld_ros_peer_start(Run *r,
                         WeldRos *peer,
                         int i,
                         const char *distro,
                         int domain,
                         const char *topic) {
    char dom[ENV_LEN];
    char rmw[ENV_LEN];
    char ament[ENV_LEN];
    char ld[ENV_LEN];
    char path[ENV_LEN];
    char pypath[ENV_LEN];
    char rdistro[ENV_LEN];
    char container[64];
    char rosbin[128];
    char pre[32];
    char py[16];
    char cap[PATH_LEN];
    char *av[AV_LEN];
    int fd;
    int k;
    int m;
    pid_t pid;

    if (r == nullptr || peer == nullptr)
        return (false);
    if (distro == nullptr ||
        distro[0] == '\0')
        distro = WELD_DISTRO_DEFAULT;
    if (!distro_ok(distro))
        return (false);
    if (!dom_ok(domain))
        return (false);
    if (!topic_ok(topic))
        return (false);
    if (i < 0 || i >= r->n)
        return (false);
    if (!mkdir_ok(r->net.statedir))
        return (false);
    m = snprintf(pre, sizeof(pre),
                 "/opt/ros/%s", distro);
    if (m < 0 || m >= (int)sizeof(pre))
        return (false);
    if (!distro_py(distro, py,
                   sizeof(py)))
        return (false);
    m = snprintf(dom, sizeof(dom),
                 "ROS_DOMAIN_ID=%d",
                 domain);
    if (m < 0 || m >= (int)sizeof(dom))
        return (false);
    m = snprintf(rmw, sizeof(rmw),
                 "RMW_IMPLEMENTATION=%s",
                 WELD_RMW_DEFAULT);
    if (m < 0 || m >= (int)sizeof(rmw))
        return (false);
    m = snprintf(ament, sizeof(ament),
                 "AMENT_PREFIX_PATH=%s",
                 pre);
    if (m < 0 ||
        m >= (int)sizeof(ament))
        return (false);
    m = snprintf(ld, sizeof(ld),
                 "LD_LIBRARY_PATH=%s/opt/"
                 "zenoh_cpp_vendor/lib:%s/"
                 "lib/x86_64-linux-gnu:%s"
                 "/lib",
                 pre, pre, pre);
    if (m < 0 || m >= (int)sizeof(ld))
        return (false);
    m = snprintf(path, sizeof(path),
                 "PATH=%s/bin:/usr/local/"
                 "sbin:/usr/local/bin:/usr/"
                 "sbin:/usr/bin:/sbin:/bin",
                 pre);
    if (m < 0 ||
        m >= (int)sizeof(path))
        return (false);
    if (strcmp(distro, "humble") == 0)
        m = snprintf(pypath,
                     sizeof(pypath),
                     "PYTHONPATH=%s/lib/%s/"
                     "site-packages:%s/local"
                     "/lib/python3.10/"
                     "dist-packages",
                     pre, py, pre);
    else
        m = snprintf(pypath,
                     sizeof(pypath),
                     "PYTHONPATH=%s/lib/%s/"
                     "site-packages",
                     pre, py);
    if (m < 0 ||
        m >= (int)sizeof(pypath))
        return (false);
    m = snprintf(rdistro, sizeof(rdistro),
                 "ROS_DISTRO=%s", distro);
    if (m < 0 ||
        m >= (int)sizeof(rdistro))
        return (false);
    m = snprintf(container,
                 sizeof(container),
                 "weld-%s", distro);
    if (m < 0 ||
        m >= (int)sizeof(container))
        return (false);
    m = snprintf(rosbin, sizeof(rosbin),
                 "%s/bin/ros2", pre);
    if (m < 0 ||
        m >= (int)sizeof(rosbin))
        return (false);
    if (!ros_cap(r->net.statedir, cap,
                 sizeof(cap), i))
        return (false);
    fd = open(cap,
              O_WRONLY | O_CREAT |
                  O_TRUNC | O_CLOEXEC,
              0644);
    if (fd < 0)
        return (false);
    k = 0;
    av[k++] = "docker";
    av[k++] = "exec";
    av[k++] = "-e";
    av[k++] = dom;
    av[k++] = "-e";
    av[k++] = rmw;
    av[k++] = "-e";
    av[k++] = ament;
    av[k++] = "-e";
    av[k++] = ld;
    av[k++] = "-e";
    av[k++] = path;
    av[k++] = "-e";
    av[k++] = pypath;
    av[k++] = "-e";
    av[k++] = rdistro;
    av[k++] = "-e";
    av[k++] = "ROS_VERSION=2";
    av[k++] = "-e";
    av[k++] = "ROS_PYTHON_VERSION=3";
    av[k++] = container;
    av[k++] = rosbin;
    av[k++] = "topic";
    av[k++] = "echo";
    av[k++] = (char *)topic;
    av[k++] = (char *)WELD_ROS_TYPE;
    av[k++] = nullptr;
    pid = fork();
    if (pid < 0) {
        close(fd);
        return (false);
    }
    if (pid == 0) {
        if (dup2(fd, 1) < 0)
            _exit(127);
        if (dup2(fd, 2) < 0)
            _exit(127);
        close(fd);
        execvp("docker", av);
        _exit(127);
    }
    close(fd);
    peer->pid = pid;
    m = snprintf(peer->container,
                 sizeof(peer->container),
                 "%s", container);
    if (m < 0 ||
        m >= (int)sizeof(peer->container))
        return (false);
    m = snprintf(peer->topic,
                 sizeof(peer->topic), "%s",
                 topic);
    if (m < 0 ||
        m >= (int)sizeof(peer->topic))
        return (false);
    m = snprintf(peer->cap,
                 sizeof(peer->cap), "%s",
                 cap);
    if (m < 0 ||
        m >= (int)sizeof(peer->cap))
        return (false);
    return (true);
}

// weld_ros_peer_stop ends the peer:
// TERM plus reap the client, then a
// scoped pkill by topic for survivors.
// Client reap bound by KILL_WAIT_S
// then KILL, as in run teardown:
// diverges from check/ block.
// Pkill itself is bounded.
bool weld_ros_peer_stop(WeldRos *peer) {
    if (peer == nullptr)
        return (false);
    if (peer->pid > 0) {
        kill(peer->pid, SIGTERM);
        reap_bounded(peer->pid,
                     KILL_WAIT_S);
        peer->pid = -1;
    }
    ros_pkill(peer);
    return (true);
}

// RELAY_EARLY_N times
// RELAY_EARLY_MS polls a fresh echo
// for a fast exit, so start fails
// loudly on bad bin or netns. 500
// ms total, as in the perf probe.
static constexpr int RELAY_EARLY_N =
    5;
static constexpr int RELAY_EARLY_MS =
    100;

// ROS_YAML_LEN caps the one-shot
// {data: text} yaml. Truncation
// fails closed, never clips. Sized
// for the largest bench payload
// (tag plus 65536), matching the
// weld argv path.
static constexpr int ROS_YAML_LEN =
    66000;

// ROS_TOPIC_LEN caps the one-shot
// topic: must fit the peer topic
// record, so every published topic
// stays in the scoped pkill length
// domain. Longer fails closed.
static constexpr int ROS_TOPIC_LEN =
    128;

// RelaySlot holds one live relay:
// run plus node key, echo child,
// pump child. Zero init is idle.
// Never in r pids, so teardown
// cannot touch them: stop owns
// both children.
typedef struct {
    const Run *r;
    int node;
    pid_t echo_pid;
    pid_t pump_pid;
    bool live;
} RelaySlot;

// relay_tab holds every live relay.
// Keyed by run plus node, so two
// runs in one process stay apart.
static RelaySlot relay_tab[TUN_MAP_MAX];

// relay_find reports the live slot
// for run plus node, or nullptr.
static RelaySlot *relay_find(
    const Run *r, int node) {
    int k;

    if (r == nullptr)
        return (nullptr);
    for (k = 0; k < TUN_MAP_MAX;
         k++) {
        if (relay_tab[k].live &&
            relay_tab[k].r == r &&
            relay_tab[k].node ==
                node)
            return (&relay_tab[k]);
    }
    return (nullptr);
}

// env_domain reads ROS_DOMAIN_ID,
// mirroring the weld CLI --domain
// default. Unset or empty fails:
// the harness never silently uses
// domain 0. False on bad value or
// out of the weld range.
static bool env_domain(int *out) {
    const char *s;
    long v;
    char *end;

    if (out == nullptr)
        return (false);
    s = getenv("ROS_DOMAIN_ID");
    if (s == nullptr)
        return (false);
    if (s[0] == '\0')
        return (false);
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno != 0)
        return (false);
    if (end == s)
        return (false);
    if (*end != '\0')
        return (false);
    if (v < WELD_DOMAIN_MIN)
        return (false);
    if (v > WELD_DOMAIN_MAX)
        return (false);
    *out = (int)v;
    return (true);
}

// relay_cap writes statedir/relay-i.
// False on truncation.
static bool relay_cap(
    const char *statedir, char *buf,
    size_t n, int i) {
    int m;

    if (statedir == nullptr)
        return (false);
    m = snprintf(buf, n,
                 "%s/relay-%d.log",
                 statedir, i);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// pong_topic writes <base>/pong.
// False on bad base or truncation.
static bool pong_topic(
    const char *base, char *buf,
    size_t n) {
    int m;

    if (!topic_ok(base))
        return (false);
    m = snprintf(buf, n, "%s/pong",
                 base);
    if (m < 0 || m >= (int)n)
        return (false);
    return (true);
}

// relay_pump reads lines from fd and
// republishes each to pong via the
// one-shot pub helper until EOF or
// error, then exits. Child only, so
// it never returns: _exit skips the
// parent stdio flush. Net is a local
// copy; bin stays borrowed, so the
// caller keeps it alive. Lines keep
// every byte but the \n terminator,
// plus one \r for CRLF writers;
// bench payloads are single-line,
// so the relay stays byte exact.
static void relay_pump(
    int fd, const SimNet *net,
    int node, int domain,
    const char *bin,
    const char *pong) {
    FILE *fp;
    char *line;
    size_t cap;
    ssize_t k;

    fp = fdopen(fd, "r");
    if (fp == nullptr)
        _exit(1);
    line = nullptr;
    cap = 0;
    for (;;) {
        k = getline(&line, &cap, fp);
        if (k < 0)
            break;
        if (k > 0 &&
            line[k - 1] == '\n') {
            k--;
            line[k] = '\0';
        }
        if (k > 0 &&
            line[k - 1] == '\r') {
            k--;
            line[k] = '\0';
        }
        (void)weld_pub(net, node,
                       domain, bin,
                       pong, line);
    }
    free(line);
    fclose(fp);
    _exit(0);
}

// relay_died_early reports the echo
// child dead within the settle
// window. Reaps it exactly once
// when found dead, so start fails
// loudly on bad bin or netns.
// Alive past the window reads live.
static bool relay_died_early(
    pid_t pid) {
    int k;

    for (k = 0; k < RELAY_EARLY_N;
         k++) {
        pid_t w;
        struct timespec nap;
        int st;

        w = waitpid(pid, &st,
                    WNOHANG);
        if (w == pid)
            return (true);
        if (w < 0 &&
            errno == ECHILD)
            return (true);
        if (w < 0 && errno != EINTR) {
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return (true);
        }
        nap.tv_sec = 0;
        nap.tv_nsec =
            RELAY_EARLY_MS * 1000000L;
        nanosleep(&nap, nullptr);
    }
    return (false);
}

// weld_relay_start forks a pong relay
// for node: echo on topic piped to a
// pump child republishing each line
// to <topic>/pong. Checks args, the
// ROS_DOMAIN_ID domain, bin exec
// rights, then probes the echo child
// before forking the pump, so setup
// fails loudly. Null topic or bin
// mean defaults. The pump holds its
// own net, domain, and pong copies;
// only bin stays borrowed.
bool weld_relay_start(Run *r, int node,
                      const char *topic,
                      const char *bin) {
    char pong[PATH_LEN];
    char dom[DOM_LEN];
    char cap[PATH_LEN];
    RelaySlot *slot;
    int pipedes[2];
    int domain;
    int fd;
    int m;
    int k;
    pid_t echo;
    pid_t pump;

    if (r == nullptr)
        return (false);
    if (node < 0 || node >= r->n)
        return (false);
    if (bin == nullptr)
        bin = WELD_BIN_DEFAULT;
    if (topic == nullptr)
        topic = WELD_TOPIC_DEFAULT;
    if (!topic_ok(topic))
        return (false);
    if (!env_domain(&domain))
        return (false);
    if (!pong_topic(topic, pong,
                    sizeof(pong)))
        return (false);
    if (relay_find(r, node) !=
        nullptr)
        return (false);
    if (access(bin, X_OK) != 0)
        return (false);
    if (!mkdir_ok(r->net.statedir))
        return (false);
    if (!relay_cap(r->net.statedir,
                   cap, sizeof(cap),
                   node))
        return (false);
    m = snprintf(dom, sizeof(dom),
                 "%d", domain);
    if (m < 0 ||
        m >= (int)sizeof(dom))
        return (false);
    slot = nullptr;
    for (k = 0; k < TUN_MAP_MAX;
         k++) {
        if (!relay_tab[k].live) {
            slot = &relay_tab[k];
            break;
        }
    }
    if (slot == nullptr)
        return (false);
    fd = open(cap,
              O_WRONLY | O_CREAT |
                  O_TRUNC | O_CLOEXEC,
              0644);
    if (fd < 0)
        return (false);
    if (pipe(pipedes) != 0) {
        close(fd);
        return (false);
    }
    mark_cloexec(pipedes[0]);
    mark_cloexec(pipedes[1]);
    echo = fork();
    if (echo < 0) {
        close(pipedes[0]);
        close(pipedes[1]);
        close(fd);
        return (false);
    }
    if (echo == 0) {
        setenv("ROS_DOMAIN_ID", dom,
               1);
        if (!enter_ns(&r->net, node))
            _exit(127);
        if (dup2(pipedes[1], 1) < 0)
            _exit(127);
        if (dup2(fd, 2) < 0)
            _exit(127);
        close(pipedes[0]);
        close(pipedes[1]);
        close(fd);
        execl(bin, "weld", "echo",
              "--topic", topic,
              "--domain", dom,
              "--timeout", "0",
              (char *)nullptr);
        _exit(127);
    }
    close(fd);
    if (relay_died_early(echo)) {
        close(pipedes[0]);
        close(pipedes[1]);
        return (false);
    }
    pump = fork();
    if (pump < 0) {
        kill(echo, SIGTERM);
        reap_bounded(echo,
                     KILL_WAIT_S);
        close(pipedes[0]);
        close(pipedes[1]);
        return (false);
    }
    if (pump == 0) {
        close(pipedes[1]);
        relay_pump(pipedes[0],
                   &r->net, node,
                   domain, bin, pong);
    }
    close(pipedes[0]);
    close(pipedes[1]);
    slot->r = r;
    slot->node = node;
    slot->echo_pid = echo;
    slot->pump_pid = pump;
    slot->live = true;
    return (true);
}

// weld_relay_stop ends the node relay:
// TERM plus reap of both children,
// bound by KILL_WAIT_S then KILL, as
// in peer stop. Claims the slot
// first, so a second call is a
// no-op. Never touches r pids, only
// the relay pair.
void weld_relay_stop(Run *r,
                     int node) {
    RelaySlot *slot;

    slot = relay_find(r, node);
    if (slot == nullptr)
        return;
    slot->live = false;
    if (slot->pump_pid > 0)
        kill(slot->pump_pid,
             SIGTERM);
    if (slot->echo_pid > 0)
        kill(slot->echo_pid,
             SIGTERM);
    reap_bounded(slot->pump_pid,
                 KILL_WAIT_S);
    reap_bounded(slot->echo_pid,
                 KILL_WAIT_S);
    slot->r = nullptr;
    slot->node = 0;
    slot->pump_pid = 0;
    slot->echo_pid = 0;
}

// ros_pub_once publishes text once
// via ros2 topic pub in weld-<distro>
// through docker exec argv shaped
// like the peer env, with no shell.
// Yaml is {data: text}, as in the
// weld Go harness. Null or empty
// distro means the default; unknown
// distros fail closed.
bool ros_pub_once(const SimNet *net,
                  int node,
                  const char *topic,
                  const char *text,
                  const char *distro) {
    char dom[ENV_LEN];
    char rmw[ENV_LEN];
    char ament[ENV_LEN];
    char ld[ENV_LEN];
    char path[ENV_LEN];
    char pypath[ENV_LEN];
    char rdistro[ENV_LEN];
    char container[64];
    char rosbin[128];
    char pre[32];
    char py[16];
    char yaml[ROS_YAML_LEN];
    char *av[AV_LEN];
    int domain;
    int dev;
    int k;
    int m;
    int st;
    time_t end;
    pid_t pid;

    if (net == nullptr)
        return (false);
    if (node < 0)
        return (false);
    if (!topic_ok(topic))
        return (false);
    if (strlen(topic) >=
        (size_t)ROS_TOPIC_LEN)
        return (false);
    if (text == nullptr)
        return (false);
    if (text[0] == '\0')
        return (false);
    if (!env_domain(&domain))
        return (false);
    if (distro == nullptr ||
        distro[0] == '\0') {
        distro = WELD_DISTRO_DEFAULT;
    }
    if (!distro_ok(distro)) {
        return (false);
    }
    m = snprintf(pre, sizeof(pre),
                 "/opt/ros/%s", distro);
    if (m < 0 || m >= (int)sizeof(pre))
        return (false);
    if (!distro_py(distro, py,
                   sizeof(py)))
        return (false);
    m = snprintf(dom, sizeof(dom),
                 "ROS_DOMAIN_ID=%d",
                 domain);
    if (m < 0 ||
        m >= (int)sizeof(dom))
        return (false);
    m = snprintf(rmw, sizeof(rmw),
                 "RMW_IMPLEMENTATION=%s",
                 WELD_RMW_DEFAULT);
    if (m < 0 ||
        m >= (int)sizeof(rmw))
        return (false);
    m = snprintf(ament, sizeof(ament),
                 "AMENT_PREFIX_PATH=%s",
                 pre);
    if (m < 0 ||
        m >= (int)sizeof(ament))
        return (false);
    m = snprintf(ld, sizeof(ld),
                 "LD_LIBRARY_PATH=%s/opt/"
                 "zenoh_cpp_vendor/lib:%s/"
                 "lib/x86_64-linux-gnu:%s"
                 "/lib",
                 pre, pre, pre);
    if (m < 0 || m >= (int)sizeof(ld))
        return (false);
    m = snprintf(path, sizeof(path),
                 "PATH=%s/bin:/usr/local/"
                 "sbin:/usr/local/bin:/usr/"
                 "sbin:/usr/bin:/sbin:/bin",
                 pre);
    if (m < 0 ||
        m >= (int)sizeof(path))
        return (false);
    m = snprintf(pypath, sizeof(pypath),
                 "PYTHONPATH=%s/lib/%s/"
                 "site-packages",
                 pre, py);
    if (m < 0 ||
        m >= (int)sizeof(pypath))
        return (false);
    m = snprintf(rdistro,
                 sizeof(rdistro),
                 "ROS_DISTRO=%s", distro);
    if (m < 0 ||
        m >= (int)sizeof(rdistro))
        return (false);
    m = snprintf(container,
                 sizeof(container),
                 "weld-%s", distro);
    if (m < 0 ||
        m >= (int)sizeof(container))
        return (false);
    m = snprintf(rosbin, sizeof(rosbin),
                 "%s/bin/ros2", pre);
    if (m < 0 ||
        m >= (int)sizeof(rosbin))
        return (false);
    m = snprintf(yaml, sizeof(yaml),
                 "{data: %s}", text);
    if (m < 0 ||
        m >= (int)sizeof(yaml))
        return (false);
    k = 0;
    av[k++] = "docker";
    av[k++] = "exec";
    av[k++] = "-e";
    av[k++] = dom;
    av[k++] = "-e";
    av[k++] = rmw;
    av[k++] = "-e";
    av[k++] = ament;
    av[k++] = "-e";
    av[k++] = ld;
    av[k++] = "-e";
    av[k++] = path;
    av[k++] = "-e";
    av[k++] = pypath;
    av[k++] = "-e";
    av[k++] = rdistro;
    av[k++] = "-e";
    av[k++] = "ROS_VERSION=2";
    av[k++] = "-e";
    av[k++] = "ROS_PYTHON_VERSION=3";
    av[k++] = container;
    av[k++] = rosbin;
    av[k++] = "topic";
    av[k++] = "pub";
    av[k++] = (char *)topic;
    av[k++] = (char *)WELD_ROS_TYPE;
    av[k++] = yaml;
    av[k++] = "--times";
    av[k++] = "1";
    av[k++] = nullptr;
    pid = fork();
    if (pid < 0)
        return (false);
    if (pid == 0) {
        dev = open("/dev/null",
                   O_WRONLY);
        if (dev < 0)
            _exit(127);
        if (dup2(dev, 1) < 0)
            _exit(127);
        if (dup2(dev, 2) < 0)
            _exit(127);
        if (dev > 2)
            close(dev);
        execvp("docker", av);
        _exit(127);
    }
    end = mono_s() + PUB_TIMEOUT_S;
    for (;;) {
        pid_t w;
        struct timespec nap = {
            0, SLEEP_NS,
        };

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
        nanosleep(&nap, nullptr);
    }
    if (!WIFEXITED(st))
        return (false);
    return (WEXITSTATUS(st) == 0);
}

// weld_log_save appends one log chunk
// to <statedir>/weld-<i>.log. Ud
// carries the statedir, so it serves
// straight as the RunLog callback.
// Truncated paths drop the chunk.
void weld_log_save(void *ud, int i,
                   const char *buf,
                   size_t len) {
    const char *statedir;
    char path[PATH_LEN];
    FILE *fp;

    statedir = (const char *)ud;
    if (statedir == nullptr)
        return;
    if (i < 0)
        return;
    if (buf == nullptr)
        return;
    if (!weld_cap(statedir, path,
                  sizeof(path), i))
        return;
    fp = fopen(path, "ab");
    if (fp == nullptr)
        return;
    if (len > 0 &&
        fwrite(buf, 1, len, fp) != len) {
        fclose(fp);
        return;
    }
    if (fclose(fp) != 0)
        return;
}

// PROFILE_STAT_S bounds one perf stat
// sleep: a short sample, never a
// stall. Wait bound reuses
// WAIT_TIMEOUT_S, as in pub.
static constexpr int PROFILE_STAT_S =
    5;

// PROFILE_EARLY_N times
// PROFILE_EARLY_MS polls a fresh perf
// record for a fast exit, so start
// reaps a dead child at once.
static constexpr int PROFILE_EARLY_N =
    5;
static constexpr int PROFILE_EARLY_MS =
    100;

// PROFILE_LINE caps one proc line in
// the snapshot; PROFILE_COPY caps one
// whole proc file dump.
static constexpr int PROFILE_LINE =
    256;
static constexpr int PROFILE_COPY =
    4096;

// set_why stores s into why. Null why
// or zero nwhy keeps nothing.
// Truncates safely, never fails.
static void set_why(char *why,
                    size_t nwhy,
                    const char *s) {
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

// profdir_ok reports a safe outdir:
// non-empty, only [A-Za-z0-9/_.-].
// Outdir feeds mkdir argv plus file
// paths, so keep the same gate as
// concord statedir_ok for parity.
static bool profdir_ok(
    const char *s) {
    size_t k;

    if (s == nullptr)
        return (false);
    if (s[0] == '\0')
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

        fd = open("/dev/null",
                  O_WRONLY);
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
               (char *)nullptr);
        _exit(127);
    }
    end = mono_s() + WAIT_TIMEOUT_S;
    for (;;) {
        pid_t w;
        struct timespec nap = {
            0, SLEEP_NS,
        };

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
static bool ensure_dir(
    const char *dir) {
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
               dir, (char *)nullptr);
        _exit(127);
    }
    end = mono_s() + WAIT_TIMEOUT_S;
    for (;;) {
        pid_t w;
        struct timespec nap = {
            0, SLEEP_NS,
        };

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
        nanosleep(&nap, nullptr);
    }
    if (!WIFEXITED(st))
        return (false);
    return (WEXITSTATUS(st) == 0);
}

// dump_proc appends path into fp up to
// PROFILE_COPY bytes. Best effort: a
// missing file notes, never fails.
static void dump_proc(FILE *fp,
                      const char *path) {
    FILE *in;
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
    const char *outdir, pid_t pid,
    int node) {
    char path[PATH_LEN];
    char proc[PATH_LEN];
    struct rusage self;
    struct rusage kids;
    FILE *fp;
    int m;

    m = snprintf(path, sizeof(path),
                 "%s/snapshot.txt",
                 outdir);
    if (m < 0 ||
        m >= (int)sizeof(path))
        return (false);
    fp = fopen(path, "w");
    if (fp == nullptr)
        return (false);
    fprintf(fp, "node %d pid %d\n",
            node, (int)pid);
    if (getrusage(RUSAGE_SELF, &self) ==
        0)
        fprintf(fp,
                "self maxrss %ld\n",
                (long)self.ru_maxrss);
    if (getrusage(RUSAGE_CHILDREN,
                  &kids) == 0)
        fprintf(fp,
                "kids maxrss %ld\n",
                (long)kids.ru_maxrss);
    if (pid <= 0) {
        fprintf(fp, "child: none\n");
        return (fclose(fp) == 0);
    }
    m = snprintf(proc, sizeof(proc),
                 "/proc/%d/status",
                 (int)pid);
    if (m >= 0 &&
        m < (int)sizeof(proc))
        dump_proc(fp, proc);
    m = snprintf(proc, sizeof(proc),
                 "/proc/%d/stat",
                 (int)pid);
    if (m >= 0 &&
        m < (int)sizeof(proc))
        dump_proc(fp, proc);
    m = snprintf(proc, sizeof(proc),
                 "/proc/%d/io",
                 (int)pid);
    if (m >= 0 &&
        m < (int)sizeof(proc))
        dump_proc(fp, proc);
    return (fclose(fp) == 0);
}

// run_perf_stat runs perf stat -p pid
// for PROFILE_STAT_S into outdir/
// perf-stat.txt. True only on exit 0.
// Bounded by WAIT_TIMEOUT_S then
// KILL, as in wipe_home. Reaps
// exactly once. Failure degrades to
// the resource file, never false up.
static bool run_perf_stat(
    pid_t pid, const char *outdir) {
    char path[PATH_LEN];
    char spid[DOM_LEN];
    char secs[DOM_LEN];
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
               "-p", spid, "--",
               "sleep", secs,
               (char *)nullptr);
        _exit(127);
    }
    end = mono_s() + WAIT_TIMEOUT_S;
    for (;;) {
        pid_t w;
        struct timespec nap = {
            0, SLEEP_NS,
        };

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
        nanosleep(&nap, nullptr);
    }
    if (!WIFEXITED(st))
        return (false);
    return (WEXITSTATUS(st) == 0);
}

// weld_profile_snapshot takes a
// one-shot perf stat -p on node plus
// a resource snapshot into outdir.
// Best effort: absent perf still
// lands snapshot.txt, so true. Never
// reaps r pids, only perf children.
bool weld_profile_snapshot(
    Run *r, int node,
    const char *outdir) {
    pid_t pid;

    if (r == nullptr)
        return (false);
    if (node < 0 || node >= r->n)
        return (false);
    if (outdir == nullptr)
        return (false);
    if (!profdir_ok(outdir))
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

// weld_profile_record_start brackets a
// best effort perf record -g -p on
// node into outdir/ with -o perf.data
// plus a log. Early perf exits reap
// here and read as skip; a live child
// passes to stop. Never touches r
// pids beyond reading.
bool weld_profile_record_start(
    Run *r, int node,
    const char *outdir,
    WeldProfileRec *rec, char *why,
    size_t nwhy) {
    char data[PATH_LEN];
    char logp[PATH_LEN];
    char spid[DOM_LEN];
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
    if (!profdir_ok(outdir))
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
                 "%s/perf.data",
                 outdir);
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
               (char *)nullptr);
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

// weld_profile_record_stop ends the
// start above: SIGINT so perf writes
// perf.data, then a bounded reap by
// KILL_WAIT_S then KILL, as in kill.
// Always reaps once and idles rec.
bool weld_profile_record_stop(
    WeldProfileRec *rec, char *why,
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
