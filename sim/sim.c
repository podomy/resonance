#define _GNU_SOURCE
#include "sim.h"
#include "../node/node.h"
#include "../shared/context.h"
#include "../tun/tun.h"
#include "../world/world.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// run executes cmd via system.
static int run(const char* cmd) {
    return (system(cmd) == 0);
}

// open_in_ns opens a tun in another netns.
static int open_in_ns(const char* ns, const char* ifname,
                      int* fd) {
    int oldfd, nsfd;

    oldfd = open("/proc/self/ns/net", O_RDONLY);
    nsfd = open(ns, O_RDONLY);
    if (oldfd < 0 || nsfd < 0) {
        close(oldfd);
        close(nsfd);
        return (-1);
    }
    if (setns(nsfd, CLONE_NEWNET) != 0) {
        close(oldfd);
        close(nsfd);
        return (-1);
    }
    if (!open_tun_file(ifname, fd)) {
        setns(oldfd, CLONE_NEWNET);
        close(oldfd);
        close(nsfd);
        return (-1);
    }
    setns(oldfd, CLONE_NEWNET);
    close(oldfd);
    close(nsfd);
    return (0);
}

// ns_name writes resonance_<i> into buf.
static void ns_name(char* buf, size_t n, int i) {
    snprintf(buf, n, "resonance_%d", i);
}

// ns_path writes /var/run/netns/resonance_<i>.
static void ns_path(char* buf, size_t n, int i) {
    snprintf(buf, n, "/var/run/netns/resonance_%d", i);
}

// sim_netns_setup creates one netns per extra node.
bool sim_netns_setup(int n) {
    char name[64], cmd[128];
    int i;

    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    for (i = 1; i < n; i++) {
        ns_name(name, sizeof(name), i);
        snprintf(cmd, sizeof(cmd),
                 "ip netns del %s 2>/dev/null", name);
        run(cmd);
        snprintf(cmd, sizeof(cmd), "ip netns add %s", name);
        if (!run(cmd))
            return (false);
    }
    return (true);
}

// sim_netns_teardown deletes those netns.
bool sim_netns_teardown(int n) {
    char name[64], cmd[128];
    int i;

    for (i = 1; i < n; i++) {
        ns_name(name, sizeof(name), i);
        snprintf(cmd, sizeof(cmd),
                 "ip netns del %s 2>/dev/null", name);
        run(cmd);
    }
    run("ip netns del net_namespace_nodeb 2>/dev/null");
    return (true);
}

// sim_tuns_open opens tun0 in host, tun1.. in netns.
bool sim_tuns_open(int* fds, int n) {
    char tun[64], path[64];
    int i;

    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    if (!open_tun_file("tun0", &fds[0])) {
        printf("skip (%s)\n", strerror(errno));
        return (false);
    }
    for (i = 1; i < n; i++) {
        snprintf(tun, sizeof(tun), "tun%d", i);
        ns_path(path, sizeof(path), i);
        if (open_in_ns(path, tun, &fds[i]) < 0)
            return (false);
    }
    return (true);
}

// sim_nodes_add registers simulated nodes.
// Underlay 192.168.100.0/24 is disjoint from
// overlay 10.0.0.0/16.
bool sim_nodes_add(Context* ctx, TunMap* map, int* fds,
                   int n) {
    uint8_t ip[4];
    uint64_t id;
    int i;

    if (n < 1 || n > TUN_MAP_MAX || n > 254)
        return (false);
    ip[0] = 192;
    ip[1] = 168;
    ip[2] = 100;
    for (i = 0; i < n; i++) {
        int64_t x = (int64_t)i * 1000000000LL;
        ip[3] = (uint8_t)(i + 1);
        if (!context_add_node(ctx, x, 0, 0, 0, &id))
            return (false);
        if (!tun_map_add(map, fds[i], ip, id))
            return (false);
    }
    return (true);
}

