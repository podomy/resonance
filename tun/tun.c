#define _GNU_SOURCE
#include "tun.h"
#include "../math/math.h"
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// Read buffer and IPv4 header floor.
static constexpr int TUN_BUF_N = 2'048;
static constexpr int IPV4_MIN_N = 20;

// tun_map_add records fd plus ip and id.
// False on null, bad fd, or a full map.
bool tun_map_add(TunMap* map, int fd, const uint8_t ip[4],
                 uint64_t node_id) {
    TunSlot* s;

    if (map == nullptr || ip == nullptr || fd < 0)
        return (false);
    if (map->n >= TUN_MAP_MAX)
        return (false);
    s = &map->slot[map->n];
    s->fd = fd;
    memcpy(s->ip, ip, sizeof(s->ip));
    s->node_id = node_id;
    map->n++;
    return (true);
}

// slot_of_fd finds the slot owning fd,
// or nullptr when absent.
static TunSlot* slot_of_fd(TunMap* map, int fd) {
    size_t i;

    for (i = 0; i < map->n; i++)
        if (map->slot[i].fd == fd)
            return (&map->slot[i]);
    return (nullptr);
}

// slot_of_ip finds the slot for ip,
// skipping not_fd, or nullptr.
static TunSlot* slot_of_ip(TunMap* map, const uint8_t ip[4],
                           int not_fd) {
    size_t i;

    for (i = 0; i < map->n; i++) {
        if (map->slot[i].fd == not_fd)
            continue;
        if (memcmp(map->slot[i].ip, ip,
                   sizeof((TunSlot){}.ip)) == 0)
            return (&map->slot[i]);
    }
    return (nullptr);
}

// Golden step spreads consecutive
// counters across the hash space.
static constexpr uint64_t QSTEP =
    0x9E3779B97F4A7C15ULL;

// qmix hashes seed plus ctr; splitmix64
// finalizer, same pair replays same.
static uint64_t qmix(uint64_t seed,
                     uint64_t ctr) {
    static constexpr uint64_t M1 =
        0xBF58476D1CE4E5B9ULL;
    static constexpr uint64_t M2 =
        0x94D049BB133111EBULL;
    static constexpr int S1 = 30;
    static constexpr int S2 = 27;
    static constexpr int S3 = 31;
    uint64_t z;

    z = seed + ctr * QSTEP;
    z = (z ^ (z >> S1)) * M1;
    z = (z ^ (z >> S2)) * M2;
    return (z ^ (z >> S3));
}

// qu01 maps (seed, ctr) to [0, 1).
// Same pair replays the same value.
static double qu01(uint64_t seed,
                   uint64_t ctr) {
    static constexpr int TOP = 11;
    static constexpr double SPAN =
        9007199254740992.0;
    uint64_t h;

    h = qmix(seed, ctr) >> TOP;
    return ((double)h / SPAN);
}

// qloss samples loss01: 0 never drops,
// >= 1 always drops, else u < loss01.
static bool qloss(double loss01, uint64_t seed,
                  uint64_t ctr) {
    static constexpr int SALT = 11;

    if (loss01 <= 0.0)
        return (false);
    if (loss01 >= 1.0)
        return (true);
    return (qu01(seed ^ (uint64_t)SALT,
                 ctr) < loss01);
}

// qjit samples [0, jit); 0 stays 0.
static unsigned long long qjit(
    unsigned long long jit, uint64_t seed,
    uint64_t ctr) {
    static constexpr int SALT = 77;

    if (jit == 0)
        return (0);
    return ((unsigned long long)(qu01(
        seed ^ (uint64_t)SALT,
        ctr) * (double)jit));
}

// qdelay sums base plus jitter. Pump
// never sleeps it; Context enqueues it.
static unsigned long long qdelay(
    const TunQos* q, uint64_t ctr) {
    if (q == nullptr)
        return (0);
    return (q->base_delay_ns +
            qjit(q->jitter_ns, q->seed,
                 ctr));
}

