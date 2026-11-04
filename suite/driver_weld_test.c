#include "../driver/weld.c"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Driver weld gates plus arg
// rejects. No daemons, no docker,
// no forks: rejects return before
// mkdir, pipe, or fork.

// Truncation probe size. Volatile so
// -O2 cannot fold it into the
// snprintf calls below: the strict
// build would otherwise fail the
// intentional truncation probes.
// Always 4 at runtime.
static volatile size_t wl_n = 4;

// Domain sanity: 0..101 only.
static void wl_dom(void) {
    assert(dom_ok(0));
    assert(dom_ok(1));
    assert(dom_ok(101));
    assert(!dom_ok(-1));
    assert(!dom_ok(102));
    assert(!dom_ok(1000000));
}

// Topic: slash-led, slash plus
// alnum plus underscore only.
static void wl_topic(void) {
    assert(topic_ok("/chatter"));
    assert(topic_ok("/a_B9/x"));
    assert(topic_ok("/"));
    assert(!topic_ok(nullptr));
    assert(!topic_ok(""));
    assert(!topic_ok("chatter"));
    assert(!topic_ok("/a b"));
    assert(!topic_ok("/a-b"));
    assert(!topic_ok("/a.b"));
    assert(!topic_ok("/a*b"));
    assert(!topic_ok("/a:b"));
}

// Known peer distros only. Null
// would crash strcmp, so callers
// default it first; not probed.
static void wl_distro(void) {
    assert(distro_ok("humble"));
    assert(distro_ok("jazzy"));
    assert(distro_ok("rolling"));
    assert(!distro_ok("bogus"));
    assert(!distro_ok(""));
}

// Distro python mapping, with a
// default for unknown names.
static void wl_distro_py(void) {
    char buf[16];

    assert(distro_py("humble", buf,
                     sizeof(buf)));
    assert(strcmp(buf,
                 "python3.10") == 0);
    assert(distro_py("jazzy", buf,
                     sizeof(buf)));
    assert(strcmp(buf,
                 "python3.12") == 0);
    assert(distro_py("rolling",
                     buf, sizeof(buf)));
    assert(strcmp(buf,
                 "python3.14") == 0);
    assert(distro_py("bogus", buf,
                     sizeof(buf)));
    assert(strcmp(buf,
                 "python3.12") == 0);
    assert(!distro_py("jazzy",
                      buf,
                      wl_n));
    assert(!distro_py("jazzy",
                      buf, 0));
}

// Path writers: exact text plus
// truncation and null rejects.
static void wl_paths(void) {
    SimNet net;
    char buf[192];

    net = sim_default_net();
    assert(node_home(&net, buf,
                     sizeof(buf), 3));
    assert(strcmp(buf, "/var/tmp/"
                 "resonance/node3") ==
           0);
    assert(!node_home(&net, buf,
                      wl_n, 3));
    assert(!node_home(nullptr, buf,
                      sizeof(buf),
                      0));
    assert(weld_cap("/tmp/x", buf,
                    sizeof(buf), 1));
    assert(strcmp(buf, "/tmp/x/"
                 "weld-1.log") == 0);
    assert(!weld_cap(nullptr, buf,
                     sizeof(buf),
                     0));
    assert(!weld_cap("/tmp/x",
                     buf, wl_n,
                     0));
    assert(ros_cap("/tmp/x", buf,
                   sizeof(buf), 2));
    assert(strcmp(buf, "/tmp/x/"
                 "ros-2.log") == 0);
    assert(!ros_cap(nullptr, buf,
                    sizeof(buf),
                    0));
    assert(!ros_cap("/tmp/x",
                    buf, wl_n,
                    0));
    assert(ns_path(&net, buf,
                   sizeof(buf), 3));
    assert(strcmp(buf, "/var/run/"
                 "netns/resonance_3") ==
           0);
    assert(!ns_path(&net, buf,
                    wl_n, 3));
    assert(!ns_path(nullptr, buf,
                    sizeof(buf),
                    0));
}

// Binary skip probe: explicit paths
// decide; null follows the default
// path repeatably.
static void wl_skip_bin(void) {
    bool a;
    bool b;

    assert(!weld_skip_unless_bin(
        "/bin/sh"));
    assert(weld_skip_unless_bin(
        "/nonexistent-weld-xyz"));
    a = weld_skip_unless_bin(
        nullptr);
    b = weld_skip_unless_bin(
        nullptr);
    assert(a == b);
    assert(a == (access(
                    "./bin/weld",
                    X_OK) != 0));
}

// Pub rejects before pipe or fork:
// nulls, bad domain, bad topic,
// negative node.
static void wl_pub_reject(void) {
    SimNet net;

    net = sim_default_net();
    assert(!weld_pub(
        nullptr, 0, 1, nullptr,
        "/chatter", "hi"));
    assert(!weld_pub(&net, 0, 1,
                     nullptr,
                     "/chatter",
                     nullptr));
    assert(!weld_pub(&net, 0, -1,
                     nullptr,
                     "/chatter",
                     "hi"));
    assert(!weld_pub(&net, 0, 102,
                     nullptr,
                     "/chatter",
                     "hi"));
    assert(!weld_pub(&net, 0, 1,
                     nullptr, "x",
                     "hi"));
    assert(!weld_pub(&net, 0, 1,
                     nullptr, "/a b",
                     "hi"));
    assert(!weld_pub(&net, -1, 1,
                     nullptr,
                     "/chatter",
                     "hi"));
}

