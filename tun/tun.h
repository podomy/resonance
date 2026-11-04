#ifndef TUN_H
#define TUN_H

#include "../node/node.h"
#include "../world/world.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

static constexpr int TUN_MAP_MAX = 100;

typedef struct {
    int fd;
    uint8_t ip[4];
    uint64_t node_id;
} TunSlot;

// TunQos shapes one fd: loss share,
// base plus jitter delay, cap rate,
// dup chance, slow gate, seed.
typedef struct {
    double loss01;
    unsigned long long base_delay_ns;
    unsigned long long jitter_ns;
    unsigned long long kbps;
    int dup_pct;
    int slow_secs;
    uint64_t seed;
} TunQos;

// TunQosSlot pairs TunQos with replay
// counter, token level, drain stamps.
typedef struct {
    int fd;
    TunQos q;
    uint64_t ctr;
    unsigned long long toks;
    unsigned long long last_ns;
    time_t last_slow;
} TunQosSlot;

// TunDropEnt drops one directed edge.
typedef struct {
    int from_fd;
    int to_fd;
} TunDropEnt;

// TunFlow counts one directed pair
// (from_fd to to_fd). pkts counts
// packets written ok; bytes sums
// their n in bytes; radio_drops
// counts tun_forward gate or write
// denials; policy_drops counts QoS
// loss, bucket, or fault drops.
// All uint64_t and saturating.
// Per-packet cost: one branch plus up
// to two saturating adds per dest,
// no syscall or alloc past forward.
typedef struct {
    uint64_t pkts;
    uint64_t bytes;
    uint64_t radio_drops;
    uint64_t policy_drops;
} TunFlow;

typedef struct {
    TunSlot slot[TUN_MAP_MAX];
    size_t n;
    TunQosSlot qos[TUN_MAP_MAX];
    size_t nqos;
    TunDropEnt drop[TUN_MAP_MAX];
    size_t ndrop;
    TunFlow flow[TUN_MAP_MAX][TUN_MAP_MAX];
} TunMap;

// Map holds TUN_MAP_MAX slots; ip is IPv4.
static_assert(TUN_MAP_MAX > 0);
static_assert(sizeof((TunSlot){}.ip) == 4);
static_assert(sizeof((TunMap){}.slot) /
    sizeof(TunSlot) == (size_t)TUN_MAP_MAX);
static_assert(sizeof((TunMap){}.qos) /
    sizeof(TunQosSlot) ==
    (size_t)TUN_MAP_MAX);
static_assert(sizeof((TunMap){}.drop) /
    sizeof(TunDropEnt) ==
    (size_t)TUN_MAP_MAX);
static_assert(sizeof((TunMap){}.flow) /
    sizeof(TunFlow) ==
    (size_t)(TUN_MAP_MAX * TUN_MAP_MAX));

// open_tun_file mints name via TUNSETIFF.
// The fd is CLOEXEC at source, so all
// callers (incl. check/) never leak it.
bool open_tun_file(const char* name, int* fd);

bool tun_map_add(TunMap* map, int fd, const uint8_t ip[4],
                 uint64_t node_id);

// tun_qos_set arms QoS on fd. Zero q
// matches no QoS. Counter resets, the
// bucket refills, stamps zero out.
void tun_qos_set(TunMap *m, int fd,
                 const TunQos *q);

// tun_qos_clear disarms fd; the legacy
// path serves it again.
void tun_qos_clear(TunMap *m, int fd);

// tun_drop_set arms (true) or clears
// (false) one directed unicast edge.
// Fan-out keeps its old shape.
void tun_drop_set(TunMap *m, int from_fd,
                  int to_fd, bool drop);

// tun_drop_clear removes the edge for
// any flag; the flag only mirrors the
// set call shape.
void tun_drop_clear(TunMap *m, int from_fd,
                    int to_fd, bool drop);

// tun_stats_get copies the flow cell
// for (from_fd, to_fd) into out.
// False on null, bad fd, unknown fd;
// true copies, zeros when idle.
// Query cost is two linear fd scans.
bool tun_stats_get(const TunMap *m,
                   int from_fd, int to_fd,
                   TunFlow *out);

// tun_stats_reset zeroes all flow cells;
// slots, QoS, and drops stay intact.
void tun_stats_reset(TunMap *m);

// read(fd), dest IP -> peer, then tun_forward with Node
// positions.
void tun_pump_fd(TunMap* map, int from_fd, MediumGrid* grid,
                 RadioParams* radio, NodeList* nodes);

// tun_pump_fd_nomcast pumps like tun_pump_fd but drops
// IPv4 multicast (224.0.0.0/4) instead of broadcasting
// it. mDNS discovery goes blind while unicast gossip,
// sync, and Noise keep flowing: the anchor path alone
// must carry discovery.
void tun_pump_fd_nomcast(TunMap* map, int from_fd,
                         MediumGrid* grid,
                         RadioParams* radio,
                         NodeList* nodes);

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