// qdup samples dup_pct: <= 0 never,
// >= 100 always resends a copy.
static bool qdup(int pct, uint64_t seed,
                 uint64_t ctr) {
    static constexpr int SALT = 131;
    static constexpr int FULL = 100;
    static constexpr double SCALE = 100.0;

    if (pct <= 0)
        return (false);
    if (pct >= FULL)
        return (true);
    return (qu01(seed ^ (uint64_t)SALT,
                 ctr) * SCALE <
            (double)pct);
}

// qcap maps kbps to bucket bytes: one
// second of rate, floored at one full
// read so idle links emit one packet.
static unsigned long long qcap(
    unsigned long long kbps) {
    static constexpr unsigned long long
        PER_KBPS = 125ULL;
    unsigned long long cap;

    cap = kbps * PER_KBPS;
    if (cap < (unsigned long long)TUN_BUF_N)
        return ((unsigned long long)TUN_BUF_N);
    return (cap);
}

// qbucket refills toks over el_ns then
// spends n. 0 kbps is uncapped. Deny
// keeps toks but still stamps last.
static bool qbucket(unsigned long long kbps,
                    unsigned long long* toks,
                    unsigned long long* last,
                    unsigned long long now,
                    size_t n) {
    static constexpr int PER_NS = 8000000;
    unsigned long long cap;
    double add;

    if (kbps == 0)
        return (true);
    if (toks == nullptr || last == nullptr)
        return (false);
    cap = qcap(kbps);
    add = (double)kbps * (double)(now - *last) /
          (double)PER_NS;
    *last = now;
    if (add >= (double)cap) {
        *toks = cap;
    } else {
        *toks += (unsigned long long)add;
        if (*toks > cap)
            *toks = cap;
    }
    if (*toks < (unsigned long long)n)
        return (false);
    *toks -= (unsigned long long)n;
    return (true);
}

// qslow mirrors pump_slow: drain at most
// once per secs; secs <= 0 never defers.
static bool qslow(time_t last, time_t now,
                  int secs) {
    if (secs <= 0)
        return (true);
    return (now - last >= secs);
}

// qfind locates the QoS slot for fd,
// or nullptr when disarmed.
static TunQosSlot* qfind(TunMap* map, int fd) {
    size_t i;

    if (map == nullptr)
        return (nullptr);
    for (i = 0; i < map->nqos; i++)
        if (map->qos[i].fd == fd)
            return (&map->qos[i]);
    return (nullptr);
}

// dhas reports a set directed edge.
static bool dhas(const TunMap* map, int from,
                 int to) {
    size_t i;

    if (map == nullptr)
        return (false);
    for (i = 0; i < map->ndrop; i++)
        if (map->drop[i].from_fd == from &&
            map->drop[i].to_fd == to)
            return (true);
    return (false);
}

// sadd adds b saturating at max, so
// counters never wrap on overflow.
static void sadd(uint64_t* a, uint64_t b) {
    if (a == nullptr)
        return;
    if (*a > UINT64_MAX - b)
        *a = UINT64_MAX;
    else
        *a += b;
}

// flow_at finds cell (fi, ti) or
// nullptr when out of range.
static TunFlow* flow_at(TunMap* map,
                        size_t fi, size_t ti) {
    if (map == nullptr)
        return (nullptr);
    if (fi >= TUN_MAP_MAX ||
        ti >= TUN_MAP_MAX)
        return (nullptr);
    return (&map->flow[fi][ti]);
}

// flow_fwd bumps pkts by one plus bytes
// by n for one delivered write.
static void flow_fwd(TunMap* map, size_t fi,
                     size_t ti, size_t n) {
    TunFlow* f;

    f = flow_at(map, fi, ti);
    if (f == nullptr)
        return;
    sadd(&f->pkts, 1);
    sadd(&f->bytes, (uint64_t)n);
}

// flow_radio bumps radio_drops by one
// for one gate or write denial.
static void flow_radio(TunMap* map,
                       size_t fi, size_t ti) {
    TunFlow* f;

    f = flow_at(map, fi, ti);
    if (f == nullptr)
        return;
    sadd(&f->radio_drops, 1);
}

