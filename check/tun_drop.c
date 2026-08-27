#define _GNU_SOURCE
#include "../node/node.h"
#include "../tun/tun.h"
#include "../world/world.h"
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

static int run(const char* cmd) {
    return (system(cmd) == 0);
}

int main(void) {
    int fd_a, fd_b, oldfd, nsfd;
    pid_t pid;
    int status;
    struct pollfd p[2];
    MediumGrid grid = {0};
    RadioParams radio;
    TunMap map = {0};
    NodeList nodes = {0};
    uint8_t ip_a[4] = {10, 0, 0, 1};
    uint8_t ip_b[4] = {10, 0, 0, 2};
    Node na, nb;

    na.id = 0;
    na.x_nm = 0;
    na.y_nm = 0;
    na.vx_nm_per_ns = 0;
    na.vy_nm_per_ns = 0;
    nb.id = 1;
    nb.x_nm = 10000000000LL;
    nb.y_nm = 0;
    nb.vx_nm_per_ns = 0;
    nb.vy_nm_per_ns = 0;

    if (!open_tun_file("tun_node_a", &fd_a)) {
        printf("tun_drop: skip (%s)\n", strerror(errno));
        return (0);
    }

    run("ip netns del net_namespace_nodeb 2>/dev/null");
    if (!run("ip netns add net_namespace_nodeb"))
        return (1);

    oldfd = open("/proc/self/ns/net", O_RDONLY);
    nsfd = open("/var/run/netns/net_namespace_nodeb",
                O_RDONLY);
    if (oldfd < 0 || nsfd < 0)
        return (1);
    if (setns(nsfd, CLONE_NEWNET) != 0)
        return (1);
    if (!open_tun_file("tun_node_b", &fd_b))
        return (1);
    if (setns(oldfd, CLONE_NEWNET) != 0)
        return (1);
    close(oldfd);
    close(nsfd);

    if (!run("ip addr add 10.0.0.1/24 dev tun_node_a") ||
        !run("ip link set tun_node_a up"))
        return (1);
    if (!run("ip netns exec net_namespace_nodeb ip link "
             "set lo up") ||
        !run("ip netns exec net_namespace_nodeb ip addr "
             "add 10.0.0.2/24 dev tun_node_b") ||
        !run("ip netns exec net_namespace_nodeb ip link "
             "set tun_node_b up"))
        return (1);

    radio.range_nm = 2500000000ULL;
    if (!mediumgrid_init(&grid, 0, 0, 1000000000ULL, 8, 8,
                         MATERIAL_AIR))
        return (1);
    if (!nodelist_init(&nodes, 4) ||
        !nodelist_push(&nodes, na) ||
        !nodelist_push(&nodes, nb) ||
        !tun_map_add(&map, fd_a, ip_a, na.id) ||
        !tun_map_add(&map, fd_b, ip_b, nb.id))
        return (1);

    pid = fork();
    if (pid < 0)
        return (1);
    if (pid == 0) {
        execlp("ping", "ping", "-c2", "-W2", "10.0.0.2",
               (char*)NULL);
        _exit(127);
    }

    p[0].fd = fd_a;
    p[0].events = POLLIN;
    p[1].fd = fd_b;
    p[1].events = POLLIN;

    do {
        if (poll(p, 2, 100) <= 0)
            continue;
        if (p[0].revents & POLLIN)
            tun_pump_fd(&map, fd_a, &grid, &radio, &nodes);
        if (p[1].revents & POLLIN)
            tun_pump_fd(&map, fd_b, &grid, &radio, &nodes);
    } while (waitpid(pid, &status, WNOHANG) == 0);

    close(fd_a);
    close(fd_b);
    mediumgrid_free(&grid);
    nodelist_free(&nodes);
    run("ip netns del net_namespace_nodeb");
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        return (0);
    fprintf(stderr, "tun_drop: ping should have failed\n");
    return (1);
}
