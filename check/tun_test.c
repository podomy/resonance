#include "../tun/tun.h"
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// One ICMP probe toward an absent peer. The kernel emits
// the packet into res0 where read() picks it up.
static pid_t ping_peer(void) {
    pid_t pid;

    pid = fork();
    if (pid < 0)
        return (-1);
    if (pid == 0) {
        execlp("ping", "ping", "-c1", "-W1", "10.0.0.2",
               (char*)NULL);
        _exit(127);
    }
    return (pid);
}

// Drains packets until an IPv4 ICMP header shows up. The
// kernel queues its own IPv6 traffic when res0 comes UP.
static int read_ipv4_echo(int fd) {
    uint8_t buf[2048];
    struct pollfd pfd;
    ssize_t n;
    int attempt;

    pfd.fd = fd;
    pfd.events = POLLIN;
    for (attempt = 0; attempt < 4; attempt++) {
        if (poll(&pfd, 1, 500) != 1)
            continue;
        if ((pfd.revents & POLLIN) == 0)
            continue;
        n = read(fd, buf, sizeof(buf));
        if (n <= 0)
            continue;
        printf("tun_test: %zd bytes ver=%u proto=%u\n", n,
               (unsigned)(buf[0] >> 4), (unsigned)(buf[9]));
        if ((buf[0] >> 4) == 4 && buf[9] == 1)
            return (0);
    }
    return (-1);
}

int main(void) {
    int fd;
    pid_t pid;
    int status;
    int rc;

    if (!open_tun_file("res0", &fd)) {
        printf("tun_test: skip (%s)\n", strerror(errno));
        return (0);
    }
    if (system("ip addr add 10.0.0.1/24 dev res0") != 0 ||
        system("ip link set res0 up") != 0 ||
        system("ip -o link show res0 | grep -q UP") != 0) {
        fprintf(stderr, "tun_test: configure failed\n");
        close(fd);
        return (1);
    }

    pid = ping_peer();
    if (pid < 0) {
        close(fd);
        return (1);
    }
    rc = read_ipv4_echo(fd);
    waitpid(pid, &status, 0);

    close(fd);
    if (rc != 0)
        fprintf(stderr, "tun_test: no IPv4 ICMP seen\n");
    return (rc == 0 ? 0 : 1);
}