// flow_policy bumps policy_drops by one
// for one QoS or fault denial.
static void flow_policy(TunMap* map,
                        size_t fi, size_t ti) {
    TunFlow* f;

    f = flow_at(map, fi, ti);
    if (f == nullptr)
        return;
    sadd(&f->policy_drops, 1);
}

// fwd_counted forwards once then bumps
// fwd or radio. Write shorts fold into
// radio: valid pump args leave only the
// gate or the write as false causes.
// Per-packet cost: one branch plus two
// saturating adds, no syscall or alloc
// beyond the forward itself.
static void fwd_counted(TunMap* map,
                        size_t fi, size_t ti,
                        int to_fd,
                        const uint8_t* buf,
                        size_t n,
                        const MediumGrid* grid,
                        const RadioParams* radio,
                        int64_t ax_nm,
                        int64_t ay_nm,
                        int64_t bx_nm,
                        int64_t by_nm) {
    if (map == nullptr || buf == nullptr)
        return;
    if (tun_forward(to_fd, buf, n, grid,
                    radio, ax_nm, ay_nm,
                    bx_nm, by_nm))
        flow_fwd(map, fi, ti, n);
    else
        flow_radio(map, fi, ti);
}

// idx_of_fd maps fd to slot index, or
// -1 when absent.
static int idx_of_fd(const TunMap* map,
                     int fd) {
    size_t i;

    if (map == nullptr || fd < 0)
        return (-1);
    for (i = 0; i < map->n; i++)
        if (map->slot[i].fd == fd)
            return ((int)i);
    return (-1);
}

// qsend forwards one packet under QoS:
// loss sample, token bucket, one write,
// dup resend. Delay is decided for the
// Context split, never slept here.
// Loss or bucket denials bump policy;
// each write bumps fwd or radio.
static void qsend(TunMap* map, size_t fi,
                  size_t ti, TunQosSlot* qs,
                  int to_fd,
                  const uint8_t* buf, size_t n,
                  const MediumGrid* grid,
                  const RadioParams* radio,
                  int64_t ax_nm, int64_t ay_nm,
                  int64_t bx_nm, int64_t by_nm) {
    static constexpr int NS = 1000000000;
    uint64_t c;
    unsigned long long dly;
    struct timespec ts;
    unsigned long long now;

    if (qs == nullptr || map == nullptr)
        return;
    c = qs->ctr;
    qs->ctr++;
    if (qloss(qs->q.loss01, qs->q.seed, c)) {
        flow_policy(map, fi, ti);
        return;
    }
    if (qs->q.kbps > 0) {
        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (unsigned long long)ts.tv_sec *
              (unsigned long long)NS +
              (unsigned long long)ts.tv_nsec;
        if (!qbucket(qs->q.kbps, &qs->toks,
                     &qs->last_ns, now, n)) {
            flow_policy(map, fi, ti);
            return;
        }
    }
    dly = qdelay(&qs->q, c);
    (void)dly;
    fwd_counted(map, fi, ti, to_fd, buf, n,
                grid, radio, ax_nm, ay_nm,
                bx_nm, by_nm);
    if (qdup(qs->q.dup_pct, qs->q.seed, c))
        fwd_counted(map, fi, ti, to_fd, buf,
                    n, grid, radio, ax_nm,
                    ay_nm, bx_nm, by_nm);
}