// Makes an empty run. Pids, logs,
// fds at -1, two nodes.
static void wl_mk_run(Run *r) {
    int i;

    memset(r, 0, sizeof(*r));
    r->net = sim_default_net();
    r->n = 2;
    r->dead = -1;
    for (i = 0; i < TUN_MAP_MAX;
         i++) {
        r->pids[i] = -1;
        r->logfds[i] = -1;
        r->fds[i] = -1;
    }
}

// Spawn rejects: null, bad slot,
// bad domain, bad topic. The bad
// gates sit before mkdir.
static void wl_spawn_reject(void) {
    Run r;

    assert(!weld_spawn_node(
        nullptr, 0, 1, nullptr,
        nullptr));
    wl_mk_run(&r);
    assert(!weld_spawn_node(
        &r, 2, 1, nullptr,
        nullptr));
    assert(!weld_spawn_node(
        &r, -1, 1, nullptr,
        nullptr));
    assert(!weld_spawn_node(
        &r, 0, -1, nullptr,
        nullptr));
    assert(!weld_spawn_node(
        &r, 0, 1, nullptr, "x"));
}

// Kill on a dead slot is idempotent,
// as in concord_kill.
static void wl_kill_dead(void) {
    Run r;

    wl_mk_run(&r);
    assert(weld_kill(&r, 0,
                     SIGTERM));
    assert(r.dead == 0);
    assert(weld_kill(&r, 0,
                     SIGTERM));
    assert(!weld_kill(nullptr, 0,
                      SIGTERM));
    assert(!weld_kill(&r, 5,
                      SIGTERM));
}

// Restart rejects: null, bad slot,
// bad domain via spawn_echo before
// mkdir. Preserve skips the wipe.
static void wl_restart_reject(void) {
    Run r;

    assert(!weld_restart(
        nullptr, 0, 1, nullptr,
        nullptr, WELD_PRESERVE));
    wl_mk_run(&r);
    assert(!weld_restart(
        &r, 5, 1, nullptr,
        nullptr, WELD_PRESERVE));
    assert(!weld_restart(
        &r, 0, -1, nullptr,
        nullptr, WELD_PRESERVE));
    assert(!weld_restart(
        &r, 0, 1, nullptr, "x",
        WELD_PRESERVE));
}

// ROS peer start rejects before
// mkdir or fork: nulls, unknown
// distro, bad domain, bad topic,
// bad slot.
static void wl_ros_start(void) {
    Run r;
    WeldRos peer;

    memset(&peer, 0,
           sizeof(peer));
    assert(!weld_ros_peer_start(
        nullptr, &peer, 0,
        "jazzy", 1, "/chatter"));
    wl_mk_run(&r);
    assert(!weld_ros_peer_start(
        &r, nullptr, 0, "jazzy",
        1, "/chatter"));
    assert(!weld_ros_peer_start(
        &r, &peer, 0, "bogus", 1,
        "/chatter"));
    assert(!weld_ros_peer_start(
        &r, &peer, 0, "jazzy",
        -1, "/chatter"));
    assert(!weld_ros_peer_start(
        &r, &peer, 0, "jazzy", 1,
        nullptr));
    assert(!weld_ros_peer_start(
        &r, &peer, 0, "jazzy", 1,
        "x"));
    assert(!weld_ros_peer_start(
        &r, &peer, 9, "jazzy", 1,
        "/chatter"));
}

// ROS peer stop: null is false; an
// idle peer stops clean, no fork.
static void wl_ros_stop(void) {
    WeldRos peer;

    assert(!weld_ros_peer_stop(
        nullptr));
    memset(&peer, 0,
           sizeof(peer));
    assert(weld_ros_peer_stop(
        &peer));
    assert(peer.pid <= 0);
}

// Log save appends chunks under the
// statedir; nulls and negatives
// drop; long paths drop.
static void wl_log_save(void) {
    static const char dir[] =
        "/tmp/opencode";
    static const char path[] =
        "/tmp/opencode/weld-7.log";
    char far[300];
    FILE *fp;
    char buf[32];
    size_t n;

    weld_log_save(nullptr, 0,
                  "x", 1);
    weld_log_save((void *)dir,
                  -1, "x", 1);
    weld_log_save((void *)dir, 0,
                  nullptr, 1);
    (void)remove(path);
    weld_log_save((void *)dir, 7,
                  "hello", 5);
    weld_log_save((void *)dir, 7,
                  "!", 1);
    fp = fopen(path, "rb");
    assert(fp != nullptr);
    n = fread(buf, 1, sizeof(buf),
              fp);
    assert(fclose(fp) == 0);
    assert(n == 6);
    assert(memcmp(buf, "hello!",
                 6) == 0);
    memset(far, 'a', sizeof(far));
    far[sizeof(far) - 1] = '\0';
    weld_log_save(far, 0, "x", 1);
    assert(access(far, F_OK) != 0);
    assert(remove(path) == 0);
}

// Runs all checks. Returns 0.
int main(void) {
    wl_dom();
    wl_topic();
    wl_distro();
    wl_distro_py();
    wl_paths();
    wl_skip_bin();
    wl_pub_reject();
    wl_spawn_reject();
    wl_kill_dead();
    wl_restart_reject();
    wl_ros_start();
    wl_ros_stop();
    wl_log_save();
    printf("driver_weld_test: ok\n");
    return (0);
}
