#include "sim/sim.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

// Sim net allocator: historic run 0
// strings, validation rejects, and
// pre-priv unwinds. Nothing here
// needs root, netns, or daemons.

// Run 0 is the historic default:
// fixed dirs, tun base 0, third
// octet 100, host node 0.
static void sn_default(void) {
    SimNet net;

    net = sim_default_net();
    assert(strcmp(net.statedir,
                 "/var/tmp/resonance") ==
           0);
    assert(strcmp(net.ns_prefix,
                 "resonance_") == 0);
    assert(strcmp(net.tun_prefix,
                 "tun") == 0);
    assert(net.tun_base == 0);
    assert(net.under[0] == 192);
    assert(net.under[1] == 168);
    assert(net.under[2] == 100);
    assert(strcmp(net.cgroot,
                 "/sys/fs/cgroup/"
                 "resonance") == 0);
    assert(net.isolate_all == 0);
    assert(sim_net_valid(&net));
}

// Field gates: null, metachars,
// empties, bad underlays, negative
// tun base, overflowing tun names.
static void sn_valid(void) {
    SimNet net;
    SimNet bad;

    assert(!sim_net_valid(nullptr));
    net = sim_default_net();
    bad = net;
    (void)snprintf(
        bad.statedir,
        sizeof(bad.statedir),
        "%s", "a;b");
    assert(!sim_net_valid(&bad));
    bad = net;
    bad.statedir[0] = '\0';
    assert(!sim_net_valid(&bad));
    bad = net;
    (void)snprintf(
        bad.ns_prefix,
        sizeof(bad.ns_prefix),
        "%s", "a b");
    assert(!sim_net_valid(&bad));
    bad = net;
    (void)snprintf(
        bad.tun_prefix,
        sizeof(bad.tun_prefix),
        "%s", "a*b");
    assert(!sim_net_valid(&bad));
    bad = net;
    (void)snprintf(
        bad.cgroot,
        sizeof(bad.cgroot), "%s",
        "a$b");
    assert(!sim_net_valid(&bad));
    bad = net;
    bad.tun_base = -1;
    assert(!sim_net_valid(&bad));
    bad = net;
    bad.under[0] = 10;
    bad.under[1] = 0;
    bad.under[2] = 5;
    assert(!sim_net_valid(&bad));
    bad = net;
    bad.under[0] = 127;
    assert(!sim_net_valid(&bad));
    bad = net;
    bad.under[0] = 0;
    assert(!sim_net_valid(&bad));
    bad = net;
    bad.under[0] = 224;
    assert(!sim_net_valid(&bad));
    bad = net;
    memset(bad.tun_prefix, 'a',
           sizeof(bad.tun_prefix) -
               1);
    bad.tun_prefix[sizeof(
                       bad.tun_prefix) -
                   1] = '\0';
    assert(!sim_net_valid(&bad));
    assert(sim_net_valid(&net));
}

// Checked constructor: out of range
// ids fail; run 0 matches default;
// run 1 and 154 shift disjointly.
static void sn_try_run(void) {
    SimNet net;
    SimNet dflt;

    assert(!sim_net_try_for_run(
        -1, &net));
    assert(!sim_net_try_for_run(
        155, &net));
    assert(!sim_net_try_for_run(
        1000000, &net));
    assert(!sim_net_try_for_run(
        0, nullptr));
    assert(sim_net_try_for_run(
        0, &net));
    dflt = sim_default_net();
    assert(strcmp(net.statedir,
                 dflt.statedir) ==
           0);
    assert(strcmp(net.ns_prefix,
                 dflt.ns_prefix) ==
           0);
    assert(net.tun_base == 0);
    assert(net.under[2] == 100);
    assert(net.isolate_all == 0);
    assert(sim_net_try_for_run(
        1, &net));
    assert(strcmp(net.statedir,
                 "/var/tmp/"
                 "resonance-1") == 0);
    assert(strcmp(net.ns_prefix,
                 "resonance1_") ==
           0);
    assert(net.tun_base == 100);
    assert(net.under[0] == 192);
    assert(net.under[1] == 168);
    assert(net.under[2] == 101);
    assert(strcmp(net.cgroot,
                 "/sys/fs/cgroup/"
                 "resonance-1") == 0);
    assert(net.isolate_all == 0);
    assert(sim_net_valid(&net));
    assert(sim_net_try_for_run(
        154, &net));
    assert(strcmp(net.statedir,
                 "/var/tmp/"
                 "resonance-154") ==
           0);
    assert(strcmp(net.ns_prefix,
                 "resonance154_") ==
           0);
    assert(net.tun_base ==
           15400);
    assert(net.under[2] == 254);
    assert(sim_net_valid(&net));
}

