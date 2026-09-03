#define _GNU_SOURCE
#include "node/node.h"
#include "shared/context.h"
#include "tun/tun.h"
#include "world/world.h"
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

#define NODES 2

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

// netns_setup creates net_namespace_nodeb.
static bool netns_setup(void) {
    run("ip netns del net_namespace_nodeb 2>/dev/null");
    if (!run("ip netns add net_namespace_nodeb"))
        return (false);
    return (true);
}

// tuns_open opens tun0 and tun1 in netns.
static bool tuns_open(int fds[NODES]) {
    if (!open_tun_file("tun0", &fds[0])) {
        printf("skip (%s)\n", strerror(errno));
        return (false);
    }
    if (open_in_ns("/var/run/netns/net_namespace_nodeb",
                   "tun1", &fds[1]) < 0)
        return (false);
    return (true);
}

// nodes_add registers simulated nodes.
// Tun IPs are underlay NICs. They must not use Concord's
// overlay 10.0.0.0/16 (cn0 / WireGuard). That prefix is
// local on every node; Join would hit cn0, not the peer.
static bool nodes_add(Context* ctx, TunMap* map,
                      int fds[NODES]) {
    uint8_t ips[NODES][4] = {{192, 168, 100, 1},
                             {192, 168, 100, 2}};
    uint64_t ids[NODES];
    int i;

    for (i = 0; i < NODES; i++) {
        int64_t x = (int64_t)i * 1000000000LL;
        if (!context_add_node(ctx, x, 0, 0, 0, &ids[i]))
            return (false);
        if (!tun_map_add(map, fds[i], ips[i], ids[i]))
            return (false);
    }
    return (true);
}

// addrs_up assigns underlay IPs and brings links up.
// 192.168.100.0/24 is disjoint from overlay 10.0.0.0/16.
static bool addrs_up(void) {
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

// spawn_children forks concord nodes with
// isolated XDG_CONFIG_HOME.
static bool spawn_children(pid_t pids[NODES], int* logfds) {
    const char* nss[NODES] = {NULL, "/var/run/netns/"
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

    for (i = 0; i < NODES; i++) {
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

            // Pipe the stdout, stderr to the
            // write end of the pipe.
            dup2(pipedes[1], 1);
            dup2(pipedes[1], 2);

            // Close and remove pipe from the fd table.
            close(pipedes[0]);
            close(pipedes[1]);

            execl("./concord", "concord", (char*)NULL);
            _exit(127);
        }

        pids[i] = pid;
        close(pipedes[1]);
        logfds[i] = pipedes[0];
    }
    return (true);
}

// pump_until forwards tun traffic and logs
// until all children exit.
static void pump_until(TunMap* map, MediumGrid* grid,
                       RadioParams* radio, NodeList* nodes,
                       int fds[NODES], pid_t pids[NODES],
                       int* logfds) {
    struct pollfd p[2 * NODES];
    int i, status;
    int alive;

    // Adding fds first.
    for (i = 0; i < NODES; i++) {
        p[i].fd = fds[i];
        p[i].events = POLLIN;
    }

    // Adding the logfds next.
    for (i = NODES; i < 2 * NODES; i++) {
        p[i].fd = logfds[i - NODES];
        p[i].events = POLLIN;
    }

    do {
        if (poll(p, 2 * NODES, 100) > 0) {
            for (i = 0; i < NODES; i++) {
                if (p[i].revents & POLLIN)
                    tun_pump_fd(map, fds[i], grid, radio,
                                nodes);
            }
            for (i = 0; i < NODES; i++) {
                if (!(p[NODES + i].revents & POLLIN))
                    continue;
                char buf[1024];
                ssize_t n;
                n = read(logfds[i], buf, sizeof(buf) - 1);
                if (n > 0) {
                    buf[n] = '\0';
                    // Here we print with color.
                    printf("\033[%dm[node %d]\033[0m %s",
                           36 + i, i, buf);
                }
            }
        }
        alive = 0;
        for (i = 0; i < NODES; i++) {
            if (waitpid(pids[i], &status, WNOHANG) == 0)
                alive = 1;
        }
    } while (alive);
}

int main(void) {
    Context ctx;
    TunMap map = {0};
    int fds[NODES];
    // fds of the stdin and stdout of children for logs.
    int logfds[NODES];
    pid_t pids[NODES];
    int i;

    if (!context_init(&ctx, 32, 1)) {
        fprintf(stderr, "context_init failed\n");
        return (1);
    }
    if (!netns_setup())
        return (1);
    if (!tuns_open(fds))
        return (0);
    if (!nodes_add(&ctx, &map, fds))
        return (1);
    if (!addrs_up())
        return (1);
    if (!spawn_children(pids, logfds))
        return (1);

    printf("resonance: %zu nodes, %zu tuns ready\n",
           ctx.nodes.len, map.n);
    pump_until(&map, &ctx.grid, &ctx.radio, &ctx.nodes, fds,
               pids, logfds);

    for (i = 0; i < NODES; i++) {
        kill(pids[i], SIGTERM);
        waitpid(pids[i], NULL, 0);
        close(fds[i]);
        close(logfds[i]);
    }
    context_free(&ctx);
    run("ip netns del net_namespace_nodeb");

    // Remove isolated configs after reap.
    if (!run("rm -rf /tmp/resonance"))
        return (1);

    return (0);
}
