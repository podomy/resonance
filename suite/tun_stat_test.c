#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>

#include "../tun/tun.c"

// Tun stats cells: add, get, reset.
// QoS math plus drop admin already
// live in net_test.c; only the map
// plus stats surface is probed here.

// Add rejects: nulls, bad fd.
static void ts_add_reject(void) {
    TunMap m;
    uint8_t ip[4];

    memset(&m, 0, sizeof(m));
    ip[0] = 10;
    ip[1] = 0;
    ip[2] = 0;
    ip[3] = 1;
    assert(!tun_map_add(nullptr,
                        10, ip, 1));
    assert(!tun_map_add(&m, 10,
                        nullptr, 1));
    assert(!tun_map_add(&m, -1,
                        ip, 1));
    assert(m.n == 0);
}

// Fill to capacity; the next add
// fails; stored cells match.
static void ts_add_fill(void) {
    TunMap m;
    uint8_t ip[4];
    int i;

    memset(&m, 0, sizeof(m));
    for (i = 0; i < TUN_MAP_MAX;
         i++) {
        ip[0] = 192;
        ip[1] = 168;
        ip[2] = 7;
        ip[3] = (uint8_t)i;
        assert(tun_map_add(
            &m, 10 + i, ip,
            (uint64_t)i));
    }
    assert(m.n ==
           (size_t)TUN_MAP_MAX);
    ip[0] = 192;
    ip[1] = 168;
    ip[2] = 7;
    ip[3] = 200;
    assert(!tun_map_add(&m, 500,
                        ip, 9));
    assert(m.slot[0].fd == 10);
    assert(m.slot[0].ip[0] ==
           192);
    assert(m.slot[0].ip[3] == 0);
    assert(m.slot[0].node_id ==
           0);
    assert(m.slot[99].fd == 109);
    assert(m.slot[99].node_id ==
           99);
}

// Get rejects: nulls, negatives,
// unknown fds.
static void ts_get_reject(void) {
    TunMap m;
    TunFlow out;
    uint8_t ip[4];

    memset(&m, 0, sizeof(m));
    ip[0] = 10;
    ip[1] = 0;
    ip[2] = 0;
    ip[3] = 1;
    assert(tun_map_add(&m, 10,
                       ip, 1));
    assert(!tun_stats_get(
        nullptr, 10, 10, &out));
    assert(!tun_stats_get(
        &m, 10, 10, nullptr));
    assert(!tun_stats_get(
        &m, -1, 10, &out));
    assert(!tun_stats_get(
        &m, 10, -1, &out));
    assert(!tun_stats_get(
        &m, 500, 10, &out));
    assert(!tun_stats_get(
        &m, 10, 500, &out));
}

// Idle cells read back zero; a
// self pair reads too.
static void ts_get_zero(void) {
    TunMap m;
    TunFlow out;
    uint8_t ip[4];

    memset(&m, 0, sizeof(m));
    ip[0] = 10;
    ip[1] = 0;
    ip[2] = 0;
    ip[3] = 1;
    assert(tun_map_add(&m, 10,
                       ip, 1));
    ip[3] = 2;
    assert(tun_map_add(&m, 11,
                       ip, 2));
    assert(tun_stats_get(&m, 10,
                         11, &out));
    assert(out.pkts == 0);
    assert(out.bytes == 0);
    assert(out.radio_drops == 0);
    assert(out.policy_drops ==
           0);
    assert(tun_stats_get(&m, 10,
                         10, &out));
    assert(out.pkts == 0);
}

// Get copies: a planted cell reads
// back whole; edits to the copy
// leave the map alone.
static void ts_copy(void) {
    TunMap m;
    TunFlow out;
    TunFlow cell;
    uint8_t ip[4];

    memset(&m, 0, sizeof(m));
    ip[0] = 10;
    ip[1] = 0;
    ip[2] = 0;
    ip[3] = 1;
    assert(tun_map_add(&m, 10,
                       ip, 1));
    ip[3] = 2;
    assert(tun_map_add(&m, 11,
                       ip, 2));
    cell.pkts = 5;
    cell.bytes = 600;
    cell.radio_drops = 2;
    cell.policy_drops = 3;
    m.flow[0][1] = cell;
    assert(tun_stats_get(&m, 10,
                         11, &out));
    assert(out.pkts == 5);
    assert(out.bytes == 600);
    assert(out.radio_drops == 2);
    assert(out.policy_drops ==
           3);
    out.pkts = 99;
    assert(tun_stats_get(&m, 10,
                         11, &out));
    assert(out.pkts == 5);
    assert(tun_stats_get(&m, 11,
                         10, &out));
    assert(out.pkts == 0);
}

// Reset zeroes every flow cell but
// keeps slots; null is a no-op.
static void ts_reset(void) {
    TunMap m;
    TunFlow out;
    TunFlow cell;
    uint8_t ip[4];

    tun_stats_reset(nullptr);
    memset(&m, 0, sizeof(m));
    ip[0] = 10;
    ip[1] = 0;
    ip[2] = 0;
    ip[3] = 1;
    assert(tun_map_add(&m, 10,
                       ip, 1));
    ip[3] = 2;
    assert(tun_map_add(&m, 11,
                       ip, 2));
    cell.pkts = 5;
    cell.bytes = 600;
    cell.radio_drops = 2;
    cell.policy_drops = 3;
    m.flow[0][1] = cell;
    m.flow[1][1] = cell;
    tun_stats_reset(&m);
    assert(tun_stats_get(&m, 10,
                         11, &out));
    assert(out.pkts == 0);
    assert(out.bytes == 0);
    assert(out.radio_drops == 0);
    assert(out.policy_drops ==
           0);
    assert(tun_stats_get(&m, 11,
                         11, &out));
    assert(out.pkts == 0);
    assert(m.n == 2);
    assert(m.slot[0].fd == 10);
    assert(m.slot[1].fd == 11);
    assert(m.slot[0].ip[3] ==
           1);
}

int main(void) {
    ts_add_reject();
    ts_add_fill();
    ts_get_reject();
    ts_get_zero();
    ts_copy();
    ts_reset();
    printf("tun_stat_test: ok\n");
    return (0);
}
