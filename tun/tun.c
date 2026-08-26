#include "tun.h"
#include "../math/math.h"
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

bool tun_map_add(TunMap* map, int fd, const uint8_t ip[4],
                 int64_t x_nm, int64_t y_nm) {
    TunSlot* s;

    if (map == NULL || ip == NULL || fd < 0)
        return (false);
    if (map->n >= TUN_MAP_MAX)
        return (false);
    s = &map->slot[map->n];
    s->fd = fd;
    memcpy(s->ip, ip, 4);
    s->x_nm = x_nm;
    s->y_nm = y_nm;
    map->n++;
    return (true);
}

static TunSlot* slot_of_fd(TunMap* map, int fd) {
    size_t i;

    for (i = 0; i < map->n; i++)
        if (map->slot[i].fd == fd)
            return (&map->slot[i]);
    return (NULL);
}

static TunSlot* slot_of_ip(TunMap* map, const uint8_t ip[4],
                           int not_fd) {
    size_t i;

    for (i = 0; i < map->n; i++) {
        if (map->slot[i].fd == not_fd)
            continue;
        if (memcmp(map->slot[i].ip, ip, 4) == 0)
            return (&map->slot[i]);
    }
    return (NULL);
}

void tun_pump_fd(TunMap* map, int from_fd, MediumGrid* grid,
                 RadioParams* radio) {
    uint8_t buf[2048];
    ssize_t n;
    TunSlot* from;
    TunSlot* to;

    if (map == NULL || grid == NULL || radio == NULL)
        return;
    n = read(from_fd, buf, sizeof(buf));
    if (n < 20 || (buf[0] >> 4) != 4)
        return;
    from = slot_of_fd(map, from_fd);
    to = slot_of_ip(map, &buf[16], from_fd);
    if (from == NULL || to == NULL)
        return;
    tun_forward(to->fd, buf, (size_t)n, grid, radio,
                from->x_nm, from->y_nm, to->x_nm, to->y_nm);
}

bool tun_forward(int to_fd, const uint8_t* buf, size_t n,
                 const MediumGrid* grid,
                 const RadioParams* radio, int64_t ax_nm,
                 int64_t ay_nm, int64_t bx_nm,
                 int64_t by_nm) {
    RadioPath rp;
    ssize_t w;

    if (to_fd < 0 || buf == NULL || n == 0)
        return (false);
    if (!radio_path(grid, radio, ax_nm, ay_nm, bx_nm, by_nm,
                    &rp))
        return (false);
    w = write(to_fd, buf, n);
    return (w == (ssize_t)n);
}

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
