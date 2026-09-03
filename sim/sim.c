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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// run executes cmd via system.
static int run(const char *cmd) {
    return (system(cmd) == 0);
}

// open_in_ns opens a tun in another netns.
static int open_in_ns(const char *ns, const char *ifname,
                      int *fd) {
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

// sim_netns_setup creates net_namespace_nodeb.
bool sim_netns_setup(void) {
    run("ip netns del net_namespace_nodeb 2>/dev/null");
    if (!run("ip netns add net_namespace_nodeb"))
        return (false);
    return (true);
}

// sim_tuns_open opens tun0 and tun1 in netns.
bool sim_tuns_open(int fds[SIM_NODES]) {
    if (!open_tun_file("tun0", &fds[0])) {
        printf("skip (%s)\n", strerror(errno));
        return (false);
    }
    if (open_in_ns("/var/run/netns/net_namespace_nodeb",
                   "tun1", &fds[1]) < 0)
        return (false);
    return (true);
}

// sim_nodes_add registers simulated nodes.
// Underlay 192.168.100.0/24 is disjoint from overlay 10.0.0.0/16.
bool sim_nodes_add(Context *ctx, TunMap *map,
                   int fds[SIM_NODES]) {
    uint8_t ips[SIM_NODES][4] = {{192, 168, 100, 1},
                                 {192, 168, 100, 2}};
    uint64_t ids[SIM_NODES];
    int i;

    for (i = 0; i < SIM_NODES; i++) {
        int64_t x = (int64_t)i * 1000000000LL;
        if (!context_add_node(ctx, x, 0, 0, 0, &ids[i]))
            return (false);
        if (!tun_map_add(map, fds[i], ips[i], ids[i]))
            return (false);
    }
    return (true);
}

// sim_addrs_up assigns underlay IPs and brings links up.
// 192.168.100.0/24 is disjoint from overlay 10.0.0.0/16.
bool sim_addrs_up(void) {
    if (!run("ip addr add 192.168.100.1/24 dev tun0"))
        return (false);

    if (!run("ip link set tun0 up"))
        return (false);
    if (!run("ip link set tun0 multicast on"))
        return (false);
    if (!run("ip route add 224.0.0.0/4 dev tun0"))
        return (false);

    if (!run("ip netns exec net_namespace_nodeb ip link "
             "set lo up"))
        return (false);
    if (!run("ip netns exec net_namespace_nodeb ip link "
             "set lo multicast on"))
        return (false);

    if (!run("ip netns exec net_namespace_nodeb ip addr "
             "add 192.168.100.2/24 dev tun1"))
        return (false);

    if (!run("ip netns exec net_namespace_nodeb ip link "
             "set tun1 up"))
        return (false);
    if (!run("ip netns exec net_namespace_nodeb ip link "
             "set tun1 multicast on"))
        return (false);
    if (!run("ip netns exec net_namespace_nodeb ip route "
             "add 224.0.0.0/4 dev tun1"))
        return (false);

    return (true);
}

// sim_spawn_concord forks concord nodes with isolated XDG_CONFIG_HOME.
bool sim_spawn_concord(pid_t pids[SIM_NODES], int *logfds) {
    const char *nss[SIM_NODES] = {NULL, "/var/run/netns/"
                                         "net_namespace_nodeb"};
    int i;

    // Parent config root for all nodes.
    // Mode 0700 matches certs.Dir().
    // EEXIST is benign for idempotent setup.
    char parent_dir[64];
    snprintf(parent_dir, sizeof(parent_dir),
             "/tmp/resonance");
    if (mkdir(parent_dir, 0700) != 0 && errno != EEXIST) {
        return (false);
    }

    for (i = 0; i < SIM_NODES; i++) {
        pid_t pid;

        int pipedes[2];
        if (pipe(pipedes) < 0) {
            return (false);
        }

        // Per-node XDG_CONFIG_HOME.
        char dir[64];
        snprintf(dir, sizeof(dir), "/tmp/resonance/node%d",
                 i);
        if (mkdir(dir, 0700) != 0 && errno != EEXIST)
            return (false);

        // Seed CA from repo certs/ into
        // $XDG_CONFIG_HOME/concord/certs.
        // Concord requires ca.crt/ca.key.
        char certs[128];
        snprintf(certs, sizeof(certs), "%s/concord/certs",
                 dir);
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

        snprintf(
            cmd, sizeof(cmd),
            "mkdir -p %s/concord && uuid=$(cat "
            "/proc/sys/kernel/random/uuid) && printf "
            "'{\"id\":\"%%s\",\"memberlist_address\":\"0.0."
            "0.0:7946\",\"advertise_address\":\"192.168.100.%"
            "d\"}' \"$uuid\" > %s/concord/config.json",
            dir, i + 1, dir);
        if (!run(cmd))
            return (false);

        pid = fork();

        if (pid < 0)
            return (false);
        if (pid == 0) {
            setenv("XDG_CONFIG_HOME", dir, 1);
            if (nss[i] != NULL) {
                int nsfd;

                nsfd = open(nss[i], O_RDONLY);
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

            execl("./concord", "concord", (char *)NULL);
            _exit(127);
        }

        pids[i] = pid;
        close(pipedes[1]);
        logfds[i] = pipedes[0];
    }
    return (true);
}
