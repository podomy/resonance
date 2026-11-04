#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>

#include "../tun/tun.c"

// Pure QoS/drop unit tests: helpers plus
// set/clear admin. No fds, no clock.

static constexpr int NT_N = 64;
static constexpr int NT_MANY = 1'000;

// nt_loss_edges proves 0.0 never and
// 1.0 always drops across counters.
static void nt_loss_edges(void) {
    static constexpr uint64_t SEED = 7;
    int c;

    for (c = 0; c < NT_MANY; c++) {
        assert(!qloss(0.0, SEED,
                      (uint64_t)c));
        assert(qloss(1.0, SEED,
                     (uint64_t)c));
    }
}

// nt_loss_replay proves identical seeds
// replay identical runs, and a new seed
// changes at least one sample.
static void nt_loss_replay(void) {
    static constexpr uint64_t A = 1'234;
    static constexpr uint64_t B = 987;
    bool r1[NT_N];
    int i;
    bool same;

    for (i = 0; i < NT_N; i++)
        r1[i] = qloss(0.5, A, (uint64_t)i);
    for (i = 0; i < NT_N; i++)
        assert(qloss(0.5, A, (uint64_t)i) ==
               r1[i]);
    same = true;
    for (i = 0; i < NT_N; i++)
        if (qloss(0.5, B, (uint64_t)i) !=
            r1[i])
            same = false;
    assert(!same);
}

// nt_loss_frac proves 0.25 drops about
// a quarter over many samples.
static void nt_loss_frac(void) {
    static constexpr uint64_t SEED = 42;
    int hits;
    int i;

    hits = 0;
    for (i = 0; i < NT_MANY * 10; i++)
        if (qloss(0.25, SEED, (uint64_t)i))
            hits++;
    assert(hits > 1'500 && hits < 3'500);
}

// nt_jit_range proves samples stay in
// [0, jit), vary, replay, 0 stays 0.
static void nt_jit_range(void) {
    static constexpr uint64_t SEED = 5;
    static constexpr unsigned long long
        JIT = 1'000'000ULL;
    unsigned long long first;
    int i;
    bool varied;

    assert(qjit(0, SEED, 0) == 0);
    first = qjit(JIT, SEED, 0);
    assert(first < JIT);
    varied = false;
    for (i = 1; i < NT_MANY; i++) {
        unsigned long long v =
            qjit(JIT, SEED, (uint64_t)i);

        assert(v < JIT);
        if (v != first)
            varied = true;
    }
    assert(varied);
    assert(qjit(JIT, SEED, 0) == first);
}

// nt_delay_sum proves base plus jitter
// lands in [base, base + jit).
static void nt_delay_sum(void) {
    TunQos q = {0};
    int i;

    q.base_delay_ns = 500;
    q.jitter_ns = 100;
    q.seed = 9;
    assert(qdelay(nullptr, 3) == 0);
    for (i = 0; i < NT_MANY; i++) {
        unsigned long long d =
            qdelay(&q, (uint64_t)i);

        assert(d >= 500 && d < 600);
    }
}