// pump reads one packet from from_fd
// and forwards it by dest IP.
static void pump(TunMap* map, int from_fd, MediumGrid* grid,
                 RadioParams* radio, NodeList* nodes,
                 bool drop_mcast) {
    uint8_t buf[TUN_BUF_N];
    ssize_t n;
    TunSlot* from;
    Node* fn;
    TunQosSlot* qs;
    size_t fi;

    if (map == nullptr || grid == nullptr ||
        radio == nullptr || nodes == nullptr)
        return;

    // Slow fds defer before the read, so
    // held packets stay queued in kernel,
    // one drain window per slow_secs.
    qs = qfind(map, from_fd);
    if (qs != nullptr && qs->q.slow_secs > 0) {
        time_t now = time(nullptr);

        if (!qslow(qs->last_slow, now,
                   qs->q.slow_secs))
            return;
        qs->last_slow = now;
    }
    n = read(from_fd, buf, sizeof(buf));
    if (n < IPV4_MIN_N || (buf[0] >> 4) != 4)
        return;

    from = slot_of_fd(map, from_fd);
    if (from == nullptr)
        return;
    fn = nodelist_find(nodes, from->node_id);
    if (fn == nullptr)
        return;
    fi = (size_t)(from - map->slot);

    // Multicast goes blind in nomcast mode: mDNS
    // discovery dies while unicast still flows.
    // Overlay broadcast keeps the old behavior.
    if ((buf[16] & 0xF0) == 0xE0) {
        size_t i;

        if (drop_mcast)
            return;
        for (i = 0; i < map->n; i++) {
            TunSlot* peer = &map->slot[i];
            Node* pn;

            if (peer->fd == from_fd)
                continue;
            pn = nodelist_find(nodes, peer->node_id);
            if (pn == nullptr)
                continue;
            fwd_counted(map, fi, i, peer->fd,
                        buf, (size_t)n, grid,
                        radio, fn->x_nm, fn->y_nm,
                        pn->x_nm, pn->y_nm);
        }
        return;
    }

    // If broadcast address is specified we broadcast the
    // message.
    if (buf[16] == 10 && buf[17] == 0 && buf[18] == 0 &&
        buf[19] == 255) {
        size_t i;

        for (i = 0; i < map->n; i++) {
            TunSlot* peer = &map->slot[i];
            Node* pn;

            if (peer->fd == from_fd)
                continue;
            pn = nodelist_find(nodes, peer->node_id);
            if (pn == nullptr)
                continue;
            fwd_counted(map, fi, i, peer->fd,
                        buf, (size_t)n, grid,
                        radio, fn->x_nm, fn->y_nm,
                        pn->x_nm, pn->y_nm);
        }
        return;
    }

    TunSlot* to;
    Node* tn;
    size_t ti;

    to = slot_of_ip(map, &buf[16], from_fd);
    if (to == nullptr)
        return;
    ti = (size_t)(to - map->slot);

    // Directed drops hit unicast only;
    // fan-out above keeps old shape.
    if (dhas(map, from_fd, to->fd)) {
        flow_policy(map, fi, ti);
        return;
    }
    tn = nodelist_find(nodes, to->node_id);
    if (tn == nullptr)
        return;
    qs = qfind(map, from_fd);
    if (qs != nullptr) {
        qsend(map, fi, ti, qs, to->fd, buf,
              (size_t)n, grid, radio,
              fn->x_nm, fn->y_nm, tn->x_nm,
              tn->y_nm);
        return;
    }
    fwd_counted(map, fi, ti, to->fd, buf,
                (size_t)n, grid, radio,
                fn->x_nm, fn->y_nm, tn->x_nm,
                tn->y_nm);
}

// tun_pump_fd pumps one packet,
// broadcasting multicast.
void tun_pump_fd(TunMap* map, int from_fd, MediumGrid* grid,
                 RadioParams* radio, NodeList* nodes) {
    pump(map, from_fd, grid, radio, nodes, false);
}

// tun_pump_fd_nomcast pumps one packet,
// dropping multicast.
void tun_pump_fd_nomcast(TunMap* map, int from_fd,
                         MediumGrid* grid,
                         RadioParams* radio,
                         NodeList* nodes) {
    pump(map, from_fd, grid, radio, nodes, true);
}

// tun_forward writes buf to to_fd when
// the radio path allows it.
bool tun_forward(int to_fd, const uint8_t* buf, size_t n,
                 const MediumGrid* grid,
                 const RadioParams* radio, int64_t ax_nm,
                 int64_t ay_nm, int64_t bx_nm,
                 int64_t by_nm) {
    RadioPath rp;
    ssize_t w;

    if (to_fd < 0 || buf == nullptr || n == 0)
        return (false);
    if (!radio_path(grid, radio, ax_nm, ay_nm, bx_nm, by_nm,
                    &rp))
        return (false);
    w = write(to_fd, buf, n);
    return (w == (ssize_t)n);
}

