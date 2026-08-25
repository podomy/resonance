#define _GNU_SOURCE
#include "../tun/tun.h"
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
 * Node B gets its own network namespace, so its addresses
 * are real. The request crosses res0 -> pump -> res1 into
 * resB, whose kernel answers; the reply rides back through
 * the pump. Success is ping exiting zero.
 */

#define NODE_N 2
#define PKT_MAX 2048

typedef struct {
    int fd;
    uint8_t ip[4];
    const char* name;
} TunNode;

static TunNode node_tab[NODE_N];

static int run(const char* cmd) {
    return (system(cmd) == 0);
}

// Creates resB and opens res1 inside it. Leaves the
// process in the root namespace either way.
static int open_node_b(int* fd) {
    int nsfd;
    int oldfd;

    run("ip netns del resB 2>/dev/null");
    if (!run("ip netns add resB"))
        return (-1);

    oldfd = open("/proc/self/ns/net", O_RDONLY);
    nsfd = open("/var/run/netns/resB", O_RDONLY);
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
    *fd = open_tun_file("res1", fd) ? *fd : -1;
    setns(oldfd, CLONE_NEWNET);
    close(oldfd);
    close(nsfd);
    return (*fd);
}

// Fills the routing table with two fake machines.
static int setup(void) {
    if (!run("ip addr add 10.0.0.1/24 dev res0"))
        return (-1);
    if (!run("ip link set res0 up"))
        return (-1);
    if (!run("ip netns exec resB ip link set lo up") ||
        !run("ip netns exec resB ip addr add "
             "10.0.0.2/24 dev res1") ||
        !run("ip netns exec resB ip link set res1 up"))
        return (-1);
    return (0);
}

static pid_t ping_peer(void) {
    pid_t pid;

    pid = fork();
    if (pid < 0)
        return (-1);
    if (pid == 0) {
        execlp("ping", "ping", "-c2", "-W2", "10.0.0.2",
               (char*)NULL);
        _exit(127);
    }
    return (pid);
}

// Node whose address matches the destination, not the
// sender. NULL if the packet is junk or has no peer.
static TunNode* peer_of(int from_fd, const uint8_t* pkt,
                        ssize_t n) {
    int i;

    if (n < 20 || (pkt[0] >> 4) != 4)
        return (NULL);
    for (i = 0; i < NODE_N; i++) {
        if (node_tab[i].fd == from_fd)
            continue;
        if (memcmp(node_tab[i].ip, pkt + 16, 4) == 0)
            return (&node_tab[i]);
    }
    return (NULL);
}

// Reads one packet from fd and hands it to its peer.
// Returns 1 when a packet crossed, 0 when dropped, -1 on
// error.
static int pump_once(int fd) {
    uint8_t buf[PKT_MAX];
    TunNode* peer;
    ssize_t n;
    ssize_t w;
    int i;

    n = read(fd, buf, sizeof(buf));
    if (n <= 0)
        return (-1);
    peer = peer_of(fd, buf, n);
    if (peer == NULL)
        return (0);
    w = write(peer->fd, buf, (size_t)n);
    if (w != n)
        return (-1);
    for (i = 0; i < NODE_N; i++)
        if (node_tab[i].fd == fd)
            printf("tun_netns: %s -> %s %zd bytes\n",
                   node_tab[i].name, peer->name, n);
    return (1);
}

int main(void) {
    struct pollfd pfds[NODE_N];
    const char* names[NODE_N] = {"res0", "res1"};
    uint8_t ips[NODE_N][4] = {{10, 0, 0, 1}, {10, 0, 0, 2}};
    int fds[NODE_N];
    pid_t pid;
    int status;
    int replied;
    int i, attempt;

    memset(fds, 0, sizeof(fds));
    if (!open_tun_file(names[0], &fds[0])) {
        printf("tun_netns: skip (%s)\n", strerror(errno));
        return (0);
    }
    if (open_node_b(&fds[1]) != fds[1]) {
        fprintf(stderr, "tun_netns: node B setup failed\n");
        return (1);
    }
    if (setup() != 0) {
        fprintf(stderr, "tun_netns: configure failed\n");
        return (1);
    }
    for (i = 0; i < NODE_N; i++) {
        node_tab[i].fd = fds[i];
        node_tab[i].name = names[i];
        memcpy(node_tab[i].ip, ips[i], 4);
        pfds[i].fd = fds[i];
        pfds[i].events = POLLIN;
    }

    pid = ping_peer();
    if (pid < 0)
        return (1);

    replied = 0;
    for (attempt = 0; attempt < 60 && !replied; attempt++) {
        if (poll(pfds, NODE_N, 100) <= 0)
            continue;
        for (i = 0; i < NODE_N; i++) {
            if ((pfds[i].revents & POLLIN) == 0)
                continue;
            if (pump_once(fds[i]) == 1)
                replied++;
        }
    }
    waitpid(pid, &status, 0);

    for (i = 0; i < NODE_N; i++)
        close(fds[i]);
    run("ip netns del resB");
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        fprintf(stderr,
                "tun_netns: ping did not get a reply\n");
        return (1);
    }
    return (0);
}