// nt_cap_math proves one second of rate
// floored at one full read.
static void nt_cap_math(void) {
    assert(qcap(0) ==
           (unsigned long long)TUN_BUF_N);
    assert(qcap(8) ==
           (unsigned long long)TUN_BUF_N);
    assert(qcap(80) == 10'000ULL);
    assert(qcap(8'000) == 1'000'000ULL);
}

// nt_bucket_flow proves spend, deny, and
// refill over stated elapsed time.
static void nt_bucket_flow(void) {
    static constexpr unsigned long long
        KBPS = 80;
    static constexpr unsigned long long
        SEC = 1'000'000'000ULL;
    unsigned long long toks;
    unsigned long long last;

    toks = qcap(KBPS);
    last = SEC;
    assert(qbucket(KBPS, &toks, &last,
                   last, 1'500));
    assert(toks == 8'500ULL);
    assert(last == SEC);
    assert(!qbucket(KBPS, &toks, &last,
                    last, 9'000));
    assert(toks == 8'500ULL);
    assert(qbucket(KBPS, &toks, &last,
                   last + SEC, 10'000));
    assert(toks == 0);
    assert(last == 2 * SEC);
    assert(qbucket(0, &toks, &last, last,
                   1'000'000));
    assert(!qbucket(KBPS, nullptr, &last,
                    last, 1));
}

// nt_dup_edges proves 0 never and 100
// always resends, with seeded replay.
static void nt_dup_edges(void) {
    static constexpr uint64_t SEED = 11;
    int i;

    for (i = 0; i < NT_MANY; i++) {
        assert(!qdup(0, SEED, (uint64_t)i));
        assert(qdup(100, SEED,
                    (uint64_t)i));
        assert(!qdup(-3, SEED,
                     (uint64_t)i));
        assert(qdup(140, SEED,
                    (uint64_t)i));
    }
    for (i = 0; i < NT_N; i++)
        assert(qdup(30, SEED, (uint64_t)i) ==
               qdup(30, SEED, (uint64_t)i));
}

// nt_slow_gate proves drain at most once
// per slow_secs like pump_slow.
static void nt_slow_gate(void) {
    assert(qslow(0, 0, 0));
    assert(qslow(90, 100, 0));
    assert(!qslow(99, 100, 2));
    assert(qslow(98, 100, 2));
    assert(qslow(0, 1'000, 2));
    assert(!qslow(100, 99, 2));
}

// nt_drop_dir proves directed set/clear
// independent per edge.
static void nt_drop_dir(void) {
    TunMap m = {0};

    assert(!dhas(&m, 3, 4));
    tun_drop_set(&m, 3, 4, true);
    assert(dhas(&m, 3, 4));
    assert(!dhas(&m, 4, 3));
    assert(!dhas(&m, 3, 5));
    tun_drop_set(&m, 3, 4, true);
    assert(m.ndrop == 1);
    tun_drop_set(&m, 4, 3, true);
    assert(m.ndrop == 2);
    assert(dhas(&m, 4, 3));
    tun_drop_set(&m, 3, 4, false);
    assert(!dhas(&m, 3, 4));
    assert(dhas(&m, 4, 3));
    tun_drop_clear(&m, 4, 3, false);
    assert(!dhas(&m, 4, 3));
    tun_drop_clear(&m, 4, 3, true);
    assert(m.ndrop == 0);
    tun_drop_set(nullptr, 3, 4, true);
    tun_drop_clear(nullptr, 3, 4, true);
}

// nt_qos_admin proves set stores, set
// resets replay, clear restores legacy.
static void nt_qos_admin(void) {
    TunMap m = {0};
    TunQos q = {0};
    TunQosSlot* s;

    assert(qfind(&m, 7) == nullptr);
    q.loss01 = 0.5;
    q.seed = 77;
    q.dup_pct = 20;
    q.kbps = 80;
    tun_qos_set(&m, 7, &q);
    s = qfind(&m, 7);
    assert(s != nullptr);
    assert(s->q.loss01 == 0.5);
    assert(s->q.seed == 77);
    assert(s->ctr == 0);
    assert(s->toks == qcap(80));
    s->ctr = 41;
    tun_qos_set(&m, 7, &q);
    assert(qfind(&m, 7)->ctr == 0);
    assert(m.nqos == 1);
    tun_qos_clear(&m, 7);
    assert(qfind(&m, 7) == nullptr);
    tun_qos_clear(&m, 7);
    assert(m.nqos == 0);
    tun_qos_set(nullptr, 7, &q);
    tun_qos_set(&m, -1, &q);
    tun_qos_set(&m, 7, nullptr);
    assert(m.nqos == 0);
    tun_qos_clear(nullptr, 7);
}

// nt_legacy proves zero QoS equals the
// legacy path: keep, no dup, no delay,
// uncapped bucket, open slow gate.
static void nt_legacy(void) {
    TunQos q = {0};
    unsigned long long toks;
    unsigned long long last;
    int i;

    toks = 0;
    last = 500;
    for (i = 0; i < NT_MANY; i++) {
        assert(!qloss(q.loss01, q.seed,
                      (uint64_t)i));
        assert(!qdup(q.dup_pct, q.seed,
                     (uint64_t)i));
        assert(qdelay(&q, (uint64_t)i) == 0);
    }
    assert(qbucket(q.kbps, &toks, &last,
                   last, 2'048));
    assert(qslow(0, 0, q.slow_secs));
}

int main(void) {
    nt_loss_edges();
    nt_loss_replay();
    nt_loss_frac();
    nt_jit_range();
    nt_delay_sum();
    nt_cap_math();
    nt_bucket_flow();
    nt_dup_edges();
    nt_slow_gate();
    nt_drop_dir();
    nt_qos_admin();
    nt_legacy();
    printf("net_test: ok\n");
    return (0);
}