// tun_qos_set arms QoS on fd, resetting
// counter, refilling tokens, and zeroing
// drain stamps. Zero q equals legacy.
void tun_qos_set(TunMap *m, int fd, const TunQos *q) {
    TunQosSlot* s;

    if (m == nullptr || q == nullptr || fd < 0)
        return;
    s = qfind(m, fd);
    if (s == nullptr) {
        if (m->nqos >= TUN_MAP_MAX)
            return;
        s = &m->qos[m->nqos];
        m->nqos++;
        s->fd = fd;
    }
    s->q = *q;
    s->ctr = 0;
    s->toks = qcap(q->kbps);
    s->last_ns = 0;
    s->last_slow = 0;
}

// tun_qos_clear disarms fd; the legacy
// path serves it again.
void tun_qos_clear(TunMap *m, int fd) {
    size_t i;

    if (m == nullptr || fd < 0)
        return;
    for (i = 0; i < m->nqos; i++) {
        if (m->qos[i].fd != fd)
            continue;
        m->qos[i] = m->qos[m->nqos - 1];
        m->nqos--;
        return;
    }
}

// tun_drop_set arms (true) or clears
// (false) one directed unicast edge.
void tun_drop_set(TunMap *m, int from_fd,
                  int to_fd, bool drop) {
    size_t i;

    if (m == nullptr || from_fd < 0 ||
        to_fd < 0)
        return;
    for (i = 0; i < m->ndrop; i++) {
        if (m->drop[i].from_fd != from_fd ||
            m->drop[i].to_fd != to_fd)
            continue;
        if (!drop) {
            m->drop[i] = m->drop[m->ndrop - 1];
            m->ndrop--;
        }
        return;
    }
    if (!drop)
        return;
    if (m->ndrop >= TUN_MAP_MAX)
        return;
    m->drop[m->ndrop].from_fd = from_fd;
    m->drop[m->ndrop].to_fd = to_fd;
    m->ndrop++;
}

// tun_drop_clear removes the edge for
// any flag; the flag only mirrors the
// set call shape.
void tun_drop_clear(TunMap *m, int from_fd,
                    int to_fd, bool drop) {
    size_t i;

    (void)drop;
    if (m == nullptr || from_fd < 0 ||
        to_fd < 0)
        return;
    for (i = 0; i < m->ndrop; i++) {
        if (m->drop[i].from_fd != from_fd ||
            m->drop[i].to_fd != to_fd)
            continue;
        m->drop[i] = m->drop[m->ndrop - 1];
        m->ndrop--;
        return;
    }
}

// tun_stats_get copies cell (from_fd,
// to_fd) into out. False on null, bad,
// or unknown fd; true copies zeros
// when the pair never forwarded.
bool tun_stats_get(const TunMap *m,
                   int from_fd, int to_fd,
                   TunFlow *out) {
    int fi;
    int ti;

    if (m == nullptr || out == nullptr)
        return (false);
    if (from_fd < 0 || to_fd < 0)
        return (false);
    fi = idx_of_fd(m, from_fd);
    ti = idx_of_fd(m, to_fd);
    if (fi < 0 || ti < 0)
        return (false);
    *out = m->flow[(size_t)fi][(size_t)ti];
    return (true);
}

// tun_stats_reset zeroes all flow cells;
// slots, QoS, and drops stay intact.
void tun_stats_reset(TunMap *m) {
    if (m == nullptr)
        return;
    memset(m->flow, 0, sizeof(m->flow));
}

// open_tun_file mints name via TUNSETIFF.
// The fd is CLOEXEC at source, so all
// callers (incl. check/) never leak it.
bool open_tun_file(const char* name, int* fd) {
    int fd_internal;
    struct ifreq ifr = {};
    size_t n;

    if (name == nullptr || fd == nullptr)
        return (false);

    fd_internal = open("/dev/net/tun",
                       O_RDWR | O_CLOEXEC);
    if (fd_internal < 0)
        return (false);

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