// Teardown unwinds: null is false,
// invalid nets are a true no-op
// that never reach system().
static void sn_teardown(void) {
    SimNet bad;

    assert(!sim_netns_teardown_net(
        nullptr, 2));
    bad = sim_default_net();
    (void)snprintf(
        bad.statedir,
        sizeof(bad.statedir),
        "%s", "a;b");
    assert(!sim_net_valid(&bad));
    assert(sim_netns_teardown_net(
        &bad, 2));
    assert(sim_netns_teardown_net(
        &bad, 1000000));
}

// Skew spawn rejects before parent
// setup: nulls, bad net, bad n,
// bad skew slot, null offset.
static void sn_skew_reject(void) {
    SimNet net;
    pid_t pids[TUN_MAP_MAX];
    int logfds[TUN_MAP_MAX];
    int i;

    net = sim_default_net();
    for (i = 0; i < TUN_MAP_MAX;
         i++) {
        pids[i] = -1;
        logfds[i] = -1;
    }
    assert(!sim_spawn_concord_skew_net(
        nullptr, pids, logfds, 2,
        0, "300"));
    {
        SimNet bad;

        bad = net;
        (void)snprintf(
            bad.statedir,
            sizeof(bad.statedir),
            "%s", "a;b");
        assert(!sim_spawn_concord_skew_net(
            &bad, pids, logfds, 2,
            0, "300"));
    }
    assert(!sim_spawn_concord_skew_net(
        &net, nullptr, logfds, 2,
        0, "300"));
    assert(!sim_spawn_concord_skew_net(
        &net, pids, nullptr, 2,
        0, "300"));
    assert(!sim_spawn_concord_skew_net(
        &net, pids, logfds, 0, 0,
        "300"));
    assert(!sim_spawn_concord_skew_net(
        &net, pids, logfds, 2,
        5, "300"));
    assert(!sim_spawn_concord_skew_net(
        &net, pids, logfds, 2,
        -1, "300"));
    assert(!sim_spawn_concord_skew_net(
        &net, pids, logfds, 2,
        0, nullptr));
}

// Restart rejects before any shell:
// nulls, bad net, bad slot, busy
// pid slot, open log slot.
static void sn_restart_reject(void) {
    SimNet net;
    pid_t pids[TUN_MAP_MAX];
    int logfds[TUN_MAP_MAX];
    int i;

    net = sim_default_net();
    for (i = 0; i < TUN_MAP_MAX;
         i++) {
        pids[i] = -1;
        logfds[i] = -1;
    }
    assert(!sim_restart_concord_net(
        nullptr, pids, logfds,
        0));
    assert(!sim_restart_concord_net(
        &net, nullptr, logfds,
        0));
    assert(!sim_restart_concord_net(
        &net, pids, nullptr, 0));
    assert(!sim_restart_concord_net(
        &net, pids, logfds, -1));
    assert(!sim_restart_concord_net(
        &net, pids, logfds,
        TUN_MAP_MAX));
    pids[0] = 1;
    assert(!sim_restart_concord_net(
        &net, pids, logfds, 0));
    pids[0] = -1;
    logfds[0] = 0;
    assert(!sim_restart_concord_net(
        &net, pids, logfds, 0));
}

// Plain spawn rejects share the
// same front gates; live spawns
// need daemons, untested here.
static void sn_spawn_reject(void) {
    SimNet net;
    pid_t pids[TUN_MAP_MAX];
    int logfds[TUN_MAP_MAX];
    int i;

    net = sim_default_net();
    for (i = 0; i < TUN_MAP_MAX;
         i++) {
        pids[i] = -1;
        logfds[i] = -1;
    }
    assert(!sim_spawn_concord_net(
        nullptr, pids, logfds,
        2));
    assert(!sim_spawn_concord_net(
        &net, pids, logfds, 0));
}

// Runs all checks. Returns 0.
int main(void) {
    sn_default();
    sn_valid();
    sn_try_run();
    sn_teardown();
    sn_skew_reject();
    sn_restart_reject();
    sn_spawn_reject();
    printf("sim_net_test: ok\n");
    return (0);
}
