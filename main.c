#define _GNU_SOURCE
#include "node/node.h"
#include "shared/context.h"
#include "tun/tun.h"
#include "world/world.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Two nodes, two TUNs.
 * Node 0 lives in this namespace; node 1 lives in
 * netns net_namespace_nodeb. TunMap is fd/ip -> node_id;
 * positions live
 * on Node, not on the map. No binary is exec'd yet;
 * this only proves the wire exists for Vulkan to draw.
 */

#define NODES 2

// Runs one shell command. True if it exited zero.
static int run(const char* cmd) {
    return (system(cmd) == 0);
}

// Open a TUN while inside another netns, then come back.
// ns is a path like
// /var/run/netns/net_namespace_nodeb.
static bool open_in_ns(const char* ns, const char* ifname,
                       int* fd) {
    int oldfd, nsfd;

    // Save the current netns so we can return.
    oldfd = open("/proc/self/ns/net", O_RDONLY);
    nsfd = open(ns, O_RDONLY);
    if (oldfd < 0 || nsfd < 0) {
        close(oldfd);
        close(nsfd);
        return false;
    }
    // Enter the target namespace. New interfaces created
    // after this belong there.
    if (setns(nsfd, CLONE_NEWNET) != 0) {
        close(oldfd);
        close(nsfd);
        return false;
    }
    if (!open_tun_file(ifname, fd)) {
        setns(oldfd, CLONE_NEWNET);
        close(oldfd);
        close(nsfd);
        return false;
    }
    // Back to the host namespace. The fd stays valid;
    // the interface stays in net_namespace_nodeb.
    setns(oldfd, CLONE_NEWNET);
    close(oldfd);
    close(nsfd);
    return true;
}

int main(void) {
    Context ctx;
    TunMap map = {0};
    uint8_t ips[NODES][4] = {{10, 0, 0, 1}, {10, 0, 0, 2}};
    const char* if_names[NODES] = {"tun0", "tun1"};
    const char* ns_path[NODES] = {
        NULL, "/var/run/netns/net_namespace_nodeb"};
    uint64_t ids[NODES];
    int fds[NODES];
    pid_t pids[NODES];
    int i;

    // Clock, heap, NodeList, grid (8x8 air, 1 m cells),
    // radio range (2.5 m), Rng.
    if (!context_init(&ctx, 32, 1)) {
        fprintf(stderr, "context_init failed\n");
        return (1);
    }

    // One private stack per simulated machine. Deleting
    // first clears a leftover from a crash.
    run("ip netns del net_namespace_nodeb "
        "2>/dev/null");
    if (!run("ip netns add net_namespace_nodeb"))
        return (1);

    // Node 0's card in this ns; node 1's card inside
    // net_namespace_nodeb. Without root this fails early
    // and we skip, so make check stays green on CI.
    if (!open_tun_file(if_names[0], &fds[0])) {
        printf("skip (%s)\n", strerror(errno));
        return (0);
    }
    if (!open_in_ns(ns_path[1], if_names[1], &fds[1]))
        return (1);

    // One Node per TUN. x = i metres, y = 0, v = 0 for now.
    // TunMap only stores fd/ip/node_id; positions are read
    // from Node via nodelist_find.
    for (i = 0; i < NODES; i++) {
        int64_t x = (int64_t)i * 1000000000LL;
        if (!context_add_node(&ctx, x, 0, 0, 0, &ids[i]))
            return (1);
        if (!tun_map_add(&map, fds[i], ips[i], ids[i]))
            return (1);

        int pid = fork();
        if (pid < 0)
            continue;
        if (pid == 0) {
            if (ns_path[i] != NULL) {
                int nsfd = open(ns_path[i], O_RDONLY);
                if (nsfd >= 0) {
                    setns(nsfd, CLONE_NEWNET);
                    close(nsfd);
                }
            }

            execlp("/home/neil/Documents/concord/concord",
                   "concord", (char*)NULL);
            // Command not found code.
            _exit(127);
        }
        pids[i] = pid;
    }

    // Give each card an address and bring it up. lo in res1
    // must be up or the kernel drops local delivery there.
    if (!run("ip addr add 10.0.0.1/24 dev tun0") ||
        !run("ip link set tun0 up") ||
        !run("ip netns exec net_namespace_nodeb ip link "
             "set lo up") ||
        !run("ip netns exec net_namespace_nodeb ip addr "
             "add 10.0.0.2/24 dev tun1") ||
        !run("ip netns exec net_namespace_nodeb ip link "
             "set tun1 up"))
        return (1);

    // Wire exists. Next commits add the pump loop
    // (poll + tun_pump_fd with Node positions), exec of
    // real binaries inside each netns, and the Vulkan draw
    // of nodes/grid. Nothing is transmitted yet.
    printf("resonance: %zu nodes, %zu tuns ready\n",
           ctx.nodes.len, map.n);
    (void)pids;

    for (i = 0; i < NODES; i++)
        close(fds[i]);
    context_free(&ctx);
    run("ip netns del net_namespace_nodeb");
    return (0);
}
