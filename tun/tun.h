#ifndef TUN_H
#define TUN_H

#include "../node/node.h"
#include "../world/world.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TUN_MAP_MAX 100

typedef struct {
    int fd;
    uint8_t ip[4];
    uint64_t node_id;
} TunSlot;

typedef struct {
    TunSlot slot[TUN_MAP_MAX];
    size_t n;
} TunMap;

bool open_tun_file(const char* name, int* fd);

bool tun_map_add(TunMap* map, int fd, const uint8_t ip[4],
                 uint64_t node_id);

// read(fd), dest IP -> peer, then tun_forward with Node
// positions.
void tun_pump_fd(TunMap* map, int from_fd, MediumGrid* grid,
                 RadioParams* radio, NodeList* nodes);

/*
 * radio_path, then write. delay_ns is not slept here;
 * the Context pump will enqueue that delay later.
 */
bool tun_forward(int to_fd, const uint8_t* buf, size_t n,
                 const MediumGrid* grid,
                 const RadioParams* radio, int64_t ax_nm,
                 int64_t ay_nm, int64_t bx_nm,
                 int64_t by_nm);

#endif