// sim_addrs_up assigns underlay IPs and brings links up.
// 192.168.100.0/24 is disjoint from overlay 10.0.0.0/16.
bool sim_addrs_up(int n) {
    char name[64], cmd[256];
    int i;

    if (n < 1 || n > 254)
        return (false);
    if (!run("ip addr add 192.168.100.1/24 dev tun0"))
        return (false);
    if (!run("ip link set tun0 up"))
        return (false);
    if (!run("ip link set tun0 multicast on"))
        return (false);
    if (!run("ip route add 224.0.0.0/4 dev tun0"))
        return (false);
    for (i = 1; i < n; i++) {
        ns_name(name, sizeof(name), i);
        snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip link set lo up",
                 name);
        if (!run(cmd))
            return (false);
        snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip link set lo "
                 "multicast on",
                 name);
        if (!run(cmd))
            return (false);
        snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip addr add "
                 "192.168.100.%d/24 dev tun%d",
                 name, i + 1, i);
        if (!run(cmd))
            return (false);
        snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip link set tun%d up",
                 name, i);
        if (!run(cmd))
            return (false);
        snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip link set tun%d "
                 "multicast on",
                 name, i);
        if (!run(cmd))
            return (false);
        snprintf(cmd, sizeof(cmd),
                 "ip netns exec %s ip route add "
                 "224.0.0.0/4 dev tun%d",
                 name, i);
        if (!run(cmd))
            return (false);
    }
    return (true);
}

// ensure_gossip_key creates the shared gossip
// key once per cluster. Reuses it when present.
static bool ensure_gossip_key(void) {
    struct stat st;

    if (mkdir("/tmp/resonance", 0700) != 0 &&
        errno != EEXIST) {
        return (false);
    }
    if (stat("/tmp/resonance/gossip.key",
             &st) == 0 &&
        st.st_size == 32) {
        return (true);
    }
    if (!run("head -c 32 /dev/urandom > "
             "/tmp/resonance/gossip.key && "
             "chmod 600 "
             "/tmp/resonance/gossip.key")) {
        return (false);
    }
    return (true);
}

// spawn_one seeds node i and forks it. Stores pid and
// logfd. Parent config root must exist. clock_offset is
// the CONCORD_CLOCK_OFFSET value for this node, or NULL
// for true time. anchor_arg is an extra daemon argv
// entry (e.g. "--anchor") or NULL. anchor_octet adds an
// anchors entry for 192.168.100.anchor_octet:7946 to
// every node config when nonzero.
static bool spawn_one(pid_t* pid, int* logfd, int i,
                      const char* clock_offset,
                      const char* anchor_arg,
                      int anchor_octet) {
    int pipedes[2];

    if (pipe(pipedes) < 0) {
        return (false);
    }

    // Per-node XDG_CONFIG_HOME.
    char dir[64];
    snprintf(dir, sizeof(dir), "/tmp/resonance/node%d", i);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return (false);

    // Seed CA from repo certs/ into
    // $XDG_CONFIG_HOME/concord/certs.
    // Concord requires ca.crt/ca.key.
    char certs[128];
    snprintf(certs, sizeof(certs), "%s/concord/certs", dir);
    char cmd[512];

    snprintf(cmd, sizeof(cmd), "mkdir -p %s", certs);
    if (!run(cmd))
        return (false);

    snprintf(cmd, sizeof(cmd),
             "cp certs/ca.crt %s/ 2>/dev/null", certs);
    if (!run(cmd))
        return (false);

    snprintf(cmd, sizeof(cmd),
             "cp certs/ca.key %s/ 2>/dev/null", certs);
    if (!run(cmd))
        return false;

    // Seed shared gossip key into
    // $XDG_CONFIG_HOME/concord/memberservice.
    // Concord requires identical secret.key
    // on every node.
    if (!ensure_gossip_key())
        return (false);
    snprintf(cmd, sizeof(cmd),
             "mkdir -p %s/concord/memberservice",
             dir);
    if (!run(cmd))
        return (false);
    snprintf(cmd, sizeof(cmd),
             "cp /tmp/resonance/gossip.key "
             "%s/concord/memberservice/"
             "secret.key && chmod 600 "
             "%s/concord/memberservice/"
             "secret.key",
             dir, dir);
    if (!run(cmd))
        return (false);

    if (anchor_octet != 0) {
        snprintf(
            cmd, sizeof(cmd),
            "mkdir -p %s/concord && uuid=$(cat "
            "/proc/sys/kernel/random/uuid) && printf "
            "'{\"id\":\"%%s\",\"memberlist_address\":"
            "\"0.0.0.0:7946\",\"advertise_address\":"
            "\"192.168.100.%d\",\"anchors\":"
            "[{\"address\":\"192.168.100.%d:7946\"}]}' "
            "\"$uuid\" > %s/concord/config.json",
            dir, i + 1, anchor_octet, dir);
    } else {
        snprintf(
            cmd, sizeof(cmd),
            "mkdir -p %s/concord && uuid=$(cat "
            "/proc/sys/kernel/random/uuid) && printf "
            "'{\"id\":\"%%s\",\"memberlist_address\":\"0.0."
            "0.0:7946\",\"advertise_address\":\"192.168."
            "100.%d\"}' \"$uuid\" > %s/concord/config.json",
            dir, i + 1, dir);
    }
    if (!run(cmd))
        return (false);

    *pid = fork();

    if (*pid < 0)
        return (false);
    if (*pid == 0) {
        setenv("XDG_CONFIG_HOME", dir, 1);
        if (clock_offset != NULL) {
            setenv("CONCORD_CLOCK_OFFSET", clock_offset,
                   1);
        }
        if (i > 0) {
            char path[64];
            int nsfd;

            ns_path(path, sizeof(path), i);
            nsfd = open(path, O_RDONLY);
            if (nsfd >= 0) {
                setns(nsfd, CLONE_NEWNET);
                close(nsfd);
            }
        }

        // Pipe stdout, stderr to write end.
        dup2(pipedes[1], 1);
        dup2(pipedes[1], 2);

        // Close pipe from fd table.
        close(pipedes[0]);
        close(pipedes[1]);

        if (anchor_arg != NULL) {
            execl("./bin/concord", "concord", anchor_arg,
                  (char*)NULL);
        } else {
            execl("./bin/concord", "concord",
                  (char*)NULL);
        }
        _exit(127);
    }

    close(pipedes[1]);
    *logfd = pipedes[0];
    return (true);
}

