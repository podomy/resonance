#include "../tun/tun.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    int fd;

    if (!open_tun_file("res0", &fd)) {
        perror("open_tun_file");
        return (1);
    }
    printf("res0 fd=%d\n", fd);
    system("ip link show res0");
    close(fd);
    return (0);
}
