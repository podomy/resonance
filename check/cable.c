#define _GNU_SOURCE
#include "../node/node.h"
#include "../tun/tun.h"
#include "../world/world.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run(const char* s) {
    return (system(s) == 0);
}

// cable checks tun forwarding between netns.
int main(void) {
    int fa, fb;
    pid_t pid;
    int st;

    if (!open_tun_file("tun0", &fa)) {
        printf("cable: skip (%s)\n", strerror(errno));
        return (0);
    }
    run("ip netns del cable 2>/dev/null");
    if (!run("ip netns add cable"))
        return (1);
    int old = open("/proc/self/ns/net", O_RDONLY);
    int ns = open("/var/run/netns/cable", O_RDONLY);
    if (setns(ns, CLONE_NEWNET) != 0)
        return (1);
    if (!open_tun_file("tun1", &fb))
        return (1);
    setns(old, CLONE_NEWNET);
    close(old);
    close(ns);

    if (!run("ip addr add 192.168.100.1/24 dev tun0") ||
        !run("ip link set tun0 up") ||
        !run("ip netns exec cable ip link set lo up") ||
        !run("ip netns exec cable ip addr add 192.168.100.2/24 dev tun1") ||
        !run("ip netns exec cable ip link set tun1 up"))
        return (1);

    MediumGrid g = {0};
    RadioParams r;
    TunMap m = {0};
    NodeList nl = {0};
    Node a;
    Node b;

    r.range_nm = 2500000000ULL;
    a.id = 0;
    a.x_nm = 0;
    a.y_nm = 0;
    a.vx_nm_per_ns = 0;
    a.vy_nm_per_ns = 0;
    b.id = 1;
    b.x_nm = 0;
    b.y_nm = 0;
    b.vx_nm_per_ns = 0;
    b.vy_nm_per_ns = 0;
    uint8_t ipa[4] = {192, 168, 100, 1};
    uint8_t ipb[4] = {192, 168, 100, 2};
    mediumgrid_init(&g, 0, 0, 1000000000ULL, 8, 8, MATERIAL_AIR);
    nodelist_init(&nl, 4);
    nodelist_push(&nl, a);
    nodelist_push(&nl, b);
    tun_map_add(&m, fa, ipa, 0);
    tun_map_add(&m, fb, ipb, 1);

    pid = fork();
    if (pid == 0) {
        execlp("ping", "ping", "-c1", "-W2", "192.168.100.2", NULL);
        _exit(127);
    }
    struct pollfd p[2] = {{fa, POLLIN, 0}, {fb, POLLIN, 0}};
    while (waitpid(pid, &st, WNOHANG) == 0) {
        if (poll(p, 2, 100) <= 0)
            continue;
        if (p[0].revents & POLLIN)
            tun_pump_fd(&m, fa, &g, &r, &nl);
        if (p[1].revents & POLLIN)
            tun_pump_fd(&m, fb, &g, &r, &nl);
    }
    close(fa);
    close(fb);
    mediumgrid_free(&g);
    nodelist_free(&nl);
    run("ip netns del cable");
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
        return (0);
    fprintf(stderr, "cable: ping failed\n");
    return (1);
}