// ensure_parent creates the parent config root for
// all nodes. Mode 0700 matches certs.Dir(). EEXIST is
// benign for idempotent setup.
static bool ensure_parent(void) {
    if (mkdir("/tmp/resonance", 0700) != 0 &&
        errno != EEXIST) {
        return (false);
    }
    return (true);
}

// sim_spawn_concord forks concord nodes with isolated
// XDG_CONFIG_HOME.
bool sim_spawn_concord(pid_t* pids, int* logfds, int n) {
    int i;

    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    if (!ensure_parent())
        return (false);

    for (i = 0; i < n; i++) {
        if (!spawn_one(&pids[i], &logfds[i], i, NULL,
                       NULL, 0))
            return (false);
    }
    return (true);
}

// sim_spawn_concord_skew forks like sim_spawn_concord
// but runs node skew_node with CONCORD_CLOCK_OFFSET
// set to offset seconds (e.g. "300"). The offset env
// is the Concord test time hook; production must never
// set it.
bool sim_spawn_concord_skew(pid_t* pids, int* logfds, int n,
                            int skew_node,
                            const char* offset) {
    int i;

    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    if (!ensure_parent())
        return (false);

    for (i = 0; i < n; i++) {
        const char* off;

        off = (i == skew_node) ? offset : NULL;
        if (!spawn_one(&pids[i], &logfds[i], i, off,
                       NULL, 0))
            return (false);
    }
    return (true);
}

// sim_restart_concord wipes node i for a fresh identity
// and forks it again. Caller must have killed and reaped
// the old child and closed its logfd.
bool sim_restart_concord(pid_t* pids, int* logfds, int i) {
    char dir[64], cmd[128];

    snprintf(dir, sizeof(dir), "/tmp/resonance/node%d", i);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (!run(cmd))
        return (false);
    return (spawn_one(&pids[i], &logfds[i], i, NULL,
                      NULL, 0));
}

// sim_spawn_concord_anchor forks like sim_spawn_concord
// but runs node anchor_node with --anchor and points
// every node config at it via anchors. Underlay
// 192.168.100.anchor_octet:7946 must be the anchor
// node's address.
bool sim_spawn_concord_anchor(pid_t* pids, int* logfds,
                              int n, int anchor_node) {
    int i;

    if (n < 1 || n > TUN_MAP_MAX)
        return (false);
    if (anchor_node < 0 || anchor_node >= n)
        return (false);
    if (!ensure_parent())
        return (false);

    for (i = 0; i < n; i++) {
        const char* arg;

        arg = (i == anchor_node) ? "--anchor" : NULL;
        if (!spawn_one(&pids[i], &logfds[i], i, NULL,
                       arg, anchor_node + 1))
            return (false);
    }
    return (true);
}
