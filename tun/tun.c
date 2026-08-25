#include "tun.h"
#include "../math/math.h"
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

bool open_tun_file(const char* name, int* fd) {
    int fd_internal;
    struct ifreq ifr;
    size_t n;

    if (name == NULL || fd == NULL)
        return (false);

    fd_internal = open("/dev/net/tun", O_RDWR);
    if (fd_internal < 0)
        return (false);

    memset(&ifr, 0, sizeof(ifr));
    n = (size_t)min(2, (uint64_t)(IFNAMSIZ - 1),
                    (uint64_t)strlen(name));
    memcpy(ifr.ifr_name, name, n);
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    if (ioctl(fd_internal, TUNSETIFF, &ifr) < 0) {
        close(fd_internal);
        return (false);
    }

    *fd = fd_internal;
    return (true);
}
