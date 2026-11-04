#define _GNU_SOURCE
#include "suite/exec.h"

#include "suite/parse.h"
#include "driver/concord.h"
#include "driver/weld.h"
#include "run/run.h"
#include "sim/sim.h"
#include "tun/tun.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Caps and buffer sizes.
static constexpr int kMaxNodes = 100;
static constexpr int kMaxWl = 128;
static constexpr int kMaxCount = 100;
static constexpr int kMaxRate = 1000000;
static constexpr int kMaxMb = 65536;
static constexpr int kMaxKbps = 1000000;
static constexpr int kMaxMs = 3600000;
static constexpr int kDirLen = 128;
static constexpr int kNameLen = 64;
static constexpr int kWlLen = 64;
static constexpr int kTextLen = 256;
static constexpr int kPathLen = 192;
static constexpr int kCmdLen = 512;
static constexpr int kLogChunk = 4096;
static constexpr int kIdLen = 64;
static constexpr int kShortLen = 16;
static constexpr int kEscLen = 512;

// Cap on dumped flow pairs; past
// it one truncated marker lands.
static constexpr int kAutoPairs = 1024;

// SUT ids: concord or weld.
static constexpr int kConcord = 0;
static constexpr int kWeld = 1;

// Spawn variants: plain, skew, anchor.
static constexpr int kPlain = 0;
static constexpr int kSkew = 1;
static constexpr int kAnchor = 2;

// Restart durability ids in schema
// order: fresh, preserve, rekey
// rotate, key wipe, wipe state.
static constexpr int kDurFresh = 0;
static constexpr int kDurKeep = 1;
static constexpr int kDurRot = 2;
static constexpr int kDurWipe = 3;
static constexpr int kDurZap = 4;

// Throttle rhythm for CLI queries.
static constexpr int kQueryGap = 2;

// auto_sample re-emits the teardown
// flow plus resource set mid-run;
// defined with the dumps it shares.
static void auto_sample(Run *r,
                        size_t ph);
// Sample rhythm for resource plus
// flow series: every 250 ms on the
// monotonic clock, decoupled from
// queries so short phases still
// gain shape. Units are ns.
static constexpr unsigned long long
    kSampleGapNs = 250000000ULL;
// Benchmark caps: weld pubs are
// cheap log lines, concord issues
// mint table workloads. Grace is
// the post-issue observe dwell in
// seconds, pumped, loss past it.
static constexpr int kBenchWeld = 1000;
static constexpr int kBenchConc = 100;
static constexpr int kBenchGrace = 10;
// Bench payload buffer: tag plus
// body plus NUL. Tag is bench#seq#
// send_ns# (below 64), body is N x
// bytes. 66000 holds 65536 plus tag
// with room. Stack holds two such
// buffers in the issue plus grace
// loops, below the 8 MB default.
static constexpr int kBenchPay = 66000;
// Payload cap: deterministic fill
// of N x bytes, 0 means use text.
// 65536 covers 64 KB point clouds.
// 1 MB images need chunking plus a
// larger argv and DDS check, see the
// audit in exec.h. Weld pub argv
// fits (ARG_MAX 2 MB, per-string
// 128 KB); ROS yaml caps near 512
// (driver ROS_YAML_LEN), so large
// ROS bodies fail the run loudly,
// never silently clip. TUN fragments
// at IP level (MTU 1500, pump reads
// 2048), so end to end still counts.
static constexpr int kBenchBytes = 65536;
// File needle buffer: file_has needs
// nlen plus 1 below half the buffer.
// 65536 plus tag needs 131202, so
// 132000 holds it. Stack only, used
// in file_has alone; other log uses
// keep kLogChunk.
static constexpr int kFileBuf = 132000;
// Per-message lat cap: at most
// 2000 lat points emit per step,
// then a truncated marker lands,
// mirroring flow truncation.
static constexpr int kLatCap = 2000;
// Histogram buckets: 10 fixed
// edges match the report view.
// Edges are upper bounds in ms,
// last bucket open. Cap is 20,
// we use 10.
static constexpr int kHistN = 10;
// Edges for latency buckets in
// ms: 1 2 5 10 20 50 100 200
// 500, buckets 0-1 1-2 2-5 5-10
// 10-20 20-50 50-100 100-200
// 200-500 500 plus, as in the
// report view.
static const unsigned long long
    kHistEdge[9] = {1ULL, 2ULL, 5ULL,
                    10ULL, 20ULL,
                    50ULL, 100ULL,
                    200ULL, 500ULL};
// Us edges resolve sub-ms ticks
// that read 0 in ms: 100 500us,
// 1 5 10 20 50 100 500ms in us,
// 10 buckets 0-100us up to
// 500ms plus. Ms leaves stay for
// compat; us leaves add spread.
static const unsigned long long
    kHistUsEdge[9] = {100ULL, 500ULL,
                      1000ULL, 5000ULL,
                      10000ULL,
                      20000ULL,
                      50000ULL,
                      100000ULL,
                      500000ULL};
// Echo sides: weld relay or ros
// observer, from echo.by.
static constexpr int kEchoWeld = 0;
static constexpr int kEchoRos = 1;
// Endpoint kinds for pairing: weld
// node, ROS peer, concord node.
// Table driven; adding a kind is a
// row, not new branches.
typedef enum {
    EP_WELD = 0,
    EP_ROS = 1,
    EP_CONCORD = 2,
} EpKind;
// EpCap: per-kind verbs. pub_weld via
// weld_pub, pub_ros via ros_pub_once;
// concord has no pub verb. obs_weld
// via weld log, obs_ros via ros log;
// concord lists, not logs. relay via
// weld_relay_start only. submit via
// concord_submit, list via
// concord_list_has. False means no
// driver verb exists (see
// driver/weld.h, driver/concord.h).
typedef struct {
    bool pub_weld;
    bool pub_ros;
    bool obs_weld;
    bool obs_ros;
    bool relay;
    bool submit;
    bool list;
} EpCap;
// Capability table: weld pubs,
// observes both logs, relays,
// never submits; ros pubs via ROS,
// observes ROS only, never relays;
// concord never pubs/relays/logs,
// only submits plus lists.
static const EpCap kEpCap[3] = {
    {true, false, true, true, true,
     false, false},
    {false, true, false, true, false,
     false, false},
    {false, false, false, false,
     false, true, true},
};
// Probe names one registry entry:
// a send or observe verb wired to
// the existing driver path. Relay
// stays out; it is transport, not
// observation.
typedef struct {
    const char *via;
    bool send;
    const char *need;
} Probe;
// Registry table: three sends plus
// five observes. Needs reuse old
// names where they exist; weld.log,
// ros.log, journal.has are new.
// Old needs keep meaning.
static const Probe kProbeTab[8] = {
    {"weld-pub", true, "weld.pub"},
    {"ros-pub", true, "ros.pub"},
    {"workload-submit", true,
     "workload.submit"},
    {"log-contains", false,
     "weld.log"},
    {"ros-log-contains", false,
     "ros.log"},
    {"journal-has", false,
     "journal.has"},
    {"workload-running", false,
     "workload.list"},
    {"prometheus-scrape", false,
     "metrics.scrape"},
};
// probe_of finds via in the table.
// Nullptr when unknown.
static const Probe *probe_of(
    const char *via) {
    size_t i;

    if (via == nullptr) {
        return (nullptr);
    }
    for (i = 0; i < 8; i++) {
        if (strcmp(kProbeTab[i].via,
                   via) == 0) {
            return (&kProbeTab[i]);
        }
    }
    return (nullptr);
}
// Timeout clamp: lint holds phase
// and file timeouts to 1..3600 s
// (hold_s 0..3600) so the (int)
// casts below cannot truncate.
static constexpr int kMaxTimeoutS = 3600;

// topic_seq keeps auto weld topics
// unique in the process, beside the
// pid plus wall time derivation.
static unsigned topic_seq = 0;

// StepKind names one do verb.
typedef enum {
    ST_DROP,
    ST_HEAL,
    ST_KILL,
    ST_RESTART,
    ST_MOVE,
    ST_SUBMIT,
    ST_STOP,
    ST_PUB,
    ST_SLOW,
    ST_UPDATE,
    ST_REPLAY,
    ST_DISKFILL,
    ST_DISKRO,
    ST_JCORRUPT,
    ST_BENCH,
    ST_ROSPUB,
} StepKind;

// DropDir picks the fd set a net
// fault shapes: listed egress,
// other fds ingress, or all fds.
typedef enum {
    DIR_OUT = 0,
    DIR_IN,
    DIR_BOTH,
} DropDir;

// ExStep is one validated do step.
// as_name picks the workload name
// (submitted when has_as is false).
// has_fault marks the qos drop form
// (a fault key present); plain drops
// (dir: alone counts as plain) use
// the legacy marks plus directed
// edges.
typedef struct {
    StepKind kind;
    size_t line;
    int idx[kMaxNodes];
    size_t nidx;
    bool all;
    int node;
    int sig;
    int dur;
    double xm;
    char image[128];
    char flags[128];
    bool has_flags;
    char text[kTextLen];
    char topic[128];
    bool has_topic;
    char as_name[kWlLen];
    bool has_as;
    long long count;
    bool has_count;
    char fixed_id[kIdLen];
    bool has_id;
    long long rate;
    bool has_rate;
    bool has_fault;
    double loss;
    unsigned long long delay_ns;
    unsigned long long jitter_ns;
    unsigned long long kbps;
    int dup_pct;
    int dir;
    long long slow_secs;
    char wl[kWlLen];
    char file[kPathLen];
    unsigned long long mb;
    bool ro;
    // Benchmark extras: duration_s
    // bounds the paced loop in
    // seconds, warmup counts leading
    // issues left out of stats, obs
    // is the weld observed node.
    long long duration_s;
    bool has_dur;
    long long warmup;
    int obs;
    // Weld bench plus ros_pub add:
    // from_kind is EP_WELD for bare
    // N or weld:N, EP_ROS for ros:N,
    // EP_CONCORD for concord:N (fails,
    // no pub verb). on_kind is EP_WELD
    // for bare N or weld:N (dual logs,
    // legacy), EP_ROS for ros:N (ROS
    // log only), EP_CONCORD fails (no
    // log verb). has_echo marks echo:
    // present; echo_by is weld or ros
    // (concord fails, no log verb),
    // echo_node is the relay node with
    // echo_relay_kind (weld relays,
    // ros/concord fail, no relay).
    // Reply is always <topic>/pong.
    int from_kind;
    int on_kind;
    unsigned long long pay_bytes;
    bool has_pay;
    bool has_echo;
    int echo_by;
    int echo_node;
    int echo_relay_kind;
    // Ramp extras: has_ramp marks a
    // ramp: block (adaptive ceiling).
    // Bounds prove termination, see
    // run_ramp_weld below.
    bool has_ramp;
    long long ramp_start;
    long long ramp_step;
    long long ramp_step_dur;
    long long ramp_max_rate;
    long long ramp_max_steps;
    long long ramp_max_dur;
    long long ramp_loss_pct;
    bool ramp_has_loss;
    long long ramp_p99;
    bool ramp_has_p99;
    // Generic extras: has_generic
    // marks send plus expect blocks
    // naming registry entries. send_tpl
    // is the payload template, empty
    // means the canonical bench tag.
    // exp_match holds the prefix or
    // the running state; exp_field
    // plus exp_value hold the journal
    // map. Scrape extras hold a
    // generic Prometheus check: metric
    // base name, op, threshold, port
    // plus path. Old steps leave
    // them empty.
    bool has_generic;
    char send_via[32];
    char expect_via[32];
    char send_tpl[512];
    char exp_match[256];
    char exp_field[64];
    char exp_value[256];
    char exp_metric[128];
    char exp_op[8];
    double exp_thresh;
    long long exp_port;
    char exp_path[128];
} ExStep;

// CondKind names one check.
typedef enum {
    CD_PEERS,
    CD_MARKER,
    CD_PRESENT,
    CD_ABSENT,
    CD_OBSERVED,
    CD_ASSIGNED,
    CD_MESH,
    CD_AGREE,
    CD_ASSIGNED_ON,
    CD_EPOCH,
    CD_IMAGE,
} CondKind;

// EpochCmp picks the epoch literal
// test: none, equal, or at least.
typedef enum {
    CMP_NONE = 0,
    CMP_EQ,
    CMP_GTE,
} EpochCmp;

// ExCond is one validated until or
// sustain condition. on holds the
// expanded target nodes. workload
// picks the concord list form of
// present and absent; text picks
// the weld content form. wl names
// the workload ref, default
// submitted. node picks the journal
// node for assigned_on and epoch.
// obs is the observed node, obs_kind
// is EP_WELD for bare/weld:N (dual
// logs, legacy) or EP_ROS for ros:N
// (ROS log only); concord fails (no
// log verb).
typedef struct {
    CondKind kind;
    size_t line;
    long long count;
    char marker[kTextLen];
    int on[kMaxNodes];
    size_t non;
    bool any;
    bool workload;
    char text[kTextLen];
    int obs;
    int obs_kind;
    char wl[kWlLen];
    int node;
    int cmp;
    unsigned long long epoch;
    char imgval[128];
} ExCond;

// ExPhase is one validated phase.
// nomcast pumps it without IPv4
// multicast (anchor shape).
typedef struct {
    char name[kNameLen];
    size_t line;
    ExStep *steps;
    size_t nsteps;
    bool has_until;
    ExCond cond;
    long long hold_s;
    long long timeout_s;
    bool nomcast;
} ExPhase;

// ExFile is one validated suite file.
typedef struct {
    char name[kNameLen];
    int sut;
    long long nodes;
    long long domain;
    bool has_domain;
    char topic[128];
    bool has_topic;
    bool has_spawn;
    int variant;
    long long snode;
    bool has_snode;
    char offset[64];
    bool has_offset;
    char (*needs)[kNameLen];
    size_t nneeds;
    long long timeout_s;
    long long *rosnode;
    char (*rosdistro)[16];
    size_t nros;
    char bins[kMaxNodes][kPathLen];
    bool bin_set[kMaxNodes];
    bool has_bins;
    // disk_cap_mb caps boot state in
    // MB, 0 uncapped, concord-only.
    unsigned long long disk_cap_mb;
    ExPhase *phases;
    size_t nphases;
} ExFile;

// VCtx carries validation output.
typedef struct {
    const char *path;
    char *why;
    size_t why_n;
} VCtx;

// vbad formats path:line: msg.
static bool vbad(VCtx *v, size_t line,
                 const char *msg) {
    const char *path;

    if (v == nullptr || v->why ==
        nullptr || v->why_n == 0) {
        return (false);
    }
    path = v->path != nullptr
               ? v->path
               : "(null)";
    (void)snprintf(v->why, v->why_n,
                   "%s:%zu: %s", path,
                   line, msg);
    return (false);
}

// ellipsize marks a filled buffer
// with ... when snprintf ran out.
static void ellipsize(char *dst,
                      size_t n, int m) {
    if (dst == nullptr || n < 4) {
        return;
    }
    if (m < (int)n) {
        return;
    }
    memcpy(dst + n - 4, "...", 4);
}

// vunknown reports unknown key.
// Long text truncates to fit why.
static bool vunknown(VCtx *v,
                     size_t line,
                     const char *what,
                     const char *key) {
    char msg[320];
    int m;

    m = snprintf(msg, sizeof(msg),
                 "unknown %s '%s'",
                 what, key);
    ellipsize(msg, sizeof(msg), m);
    return (vbad(v, line, msg));
}

// key_line finds the source line of a
// key in a map, or the map line.
static size_t key_line(SuiteNode *map,
                       const char *key) {
    size_t i;

    if (map == nullptr ||
        map->kind != SUITE_MAP) {
        return (0);
    }
    for (i = 0; i <
         map->u.map.len; i++) {
        if (strcmp(map->u.map.items[i]
                       .key,
                   key) == 0) {
            return (map->u.map.items[i]
                        .key_line);
        }
    }
    return (map->line);
}

// scalar_eq reports scalar text equal.
static bool scalar_eq(SuiteNode *nd,
                      const char *s) {
    const char *t;

    t = suite_scalar_str(nd);
    if (t == nullptr || s == nullptr) {
        return (false);
    }
    return (strcmp(t, s) == 0);
}

// copy_text copies with a cap.
static bool copy_text(char *dst,
                      size_t n,
                      const char *src) {
    if (dst == nullptr ||
        src == nullptr || n == 0) {
        return (false);
    }
    if (strlen(src) + 1 > n) {
        return (false);
    }
    strcpy(dst, src);
    return (true);
}

// need_int reads int-shaped text:
// optional sign plus digits only,
// quoted or not both count.
static bool need_int(SuiteNode *nd,
                     long long *out) {
    if (!suite_scalar_int(nd, out)) {
        return (false);
    }
    return (true);
}

// need_ok reports a needs verb known
// for the SUT. Defined below; val_top
// needs the forward declaration.
static bool need_ok(int sut,
                    const char *s);

// has_ramp reports a ramp* key.
// Defined past topic_ok; val_extra
// needs the forward declaration.
static bool has_ramp(const char *k);

// ramp_bad fails with the ladder
// pointer for any ramp* key.
static bool ramp_bad(VCtx *v,
                     size_t line);

// val_extra rejects unknown map keys.
// Ramp keys fail with the ladder
// pointer, never a bare unknown.
// Exact ramp: in keep passes (valid
// ramp block); elsewhere still fails
// with the ladder pointer.
static bool val_extra(SuiteNode *map,
                      const char *const *keep,
                      size_t nkeep,
                      const char *what,
                      VCtx *v) {
    size_t i;
    size_t k;

    for (i = 0; i <
         map->u.map.len; i++) {
        bool ok = false;

        for (k = 0; k < nkeep; k++) {
            if (strcmp(map->u.map.items[i]
                           .key,
                       keep[k]) == 0) {
                ok = true;
                break;
            }
        }
        if (ok) {
            continue;
        }
        if (has_ramp(
                map->u.map.items[i]
                    .key)) {
            return (ramp_bad(
                v,
                map->u.map.items[i]
                    .key_line));
        }
        if (!ok) {
            return (vunknown(
                v,
                map->u.map.items[i]
                    .key_line,
                what,
                map->u.map.items[i]
                    .key));
        }
    }
    return (true);
}

// val_nodeset parses all or int list.
static bool val_nodeset(
    SuiteNode *nd, const ExFile *f,
    VCtx *v, int *out, size_t *nout,
    bool *all) {
    size_t i;

    *nout = 0;
    *all = false;
    if (nd->kind == SUITE_SCALAR) {
        if (!scalar_eq(nd, "all")) {
            return (vbad(
                v, nd->line,
                "want 'all' or a list"));
        }
        *all = true;
        return (true);
    }
    if (nd->kind != SUITE_LIST) {
        return (vbad(
            v, nd->line,
            "want 'all' or a list"));
    }
    if (suite_list_len(nd) == 0) {
        return (vbad(v, nd->line,
                     "empty index list"));
    }
    for (i = 0; i <
         suite_list_len(nd); i++) {
        SuiteNode *it;
        long long x;

        it = suite_list_at(nd, i);
        // Int-shaped counts, quoted ok.
        if (!need_int(it, &x) || x < 0 ||
            x >= f->nodes) {
            return (vbad(
                v, it->line,
                "node index out of range"));
        }
        if (*nout >=
            (size_t)kMaxNodes) {
            return (vbad(
                v, it->line,
                "too many indices"));
        }
        out[*nout] = (int)x;
        (*nout)++;
    }
    return (true);
}

// is_listed reports node k in the
// drop set of step s.
static bool is_listed(const ExStep *s,
                      int k) {
    size_t j;

    if (s == nullptr) {
        return (false);
    }
    if (s->all) {
        return (true);
    }
    for (j = 0; j < s->nidx; j++) {
        if (s->idx[j] == k) {
            return (true);
        }
    }
    return (false);
}

// expand_on fills cond targets.
static bool expand_on(ExCond *c,
                      const int *idx,
                      size_t n, bool all,
                      long long nodes) {
    long long i;

    c->non = 0;
    if (all) {
        for (i = 0; i < nodes; i++) {
            c->on[c->non] = (int)i;
            c->non++;
        }
        return (true);
    }
    for (i = 0; (size_t)i < n; i++) {
        c->on[c->non] = idx[i];
        c->non++;
    }
    return (true);
}

// dur_of maps a durability name.
static bool dur_of(const char *s,
                   int *out) {
    if (strcmp(s, "fresh") == 0) {
        *out = kDurFresh;
    } else if (strcmp(s, "preserve") ==
               0) {
        *out = kDurKeep;
    } else if (strcmp(s, "rekey_rotate") ==
               0) {
        *out = kDurRot;
    } else if (strcmp(s, "key_wipe") ==
               0) {
        *out = kDurWipe;
    } else if (strcmp(s, "wipe_state") ==
               0) {
        *out = kDurZap;
    } else {
        return (false);
    }
    return (true);
}

// image_ok checks a workload image ref
// for shell-safe docker characters.
static bool image_ok(const char *s) {
    size_t i;

    if (s == nullptr || s[0] == '\0') {
        return (false);
    }
    for (i = 0; s[i] != '\0'; i++) {
        char ch = s[i];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '.' ||
            ch == ':' || ch == '/' ||
            ch == '@' || ch == '+' ||
            ch == '-') {
            continue;
        }
        return (false);
    }
    return (true);
}

// flags_ok checks workload flags for
// the popen-safe charset the driver
// wants: alnum plus _ = . , space
// slash dash plus percent for the
// %host memory grammar, resolved in
// the driver before the shell.
// Mirrors the driver.
static bool flags_ok(const char *s) {
    size_t i;

    if (s == nullptr) {
        return (false);
    }
    for (i = 0; s[i] != '\0'; i++) {
        char ch = s[i];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '=' ||
            ch == '.' || ch == ',' ||
            ch == ' ' || ch == '/' ||
            ch == '-' || ch == '%') {
            continue;
        }
        return (false);
    }
    return (true);
}

// wl_name_ok checks a workload name:
// alnum plus _ . -, non-empty.
static bool wl_name_ok(const char *s) {
    size_t i;

    if (s == nullptr || s[0] == '\0') {
        return (false);
    }
    if (strlen(s) >= (size_t)kWlLen) {
        return (false);
    }
    for (i = 0; s[i] != '\0'; i++) {
        char ch = s[i];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '.' ||
            ch == '-') {
            continue;
        }
        return (false);
    }
    return (true);
}

// key_int parses a mapping key as a
// full int: optional sign plus
// digits, nothing else.
static bool key_int(const char *s,
                    long long *out) {
    size_t i = 0;

    if (s == nullptr || out == nullptr) {
        return (false);
    }
    if (s[0] == '+' || s[0] == '-') {
        i = 1;
    }
    if (s[i] == '\0') {
        return (false);
    }
    for (; s[i] != '\0'; i++) {
        if (s[i] < '0' ||
            s[i] > '9') {
            return (false);
        }
    }
    errno = 0;
    *out = strtoll(s, nullptr, 10);
    if (errno == ERANGE) {
        return (false);
    }
    return (true);
}

// wid_ok checks a workload id for
// grep safety: hex plus dash only.
static bool wid_ok(const char *s) {
    size_t i;

    if (s == nullptr || s[0] == '\0') {
        return (false);
    }
    if (strlen(s) >= (size_t)kIdLen) {
        return (false);
    }
    for (i = 0; s[i] != '\0'; i++) {
        char ch = s[i];

        if ((ch >= '0' && ch <= '9') ||
            (ch >= 'a' && ch <= 'f') ||
            (ch >= 'A' && ch <= 'F') ||
            ch == '-') {
            continue;
        }
        return (false);
    }
    return (true);
}

// statedir_ok checks a statedir for
// shell safety, mirroring the sim
// net check: alnum plus / _ . -.
static bool statedir_ok(const char *s) {
    size_t i;

    if (s == nullptr || s[0] == '\0') {
        return (false);
    }
    for (i = 0; s[i] != '\0'; i++) {
        char ch = s[i];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '/' || ch == '_' ||
            ch == '.' || ch == '-') {
            continue;
        }
        return (false);
    }
    return (true);
}

// g_ev_start_ms anchors event times:
// ms since file start, monotonic.
static unsigned long long
    g_ev_start_ms = 0;

// ev_start anchors the clock. Call
// once per run file, before setup.
static void ev_start(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC,
                      &ts) != 0) {
        g_ev_start_ms = 0;
        return;
    }
    g_ev_start_ms =
        (unsigned long long)ts.tv_sec *
            1000ULL +
        (unsigned long long)ts.tv_nsec /
            1000000ULL;
}

// ev_now_ms reports ms since the
// anchor. Zero before ev_start.
static unsigned long long
ev_now_ms(void) {
    struct timespec ts;
    unsigned long long now;

    if (clock_gettime(CLOCK_MONOTONIC,
                      &ts) != 0) {
        return (0);
    }
    now = (unsigned long long)ts
                  .tv_sec *
              1000ULL +
          (unsigned long long)ts
                  .tv_nsec /
              1000000ULL;
    if (now < g_ev_start_ms) {
        return (0);
    }
    return (now - g_ev_start_ms);
}

// json_escape quotes msg for the ev
// line: backslash, quote, and
// control bytes take escapes; long
// text truncates with the buffer
// NUL kept. Never emits raw bytes
// that could break the shape.
static void json_escape(const char *src,
                        char *dst,
                        size_t n) {
    size_t o = 0;
    size_t i;

    if (dst == nullptr || n == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    for (i = 0; src[i] != '\0'; i++) {
        unsigned char ch;

        ch = (unsigned char)src[i];
        if (ch == '"' || ch == '\\') {
            if (o + 2 >= n) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = (char)ch;
            continue;
        }
        if (ch == '\n') {
            if (o + 2 >= n) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = 'n';
            continue;
        }
        if (ch == '\r' || ch == '\t') {
            if (o + 2 >= n) {
                break;
            }
            dst[o++] = '\\';
            dst[o++] = ch == '\r'
                           ? 'r'
                           : 't';
            continue;
        }
        if (ch < 0x20) {
            if (o + 6 >= n) {
                break;
            }
            o += (size_t)snprintf(
                dst + o, n - o,
                "\\u%04x", ch);
            continue;
        }
        if (o + 1 >= n) {
            break;
        }
        dst[o++] = (char)ch;
    }
    dst[o] = '\0';
}

// emit_ev prints one timeline event.
// The only ev print site: k comes
// from a fixed set, msg is escaped
// here, so malformed lines cannot
// escape this function.
static void emit_ev(const char *k,
                    size_t ph,
                    const char *msg,
                    long long d1,
                    long long d2) {
    char esc[kEscLen];

    json_escape(msg, esc,
                sizeof(esc));
    printf("suite: ev {\"t\":%llu,"
           "\"k\":\"%s\",\"ph\":%zu,"
           "\"msg\":\"%s\",\"d1\":"
           "%lld,\"d2\":%lld}\n",
           ev_now_ms(), k, ph, esc, d1,
           d2);
    fflush(stdout);
}

// topic_ok checks a weld topic shape:
// leading slash plus the pkill-safe
// fixed charset of slash, alnum,
// and underscore.
static bool topic_ok(const char *s) {
    size_t i;

    if (s == nullptr || s[0] != '/') {
        return (false);
    }
    for (i = 1; s[i] != '\0'; i++) {
        char ch = s[i];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '/') {
            continue;
        }
        return (false);
    }
    return (true);
}

// has_ramp reports a key starting
// with ramp: adaptive ramps are
// banned, use multi-phase rate
// ladders, see bench_ramp.yaml.
static bool has_ramp(const char *k) {
    if (k == nullptr) {
        return (false);
    }
    if (strncmp(k, "ramp", 4) != 0) {
        return (false);
    }
    return (true);
}

// ramp_bad fails with the ladder
// pointer for any ramp* key.
static bool ramp_bad(VCtx *v,
                     size_t line) {
    return (vbad(v, line,
                 "ramp* unsupported: "
                 "use multi-phase "
                 "rate ladder, see "
                 "bench_ramp.yaml"));
}

// old_tag reports text looking
// like an old two-field bench
// tag bench#N# without send_ns.
// New grammar is bench#seq#
// send_ns#text; old tags never
// parse, so lint rejects them
// loudly to avoid silent loss.
static bool old_tag(const char *s) {
    size_t i = 0;
    size_t j;

    if (s == nullptr) {
        return (false);
    }
    if (strncmp(s, "bench#", 6) !=
        0) {
        return (false);
    }
    i = 6;
    if (s[i] < '0' || s[i] > '9') {
        return (false);
    }
    for (j = i; s[j] >= '0' &&
         s[j] <= '9'; j++) {
    }
    if (s[j] != '#') {
        return (false);
    }
    j++;
    // New form has digits plus #
    // here; old form has anything
    // else (space, text, end).
    if (s[j] < '0' || s[j] > '9') {
        return (true);
    }
    for (; s[j] >= '0' &&
         s[j] <= '9'; j++) {
    }
    if (s[j] != '#') {
        return (true);
    }
    return (false);
}

// via_bad fails naming the known
// primitives for an unknown via.
static bool via_bad(VCtx *v,
                    size_t line,
                    const char *via) {
    char msg[320];
    int m;

    m = snprintf(msg, sizeof(msg),
                 "unknown via '%s': want "
                 "weld-pub, ros-pub, "
                 "workload-submit, "
                 "log-contains, "
                 "ros-log-contains, "
                 "journal-has, "
                 "workload-running, "
                 "prometheus-scrape",
                 via != nullptr ? via
                                : "");
    ellipsize(msg, sizeof(msg), m);
    return (vbad(v, line, msg));
}

// ph_ok scans tpl for {{name}} and
// checks each name. Unknown names
// fail listing the allowed list.
// Wid needs allow_wid (submit send
// only), else it fails loudly.
static bool ph_ok(const char *tpl,
                  VCtx *v, size_t line,
                  bool allow_wid) {
    size_t i = 0;

    if (tpl == nullptr ||
        tpl[0] == '\0') {
        return (true);
    }
    for (i = 0; tpl[i] != '\0';
         i++) {
        size_t j;
        char nm[32];
        size_t k = 0;

        if (tpl[i] != '{' ||
            tpl[i + 1] != '{') {
            continue;
        }
        j = i + 2;
        for (; tpl[j] != '\0' &&
             k + 1 < sizeof(nm); j++) {
            if (tpl[j] == '}' &&
                tpl[j + 1] == '}') {
                break;
            }
            nm[k++] = tpl[j];
        }
        nm[k] = '\0';
        if (tpl[j] == '\0' ||
            tpl[j + 1] != '}') {
            return (vbad(
                v, line,
                "bad placeholder: want "
                "{{seq}}, {{send_ns}}, "
                "{{body}}, {{wid}}"));
        }
        i = j + 1;
        if (strcmp(nm, "seq") == 0 ||
            strcmp(nm, "send_ns") ==
                0 ||
            strcmp(nm, "body") ==
                0) {
            continue;
        }
        if (strcmp(nm, "wid") == 0) {
            if (allow_wid) {
                continue;
            }
            return (vbad(
                v, line,
                "wid is "
                "workload-submit "
                "only"));
        }
        {
            char msg[160];
            int m;

            m = snprintf(
                msg, sizeof(msg),
                "unknown placeholder "
                "'%s': want seq, "
                "send_ns, body, wid",
                nm);
            ellipsize(msg,
                      sizeof(msg), m);
            return (vbad(v, line,
                         msg));
        }
    }
    return (true);
}

// ros_from parses from: ros:N into
// node N. True on ros shape,
// false when not ros (caller
// tries int). Bad N fails loudly.
static bool ros_from(const char *s,
                     const ExFile *f,
                     int *node) {
    long long x = 0;
    size_t i = 4;

    if (s == nullptr || f == nullptr ||
        node == nullptr) {
        return (false);
    }
    if (strncmp(s, "ros:", 4) != 0) {
        return (false);
    }
    if (s[4] == '\0') {
        return (false);
    }
    for (; s[i] != '\0'; i++) {
        if (s[i] < '0' ||
            s[i] > '9') {
            return (false);
        }
    }
    errno = 0;
    x = strtoll(s + 4, nullptr, 10);
    if (errno == ERANGE) {
        return (false);
    }
    if (x < 0 || x >= f->nodes) {
        return (false);
    }
    *node = (int)x;
    return (true);
}

// ep_name names a kind for loud
// capability errors.
static const char *ep_name(int k) {
    static const char *tab[3] = {
        "weld", "ros", "concord",
    };

    if (k < 0 || k > 2) {
        return ("unknown");
    }
    return (tab[k]);
}

// scrape_metric_ok reports a valid
// Prometheus base name: start
// letter, underscore, or colon,
// then alnum, underscore, or colon.
// Concord plus weld both emit this
// shape, so one check covers both.
static bool scrape_metric_ok(
    const char *s) {
    size_t i = 0;

    if (s == nullptr ||
        s[0] == '\0') {
        return (false);
    }
    if (!((s[0] >= 'A' &&
           s[0] <= 'Z') ||
          (s[0] >= 'a' &&
           s[0] <= 'z') ||
          s[0] == '_' ||
          s[0] == ':')) {
        return (false);
    }
    for (i = 1; s[i] != '\0'; i++) {
        if ((s[i] >= 'A' &&
             s[i] <= 'Z') ||
            (s[i] >= 'a' &&
             s[i] <= 'z') ||
            (s[i] >= '0' &&
             s[i] <= '9') ||
            s[i] == '_' ||
            s[i] == ':') {
            continue;
        }
        return (false);
    }
    return (i < 128);
}

// scrape_op_ok reports a known
// comparison operator.
static bool scrape_op_ok(
    const char *s) {
    if (s == nullptr) {
        return (false);
    }
    return (strcmp(s, "eq") == 0 ||
            strcmp(s, "ne") == 0 ||
            strcmp(s, "lt") == 0 ||
            strcmp(s, "le") == 0 ||
            strcmp(s, "gt") == 0 ||
            strcmp(s, "ge") == 0);
}

// scrape_path_ok reports a safe
// HTTP path: starts with slash,
// then alnum, slash, dot, dash,
// or underscore. Rejects shell
// metachars so the scrape command
// never escapes.
static bool scrape_path_ok(
    const char *s) {
    size_t i = 0;

    if (s == nullptr ||
        s[0] != '/') {
        return (false);
    }
    for (i = 1; s[i] != '\0'; i++) {
        if ((s[i] >= 'A' &&
             s[i] <= 'Z') ||
            (s[i] >= 'a' &&
             s[i] <= 'z') ||
            (s[i] >= '0' &&
             s[i] <= '9') ||
            s[i] == '/' ||
            s[i] == '.' ||
            s[i] == '-' ||
            s[i] == '_') {
            continue;
        }
        return (false);
    }
    return (i < 128);
}

// ep_prefix parses prefix:N text
// (weld:/ros:/concord:) into kind
// plus node. True on shape, false
// when no prefix. Range checked.
static bool ep_prefix(const char *s,
                      const ExFile *f,
                      int *kind,
                      int *node) {
    const char *p = nullptr;
    long long x = 0;
    size_t n = 0;

    if (s == nullptr || f == nullptr ||
        kind == nullptr ||
        node == nullptr) {
        return (false);
    }
    if (strncmp(s, "weld:", 5) ==
        0) {
        p = s + 5;
        n = 5;
        *kind = EP_WELD;
    } else if (strncmp(s, "ros:", 4) ==
               0) {
        p = s + 4;
        n = 4;
        *kind = EP_ROS;
    } else if (strncmp(
                   s, "concord:", 8) ==
               0) {
        p = s + 8;
        n = 8;
        *kind = EP_CONCORD;
    } else {
        return (false);
    }
    if (p[0] == '\0') {
        return (false);
    }
    for (size_t i = n; s[i] != '\0';
         i++) {
        if (s[i] < '0' ||
            s[i] > '9') {
            return (false);
        }
    }
    errno = 0;
    x = strtoll(p, nullptr, 10);
    if (errno == ERANGE) {
        return (false);
    }
    if (x < 0 || x >= f->nodes) {
        return (false);
    }
    *node = (int)x;
    return (true);
}

// sut_def_ep maps a SUT id to its
// default endpoint kind via a table,
// not a branch: concord 0 maps to
// concord, weld 1 maps to weld.
static int sut_def_ep(int sut) {
    static const int tab[2] = {
        EP_CONCORD, EP_WELD,
    };

    if (sut < 0 || sut > 1) {
        return (EP_CONCORD);
    }
    return (tab[sut]);
}

// ep_parse_node parses a scalar node
// ref: weld:N, ros:N, concord:N, or
// bare N (default kind for the file
// SUT). Sets kind plus node. False
// when neither shape holds.
static bool ep_parse_node(
    SuiteNode *nd, const ExFile *f,
    int *kind, int *node) {
    const char *s = nullptr;
    long long x = 0;

    if (nd == nullptr || f == nullptr ||
        kind == nullptr ||
        node == nullptr) {
        return (false);
    }
    s = suite_scalar_str(nd);
    if (s != nullptr) {
        int k = 0;
        int n = 0;

        if (ep_prefix(s, f, &k, &n)) {
            *kind = k;
            *node = n;
            return (true);
        }
        if (ros_from(s, f, &n)) {
            *kind = EP_ROS;
            *node = n;
            return (true);
        }
    }
    if (!need_int(nd, &x) || x < 0 ||
        x >= f->nodes) {
        return (false);
    }
    *kind = sut_def_ep(f->sut);
    *node = (int)x;
    return (true);
}

// needs_has reports need present
// in the file needs list.
static bool needs_has(const ExFile *f,
                      const char *need) {
    size_t i;

    if (f == nullptr ||
        need == nullptr) {
        return (false);
    }
    for (i = 0; i < f->nneeds; i++) {
        if (strcmp(f->needs[i],
                   need) == 0) {
            return (true);
        }
    }
    return (false);
}

// hist_bucket maps ms to a bucket
// 0..9 under kHistEdge: 0-1 1-2
// 2-5 5-10 10-20 20-50 50-100
// 100-200 200-500 500 plus.
static int hist_bucket(
    unsigned long long ms) {
    if (ms <= kHistEdge[0]) {
        return (0);
    }
    if (ms <= kHistEdge[1]) {
        return (1);
    }
    if (ms <= kHistEdge[2]) {
        return (2);
    }
    if (ms <= kHistEdge[3]) {
        return (3);
    }
    if (ms <= kHistEdge[4]) {
        return (4);
    }
    if (ms <= kHistEdge[5]) {
        return (5);
    }
    if (ms <= kHistEdge[6]) {
        return (6);
    }
    if (ms <= kHistEdge[7]) {
        return (7);
    }
    if (ms <= kHistEdge[8]) {
        return (8);
    }
    return (9);
}

// hist_bucket_us maps us to a bucket
// 0..9 under kHistUsEdge.
static int hist_bucket_us(
    unsigned long long us) {
    if (us <= kHistUsEdge[0]) {
        return (0);
    }
    if (us <= kHistUsEdge[1]) {
        return (1);
    }
    if (us <= kHistUsEdge[2]) {
        return (2);
    }
    if (us <= kHistUsEdge[3]) {
        return (3);
    }
    if (us <= kHistUsEdge[4]) {
        return (4);
    }
    if (us <= kHistUsEdge[5]) {
        return (5);
    }
    if (us <= kHistUsEdge[6]) {
        return (6);
    }
    if (us <= kHistUsEdge[7]) {
        return (7);
    }
    if (us <= kHistUsEdge[8]) {
        return (8);
    }
    return (9);
}

// float_of parses decimal int/float
// via strtod: digits, dot, and e/E
// science count; hex (x/X) and
// leading space do not; range caps
// at +-1e6, so inf/nan fall out.
static bool float_of(const char *s,
                     double *out) {
    char *end = nullptr;
    double x;
    size_t i;

    if (s == nullptr || out == nullptr) {
        return (false);
    }
    if (s[0] == '\0') {
        return (false);
    }
    if (s[0] == ' ' || s[0] == '\t') {
        return (false);
    }
    for (i = 0; s[i] != '\0'; i++) {
        if (s[i] == 'x' ||
            s[i] == 'X') {
            return (false);
        }
    }
    x = strtod(s, &end);
    if (end == nullptr ||
        *end != '\0') {
        return (false);
    }
    if (!(x >= -1000000.0 &&
          x <= 1000000.0)) {
        return (false);
    }
    *out = x;
    return (true);
}

// val_on parses on: any|all|[indices].
static bool val_on(SuiteNode *map,
                   const ExFile *f, VCtx *v,
                   bool allow_any,
                   int *idx, size_t *nidx,
                   bool *all, bool *any) {
    SuiteNode *nd;

    *nidx = 0;
    *all = false;
    *any = false;
    nd = suite_map_find(map, "on");
    if (nd == nullptr) {
        return (vbad(v, map->line,
                     "missing 'on'"));
    }
    if (nd->kind == SUITE_SCALAR) {
        if (scalar_eq(nd, "all")) {
            *all = true;
            return (true);
        }
        if (allow_any &&
            scalar_eq(nd, "any")) {
            *any = true;
            return (true);
        }
        return (vbad(v, nd->line,
                     "bad 'on' scalar"));
    }
    if (nd->kind != SUITE_LIST) {
        return (vbad(v, nd->line,
                     "bad 'on' value"));
    }
    if (suite_list_len(nd) == 0) {
        return (vbad(v, nd->line,
                     "empty 'on' list"));
    }
    {
        size_t i;

        for (i = 0; i <
             suite_list_len(nd); i++) {
            SuiteNode *it;
            long long x;

            it = suite_list_at(nd, i);
            // Int-shaped counts, quoted ok.
            if (!need_int(it, &x) ||
                x < 0 ||
                x >= f->nodes) {
                return (vbad(
                    v, it->line,
                    "node index out of "
                    "range"));
            }
            if (*nidx >=
                (size_t)kMaxNodes) {
                return (vbad(
                    v, it->line,
                    "too many indices"));
            }
            idx[*nidx] = (int)x;
            (*nidx)++;
        }
    }
    return (true);
}

// val_wlref reads an optional
// workload key into dst, default
// submitted. Loud on bad names.
static bool val_wlref(SuiteNode *map,
                      VCtx *v,
                      char *dst) {
    SuiteNode *w;
    const char *s;

    (void)strcpy(dst, "submitted");
    w = suite_map_find(map,
                       "workload");
    if (w == nullptr) {
        return (true);
    }
    s = suite_scalar_str(w);
    if (s == nullptr ||
        !wl_name_ok(s)) {
        return (vbad(v, w->line,
                     "bad workload "
                     "name"));
    }
    (void)strcpy(dst, s);
    return (true);
}

// val_cond validates one until or
// sustain mapping. V1 holds exactly
// one condition per mapping; ALL
// per-node checks must become true.
static bool val_cond(SuiteNode *map,
                     const ExFile *f,
                     VCtx *v, ExCond *c) {
    const char *known[] = {
        "peers",    "marker",  "present",
        "absent",   "observed",
        "assigned", "mesh_count",
        "agree",    "assigned_on",
        "epoch",    "image",
    };
    const char *hit[11];
    size_t nhit = 0;
    size_t i;

    if (map->kind != SUITE_MAP) {
        return (vbad(v, map->line,
                     "condition must be a "
                     "mapping"));
    }
    for (i = 0; i <
         map->u.map.len; i++) {
        const char *k;
        size_t j;
        bool ok = false;

        k = map->u.map.items[i].key;
        if (strcmp(k, "on") == 0) {
            continue;
        }
        if (has_ramp(k)) {
            return (ramp_bad(
                v,
                map->u.map.items[i]
                    .key_line));
        }
        for (j = 0; j < 11; j++) {
            if (strcmp(k, known[j]) ==
                0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return (vunknown(
                v,
                map->u.map.items[i]
                    .key_line,
                "condition", k));
        }
        hit[nhit] = k;
        nhit++;
    }
    if (nhit == 0) {
        return (vbad(v, map->line,
                     "empty condition"));
    }
    if (nhit > 1) {
        return (vbad(
            v, map->line,
            "one condition per phase "
            "in v1 (chain phases for "
            "AND)"));
    }
    memset(c, 0, sizeof(*c));
    c->line = map->line;
    if (strcmp(hit[0], "peers") == 0) {
        SuiteNode *nd;
        long long n;
        int idx[kMaxNodes];
        size_t ni = 0;
        bool all = false;
        bool any = false;
        static const char *keep[] = {
            "peers",
            "on",
        };

        if (f->sut != kConcord) {
            return (vbad(
                v, map->line,
                "peers is concord-only"));
        }
        if (!val_extra(map, keep, 2,
                       "condition", v)) {
            return (false);
        }
        c->kind = CD_PEERS;
        nd = suite_map_find(map,
                            "peers");
        // Int-shaped counts, quoted ok.
        if (!need_int(nd, &n) ||
            n < 1 || n > kMaxNodes) {
            return (vbad(
                v, nd->line,
                "peers count out of "
                "range"));
        }
        c->count = n;
        if (n > f->nodes) {
            return (vbad(
                v, nd->line,
                "peers count above "
                "nodes"));
        }
        if (!val_on(map, f, v, false,
                    idx, &ni, &all,
                    &any)) {
            return (false);
        }
        return (expand_on(c, idx, ni,
                          all,
                          f->nodes));
    }
    if (strcmp(hit[0], "marker") ==
        0) {
        SuiteNode *nd;
        const char *s;
        int idx[kMaxNodes];
        size_t ni = 0;
        bool all = false;
        bool any = false;
        static const char *keep[] = {
            "marker",
            "on",
        };

        if (!val_extra(map, keep, 2,
                       "condition", v)) {
            return (false);
        }
        c->kind = CD_MARKER;
        nd = suite_map_find(map,
                            "marker");
        s = suite_scalar_str(nd);
        if (s == nullptr ||
            s[0] == '\0') {
            return (vbad(
                v, nd->line,
                "marker must be a "
                "string"));
        }
        if (!copy_text(c->marker,
                       sizeof(c->marker),
                       s)) {
            return (vbad(
                v, nd->line,
                "marker too long"));
        }
        if (!val_on(map, f, v, true,
                    idx, &ni, &all,
                    &any)) {
            return (false);
        }
        c->any = any;
        if (any) {
            return (true);
        }
        return (expand_on(c, idx, ni,
                          all,
                          f->nodes));
    }
    if (strcmp(hit[0], "present") ==
        0 ||
        strcmp(hit[0], "absent") ==
            0) {
        SuiteNode *nd;

        c->kind = strcmp(hit[0],
                         "present") == 0
                      ? CD_PRESENT
                      : CD_ABSENT;
        c->line = key_line(map,
                           hit[0]);
        {
            const char *one[] = {
                hit[0],
            };

            if (!val_extra(
                    map, one, 1,
                    "condition", v)) {
                return (false);
            }
        }
        nd = suite_map_find(map,
                            hit[0]);
        if (nd == nullptr ||
            nd->kind != SUITE_MAP) {
            return (vbad(
                v, c->line,
                "present/absent need a "
                "mapping"));
        }
        {
            static const char *keep[] = {
                "workload",
                "text",
                "on",
            };
            SuiteNode *w;
            SuiteNode *t;

            if (!val_extra(
                    nd, keep, 3,
                    "condition", v)) {
                return (false);
            }
            w = suite_map_find(
                nd, "workload");
            t = suite_map_find(nd,
                               "text");
            if (w != nullptr &&
                t != nullptr) {
                return (vbad(
                    v, nd->line,
                    "want workload or "
                    "text"));
            }
            // No key defaults to the
            // submitted workload ref.
            if (w == nullptr &&
                t == nullptr) {
                if (f->sut !=
                    kConcord) {
                    return (vbad(
                        v, nd->line,
                        "workload present "
                        "is concord-only"));
                }
                (void)strcpy(c->wl,
                             "submitted");
                c->workload = true;
            } else if (w != nullptr) {
                    if (f->sut !=
                        kConcord) {
                        return (vbad(
                            v, nd->line,
                            "workload "
                            "present is "
                            "concord-only"));
                    }
                    if (!val_wlref(
                            nd, v,
                            c->wl)) {
                        return (false);
                    }
                    c->workload = true;
                } else {
                const char *s;

                if (f->sut != kWeld) {
                    return (vbad(
                        v, nd->line,
                        "text present is "
                        "weld-only"));
                }
                s = suite_scalar_str(
                    t);
                if (s == nullptr ||
                    s[0] == '\0') {
                    return (vbad(
                        v, t->line,
                        "text must be a "
                        "string"));
                }
                if (!copy_text(
                        c->text,
                        sizeof(c->text),
                        s)) {
                    return (vbad(
                        v, t->line,
                        "text too long"));
                }
                c->workload = false;
            }
            {
                int idx[kMaxNodes];
                size_t ni = 0;
                bool all = false;
                bool any = false;

                if (!val_on(nd, f, v,
                            false, idx,
                            &ni, &all,
                            &any)) {
                    return (false);
                }
                return (expand_on(
                    c, idx, ni, all,
                    f->nodes));
            }
        }
    }
    if (strcmp(hit[0], "observed") ==
        0) {
        SuiteNode *nd;
        SuiteNode *w;
        SuiteNode *t;
        SuiteNode *wi;
        const char *s;
        static const char *keep[] = {
            "on",
            "text",
        };

        c->kind = CD_OBSERVED;
        c->line = key_line(map,
                           "observed");
        {
            static const char *one[] = {
                "observed",
            };

            if (!val_extra(
                    map, one, 1,
                    "condition", v)) {
                return (false);
            }
        }
        nd = suite_map_find(map,
                            "observed");
        if (nd == nullptr ||
            nd->kind != SUITE_MAP) {
            return (vbad(
                v, c->line,
                "observed needs a "
                "mapping"));
        }
        if (!val_extra(nd, keep, 2,
                       "condition", v)) {
            return (false);
        }
        w = suite_map_find(nd, "on");
        t = suite_map_find(nd,
                           "text");
        if (w == nullptr ||
            t == nullptr) {
            return (vbad(
                v, nd->line,
                "observed needs on "
                "plus text"));
        }
        // One scalar index; a lone
        // list item counts as scalar.
        // Endpoint forms weld:N,
        // ros:N name the kind; bare N
        // means the file default.
        wi = w;
        if (w->kind == SUITE_LIST &&
            suite_list_len(w) == 1) {
            wi = suite_list_at(w, 0);
        } else if (w->kind !=
                   SUITE_SCALAR) {
            return (vbad(
                v, w->line,
                "observed on must be "
                "a scalar node index"));
        }
        {
            int ok = EP_WELD;
            int on = 0;
            char em[96];

            if (!ep_parse_node(
                    wi, f, &ok,
                    &on)) {
                return (vbad(
                    v, wi->line,
                    "node index out "
                    "of range or bad "
                    "endpoint"));
            }
            if (!kEpCap[ok].obs_weld &&
                !kEpCap[ok].obs_ros) {
                (void)snprintf(
                    em, sizeof(em),
                    "%s cannot "
                    "observe: no log "
                    "verb",
                    ep_name(ok));
                return (vbad(
                    v, wi->line, em));
            }
            c->obs = on;
            c->obs_kind = ok;
        }
        s = suite_scalar_str(t);
        if (s == nullptr ||
            s[0] == '\0') {
            return (vbad(
                v, t->line,
                "text must be a "
                "string"));
        }
        if (!copy_text(c->text,
                       sizeof(c->text),
                       s)) {
            return (vbad(
                v, t->line,
                "text too long"));
        }
        return (true);
    }
    if (strcmp(hit[0], "assigned") ==
        0) {
        SuiteNode *nd;
        static const char *keep[] = {
            "workload",
        };

        if (f->sut != kConcord) {
            return (vbad(
                v, map->line,
                "assigned is "
                "concord-only"));
        }
        c->kind = CD_ASSIGNED;
        c->line = key_line(map,
                           "assigned");
        {
            static const char *one[] = {
                "assigned",
            };

            if (!val_extra(
                    map, one, 1,
                    "condition", v)) {
                return (false);
            }
        }
        nd = suite_map_find(map,
                            "assigned");
        if (nd == nullptr ||
            nd->kind != SUITE_MAP) {
            return (vbad(
                v, c->line,
                "assigned needs a "
                "mapping"));
        }
        if (!val_extra(nd, keep, 1,
                       "condition", v)) {
            return (false);
        }
        if (!val_wlref(nd, v,
                       c->wl)) {
            return (false);
        }
        return (true);
    }
    if (strcmp(hit[0], "agree") ==
        0) {
        SuiteNode *nd;
        static const char *keep[] = {
            "workload",
        };

        if (f->sut != kConcord) {
            return (vbad(
                v, map->line,
                "agree is "
                "concord-only"));
        }
        c->kind = CD_AGREE;
        c->line = key_line(map,
                           "agree");
        {
            static const char *one[] = {
                "agree",
            };

            if (!val_extra(
                    map, one, 1,
                    "condition", v)) {
                return (false);
            }
        }
        nd = suite_map_find(map,
                            "agree");
        if (nd == nullptr ||
            nd->kind != SUITE_MAP) {
            return (vbad(
                v, c->line,
                "agree needs a "
                "mapping"));
        }
        if (!val_extra(nd, keep, 1,
                       "condition", v)) {
            return (false);
        }
        if (!val_wlref(nd, v,
                       c->wl)) {
            return (false);
        }
        return (true);
    }
    if (strcmp(hit[0], "assigned_on") ==
        0) {
        SuiteNode *nd;
        SuiteNode *a;
        long long x;
        static const char *keep[] = {
            "workload",
            "node",
        };

        if (f->sut != kConcord) {
            return (vbad(
                v, map->line,
                "assigned_on is "
                "concord-only"));
        }
        c->kind = CD_ASSIGNED_ON;
        c->line = key_line(
            map, "assigned_on");
        {
            static const char *one[] = {
                "assigned_on",
            };

            if (!val_extra(
                    map, one, 1,
                    "condition", v)) {
                return (false);
            }
        }
        nd = suite_map_find(
            map, "assigned_on");
        if (nd == nullptr ||
            nd->kind != SUITE_MAP) {
            return (vbad(
                v, c->line,
                "assigned_on needs a "
                "mapping"));
        }
        if (!val_extra(nd, keep, 2,
                       "condition", v)) {
            return (false);
        }
        if (!val_wlref(nd, v,
                       c->wl)) {
            return (false);
        }
        a = suite_map_find(nd,
                           "node");
        if (a == nullptr) {
            return (vbad(
                v, nd->line,
                "assigned_on needs "
                "node"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        c->node = (int)x;
        return (true);
    }
    if (strcmp(hit[0], "epoch") ==
        0) {
        SuiteNode *nd;
        SuiteNode *a;
        SuiteNode *q;
        SuiteNode *g;
        long long x;
        static const char *keep[] = {
            "workload",
            "node",
            "eq",
            "gte",
        };

        if (f->sut != kConcord) {
            return (vbad(
                v, map->line,
                "epoch is "
                "concord-only"));
        }
        c->kind = CD_EPOCH;
        c->line = key_line(map,
                           "epoch");
        {
            static const char *one[] = {
                "epoch",
            };

            if (!val_extra(
                    map, one, 1,
                    "condition", v)) {
                return (false);
            }
        }
        nd = suite_map_find(map,
                            "epoch");
        if (nd == nullptr ||
            nd->kind != SUITE_MAP) {
            return (vbad(
                v, c->line,
                "epoch needs a "
                "mapping"));
        }
        if (!val_extra(nd, keep, 4,
                       "condition", v)) {
            return (false);
        }
        if (!val_wlref(nd, v,
                       c->wl)) {
            return (false);
        }
        a = suite_map_find(nd,
                           "node");
        if (a == nullptr) {
            return (vbad(
                v, nd->line,
                "epoch needs node"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        c->node = (int)x;
        q = suite_map_find(nd,
                           "eq");
        g = suite_map_find(nd,
                           "gte");
        if (q != nullptr &&
            g != nullptr) {
            return (vbad(
                v, nd->line,
                "epoch takes eq or "
                "gte, not both"));
        }
        if (q != nullptr) {
            // Int-shaped counts, ok.
            if (!need_int(q, &x) ||
                x < 0) {
                return (vbad(
                    v, q->line,
                    "epoch eq must be "
                    "0 or more"));
            }
            c->cmp = CMP_EQ;
            c->epoch =
                (unsigned long long)x;
        }
        if (g != nullptr) {
            // Int-shaped counts, ok.
            if (!need_int(g, &x) ||
                x < 0) {
                return (vbad(
                    v, g->line,
                    "epoch gte must "
                    "be 0 or more"));
            }
            c->cmp = CMP_GTE;
            c->epoch =
                (unsigned long long)x;
        }
        return (true);
    }
    if (strcmp(hit[0], "image") ==
        0) {
        SuiteNode *nd;
        SuiteNode *a;
        const char *t;
        int idx[kMaxNodes];
        size_t ni = 0;
        bool all = false;
        bool any = false;
        static const char *keep[] = {
            "workload",
            "value",
            "on",
        };

        if (f->sut != kConcord) {
            return (vbad(
                v, map->line,
                "image is "
                "concord-only"));
        }
        c->kind = CD_IMAGE;
        c->line = key_line(map,
                           "image");
        {
            static const char *one[] = {
                "image",
            };

            if (!val_extra(
                    map, one, 1,
                    "condition", v)) {
                return (false);
            }
        }
        nd = suite_map_find(map,
                            "image");
        if (nd == nullptr ||
            nd->kind != SUITE_MAP) {
            return (vbad(
                v, c->line,
                "image needs a "
                "mapping"));
        }
        if (!val_extra(nd, keep, 3,
                       "condition", v)) {
            return (false);
        }
        if (!val_wlref(nd, v,
                       c->wl)) {
            return (false);
        }
        a = suite_map_find(nd,
                           "value");
        if (a == nullptr) {
            return (vbad(
                v, nd->line,
                "image needs value"));
        }
        t = suite_scalar_str(a);
        if (t == nullptr ||
            !image_ok(t)) {
            return (vbad(
                v, a->line,
                "bad image ref"));
        }
        if (!copy_text(
                c->imgval,
                sizeof(c->imgval),
                t)) {
            return (vbad(
                v, a->line,
                "image too long"));
        }
        if (!val_on(nd, f, v, false,
                    idx, &ni, &all,
                    &any)) {
            return (false);
        }
        return (expand_on(c, idx, ni,
                          all,
                          f->nodes));
    }
    {
        SuiteNode *nd;
        long long n;
        static const char *one[] = {
            "mesh_count",
        };

        if (!val_extra(map, one, 1,
                       "condition", v)) {
            return (false);
        }
        c->kind = CD_MESH;
        c->line = key_line(map,
                           "mesh_count");
        nd = suite_map_find(
            map, "mesh_count");
        // Int-shaped counts, quoted ok.
        if (!need_int(nd, &n) ||
            n < 1 || n > kMaxNodes) {
            return (vbad(
                v, nd->line,
                "mesh_count out of "
                "range"));
        }
        if (n > f->nodes) {
            return (vbad(
                v, nd->line,
                "mesh_count above "
                "nodes"));
        }
        c->count = n;
        return (true);
    }
}

// Ramp step cap: at most 10 steps,
// so the ladder stays bounded (see
// run_ramp_weld proof).
static constexpr int kRampSteps = 10;

// val_ramp validates a ramp: block
// inside benchmark. All rates are
// 1..1000000, durations 1..3600 s,
// steps 1..10, loss 0..100 pct, p99
// 0..3600000 ms. At least one stop
// (loss/p99) is required; max_rate,
// max_steps and max_duration_s are
// hard backstops. Returns true with
// s ramp fields set.
static bool val_ramp(SuiteNode *nd,
                     VCtx *v, ExStep *s,
                     long long fam_cap) {
    SuiteNode *q = nullptr;
    long long x = 0;
    static const char *keep[] = {
        "start_rate", "step_rate",
        "step_duration_s",
        "max_rate", "max_steps",
        "max_duration_s",
        "loss_pct", "p99_ms",
    };

    if (nd == nullptr ||
        nd->kind != SUITE_MAP) {
        return (vbad(v, nd != nullptr
                       ? nd->line
                       : 0,
                    "ramp needs a "
                    "mapping"));
    }
    if (!val_extra(nd, keep, 8,
                   "ramp key", v)) {
        return (false);
    }
    q = suite_map_find(
        nd, "start_rate");
    if (q == nullptr ||
        !need_int(q, &x) || x < 1 ||
        x > kMaxRate) {
        return (vbad(
            v,
            q != nullptr ? q->line
                         : nd->line,
            "start_rate must be "
            "1..1000000"));
    }
    s->ramp_start = x;
    q = suite_map_find(
        nd, "step_rate");
    if (q == nullptr ||
        !need_int(q, &x) || x < 1 ||
        x > kMaxRate) {
        return (vbad(
            v,
            q != nullptr ? q->line
                         : nd->line,
            "step_rate must be "
            "1..1000000"));
    }
    s->ramp_step = x;
    q = suite_map_find(
        nd, "step_duration_s");
    if (q == nullptr ||
        !need_int(q, &x) || x < 1 ||
        x > kMaxTimeoutS) {
        return (vbad(
            v,
            q != nullptr ? q->line
                         : nd->line,
            "step_duration_s must "
            "be 1..3600"));
    }
    s->ramp_step_dur = x;
    q = suite_map_find(
        nd, "max_rate");
    if (q == nullptr ||
        !need_int(q, &x) || x < 1 ||
        x > kMaxRate) {
        return (vbad(
            v,
            q != nullptr ? q->line
                         : nd->line,
            "max_rate must be "
            "1..1000000"));
    }
    s->ramp_max_rate = x;
    if (s->ramp_max_rate <
        s->ramp_start) {
        return (vbad(
            v, q->line,
            "max_rate below "
            "start_rate"));
    }
    q = suite_map_find(
        nd, "max_steps");
    if (q == nullptr ||
        !need_int(q, &x) || x < 1 ||
        x > kRampSteps) {
        return (vbad(
            v,
            q != nullptr ? q->line
                         : nd->line,
            "max_steps must be "
            "1..10"));
    }
    s->ramp_max_steps = x;
    q = suite_map_find(
        nd, "max_duration_s");
    if (q == nullptr ||
        !need_int(q, &x) || x < 1 ||
        x > kMaxTimeoutS) {
        return (vbad(
            v,
            q != nullptr ? q->line
                         : nd->line,
            "max_duration_s must "
            "be 1..3600"));
    }
    s->ramp_max_dur = x;
    q = suite_map_find(
        nd, "loss_pct");
    if (q != nullptr) {
        if (!need_int(q, &x) ||
            x < 0 || x > 100) {
            return (vbad(
                v, q->line,
                "loss_pct must be "
                "0..100"));
        }
        s->ramp_loss_pct = x;
        s->ramp_has_loss = true;
    }
    q = suite_map_find(
        nd, "p99_ms");
    if (q != nullptr) {
        if (!need_int(q, &x) ||
            x < 0 || x > kMaxMs) {
            return (vbad(
                v, q->line,
                "p99_ms must be "
                "0..3600000"));
        }
        s->ramp_p99 = x;
        s->ramp_has_p99 = true;
    }
    if (!s->ramp_has_loss &&
        !s->ramp_has_p99) {
        return (vbad(
            v, nd->line,
            "ramp needs loss_pct "
            "or p99_ms"));
    }
    {
        unsigned long long prod;

        prod = (unsigned long long)
                s->ramp_max_rate *
            (unsigned long long)
                s->ramp_step_dur;
        if (prod >
            (unsigned long long)
                fam_cap) {
            return (vbad(
                v, nd->line,
                "ramp step too "
                "large"));
        }
        if (fam_cap ==
            (unsigned long long)
                kBenchConc) {
            unsigned long long tot;

            tot = (unsigned long long)
                    s->ramp_max_steps *
                prod;
            if (tot >
                (unsigned long long)
                    kMaxWl) {
                return (vbad(
                    v, nd->line,
                    "ramp too many "
                    "workloads"));
            }
        }
    }
    s->has_ramp = true;
    return (true);
}

// val_bench_generic validates a
// benchmark with send plus expect
// blocks naming registry entries.
// Old from/on/text keys are
// forbidden here; the caller takes
// the old path when neither send
// nor expect is present. Any via
// pair lints clean, even nonsense;
// nonsense measures 100 percent
// loss at runtime, never a lint
// error. Echo stays unchanged.
static bool val_bench_generic(
    SuiteNode *val, const ExFile *f,
    VCtx *v, ExStep *s, size_t line) {
    SuiteNode *snd;
    SuiteNode *exp;
    SuiteNode *q;
    long long x;
    bool is_c = false;
    static const char *keep[] = {
        "send",        "expect",
        "rate",        "count",
        "duration_s",  "warmup",
        "echo",        "ramp",
    };

    if (val->kind != SUITE_MAP) {
        return (vbad(
            v, line,
            "benchmark needs a "
            "mapping"));
    }
    if (!val_extra(val, keep, 8,
                   "step", v)) {
        return (false);
    }
    snd = suite_map_find(val,
                         "send");
    exp = suite_map_find(val,
                         "expect");
    if (snd == nullptr ||
        exp == nullptr) {
        return (vbad(
            v, line,
            "benchmark needs send "
            "plus expect"));
    }
    if (snd->kind != SUITE_MAP) {
        return (vbad(
            v, snd->line,
            "send needs a mapping"));
    }
    if (exp->kind != SUITE_MAP) {
        return (vbad(
            v, exp->line,
            "expect needs a mapping"));
    }
    {
        static const char *sk[] = {
            "node",          "via",
            "topic",         "image",
            "flags",         "as",
            "body",          "payload",
            "payload_bytes",
        };
        static const char *ek[] = {
            "node",
            "via",
            "match",
        };

        if (!val_extra(snd, sk, 9,
                       "step", v)) {
            return (false);
        }
        if (!val_extra(exp, ek, 3,
                       "step", v)) {
            return (false);
        }
    }
    {
        SuiteNode *a;
        SuiteNode *t;
        const char *vs;

        a = suite_map_find(snd,
                           "node");
        t = suite_map_find(snd,
                           "via");
        if (a == nullptr ||
            t == nullptr) {
            return (vbad(
                v, snd->line,
                "send needs node plus "
                "via"));
        }
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        s->node = (int)x;
        vs = suite_scalar_str(t);
        if (vs == nullptr) {
            return (via_bad(
                v, t->line, ""));
        }
        {
            const Probe *p;

            p = probe_of(vs);
            if (p == nullptr ||
                !p->send) {
                if (p != nullptr) {
                    char msg[96];
                    int m;

                    m = snprintf(
                        msg,
                        sizeof(msg),
                        "%s is "
                        "observe-only",
                        vs);
                    ellipsize(
                        msg,
                        sizeof(msg),
                        m);
                    return (vbad(
                        v, t->line,
                        msg));
                }
                return (via_bad(
                    v, t->line, vs));
            }
            if (!copy_text(
                    s->send_via,
                    sizeof(
                        s->send_via),
                    vs)) {
                return (vbad(
                    v, t->line,
                    "via too long"));
            }
            is_c = strcmp(
                       vs,
                       "workload-submit") ==
                0;
            if (is_c) {
                s->from_kind =
                    EP_CONCORD;
            } else if (strcmp(
                           vs,
                           "ros-pub") ==
                0) {
                s->from_kind = EP_ROS;
            } else {
                s->from_kind =
                    EP_WELD;
            }
        }
        if (!is_c) {
            SuiteNode *b;
            SuiteNode *pb;
            SuiteNode *pl;
            SuiteNode *tp;
            const char *ts;

            if (suite_map_find(
                    snd,
                    "image") !=
                nullptr) {
                return (vbad(
                    v, snd->line,
                    "weld-pub forbids "
                    "image"));
            }
            if (suite_map_find(
                    snd,
                    "flags") !=
                nullptr) {
                return (vbad(
                    v, snd->line,
                    "weld-pub forbids "
                    "flags"));
            }
            if (suite_map_find(
                    snd, "as") !=
                nullptr) {
                return (vbad(
                    v, snd->line,
                    "weld-pub forbids "
                    "as"));
            }
            if (strcmp(s->send_via,
                       "ros-pub") ==
                0) {
                if (suite_map_find(
                        snd,
                        "image") !=
                    nullptr) {
                    return (vbad(
                        v, snd->line,
                        "ros-pub forbids "
                        "image"));
                }
            }
            b = suite_map_find(
                snd, "body");
            pb = suite_map_find(
                snd,
                "payload_bytes");
            if (b == nullptr &&
                pb == nullptr) {
                char msg[96];
                int m;

                m = snprintf(
                    msg,
                    sizeof(msg),
                    "%s needs body or "
                    "payload_bytes",
                    s->send_via);
                ellipsize(
                    msg, sizeof(msg),
                    m);
                return (vbad(
                    v, snd->line,
                    msg));
            }
            if (b != nullptr) {
                ts = suite_scalar_str(
                    b);
                if (ts == nullptr ||
                    ts[0] == '\0') {
                    return (vbad(
                        v, b->line,
                        "body must be a "
                        "string"));
                }
                if (old_tag(ts)) {
                    return (vbad(
                        v, b->line,
                        "old bench tag: "
                        "use bench#seq#"
                        "send_ns#text"));
                }
                if (!copy_text(
                        s->text,
                        sizeof(s->text),
                        ts)) {
                    return (vbad(
                        v, b->line,
                        "body too long"));
                }
            } else {
                s->text[0] = '\0';
            }
            if (pb != nullptr) {
                if (!need_int(
                        pb, &x) ||
                    x < 0 ||
                    x > kBenchBytes) {
                    return (vbad(
                        v, pb->line,
                        "payload_bytes "
                        "must be "
                        "0..65536"));
                }
                if (x > 0) {
                    s->pay_bytes =
                        (unsigned long long)
                            x;
                    s->has_pay =
                        true;
                }
            }
            pl = suite_map_find(
                snd, "payload");
            if (pl != nullptr) {
                ts = suite_scalar_str(
                    pl);
                if (ts == nullptr ||
                    ts[0] == '\0') {
                    return (vbad(
                        v, pl->line,
                        "payload must "
                        "be a string"));
                }
                if (!ph_ok(
                        ts, v,
                        pl->line,
                        false)) {
                    return (false);
                }
                if (!copy_text(
                        s->send_tpl,
                        sizeof(
                            s->send_tpl),
                        ts)) {
                    return (vbad(
                        v, pl->line,
                        "payload too "
                        "long"));
                }
            }
            tp = suite_map_find(
                snd, "topic");
            if (tp != nullptr) {
                ts = suite_scalar_str(
                    tp);
                if (ts == nullptr ||
                    !topic_ok(ts)) {
                    return (vbad(
                        v, tp->line,
                        "bad topic"));
                }
                if (!copy_text(
                        s->topic,
                        sizeof(s->topic),
                        ts)) {
                    return (vbad(
                        v, tp->line,
                        "topic too "
                        "long"));
                }
                s->has_topic = true;
            }
            if (strcmp(s->send_via,
                       "ros-pub") ==
                    0 &&
                !needs_has(
                    f, "ros.pub")) {
                return (vbad(
                    v, snd->line,
                    "ros publish needs "
                    "ros.pub need"));
            }
        } else {
            SuiteNode *b;
            SuiteNode *c2;
            SuiteNode *fl;
            const char *ts;

            if (suite_map_find(
                    snd,
                    "topic") !=
                nullptr) {
                return (vbad(
                    v, snd->line,
                    "workload-submit "
                    "forbids topic"));
            }
            if (suite_map_find(
                    snd, "body") !=
                nullptr) {
                return (vbad(
                    v, snd->line,
                    "workload-submit "
                    "forbids body"));
            }
            if (suite_map_find(
                    snd,
                    "payload") !=
                nullptr) {
                return (vbad(
                    v, snd->line,
                    "workload-submit "
                    "forbids payload"));
            }
            if (suite_map_find(
                    snd,
                    "payload_bytes") !=
                nullptr) {
                return (vbad(
                    v, snd->line,
                    "workload-submit "
                    "forbids "
                    "payload_bytes"));
            }
            b = suite_map_find(
                snd, "image");
            c2 = suite_map_find(
                snd, "as");
            if (b == nullptr ||
                c2 == nullptr) {
                return (vbad(
                    v, snd->line,
                    "workload-submit "
                    "needs image plus "
                    "as"));
            }
            ts = suite_scalar_str(
                b);
            if (ts == nullptr ||
                !image_ok(ts)) {
                return (vbad(
                    v, b->line,
                    "bad image ref"));
            }
            if (!copy_text(
                    s->image,
                    sizeof(s->image),
                    ts)) {
                return (vbad(
                    v, b->line,
                    "image too long"));
            }
            ts = suite_scalar_str(
                c2);
            if (ts == nullptr ||
                !wl_name_ok(ts)) {
                return (vbad(
                    v, c2->line,
                    "bad workload "
                    "name"));
            }
            if (!copy_text(
                    s->as_name,
                    sizeof(s->as_name),
                    ts)) {
                return (vbad(
                    v, c2->line,
                    "workload name too "
                    "long"));
            }
            s->has_as = true;
            fl = suite_map_find(
                snd, "flags");
            if (fl != nullptr) {
                ts = suite_scalar_str(
                    fl);
                if (ts ==
                    nullptr) {
                    return (vbad(
                        v, fl->line,
                        "flags must be "
                        "a string"));
                }
                if (!flags_ok(
                        ts)) {
                    return (vbad(
                        v, fl->line,
                        "bad flags"));
                }
                if (!copy_text(
                        s->flags,
                        sizeof(s->flags),
                        ts)) {
                    return (vbad(
                        v, fl->line,
                        "flags too "
                        "long"));
                }
                s->has_flags = true;
            }
        }
    }
    {
        SuiteNode *a;
        SuiteNode *t;
        SuiteNode *m;
        const char *vs;
        bool allow_wid = is_c;

        a = suite_map_find(exp,
                           "node");
        t = suite_map_find(exp,
                           "via");
        m = suite_map_find(exp,
                           "match");
        if (a == nullptr ||
            t == nullptr ||
            m == nullptr) {
            return (vbad(
                v, exp->line,
                "expect needs node "
                "plus via plus match"));
        }
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        s->obs = (int)x;
        vs = suite_scalar_str(t);
        if (vs == nullptr) {
            return (via_bad(
                v, t->line, ""));
        }
        {
            const Probe *p;

            p = probe_of(vs);
            if (p == nullptr ||
                p->send) {
                if (p != nullptr) {
                    char msg[96];
                    int mm;

                    mm = snprintf(
                        msg,
                        sizeof(msg),
                        "%s is "
                        "send-only",
                        vs);
                    ellipsize(
                        msg,
                        sizeof(msg),
                        mm);
                    return (vbad(
                        v, t->line,
                        msg));
                }
                return (via_bad(
                    v, t->line, vs));
            }
            if (!copy_text(
                    s->expect_via,
                    sizeof(
                        s->expect_via),
                    vs)) {
                return (vbad(
                    v, t->line,
                    "via too long"));
            }
            if (strcmp(
                    vs,
                    "ros-log-contains") ==
                0) {
                s->on_kind = EP_ROS;
            } else if (strcmp(
                           vs,
                           "log-contains") ==
                0) {
                s->on_kind = EP_WELD;
            } else if (strcmp(
                           vs,
                           "prometheus-scrape") ==
                       0) {
                s->on_kind = sut_def_ep(
                    f->sut);
            } else {
                s->on_kind =
                    EP_CONCORD;
            }
        }
        if (strcmp(s->expect_via,
                   "journal-has") ==
            0) {
            SuiteNode *fd;
            SuiteNode *vl;
            const char *fs;
            const char *vv;
            static const char *jk[] = {
                "field",
                "value",
            };

            if (m->kind !=
                SUITE_MAP) {
                return (vbad(
                    v, m->line,
                    "journal-has needs "
                    "match mapping"));
            }
            if (!val_extra(
                    m, jk, 2, "step",
                    v)) {
                return (false);
            }
            fd = suite_map_find(
                m, "field");
            vl = suite_map_find(
                m, "value");
            if (fd == nullptr ||
                vl == nullptr) {
                return (vbad(
                    v, m->line,
                    "journal-has needs "
                    "field plus value"));
            }
            fs = suite_scalar_str(
                fd);
            if (fs == nullptr ||
                (strcmp(
                     fs,
                     "AssignedNodeID") !=
                     0 &&
                 strcmp(
                     fs,
                     "AssignmentEpoch") !=
                     0 &&
                 strcmp(fs,
                        "Image") !=
                     0)) {
                return (vbad(
                    v,
                    fd != nullptr
                        ? fd->line
                        : m->line,
                    "journal-has needs "
                    "field "
                    "AssignedNodeID, "
                    "AssignmentEpoch, "
                    "or Image"));
            }
            vv = suite_scalar_str(
                vl);
            if (vv == nullptr ||
                vv[0] == '\0') {
                return (vbad(
                    v,
                    vl != nullptr
                        ? vl->line
                        : m->line,
                    "journal-has needs "
                    "value"));
            }
            if (!ph_ok(
                    vv, v, vl->line,
                    allow_wid)) {
                return (false);
            }
            if (!copy_text(
                    s->exp_field,
                    sizeof(
                        s->exp_field),
                    fs)) {
                return (vbad(
                    v, fd->line,
                    "field too long"));
            }
            if (!copy_text(
                    s->exp_value,
                    sizeof(
                        s->exp_value),
                    vv)) {
                return (vbad(
                    v, vl->line,
                    "value too long"));
            }
        } else if (strcmp(
                       s->expect_via,
                       "workload-running") ==
            0) {
            const char *ms;

            if (m->kind !=
                SUITE_SCALAR) {
                return (vbad(
                    v, m->line,
                    "workload-running "
                    "needs match "
                    "running"));
            }
            ms = suite_scalar_str(
                m);
            if (ms == nullptr ||
                strcmp(ms,
                       "running") !=
                    0) {
                return (vbad(
                    v, m->line,
                    "workload-running "
                    "needs match "
                    "running"));
            }
            if (!copy_text(
                    s->exp_match,
                    sizeof(
                        s->exp_match),
                    ms)) {
                return (vbad(
                    v, m->line,
                    "match too long"));
            }
        } else if (strcmp(
                       s->expect_via,
                       "prometheus-scrape") ==
                   0) {
            SuiteNode *mm;
            SuiteNode *op;
            SuiteNode *vv;
            SuiteNode *pt;
            SuiteNode *ph;
            const char *ms;
            const char *os;
            const char *vs;
            const char *ps;
            long long port = 0;
            double thresh = 0.0;
            static const char *sk[] = {
                "metric", "op",
                "value",  "port",
                "path",
            };

            if (m->kind !=
                SUITE_MAP) {
                return (vbad(
                    v, m->line,
                    "prometheus-scrape "
                    "needs match "
                    "mapping"));
            }
            if (!val_extra(
                    m, sk, 5, "step",
                    v)) {
                return (false);
            }
            mm = suite_map_find(
                m, "metric");
            op = suite_map_find(
                m, "op");
            vv = suite_map_find(
                m, "value");
            pt = suite_map_find(
                m, "port");
            if (mm == nullptr ||
                op == nullptr ||
                vv == nullptr ||
                pt == nullptr) {
                return (vbad(
                    v, m->line,
                    "prometheus-scrape "
                    "needs metric plus "
                    "op plus value "
                    "plus port"));
            }
            ms = suite_scalar_str(
                mm);
            if (!scrape_metric_ok(
                    ms)) {
                return (vbad(
                    v, mm->line,
                    "bad metric name"));
            }
            os = suite_scalar_str(
                op);
            if (!scrape_op_ok(os)) {
                return (vbad(
                    v, op->line,
                    "bad op: want eq, "
                    "ne, lt, le, gt, "
                    "ge"));
            }
            vs = suite_scalar_str(
                vv);
            if (vs == nullptr ||
                !float_of(vs,
                          &thresh)) {
                return (vbad(
                    v, vv->line,
                    "bad value: want "
                    "number"));
            }
            if (!need_int(pt,
                          &port) ||
                port < 1 ||
                port > 65535) {
                return (vbad(
                    v, pt->line,
                    "bad port: want "
                    "1..65535"));
            }
            ps = "/metrics";
            ph = suite_map_find(
                m, "path");
            if (ph != nullptr) {
                ps = suite_scalar_str(
                    ph);
                if (!scrape_path_ok(
                        ps)) {
                    return (vbad(
                        v, ph->line,
                        "bad path: want "
                        "/metrics style"));
                }
                if (!copy_text(
                        s->exp_path,
                        sizeof(
                            s->exp_path),
                        ps)) {
                    return (vbad(
                        v, ph->line,
                        "path too long"));
                }
            } else {
                if (!copy_text(
                        s->exp_path,
                        sizeof(
                            s->exp_path),
                        ps)) {
                    return (vbad(
                        v, m->line,
                        "path too long"));
                }
            }
            if (!copy_text(
                    s->exp_metric,
                    sizeof(
                        s->exp_metric),
                    ms)) {
                return (vbad(
                    v, mm->line,
                    "metric too long"));
            }
            if (!copy_text(
                    s->exp_op,
                    sizeof(s->exp_op),
                    os)) {
                return (vbad(
                    v, op->line,
                    "op too long"));
            }
            s->exp_thresh = thresh;
            s->exp_port = port;
        } else {
            const char *ms;

            if (m->kind !=
                SUITE_SCALAR) {
                return (vbad(
                    v, m->line,
                    "match must be a "
                    "string"));
            }
            ms = suite_scalar_str(
                m);
            if (ms == nullptr ||
                ms[0] == '\0') {
                char msg[96];
                int mm;

                mm = snprintf(
                    msg,
                    sizeof(msg),
                    "%s needs match",
                    s->expect_via);
                ellipsize(
                    msg, sizeof(msg),
                    mm);
                return (vbad(
                    v, m->line, msg));
            }
            if (old_tag(ms)) {
                return (vbad(
                    v, m->line,
                    "old bench tag: "
                    "use bench#seq#"
                    "send_ns#text"));
            }
            if (!ph_ok(
                    ms, v, m->line,
                    allow_wid)) {
                return (false);
            }
            if (!copy_text(
                    s->exp_match,
                    sizeof(
                        s->exp_match),
                    ms)) {
                return (vbad(
                    v, m->line,
                    "match too long"));
            }
        }
    }
    {
        long long cap;

        cap = is_c ? kBenchConc
                   : kBenchWeld;
        {
            SuiteNode *rm;

            rm = suite_map_find(
                val, "ramp");
            if (rm != nullptr) {
                if (suite_map_find(
                        val,
                        "rate") !=
                    nullptr) {
                    return (vbad(
                        v, line,
                        "ramp forbids "
                        "rate: use ramp "
                        "start"));
                }
                if (suite_map_find(
                        val,
                        "count") !=
                    nullptr) {
                    return (vbad(
                        v, line,
                        "ramp forbids "
                        "count: use ramp "
                        "steps"));
                }
                if (suite_map_find(
                        val,
                        "duration_s") !=
                    nullptr) {
                    return (vbad(
                        v, line,
                        "ramp forbids "
                        "duration_s"));
                }
                if (suite_map_find(
                        val,
                        "warmup") !=
                    nullptr) {
                    return (vbad(
                        v, line,
                        "ramp forbids "
                        "warmup"));
                }
                if (!val_ramp(
                        rm, v, s,
                        cap)) {
                    return (false);
                }
            }
        }
        if (!s->has_ramp) {
            q = suite_map_find(val,
                               "rate");
            if (q == nullptr) {
                return (vbad(
                    v, line,
                    "benchmark needs "
                    "rate"));
            }
            if (!need_int(q, &x) ||
                x < 1 ||
                x > kMaxRate) {
                return (vbad(
                    v, q->line,
                    "rate must be "
                    "1..1000000"));
            }
            s->rate = x;
            s->has_rate = true;
        }
        if (!s->has_ramp) {
            q = suite_map_find(val,
                               "count");
            if (q != nullptr) {
                if (!need_int(
                        q, &x) ||
                    x < 1 || x > cap) {
                    return (vbad(
                        v, q->line,
                        "benchmark count "
                        "out of range"));
                }
                s->count = x;
                s->has_count = true;
            }
            q = suite_map_find(
                val, "duration_s");
            if (q != nullptr) {
                if (!need_int(
                        q, &x) ||
                    x < 1 ||
                    x > kMaxTimeoutS) {
                    return (vbad(
                        v, q->line,
                        "duration_s must "
                        "be 1..3600"));
                }
                s->duration_s = x;
                s->has_dur = true;
            }
            if (!s->has_count &&
                !s->has_dur) {
                return (vbad(
                    v, line,
                    "benchmark needs "
                    "count or "
                    "duration_s"));
            }
            q = suite_map_find(val,
                               "warmup");
            if (q != nullptr) {
                if (!need_int(
                        q, &x) ||
                    x < 0 ||
                    x > kBenchWeld) {
                    return (vbad(
                        v, q->line,
                        "warmup must be "
                        "0..1000"));
                }
                s->warmup = x;
            }
            if (s->has_count &&
                s->warmup >=
                    s->count) {
                return (vbad(
                    v, line,
                    "warmup must be "
                    "below count"));
            }
            if (s->has_dur) {
                unsigned long long prod;

                prod =
                    (unsigned long long)
                        s->rate *
                    (unsigned long long)
                        s->duration_s;
                if (prod >
                    (unsigned long long)
                        cap) {
                    return (vbad(
                        v, line,
                        "benchmark too "
                        "large"));
                }
            }
            if (s->has_count &&
                strlen(s->as_name) +
                        4 >
                    (size_t)kWlLen &&
                is_c) {
                return (vbad(
                    v, line,
                    "workload name too "
                    "long for count"));
            }
            if (s->has_dur && is_c &&
                strlen(s->as_name) +
                        4 >
                    (size_t)kWlLen) {
                return (vbad(
                    v, line,
                    "workload name too "
                    "long for count"));
            }
            if (s->has_ramp && is_c &&
                strlen(s->as_name) +
                        4 >
                    (size_t)kWlLen) {
                return (vbad(
                    v, line,
                    "workload name too "
                    "long for count"));
            }
        }
        {
            SuiteNode *e;
            SuiteNode *by;
            SuiteNode *en;

            e = suite_map_find(
                val, "echo");
            if (e != nullptr) {
                const char *ts;

                if (e->kind !=
                    SUITE_MAP) {
                    return (vbad(
                        v, e->line,
                        "echo needs a "
                        "mapping"));
                }
                {
                    static const char
                        *ek[] = {
                            "by",
                            "node",
                        };

                    if (!val_extra(
                            e, ek, 2,
                            "step", v)) {
                        return (false);
                    }
                }
                by = suite_map_find(
                    e, "by");
                en = suite_map_find(
                    e, "node");
                if (by == nullptr ||
                    en == nullptr) {
                    return (vbad(
                        v, e->line,
                        "echo needs by "
                        "plus node"));
                }
                ts =
                    suite_scalar_str(
                        by);
                if (ts == nullptr) {
                    return (vbad(
                        v, by->line,
                        "by must be weld, "
                        "ros or concord"));
                }
                {
                    int bk = EP_WELD;
                    char em[96];

                    if (strcmp(
                            ts,
                            "weld") ==
                        0) {
                        s->echo_by =
                            kEchoWeld;
                        bk = EP_WELD;
                    } else if (strcmp(
                                   ts,
                                   "ros") ==
                        0) {
                        s->echo_by =
                            kEchoRos;
                        bk = EP_ROS;
                    } else if (strcmp(
                                   ts,
                                   "concord") ==
                        0) {
                        s->echo_by = 2;
                        bk = EP_CONCORD;
                    } else {
                        return (vbad(
                            v, by->line,
                            "by must be "
                            "weld, ros or "
                            "concord"));
                    }
                    if (!kEpCap[bk]
                             .obs_weld &&
                        !kEpCap[bk]
                             .obs_ros) {
                        (void)snprintf(
                            em,
                            sizeof(em),
                            "%s cannot "
                            "observe: no "
                            "log verb",
                            ep_name(bk));
                        return (vbad(
                            v, by->line,
                            em));
                    }
                }
                {
                    int rk = EP_WELD;
                    int rn = 0;
                    char em[96];

                    if (!ep_parse_node(
                            en, f, &rk,
                            &rn)) {
                        return (vbad(
                            v, en->line,
                            "node index out "
                            "of range or bad "
                            "endpoint"));
                    }
                    if (!kEpCap[rk]
                             .relay) {
                        (void)snprintf(
                            em,
                            sizeof(em),
                            "%s cannot "
                            "relay: no relay "
                            "verb",
                            ep_name(rk));
                        return (vbad(
                            v, en->line,
                            em));
                    }
                    s->echo_node = rn;
                    s->echo_relay_kind =
                        rk;
                }
                s->has_echo = true;
                if (!needs_has(
                        f,
                        "relay.echo")) {
                    return (vbad(
                        v, e->line,
                        "echo needs "
                        "relay.echo "
                        "need"));
                }
            }
        }
    }
    s->has_generic = true;
    return (true);
}

// val_step validates one do entry.
// Each step map holds exactly one
// verb key.
static bool val_step(SuiteNode *map,
                     const ExFile *f,
                     VCtx *v, ExStep *s) {
    const char *key;
    SuiteNode *val;
    size_t line;

    if (map->kind != SUITE_MAP) {
        return (vbad(v, map->line,
                     "step must be a "
                     "mapping"));
    }
    if (map->u.map.len != 1) {
        return (vbad(
            v, map->line,
            "step must hold one verb"));
    }
    key = map->u.map.items[0].key;
    val = map->u.map.items[0].value;
    line = map->u.map.items[0]
               .key_line;
    memset(s, 0, sizeof(*s));
    s->line = line;
    if (has_ramp(key)) {
        return (ramp_bad(v, line));
    }
    if (strcmp(key, "drop") == 0 ||
        strcmp(key, "heal") == 0) {
        s->kind = key[0] == 'd'
                      ? ST_DROP
                      : ST_HEAL;
        if (val->kind != SUITE_MAP ||
            s->kind == ST_HEAL) {
            return (val_nodeset(
                val, f, v, s->idx,
                &s->nidx, &s->all));
        }
        {
            SuiteNode *a;
            SuiteNode *q;
            static const char *keep[] = {
                "nodes",    "loss",
                "delay_ms", "jitter_ms",
                "kbps",     "dup_pct",
                "dir",
            };

            if (!val_extra(
                    val, keep, 7,
                    "step", v)) {
                return (false);
            }
            a = suite_map_find(
                val, "nodes");
            if (a == nullptr) {
                return (vbad(
                    v, line,
                    "drop needs nodes"));
            }
            if (!val_nodeset(
                    a, f, v, s->idx,
                    &s->nidx,
                    &s->all)) {
                return (false);
            }
            q = suite_map_find(
                val, "loss");
            if (q != nullptr) {
                const char *t;
                double x;

                t = suite_scalar_str(
                    q);
                if (t == nullptr ||
                    !float_of(t,
                              &x) ||
                    x < 0.0 ||
                    x > 1.0) {
                    return (vbad(
                        v, q->line,
                        "loss must be "
                        "0..1"));
                }
                s->loss = x;
                s->has_fault = true;
            }
            q = suite_map_find(
                val, "delay_ms");
            if (q != nullptr) {
                long long x;

                // Int counts, quoted ok.
                if (!need_int(
                        q, &x) ||
                    x < 0 ||
                    x > kMaxMs) {
                    return (vbad(
                        v, q->line,
                        "delay_ms must "
                        "be 0..3600000"));
                }
                s->delay_ns =
                    (unsigned long long)
                        x * 1000000ULL;
                s->has_fault = true;
            }
            q = suite_map_find(
                val, "jitter_ms");
            if (q != nullptr) {
                long long x;

                // Int counts, quoted ok.
                if (!need_int(
                        q, &x) ||
                    x < 0 ||
                    x > kMaxMs) {
                    return (vbad(
                        v, q->line,
                        "jitter_ms must "
                        "be 0..3600000"));
                }
                s->jitter_ns =
                    (unsigned long long)
                        x * 1000000ULL;
                s->has_fault = true;
            }
            q = suite_map_find(
                val, "kbps");
            if (q != nullptr) {
                long long x;

                // Int counts, quoted ok.
                if (!need_int(
                        q, &x) ||
                    x < 0 ||
                    x > kMaxKbps) {
                    return (vbad(
                        v, q->line,
                        "kbps must be "
                        "0..1000000"));
                }
                s->kbps =
                    (unsigned long long)
                        x;
                s->has_fault = true;
            }
            q = suite_map_find(
                val, "dup_pct");
            if (q != nullptr) {
                long long x;

                // Int counts, quoted ok.
                if (!need_int(
                        q, &x) ||
                    x < 0 || x > 100) {
                    return (vbad(
                        v, q->line,
                        "dup_pct must "
                        "be 0..100"));
                }
                s->dup_pct = (int)x;
                s->has_fault = true;
            }
            // dir: alone stays plain:
            // only a fault key above
            // arms the qos path.
            q = suite_map_find(
                val, "dir");
            if (q != nullptr) {
                const char *t;

                t = suite_scalar_str(
                    q);
                if (t == nullptr) {
                    return (vbad(
                        v, q->line,
                        "dir must be a "
                        "string"));
                }
                if (strcmp(
                        t, "out") ==
                    0) {
                    s->dir = DIR_OUT;
                } else if (strcmp(
                               t,
                               "in") ==
                           0) {
                    s->dir = DIR_IN;
                } else if (strcmp(
                               t,
                               "both") ==
                           0) {
                    s->dir = DIR_BOTH;
                } else {
                    return (vbad(
                        v, q->line,
                        "dir must be "
                        "out, in, or "
                        "both"));
                }
            }
            return (true);
        }
    }
    if (strcmp(key, "kill") == 0) {
        SuiteNode *a;
        SuiteNode *b;
        long long x;
        const char *t;
        static const char *keep[] = {
            "node",
            "signal",
        };

        s->kind = ST_KILL;
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "kill needs a mapping"));
        }
        if (!val_extra(val, keep, 2,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "node");
        b = suite_map_find(val,
                           "signal");
        if (a == nullptr ||
            b == nullptr) {
            return (vbad(
                v, line,
                "kill needs node plus "
                "signal"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        s->node = (int)x;
        t = suite_scalar_str(b);
        if (t == nullptr) {
            return (vbad(
                v, b->line,
                "signal must be a "
                "string"));
        }
        if (strcmp(t, "KILL") == 0) {
            s->sig = SIGKILL;
        } else if (strcmp(t, "TERM") ==
                   0) {
            s->sig = SIGTERM;
        } else {
            return (vbad(
                v, b->line,
                "signal must be "
                "KILL or TERM"));
        }
        return (true);
    }
    if (strcmp(key, "restart") == 0) {
        SuiteNode *a;
        SuiteNode *b;
        long long x;
        const char *t;
        static const char *keep[] = {
            "node",
            "durability",
        };

        s->kind = ST_RESTART;
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "restart needs a "
                "mapping"));
        }
        if (!val_extra(val, keep, 2,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "node");
        b = suite_map_find(
            val, "durability");
        if (a == nullptr ||
            b == nullptr) {
            return (vbad(
                v, line,
                "restart needs node "
                "plus durability"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        s->node = (int)x;
        t = suite_scalar_str(b);
        if (t == nullptr ||
            !dur_of(t, &s->dur)) {
            return (vbad(
                v, b->line,
                "bad durability"));
        }
        if (f->sut == kWeld &&
            s->dur > kDurKeep) {
            return (vbad(
                v, b->line,
                "durability is "
                "concord-only"));
        }
        return (true);
    }
    if (strcmp(key, "move") == 0) {
        SuiteNode *a;
        SuiteNode *b;
        long long x;
        const char *t;
        static const char *keep[] = {
            "node",
            "x_m",
        };

        s->kind = ST_MOVE;
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "move needs a mapping"));
        }
        if (!val_extra(val, keep, 2,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "node");
        b = suite_map_find(val,
                           "x_m");
        if (a == nullptr ||
            b == nullptr) {
            return (vbad(
                v, line,
                "move needs node plus "
                "x_m"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        s->node = (int)x;
        t = suite_scalar_str(b);
        if (t == nullptr ||
            !float_of(t, &s->xm)) {
            return (vbad(
                v, b->line,
                "x_m must be a decimal "
                "in -1e6..1e6"));
        }
        return (true);
    }
    if (strcmp(key, "submit") == 0) {
        SuiteNode *a;
        SuiteNode *b;
        SuiteNode *c2;
        SuiteNode *q;
        long long x;
        const char *t;
        static const char *keep[] = {
            "node",  "image", "flags",
            "as",    "count", "id",
            "rate",
        };

        s->kind = ST_SUBMIT;
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "submit needs a "
                "mapping"));
        }
        if (!val_extra(val, keep, 7,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "node");
        b = suite_map_find(val,
                           "image");
        if (a == nullptr ||
            b == nullptr) {
            return (vbad(
                v, line,
                "submit needs node "
                "plus image"));
        }
        {
            int k = EP_CONCORD;
            int n = 0;
            char em[96];

            if (!ep_parse_node(
                    a, f, &k, &n)) {
                return (vbad(
                    v, a->line,
                    "node index out "
                    "of range"));
            }
            if (!kEpCap[k].submit) {
                (void)snprintf(
                    em, sizeof(em),
                    "%s cannot "
                    "submit: no submit "
                    "verb",
                    ep_name(k));
                return (vbad(
                    v, a->line, em));
            }
            s->node = n;
            s->from_kind = k;
        }
        t = suite_scalar_str(b);
        if (t == nullptr ||
            !image_ok(t)) {
            return (vbad(
                v, b->line,
                "bad image ref"));
        }
        if (!copy_text(
                s->image,
                sizeof(s->image), t)) {
            return (vbad(
                v, b->line,
                "image too long"));
        }
        c2 = suite_map_find(val,
                            "flags");
        if (c2 != nullptr) {
            t = suite_scalar_str(c2);
            if (t == nullptr) {
                return (vbad(
                    v, c2->line,
                    "flags must be a "
                    "string"));
            }
            if (!flags_ok(t)) {
                return (vbad(
                    v, c2->line,
                    "bad flags"));
            }
            if (!copy_text(
                    s->flags,
                    sizeof(s->flags),
                    t)) {
                return (vbad(
                    v, c2->line,
                    "flags too long"));
            }
            s->has_flags = true;
        }
        q = suite_map_find(val,
                           "as");
        if (q != nullptr) {
            t = suite_scalar_str(q);
            if (t == nullptr ||
                !wl_name_ok(t)) {
                return (vbad(
                    v, q->line,
                    "bad workload "
                    "name"));
            }
            if (!copy_text(
                    s->as_name,
                    sizeof(s->as_name),
                    t)) {
                return (vbad(
                    v, q->line,
                    "workload name too "
                    "long"));
            }
            s->has_as = true;
        }
        q = suite_map_find(val,
                           "count");
        if (q != nullptr) {
            // Int counts, quoted ok.
            if (!need_int(q, &x) ||
                x < 1 ||
                x > kMaxCount) {
                return (vbad(
                    v, q->line,
                    "count must be "
                    "1..100"));
            }
            if (!s->has_as) {
                return (vbad(
                    v, q->line,
                    "count needs as:"));
            }
            if (strlen(s->as_name) +
                    4 >
                (size_t)kWlLen) {
                return (vbad(
                    v, q->line,
                    "workload name too "
                    "long for count"));
            }
            s->count = x;
            s->has_count = true;
        }
        q = suite_map_find(val,
                           "id");
        if (q != nullptr) {
            t = suite_scalar_str(q);
            if (t == nullptr ||
                t[0] == '\0') {
                return (vbad(
                    v, q->line,
                    "id must be a "
                    "string"));
            }
            if (!copy_text(
                    s->fixed_id,
                    sizeof(
                        s->fixed_id),
                    t)) {
                return (vbad(
                    v, q->line,
                    "id too long"));
            }
            s->has_id = true;
        }
        q = suite_map_find(val,
                           "rate");
        if (q != nullptr) {
            // Int counts, quoted ok.
            if (!need_int(q, &x) ||
                x < 1 ||
                x > kMaxRate) {
                return (vbad(
                    v, q->line,
                    "rate must be "
                    "1..1000000"));
            }
            s->rate = x;
            s->has_rate = true;
        }
        return (true);
    }
    if (strcmp(key, "stop") == 0) {
        SuiteNode *a;
        static const char *keep[] = {
            "workload",
        };

        s->kind = ST_STOP;
        if (f->sut != kConcord) {
            return (vbad(
                v, line,
                "stop is "
                "concord-only"));
        }
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "stop needs a mapping"));
        }
        if (!val_extra(val, keep, 1,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "workload");
        if (a != nullptr) {
            const char *t;

            t = suite_scalar_str(a);
            if (t == nullptr ||
                !wl_name_ok(t)) {
                return (vbad(
                    v, a->line,
                    "bad workload "
                    "name"));
            }
            if (!copy_text(
                    s->wl,
                    sizeof(s->wl),
                    t)) {
                return (vbad(
                    v, a->line,
                    "workload name too "
                    "long"));
            }
        } else {
            (void)strcpy(s->wl,
                         "submitted");
        }
        return (true);
    }
    if (strcmp(key, "pub") == 0) {
        SuiteNode *a;
        SuiteNode *b;
        SuiteNode *c2;
        SuiteNode *q;
        long long x;
        const char *t;
        static const char *keep[] = {
            "from",
            "text",
            "topic",
            "rate",
        };

        s->kind = ST_PUB;
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "pub needs a mapping"));
        }
        if (!val_extra(val, keep, 4,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "from");
        b = suite_map_find(val,
                           "text");
        if (a == nullptr ||
            b == nullptr) {
            return (vbad(
                v, line,
                "pub needs from plus "
                "text"));
        }
        {
            int k = EP_WELD;
            int n = 0;
            char em[96];

            if (!ep_parse_node(
                    a, f, &k, &n)) {
                return (vbad(
                    v, a->line,
                    "node index out "
                    "of range"));
            }
            if (!kEpCap[k].pub_weld) {
                (void)snprintf(
                    em, sizeof(em),
                    "%s cannot "
                    "publish: no pub "
                    "verb",
                    ep_name(k));
                return (vbad(
                    v, a->line, em));
            }
            s->node = n;
            s->from_kind = k;
        }
        t = suite_scalar_str(b);
        if (t == nullptr ||
            t[0] == '\0') {
            return (vbad(
                v, b->line,
                "text must be a "
                "string"));
        }
        if (!copy_text(
                s->text,
                sizeof(s->text), t)) {
            return (vbad(
                v, b->line,
                "text too long"));
        }
        c2 = suite_map_find(val,
                            "topic");
        if (c2 != nullptr) {
            t = suite_scalar_str(c2);
            if (t == nullptr ||
                !topic_ok(t)) {
                return (vbad(
                    v, c2->line,
                    "bad topic"));
            }
            if (!copy_text(
                    s->topic,
                    sizeof(s->topic),
                    t)) {
                return (vbad(
                    v, c2->line,
                    "topic too long"));
            }
            s->has_topic = true;
        }
        q = suite_map_find(val,
                           "rate");
        if (q != nullptr) {
            // Int counts, quoted ok.
            if (!need_int(q, &x) ||
                x < 1 ||
                x > kMaxRate) {
                return (vbad(
                    v, q->line,
                    "rate must be "
                    "1..1000000"));
            }
            s->rate = x;
            s->has_rate = true;
        }
        return (true);
    }
    if (strcmp(key, "benchmark") ==
        0) {
        SuiteNode *q;
        long long x;
        size_t bi;
        static const char *keep[] = {
            "from",   "text",  "topic",
            "on",     "node",  "image",
            "flags",  "as",    "rate",
            "count",  "duration_s",
            "warmup", "echo",
            "payload_bytes",
            "ramp",
        };

        s->kind = ST_BENCH;
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "benchmark needs a "
                "mapping"));
        }
        if (suite_map_find(val,
                           "send") !=
                nullptr ||
            suite_map_find(val,
                           "expect") !=
                nullptr) {
            return (val_bench_generic(
                val, f, v, s, line));
        }
        for (bi = 0; bi <
             val->u.map.len; bi++) {
            const char *rk;

            rk = val->u.map.items[bi]
                     .key;
            if (strcmp(rk, "ramp") ==
                0) {
                continue;
            }
            if (has_ramp(rk)) {
                return (ramp_bad(
                    v,
                    val->u.map.items[bi]
                        .key_line));
            }
        }
        if (!val_extra(val, keep, 15,
                       "step", v)) {
            return (false);
        }
        {
            SuiteNode *rm;

            rm = suite_map_find(
                val, "ramp");
            if (rm != nullptr) {
                bool is_c = false;
                bool is_w = false;
                long long fam = 0;

                if (suite_map_find(
                        val, "node") !=
                        nullptr ||
                    suite_map_find(
                        val,
                        "image") !=
                        nullptr ||
                    suite_map_find(
                        val, "as") !=
                        nullptr ||
                    suite_map_find(
                        val,
                        "flags") !=
                        nullptr) {
                    is_c = true;
                }
                if (suite_map_find(
                        val, "from") !=
                        nullptr ||
                    suite_map_find(
                        val, "on") !=
                        nullptr ||
                    suite_map_find(
                        val, "text") !=
                        nullptr ||
                    suite_map_find(
                        val,
                        "topic") !=
                        nullptr ||
                    suite_map_find(
                        val, "echo") !=
                        nullptr ||
                    suite_map_find(
                        val,
                        "payload_bytes") !=
                        nullptr) {
                    is_w = true;
                }
                if (is_c && is_w) {
                    return (vbad(
                        v, line,
                        "benchmark "
                        "mixes weld and "
                        "concord keys"));
                }
                fam = is_c
                    ? kBenchConc
                    : kBenchWeld;
                if (suite_map_find(
                        val, "rate") !=
                    nullptr) {
                    return (vbad(
                        v, line,
                        "ramp forbids "
                        "rate: use "
                        "ramp start"));
                }
                if (suite_map_find(
                        val, "count") !=
                    nullptr) {
                    return (vbad(
                        v, line,
                        "ramp forbids "
                        "count: use "
                        "ramp steps"));
                }
                if (suite_map_find(
                        val,
                        "duration_s") !=
                    nullptr) {
                    return (vbad(
                        v, line,
                        "ramp forbids "
                        "duration_s"));
                }
                if (suite_map_find(
                        val,
                        "warmup") !=
                    nullptr) {
                    return (vbad(
                        v, line,
                        "ramp forbids "
                        "warmup"));
                }
                if (!val_ramp(
                        rm, v, s,
                        fam)) {
                    return (false);
                }
            }
        }
        if (!s->has_ramp) {
            q = suite_map_find(val,
                               "rate");
            if (q == nullptr) {
                return (vbad(
                    v, line,
                    "benchmark needs "
                    "rate"));
            }
            // Int counts, quoted ok.
            if (!need_int(q, &x) ||
                x < 1 || x > kMaxRate) {
                return (vbad(
                    v, q->line,
                    "rate must be "
                    "1..1000000"));
            }
            s->rate = x;
            s->has_rate = true;
        }
        if (!s->has_ramp) {
            q = suite_map_find(val,
                               "count");
            if (q != nullptr) {
                long long cap = 0;
                bool is_c = false;

                if (suite_map_find(
                        val, "node") !=
                        nullptr ||
                    suite_map_find(
                        val,
                        "image") !=
                        nullptr ||
                    suite_map_find(
                        val, "as") !=
                        nullptr) {
                    is_c = true;
                }
                cap = is_c
                    ? kBenchConc
                    : kBenchWeld;
                // Int counts, quoted
                // ok.
                if (!need_int(
                        q, &x) ||
                    x < 1 || x > cap) {
                    return (vbad(
                        v, q->line,
                        "benchmark "
                        "count out of "
                        "range"));
                }
                s->count = x;
                s->has_count = true;
            }
            q = suite_map_find(
                val, "duration_s");
            if (q != nullptr) {
                // Int counts, quoted
                // ok.
                if (!need_int(
                        q, &x) ||
                    x < 1 ||
                    x > kMaxTimeoutS) {
                    return (vbad(
                        v, q->line,
                        "duration_s "
                        "must be "
                        "1..3600"));
                }
                s->duration_s = x;
                s->has_dur = true;
            }
            if (!s->has_count &&
                !s->has_dur) {
                return (vbad(
                    v, line,
                    "benchmark needs "
                    "count or "
                    "duration_s"));
            }
            q = suite_map_find(val,
                               "warmup");
            if (q != nullptr) {
                // Int counts, quoted
                // ok.
                if (!need_int(
                        q, &x) ||
                    x < 0 ||
                    x > kBenchWeld) {
                    return (vbad(
                        v, q->line,
                        "warmup must be "
                        "0..1000"));
                }
                s->warmup = x;
            }
            if (s->has_count &&
                s->warmup >= s->count) {
                return (vbad(
                    v, line,
                    "warmup must be "
                    "below count"));
            }
            if (s->has_dur) {
                unsigned long long prod;
                long long cap = 0;
                bool is_c = false;

                if (suite_map_find(
                        val, "node") !=
                        nullptr ||
                    suite_map_find(
                        val,
                        "image") !=
                        nullptr ||
                    suite_map_find(
                        val, "as") !=
                        nullptr) {
                    is_c = true;
                }
                cap = is_c
                    ? kBenchConc
                    : kBenchWeld;
                prod =
                    (unsigned long long)
                        s->rate *
                    (unsigned long long)
                        s->duration_s;
                // Rate 1..1e6 and dur
                // 1..3600 fit, no wrap.
                if (prod >
                    (unsigned long long)
                        cap) {
                    return (vbad(
                        v, line,
                        "benchmark too "
                        "large"));
                }
            }
        }
        q = suite_map_find(
            val, "payload_bytes");
        if (q != nullptr) {
            // Int counts, quoted ok.
            // 0 means use text.
            if (!need_int(q, &x) ||
                x < 0 ||
                x > kBenchBytes) {
                return (vbad(
                    v, q->line,
                    "payload_bytes "
                    "must be "
                    "0..65536"));
            }
            if (x > 0) {
                s->pay_bytes =
                    (unsigned long long)
                        x;
                s->has_pay = true;
            }
        }
        {
            bool is_c = false;
            bool is_w = false;

            if (suite_map_find(
                    val, "node") !=
                nullptr ||
                suite_map_find(
                    val, "image") !=
                    nullptr ||
                suite_map_find(
                    val, "as") !=
                    nullptr ||
                suite_map_find(
                    val, "flags") !=
                    nullptr) {
                is_c = true;
            }
            if (suite_map_find(
                    val, "from") !=
                nullptr ||
                suite_map_find(
                    val, "on") !=
                    nullptr ||
                suite_map_find(
                    val, "text") !=
                    nullptr ||
                suite_map_find(
                    val, "topic") !=
                    nullptr ||
                suite_map_find(
                    val, "echo") !=
                    nullptr ||
                suite_map_find(
                    val,
                    "payload_bytes") !=
                nullptr) {
                is_w = true;
            }
            if (is_c && is_w) {
                return (vbad(
                    v, line,
                    "benchmark mixes "
                    "weld and concord "
                    "keys"));
            }
            if (is_w) {
            SuiteNode *a;
            SuiteNode *b;
            SuiteNode *w;
            SuiteNode *c2;
            const char *t;

            if (suite_map_find(
                    val, "node") !=
                nullptr) {
                return (vbad(
                    v, line,
                    "node is "
                    "concord-only"));
            }
            if (suite_map_find(
                    val, "image") !=
                nullptr) {
                return (vbad(
                    v, line,
                    "image is "
                    "concord-only"));
            }
            if (suite_map_find(
                    val, "flags") !=
                nullptr) {
                return (vbad(
                    v, line,
                    "flags is "
                    "concord-only"));
            }
            if (suite_map_find(
                    val, "as") !=
                nullptr) {
                return (vbad(
                    v, line,
                    "as is "
                    "concord-only"));
            }
            a = suite_map_find(val,
                               "from");
            b = suite_map_find(val,
                               "text");
            w = suite_map_find(val,
                               "on");
            if (a == nullptr ||
                w == nullptr) {
                return (vbad(
                    v, line,
                    "benchmark needs "
                    "from plus text "
                    "plus on"));
            }
            if (b == nullptr &&
                !s->has_pay) {
                return (vbad(
                    v, line,
                    "benchmark needs "
                    "from plus text "
                    "plus on"));
            }
            {
                int fk = EP_WELD;
                int fn = 0;
                char em[96];

                if (!ep_parse_node(
                        a, f, &fk,
                        &fn)) {
                    return (vbad(
                        v, a->line,
                        "node index out "
                        "of range or bad "
                        "endpoint"));
                }
                if (!kEpCap[fk].pub_weld &&
                    !kEpCap[fk].pub_ros) {
                    (void)snprintf(
                        em, sizeof(em),
                        "%s cannot "
                        "publish: no pub "
                        "verb",
                        ep_name(fk));
                    return (vbad(
                        v, a->line, em));
                }
                if (fk == EP_ROS &&
                    !needs_has(
                        f, "ros.pub")) {
                    return (vbad(
                        v, a->line,
                        "ros publish "
                        "needs ros.pub "
                        "need"));
                }
                s->node = fn;
                s->from_kind = fk;
            }
            if (b != nullptr) {
                t = suite_scalar_str(
                    b);
                if (t == nullptr ||
                    t[0] == '\0') {
                    return (vbad(
                        v, b->line,
                        "text must be "
                        "a string"));
                }
                if (old_tag(t)) {
                    return (vbad(
                        v, b->line,
                        "old bench tag: "
                        "use bench#seq#"
                        "send_ns#text"));
                }
                if (!copy_text(
                        s->text,
                        sizeof(s->text),
                        t)) {
                    return (vbad(
                        v, b->line,
                        "text too "
                        "long"));
                }
            } else {
                s->text[0] = '\0';
            }
            c2 = suite_map_find(
                val, "topic");
            if (c2 != nullptr) {
                t = suite_scalar_str(
                    c2);
                if (t == nullptr ||
                    !topic_ok(t)) {
                    return (vbad(
                        v, c2->line,
                        "bad topic"));
                }
                if (!copy_text(
                        s->topic,
                        sizeof(s->topic),
                        t)) {
                    return (vbad(
                        v, c2->line,
                        "topic too "
                        "long"));
                }
                s->has_topic = true;
            }
            {
                SuiteNode *wi = w;
                int ok = EP_WELD;
                int on = 0;
                char em[96];

                // One scalar index;
                // a lone list item
                // counts as scalar.
                if (w->kind ==
                        SUITE_LIST &&
                    suite_list_len(
                        w) == 1) {
                    wi = suite_list_at(
                        w, 0);
                } else if (
                    w->kind !=
                    SUITE_SCALAR) {
                    return (vbad(
                        v, w->line,
                        "benchmark on "
                        "must be a "
                        "scalar node "
                        "index"));
                }
                if (!ep_parse_node(
                        wi, f, &ok,
                        &on)) {
                    return (vbad(
                        v, wi->line,
                        "node index out "
                        "of range or bad "
                        "endpoint"));
                }
                if (!kEpCap[ok].obs_weld &&
                    !kEpCap[ok].obs_ros) {
                    (void)snprintf(
                        em, sizeof(em),
                        "%s cannot "
                        "observe: no log "
                        "verb",
                        ep_name(ok));
                    return (vbad(
                        v, wi->line,
                        em));
                }
                s->obs = on;
                s->on_kind = ok;
            }
            {
                SuiteNode *e;
                SuiteNode *by;
                SuiteNode *en;

                e = suite_map_find(
                    val, "echo");
                if (e != nullptr) {
                    const char *ts;

                    if (e->kind !=
                        SUITE_MAP) {
                        return (vbad(
                            v, e->line,
                            "echo needs "
                            "a mapping"));
                    }
                    {
                        static const
                            char *ek[] = {
                                "by",
                                "node",
                            };

                        if (!val_extra(
                                e, ek, 2,
                                "step",
                                v)) {
                            return (
                                false);
                        }
                    }
                    by = suite_map_find(
                        e, "by");
                    en = suite_map_find(
                        e, "node");
                    if (by == nullptr ||
                        en == nullptr) {
                        return (vbad(
                            v, e->line,
                            "echo needs "
                            "by plus "
                            "node"));
                    }
                    ts =
                        suite_scalar_str(
                            by);
                    if (ts ==
                        nullptr) {
                        return (vbad(
                            v, by->line,
                            "by must be "
                            "weld, ros or "
                            "concord"));
                    }
                    {
                        int bk = EP_WELD;
                        char em[96];

                        if (strcmp(
                                ts,
                                "weld") ==
                            0) {
                            s->echo_by =
                                kEchoWeld;
                            bk = EP_WELD;
                        } else if (
                            strcmp(
                                ts,
                                "ros") ==
                            0) {
                            s->echo_by =
                                kEchoRos;
                            bk = EP_ROS;
                        } else if (
                            strcmp(
                                ts,
                                "concord") ==
                            0) {
                            s->echo_by = 2;
                            bk = EP_CONCORD;
                        } else {
                            return (vbad(
                                v, by->line,
                                "by must be "
                                "weld, ros or "
                                "concord"));
                        }
                        if (!kEpCap[bk]
                                 .obs_weld &&
                            !kEpCap[bk]
                                 .obs_ros) {
                            (void)snprintf(
                                em,
                                sizeof(em),
                                "%s cannot "
                                "observe: no "
                                "log verb",
                                ep_name(
                                    bk));
                            return (vbad(
                                v, by->line,
                                em));
                        }
                    }
                    {
                        int rk = EP_WELD;
                        int rn = 0;
                        char em[96];

                        if (!ep_parse_node(
                                en, f, &rk,
                                &rn)) {
                            return (vbad(
                                v, en->line,
                                "node index "
                                "out of range "
                                "or bad "
                                "endpoint"));
                        }
                        if (!kEpCap[rk]
                                 .relay) {
                            (void)snprintf(
                                em,
                                sizeof(em),
                                "%s cannot "
                                "relay: no "
                                "relay verb",
                                ep_name(
                                    rk));
                            return (vbad(
                                v, en->line,
                                em));
                        }
                        s->echo_node = rn;
                        s->echo_relay_kind =
                            rk;
                    }
                    s->has_echo =
                        true;
                    if (!needs_has(
                            f,
                            "relay.echo")) {
                        return (vbad(
                            v, e->line,
                            "echo needs "
                            "relay.echo "
                            "need"));
                    }
                }
            }
            return (true);
        }
        {
            SuiteNode *a;
            SuiteNode *b;
            SuiteNode *c2;
            const char *t;

            if (suite_map_find(
                    val, "from") !=
                nullptr) {
                return (vbad(
                    v, line,
                    "weld from needs "
                    "pub verb: concord "
                    "cannot publish"));
            }
            if (suite_map_find(
                    val, "text") !=
                nullptr) {
                return (vbad(
                    v, line,
                    "weld text needs "
                    "pub verb: concord "
                    "cannot publish"));
            }
            if (suite_map_find(
                    val, "topic") !=
                nullptr) {
                return (vbad(
                    v, line,
                    "weld topic needs "
                    "pub verb: concord "
                    "cannot publish"));
            }
            if (suite_map_find(
                    val, "on") !=
                nullptr) {
                return (vbad(
                    v, line,
                    "weld on needs log "
                    "verb: concord "
                    "cannot observe"));
            }
            if (suite_map_find(
                    val, "echo") !=
                nullptr) {
                return (vbad(
                    v, line,
                    "weld echo needs "
                    "relay verb: "
                    "concord cannot "
                    "relay"));
            }
            if (suite_map_find(
                    val,
                    "payload_bytes") !=
                nullptr) {
                return (vbad(
                    v, line,
                    "weld payload needs "
                    "pub verb: concord "
                    "cannot publish"));
            }
            a = suite_map_find(val,
                               "node");
            b = suite_map_find(val,
                               "image");
            c2 = suite_map_find(val,
                                "as");
            if (a == nullptr ||
                b == nullptr ||
                c2 == nullptr) {
                return (vbad(
                    v, line,
                    "benchmark needs "
                    "node plus image "
                    "plus as"));
            }
            {
                int nk = EP_CONCORD;
                int nn = 0;
                char em[96];

                if (!ep_parse_node(
                        a, f, &nk,
                        &nn)) {
                    return (vbad(
                        v, a->line,
                        "node index out "
                        "of range"));
                }
                if (!kEpCap[nk]
                         .submit) {
                    (void)snprintf(
                        em, sizeof(em),
                        "%s cannot "
                        "submit: no "
                        "submit verb",
                        ep_name(nk));
                    return (vbad(
                        v, a->line, em));
                }
                s->node = nn;
                s->from_kind = nk;
            }
            t = suite_scalar_str(b);
            if (t == nullptr ||
                !image_ok(t)) {
                return (vbad(
                    v, b->line,
                    "bad image ref"));
            }
            if (!copy_text(
                    s->image,
                    sizeof(s->image),
                    t)) {
                return (vbad(
                    v, b->line,
                    "image too long"));
            }
            t = suite_scalar_str(c2);
            if (t == nullptr ||
                !wl_name_ok(t)) {
                return (vbad(
                    v, c2->line,
                    "bad workload "
                    "name"));
            }
            if (!copy_text(
                    s->as_name,
                    sizeof(s->as_name),
                    t)) {
                return (vbad(
                    v, c2->line,
                    "workload name "
                    "too long"));
            }
            s->has_as = true;
            if (s->has_count &&
                strlen(s->as_name) +
                        4 >
                    (size_t)kWlLen) {
                return (vbad(
                    v, c2->line,
                    "workload name "
                    "too long for "
                    "count"));
            }
            if (s->has_dur &&
                strlen(s->as_name) +
                        4 >
                    (size_t)kWlLen) {
                return (vbad(
                    v, c2->line,
                    "workload name "
                    "too long for "
                    "count"));
            }
            if (s->has_ramp &&
                strlen(s->as_name) +
                        4 >
                    (size_t)kWlLen) {
                return (vbad(
                    v, c2->line,
                    "workload name "
                    "too long for "
                    "count"));
            }
            q = suite_map_find(val,
                               "flags");
            if (q != nullptr) {
                t = suite_scalar_str(
                    q);
                if (t == nullptr) {
                    return (vbad(
                        v, q->line,
                        "flags must "
                        "be a string"));
                }
                if (!flags_ok(t)) {
                    return (vbad(
                        v, q->line,
                        "bad flags"));
                }
                if (!copy_text(
                        s->flags,
                        sizeof(s->flags),
                        t)) {
                    return (vbad(
                        v, q->line,
                        "flags too "
                        "long"));
                }
                s->has_flags = true;
            }
            return (true);
        }
        }
    }
    if (strcmp(key, "ros_pub") == 0) {
        SuiteNode *a;
        SuiteNode *b;
        SuiteNode *c2;
        SuiteNode *q;
        long long x;
        const char *t;
        size_t bi;
        static const char *keep[] = {
            "from",   "topic", "text",
            "payload_bytes",
            "rate",   "count",
            "duration_s",
        };

        s->kind = ST_ROSPUB;
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "ros_pub needs a "
                "mapping"));
        }
        for (bi = 0; bi <
             val->u.map.len; bi++) {
            if (has_ramp(
                    val->u.map.items[bi]
                        .key)) {
                return (ramp_bad(
                    v,
                    val->u.map.items[bi]
                        .key_line));
            }
        }
        if (!val_extra(val, keep, 7,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "from");
        c2 = suite_map_find(val,
                            "topic");
        if (a == nullptr ||
            c2 == nullptr) {
            return (vbad(
                v, line,
                "ros_pub needs "
                "from plus topic"));
        }
        {
            int k = EP_WELD;
            int n = 0;
            char em[96];

            if (!ep_parse_node(
                    a, f, &k, &n)) {
                return (vbad(
                    v, a->line,
                    "node index out "
                    "of range or bad "
                    "endpoint"));
            }
            if (k == EP_CONCORD) {
                (void)snprintf(
                    em, sizeof(em),
                    "%s cannot "
                    "publish: no pub "
                    "verb",
                    ep_name(k));
                return (vbad(
                    v, a->line, em));
            }
            s->node = n;
            s->from_kind = EP_ROS;
            if (!needs_has(
                    f, "ros.pub")) {
                return (vbad(
                    v, line,
                    "ros_pub needs "
                    "ros.pub need"));
            }
        }
        t = suite_scalar_str(c2);
        if (t == nullptr ||
            !topic_ok(t)) {
            return (vbad(
                v, c2->line,
                "bad topic"));
        }
        if (!copy_text(
                s->topic,
                sizeof(s->topic),
                t)) {
            return (vbad(
                v, c2->line,
                "topic too long"));
        }
        s->has_topic = true;
        b = suite_map_find(val,
                           "text");
        q = suite_map_find(
            val, "payload_bytes");
        if (q != nullptr) {
            if (!need_int(q, &x) ||
                x < 0 ||
                x > kBenchBytes) {
                return (vbad(
                    v, q->line,
                    "payload_bytes "
                    "must be "
                    "0..65536"));
            }
            if (x > 0) {
                s->pay_bytes =
                    (unsigned long long)
                        x;
                s->has_pay = true;
            }
        }
        if (b != nullptr) {
            t = suite_scalar_str(b);
            if (t == nullptr ||
                t[0] == '\0') {
                return (vbad(
                    v, b->line,
                    "text must be "
                    "a string"));
            }
            if (old_tag(t)) {
                return (vbad(
                    v, b->line,
                    "old bench tag: "
                    "use bench#seq#"
                    "send_ns#text"));
            }
            if (!copy_text(
                    s->text,
                    sizeof(s->text),
                    t)) {
                return (vbad(
                    v, b->line,
                    "text too long"));
            }
        } else if (!s->has_pay) {
            return (vbad(
                v, line,
                "ros_pub needs "
                "text or "
                "payload_bytes"));
        } else {
            s->text[0] = '\0';
        }
        q = suite_map_find(val,
                           "rate");
        if (q == nullptr) {
            return (vbad(
                v, line,
                "ros_pub needs "
                "rate"));
        }
        if (!need_int(q, &x) ||
            x < 1 || x > kMaxRate) {
            return (vbad(
                v, q->line,
                "rate must be "
                "1..1000000"));
        }
        s->rate = x;
        s->has_rate = true;
        q = suite_map_find(val,
                           "count");
        if (q != nullptr) {
            if (!need_int(q, &x) ||
                x < 1 ||
                x > kBenchWeld) {
                return (vbad(
                    v, q->line,
                    "ros_pub count "
                    "out of range"));
            }
            s->count = x;
            s->has_count = true;
        }
        q = suite_map_find(
            val, "duration_s");
        if (q != nullptr) {
            if (!need_int(q, &x) ||
                x < 1 ||
                x > kMaxTimeoutS) {
                return (vbad(
                    v, q->line,
                    "duration_s "
                    "must be "
                    "1..3600"));
            }
            s->duration_s = x;
            s->has_dur = true;
        }
        if (!s->has_count &&
            !s->has_dur) {
            return (vbad(
                v, line,
                "ros_pub needs "
                "count or "
                "duration_s"));
        }
        if (s->has_dur) {
            unsigned long long prod;

            prod = (unsigned long long)
                       s->rate *
                   (unsigned long long)
                       s->duration_s;
            if (prod >
                (unsigned long long)
                    kBenchWeld) {
                return (vbad(
                    v, line,
                    "ros_pub too "
                    "large"));
            }
        }
        if (!needs_has(f,
                       "ros.pub")) {
            return (vbad(
                v, line,
                "ros_pub needs "
                "ros.pub need"));
        }
        return (true);
    }
    if (strcmp(key, "slow") == 0) {
        SuiteNode *a;
        SuiteNode *b;
        long long x;
        static const char *keep[] = {
            "nodes",
            "secs",
        };

        s->kind = ST_SLOW;
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "slow needs a mapping"));
        }
        if (!val_extra(val, keep, 2,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "nodes");
        b = suite_map_find(val,
                           "secs");
        if (a == nullptr ||
            b == nullptr) {
            return (vbad(
                v, line,
                "slow needs nodes "
                "plus secs"));
        }
        if (!val_nodeset(
                a, f, v, s->idx,
                &s->nidx, &s->all)) {
            return (false);
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(b, &x) ||
            x < 1 ||
            x > kMaxTimeoutS) {
            return (vbad(
                v, b->line,
                "slow secs must be "
                "1..3600"));
        }
        s->slow_secs = x;
        return (true);
    }
    if (strcmp(key, "update") == 0) {
        SuiteNode *a;
        SuiteNode *b;
        const char *t;
        static const char *keep[] = {
            "workload",
            "image",
        };

        s->kind = ST_UPDATE;
        if (f->sut != kConcord) {
            return (vbad(
                v, line,
                "update is "
                "concord-only"));
        }
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "update needs a "
                "mapping"));
        }
        if (!val_extra(val, keep, 2,
                       "step", v)) {
            return (false);
        }
        b = suite_map_find(val,
                           "image");
        if (b == nullptr) {
            return (vbad(
                v, line,
                "update needs image"));
        }
        t = suite_scalar_str(b);
        if (t == nullptr ||
            !image_ok(t)) {
            return (vbad(
                v, b->line,
                "bad image ref"));
        }
        if (!copy_text(
                s->image,
                sizeof(s->image), t)) {
            return (vbad(
                v, b->line,
                "image too long"));
        }
        a = suite_map_find(val,
                           "workload");
        if (a != nullptr) {
            t = suite_scalar_str(a);
            if (t == nullptr ||
                !wl_name_ok(t)) {
                return (vbad(
                    v, a->line,
                    "bad workload "
                    "name"));
            }
            if (!copy_text(
                    s->wl,
                    sizeof(s->wl),
                    t)) {
                return (vbad(
                    v, a->line,
                    "workload name too "
                    "long"));
            }
        } else {
            (void)strcpy(s->wl,
                         "submitted");
        }
        return (true);
    }
    if (strcmp(key, "replay") == 0) {
        SuiteNode *a;
        SuiteNode *b;
        SuiteNode *c2;
        long long x;
        const char *t;
        static const char *keep[] = {
            "file",
            "topic",
            "from",
        };

        s->kind = ST_REPLAY;
        if (f->sut != kWeld) {
            return (vbad(
                v, line,
                "replay is weld-only"));
        }
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "replay needs a "
                "mapping"));
        }
        if (!val_extra(val, keep, 3,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "file");
        if (a == nullptr) {
            return (vbad(
                v, line,
                "replay needs file"));
        }
        t = suite_scalar_str(a);
        if (t == nullptr ||
            t[0] == '\0') {
            return (vbad(
                v, a->line,
                "file must be a "
                "string"));
        }
        if (!copy_text(
                s->file,
                sizeof(s->file), t)) {
            return (vbad(
                v, a->line,
                "file too long"));
        }
        b = suite_map_find(val,
                           "from");
        s->node = 0;
        if (b != nullptr) {
            // Int counts, quoted ok.
            if (!need_int(b, &x) ||
                x < 0 ||
                x >= f->nodes) {
                return (vbad(
                    v, b->line,
                    "node index out of "
                    "range"));
            }
            s->node = (int)x;
        }
        c2 = suite_map_find(val,
                            "topic");
        if (c2 != nullptr) {
            t = suite_scalar_str(c2);
            if (t == nullptr ||
                !topic_ok(t)) {
                return (vbad(
                    v, c2->line,
                    "bad topic"));
            }
            if (!copy_text(
                    s->topic,
                    sizeof(s->topic),
                    t)) {
                return (vbad(
                    v, c2->line,
                    "topic too long"));
            }
            s->has_topic = true;
        }
        return (true);
    }
    if (strcmp(key, "diskfill") ==
        0) {
        SuiteNode *a;
        SuiteNode *b;
        long long x;
        long long m;
        static const char *keep[] = {
            "node",
            "mb",
        };

        s->kind = ST_DISKFILL;
        if (f->sut != kConcord) {
            return (vbad(
                v, line,
                "diskfill is "
                "concord-only"));
        }
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "diskfill needs a "
                "mapping"));
        }
        if (!val_extra(val, keep, 2,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "node");
        b = suite_map_find(val,
                           "mb");
        if (a == nullptr ||
            b == nullptr) {
            return (vbad(
                v, line,
                "diskfill needs node "
                "plus mb"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        s->node = (int)x;
        // Int-shaped counts, quoted ok.
        if (!need_int(b, &m) ||
            m < 0 || m > kMaxMb) {
            return (vbad(
                v, b->line,
                "mb must be "
                "0..65536"));
        }
        s->mb = (unsigned long long)m;
        return (true);
    }
    if (strcmp(key, "diskreadonly") ==
        0) {
        SuiteNode *a;
        SuiteNode *b;
        long long x;
        const char *t;
        static const char *keep[] = {
            "node",
            "state",
        };

        s->kind = ST_DISKRO;
        if (f->sut != kConcord) {
            return (vbad(
                v, line,
                "diskreadonly is "
                "concord-only"));
        }
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "diskreadonly needs "
                "a mapping"));
        }
        if (!val_extra(val, keep, 2,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "node");
        b = suite_map_find(
            val, "state");
        if (a == nullptr ||
            b == nullptr) {
            return (vbad(
                v, line,
                "diskreadonly needs "
                "node plus state"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        s->node = (int)x;
        t = suite_scalar_str(b);
        if (t == nullptr) {
            return (vbad(
                v, b->line,
                "state must be a "
                "string"));
        }
        if (strcmp(t, "on") == 0) {
            s->ro = true;
        } else if (strcmp(t, "off") ==
                   0) {
            s->ro = false;
        } else {
            return (vbad(
                v, b->line,
                "state must be on "
                "or off"));
        }
        return (true);
    }
    if (strcmp(key, "journalcorrupt") ==
        0) {
        SuiteNode *a;
        long long x;
        static const char *keep[] = {
            "node",
        };

        s->kind = ST_JCORRUPT;
        if (f->sut != kConcord) {
            return (vbad(
                v, line,
                "journalcorrupt is "
                "concord-only"));
        }
        if (val->kind != SUITE_MAP) {
            return (vbad(
                v, line,
                "journalcorrupt "
                "needs a mapping"));
        }
        if (!val_extra(val, keep, 1,
                       "step", v)) {
            return (false);
        }
        a = suite_map_find(val,
                           "node");
        if (a == nullptr) {
            return (vbad(
                v, line,
                "journalcorrupt "
                "needs node"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        s->node = (int)x;
        return (true);
    }
    return (vunknown(v, line, "step",
                     key));
}

// val_phase validates one phase map.
static bool val_phase(SuiteNode *map,
                      const ExFile *f,
                      VCtx *v,
                      ExPhase *p) {
    SuiteNode *nd;
    const char *s;
    size_t bi;
    static const char *keep[] = {
        "name",    "do",      "until",
        "sustain", "hold_s",
        "timeout_s", "multicast",
    };
    SuiteNode *u;
    SuiteNode *t;

    if (map->kind != SUITE_MAP) {
        return (vbad(v, map->line,
                     "phase must be a "
                     "mapping"));
    }
    for (bi = 0; bi <
         map->u.map.len; bi++) {
        if (has_ramp(
                map->u.map.items[bi]
                    .key)) {
            return (ramp_bad(
                v,
                map->u.map.items[bi]
                    .key_line));
        }
    }
    if (!val_extra(map, keep, 7,
                   "phase key", v)) {
        return (false);
    }
    memset(p, 0, sizeof(*p));
    p->line = map->line;
    nd = suite_map_find(map, "name");
    if (nd == nullptr) {
        return (vbad(v, map->line,
                     "missing phase "
                     "key 'name'"));
    }
    s = suite_scalar_str(nd);
    if (s == nullptr ||
        s[0] == '\0') {
        return (vbad(v, nd->line,
                     "phase name must be "
                     "a string"));
    }
    if (!copy_text(p->name,
                   sizeof(p->name),
                   s)) {
        return (vbad(v, nd->line,
                     "phase name too "
                     "long"));
    }
    nd = suite_map_find(map, "do");
    if (nd != nullptr) {
        size_t i;

        if (nd->kind != SUITE_LIST) {
            return (vbad(
                v, nd->line,
                "do must be a list"));
        }
        p->nsteps = suite_list_len(
            nd);
        if (p->nsteps > 0) {
            p->steps = calloc(
                p->nsteps,
                sizeof(*p->steps));
            if (p->steps ==
                nullptr) {
                return (vbad(
                    v, nd->line,
                    "no memory"));
            }
        }
        for (i = 0; i < p->nsteps;
             i++) {
            if (!val_step(
                    suite_list_at(nd,
                                  i),
                    f, v,
                    &p->steps[i])) {
                return (false);
            }
        }
    }
    u = suite_map_find(map, "until");
    t = suite_map_find(map,
                       "sustain");
    if (u != nullptr &&
        t != nullptr) {
        return (vbad(
            v, map->line,
            "until plus sustain "
            "together"));
    }
    if (u == nullptr &&
        t == nullptr) {
        return (vbad(
            v, map->line,
            "until or sustain "
            "required"));
    }
    p->has_until = u != nullptr;
    if (!val_cond(p->has_until ? u : t,
                  f, v, &p->cond)) {
        return (false);
    }
    nd = suite_map_find(map,
                        "hold_s");
    if (nd != nullptr) {
        long long x;

        // Int-shaped counts, quoted ok.
        if (!need_int(nd, &x) ||
            x < 0 ||
            x > kMaxTimeoutS) {
            return (vbad(
                v, nd->line,
                "hold_s must be "
                "0..3600"));
        }
        p->hold_s = x;
    }
    if (!p->has_until &&
        p->hold_s > 0) {
        return (vbad(
            v, map->line,
            "hold_s needs until"));
    }
    nd = suite_map_find(map,
                        "timeout_s");
    if (nd == nullptr) {
        return (vbad(v, map->line,
                     "missing phase "
                     "key 'timeout_s'"));
    }
    {
        long long x;

        // Int-shaped counts, quoted ok.
        if (!need_int(nd, &x) ||
            x < 1 ||
            x > kMaxTimeoutS) {
            return (vbad(
                v, nd->line,
                "timeout_s must be "
                "1..3600"));
        }
        p->timeout_s = x;
    }
    if (p->hold_s > p->timeout_s) {
        return (vbad(
            v, map->line,
            "hold_s above timeout_s"));
    }
    nd = suite_map_find(map,
                        "multicast");
    if (nd != nullptr) {
        const char *t;

        t = suite_scalar_str(nd);
        if (t == nullptr) {
            return (vbad(
                v, nd->line,
                "multicast must be a "
                "string"));
        }
        if (strcmp(t, "off") == 0) {
            p->nomcast = true;
        } else if (strcmp(
                       t, "on") != 0) {
            return (vbad(
                v, nd->line,
                "multicast must be on "
                "or off"));
        }
    }
    return (true);
}

// val_spawn validates spawn mapping.
static bool val_spawn(SuiteNode *nd,
                      const ExFile *f,
                      VCtx *v, ExFile *o) {
    SuiteNode *a;
    SuiteNode *b;
    SuiteNode *c2;
    const char *t;
    static const char *keep[] = {
        "variant",
        "node",
        "offset",
    };

    if (nd->kind != SUITE_MAP) {
        return (vbad(v, nd->line,
                     "spawn must be a "
                     "mapping"));
    }
    if (!val_extra(nd, keep, 3,
                   "spawn key", v)) {
        return (false);
    }
    o->has_spawn = true;
    o->variant = kPlain;
    a = suite_map_find(nd,
                       "variant");
    if (a != nullptr) {
        t = suite_scalar_str(a);
        if (t == nullptr) {
            return (vbad(
                v, a->line,
                "variant must be a "
                "string"));
        }
        if (strcmp(t, "plain") ==
            0) {
            o->variant = kPlain;
        } else if (strcmp(t, "anchor") ==
                   0) {
            o->variant = kAnchor;
        } else if (strcmp(t, "skew") ==
                   0) {
            o->variant = kSkew;
        } else {
            return (vbad(
                v, a->line,
                "bad spawn variant"));
        }
    }
    b = suite_map_find(nd, "node");
    c2 = suite_map_find(nd,
                        "offset");
    if (f->sut == kWeld) {
        if (o->variant != kPlain) {
            return (vbad(
                v, nd->line,
                "spawn variant is "
                "concord-only"));
        }
        if (b != nullptr) {
            return (vbad(
                v, b->line,
                "spawn node is "
                "concord-only"));
        }
        if (c2 != nullptr) {
            return (vbad(
                v, c2->line,
                "spawn offset is "
                "concord-only"));
        }
        return (true);
    }
    if (b != nullptr) {
        long long x;

        // Int-shaped counts, quoted ok.
        if (!need_int(b, &x) ||
            x < 0 ||
            x >= f->nodes) {
            return (vbad(
                v, b->line,
                "node index out of "
                "range"));
        }
        o->snode = x;
        o->has_snode = true;
    }
    if (c2 != nullptr) {
        t = suite_scalar_str(c2);
        if (t == nullptr ||
            t[0] == '\0') {
            return (vbad(
                v, c2->line,
                "offset must be a "
                "string"));
        }
        if (!copy_text(
                o->offset,
                sizeof(o->offset),
                t)) {
            return (vbad(
                v, c2->line,
                "offset too long"));
        }
        o->has_offset = true;
    }
    if (o->variant == kSkew) {
        if (!o->has_snode) {
            return (vbad(
                v, nd->line,
                "skew needs node"));
        }
        if (!o->has_offset) {
            return (vbad(
                v, nd->line,
                "skew needs offset"));
        }
    }
    if (o->variant == kAnchor &&
        !o->has_snode) {
        return (vbad(v, nd->line,
                     "anchor needs "
                     "node"));
    }
    return (true);
}

// val_ros validates ros peer entries.
static bool val_ros(SuiteNode *nd,
                    const ExFile *f,
                    VCtx *v, ExFile *o) {
    size_t i;

    if (f->sut != kWeld) {
        return (vbad(v, nd->line,
                     "ros_peers is "
                     "weld-only"));
    }
    if (nd->kind != SUITE_LIST) {
        return (vbad(v, nd->line,
                     "ros_peers must be "
                     "a list"));
    }
    o->nros = suite_list_len(nd);
    if (o->nros == 0) {
        return (vbad(v, nd->line,
                     "empty ros_peers"));
    }
    if (o->nros >
        (size_t)kMaxNodes) {
        return (vbad(v, nd->line,
                     "too many ros "
                     "peers"));
    }
    o->rosnode = calloc(o->nros,
                        sizeof(*o->rosnode));
    o->rosdistro = calloc(
        o->nros, sizeof(*o->rosdistro));
    if (o->rosnode == nullptr ||
        o->rosdistro == nullptr) {
        return (vbad(v, nd->line,
                     "no memory"));
    }
    for (i = 0; i < o->nros; i++) {
        SuiteNode *it;
        SuiteNode *a;
        SuiteNode *b;
        long long x;
        const char *t;
        static const char *keep[] = {
            "node",
            "distro",
        };

        it = suite_list_at(nd, i);
        if (it->kind != SUITE_MAP) {
            return (vbad(
                v, it->line,
                "ros peer must be a "
                "mapping"));
        }
        if (!val_extra(it, keep, 2,
                       "ros peer key",
                       v)) {
            return (false);
        }
        a = suite_map_find(it,
                           "node");
        b = suite_map_find(it,
                           "distro");
        if (a == nullptr ||
            b == nullptr) {
            return (vbad(
                v, it->line,
                "ros peer needs node "
                "plus distro"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(a, &x) ||
            x < 0 || x >= f->nodes) {
            return (vbad(
                v, a->line,
                "node index out of "
                "range"));
        }
        o->rosnode[i] = x;
        t = suite_scalar_str(b);
        if (t == nullptr ||
            (strcmp(t, "humble") !=
                 0 &&
             strcmp(t, "jazzy") != 0 &&
             strcmp(t, "rolling") !=
                 0)) {
            return (vbad(
                v, b->line,
                "bad distro"));
        }
        if (!copy_text(
                o->rosdistro[i],
                sizeof(*o->rosdistro),
                t)) {
            return (vbad(
                v, b->line,
                "distro too long"));
        }
    }
    return (true);
}

// reg_member reports ref as a
// generated member of base B with
// count C: B-N with N below C.
static bool reg_member(const char *b,
                       long long c,
                       const char *ref) {
    size_t n;
    size_t i;
    long long v = 0;

    n = strlen(b);
    if (strncmp(ref, b, n) != 0) {
        return (false);
    }
    if (ref[n] != '-') {
        return (false);
    }
    if (ref[n + 1] == '\0') {
        return (false);
    }
    for (i = n + 1; ref[i] != '\0';
         i++) {
        if (ref[i] < '0' ||
            ref[i] > '9') {
            return (false);
        }
        v = v * 10 +
            (ref[i] - '0');
        if (v >= c) {
            return (false);
        }
    }
    return (true);
}

// reg_has reports a workload ref
// known: an exact submit, a counted
// base, or a generated member.
static bool reg_has(
    char names[][kWlLen], size_t nn,
    char bases[][kWlLen],
    long long *bcount, size_t nb,
    const char *ref) {
    size_t i;

    for (i = 0; i < nn; i++) {
        if (strcmp(names[i], ref) ==
            0) {
            return (true);
        }
    }
    for (i = 0; i < nb; i++) {
        if (strcmp(bases[i], ref) ==
            0) {
            return (true);
        }
        if (reg_member(bases[i],
                       bcount[i],
                       ref)) {
            return (true);
        }
    }
    return (false);
}

// cond_wl reports whether a cond
// carries a workload ref to check.
static bool cond_wl(const ExCond *c) {
    switch (c->kind) {
    case CD_PRESENT:
    case CD_ABSENT:
        return (c->workload);
    case CD_ASSIGNED:
    case CD_AGREE:
    case CD_ASSIGNED_ON:
    case CD_EPOCH:
    case CD_IMAGE:
        return (true);
    default:
        return (false);
    }
}

// free_file releases a model.
static void free_file(ExFile *f) {
    size_t i;

    if (f == nullptr) {
        return;
    }
    for (i = 0; i < f->nphases; i++) {
        free(f->phases[i].steps);
    }
    free(f->phases);
    free(f->needs);
    free(f->rosnode);
    free(f->rosdistro);
    free(f);
}

// val_top validates the whole doc.
static bool val_top(SuiteDoc *doc,
                    VCtx *v, ExFile *o) {
    SuiteNode *root;
    SuiteNode *nd;
    const char *s;
    long long x;
    size_t i;
    size_t bi;
    static const char *keep[] = {
        "name",    "version",  "sut",
        "needs",   "nodes",    "domain",
        "topic",   "spawn",
        "ros_peers", "timeout_s",
        "phases", "bins",
        "disk_cap_mb",
    };

    root = doc->root;
    if (root->kind != SUITE_MAP) {
        return (vbad(v, root->line,
                     "top level must be "
                     "a mapping"));
    }
    for (bi = 0; bi <
         root->u.map.len; bi++) {
        if (has_ramp(
                root->u.map.items[bi]
                    .key)) {
            return (ramp_bad(
                v,
                root->u.map.items[bi]
                    .key_line));
        }
    }
    if (!val_extra(root, keep, 13,
                   "top-level key",
                   v)) {
        return (false);
    }
    memset(o, 0, sizeof(*o));
    nd = suite_map_find(root, "name");
    if (nd == nullptr) {
        return (vbad(v, root->line,
                     "missing top-level "
                     "key 'name'"));
    }
    s = suite_scalar_str(nd);
    if (s == nullptr ||
        s[0] == '\0') {
        return (vbad(v, nd->line,
                     "name must be a "
                     "string"));
    }
    if (!copy_text(o->name,
                   sizeof(o->name),
                   s)) {
        return (vbad(v, nd->line,
                     "name too long"));
    }
    nd = suite_map_find(root,
                        "version");
    if (nd == nullptr) {
        return (vbad(v, root->line,
                     "missing top-level "
                     "key 'version'"));
    }
    // Int-shaped counts, quoted ok.
    if (!need_int(nd, &x)) {
        return (vbad(v, nd->line,
                     "version must be "
                     "an int"));
    }
    if (x != 1) {
        return (vbad(v, nd->line,
                     "version must equal "
                     "1"));
    }
    nd = suite_map_find(root, "sut");
    if (nd == nullptr) {
        return (vbad(v, root->line,
                     "missing top-level "
                     "key 'sut'"));
    }
    s = suite_scalar_str(nd);
    if (s == nullptr) {
        return (vbad(v, nd->line,
                     "sut must be a "
                     "string"));
    }
    if (strcmp(s, "concord") == 0) {
        o->sut = kConcord;
    } else if (strcmp(s, "weld") ==
               0) {
        o->sut = kWeld;
    } else {
        return (vbad(v, nd->line,
                     "sut must be "
                     "concord or weld"));
    }
    nd = suite_map_find(root,
                        "nodes");
    if (nd == nullptr) {
        return (vbad(v, root->line,
                     "missing top-level "
                     "key 'nodes'"));
    }
    // Int-shaped counts, quoted ok.
    if (!need_int(nd, &x) || x < 1 ||
        x > kMaxNodes) {
        return (vbad(v, nd->line,
                     "nodes must be "
                     "1..100"));
    }
    o->nodes = x;
    nd = suite_map_find(root,
                        "needs");
    if (nd != nullptr) {
        if (nd->kind != SUITE_LIST) {
            return (vbad(
                v, nd->line,
                "needs must be a "
                "list"));
        }
        o->nneeds = suite_list_len(
            nd);
        if (o->nneeds > 0) {
            o->needs = calloc(
                o->nneeds,
                sizeof(*o->needs));
            if (o->needs ==
                nullptr) {
                return (vbad(
                    v, nd->line,
                    "no memory"));
            }
        }
        for (i = 0; i < o->nneeds;
             i++) {
            SuiteNode *it;

            it = suite_list_at(nd,
                               i);
            s = suite_scalar_str(
                it);
            if (s == nullptr) {
                return (vbad(
                    v, it->line,
                    "needs entries "
                    "must be strings"));
            }
            if (!copy_text(
                    o->needs[i],
                    sizeof(*o->needs),
                    s)) {
                return (vbad(
                    v, it->line,
                    "need too long"));
            }
            // Typo-safety: unknown verbs
            // fail here; runtime keeps
            // its skip for verbs the
            // linter cannot know.
            if (!need_ok(o->sut, s)) {
                return (vunknown(
                    v, it->line,
                    "need", s));
            }
        }
    }
    nd = suite_map_find(root,
                        "domain");
    if (nd != nullptr) {
        if (o->sut != kWeld) {
            return (vbad(
                v, nd->line,
                "domain is "
                "weld-only"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(nd, &x) ||
            x < 0 || x > 101) {
            return (vbad(
                v, nd->line,
                "domain must be "
                "0..101"));
        }
        o->domain = x;
        o->has_domain = true;
    } else if (o->sut == kWeld) {
        return (vbad(v, root->line,
                     "missing top-level "
                     "key 'domain'"));
    }
    nd = suite_map_find(root,
                        "topic");
    if (nd != nullptr) {
        if (o->sut != kWeld) {
            return (vbad(
                v, nd->line,
                "topic is "
                "weld-only"));
        }
        s = suite_scalar_str(nd);
        if (s == nullptr ||
            !topic_ok(s)) {
            return (vbad(
                v, nd->line,
                "bad topic"));
        }
        if (!copy_text(
                o->topic,
                sizeof(o->topic), s)) {
            return (vbad(
                v, nd->line,
                "topic too long"));
        }
        o->has_topic = true;
    }
    nd = suite_map_find(root,
                        "spawn");
    if (nd != nullptr) {
        if (!val_spawn(nd, o, v,
                       o)) {
            return (false);
        }
    }
    nd = suite_map_find(root,
                        "ros_peers");
    if (nd != nullptr) {
        if (!val_ros(nd, o, v, o)) {
            return (false);
        }
    }
    nd = suite_map_find(root, "bins");
    if (nd != nullptr) {
        size_t bi;

        if (nd->kind != SUITE_MAP) {
            return (vbad(
                v, nd->line,
                "bins must be a "
                "mapping"));
        }
        if (nd->u.map.len == 0) {
            return (vbad(
                v, nd->line,
                "empty bins"));
        }
        for (bi = 0;
             bi < nd->u.map.len; bi++) {
            const char *bk;
            SuiteNode *bv;
            const char *t;
            long long x;

            bk = nd->u.map.items[bi]
                     .key;
            bv = nd->u.map.items[bi]
                     .value;
            if (!key_int(bk, &x) ||
                x < 0 ||
                x >= o->nodes) {
                return (vbad(
                    v,
                    nd->u.map.items[bi]
                        .key_line,
                    "bins node out of "
                    "range"));
            }
            t = suite_scalar_str(bv);
            if (t == nullptr ||
                t[0] == '\0') {
                return (vbad(
                    v, bv->line,
                    "bins path must be "
                    "a string"));
            }
            if (!copy_text(
                    o->bins[x],
                    sizeof(o->bins[x]),
                    t)) {
                return (vbad(
                    v, bv->line,
                    "bins path too "
                    "long"));
            }
            o->bin_set[x] = true;
            o->has_bins = true;
        }
    }
    nd = suite_map_find(root,
                        "disk_cap_mb");
    if (nd != nullptr) {
        if (o->sut != kConcord) {
            return (vbad(
                v, nd->line,
                "disk_cap_mb is "
                "concord-only"));
        }
        // Int-shaped counts, quoted ok.
        if (!need_int(nd, &x) ||
            x < 0 || x > kMaxMb) {
            return (vbad(
                v, nd->line,
                "disk_cap_mb must be "
                "0..65536"));
        }
        o->disk_cap_mb =
            (unsigned long long)x;
    }
    nd = suite_map_find(root,
                        "timeout_s");
    if (nd == nullptr) {
        return (vbad(v, root->line,
                     "missing top-level "
                     "key 'timeout_s'"));
    }
    // Int-shaped counts, quoted ok.
    if (!need_int(nd, &x) ||
        x < 1 ||
        x > kMaxTimeoutS) {
        return (vbad(v, nd->line,
                     "timeout_s must be "
                     "1..3600"));
    }
    o->timeout_s = x;
    nd = suite_map_find(root,
                        "phases");
    if (nd == nullptr) {
        return (vbad(v, root->line,
                     "missing top-level "
                     "key 'phases'"));
    }
    if (nd->kind != SUITE_LIST ||
        suite_list_len(nd) == 0) {
        return (vbad(v, nd->line,
                     "phases must be a "
                     "non-empty list"));
    }
    o->nphases = suite_list_len(nd);
    o->phases = calloc(o->nphases,
                       sizeof(*o->phases));
    if (o->phases == nullptr) {
        return (vbad(v, nd->line,
                     "no memory"));
    }
    for (i = 0; i < o->nphases;
         i++) {
        if (!val_phase(
                suite_list_at(nd, i),
                o, v,
                &o->phases[i])) {
            return (false);
        }
    }
    // Names index nothing (frac uses
    // the numeric slot) but humans
    // misread repeats, so lint owns
    // the duplicate check.
    for (i = 0; i < o->nphases;
         i++) {
        size_t j;

        for (j = 0; j < i; j++) {
            if (strcmp(
                    o->phases[i].name,
                    o->phases[j].name) ==
                0) {
                return (vbad(
                    v,
                    o->phases[i].line,
                    "duplicate phase "
                    "name"));
            }
        }
    }
    {
        char names[kMaxWl][kWlLen];
        char bases[kMaxWl][kWlLen];
        long long bcount[kMaxWl];
        size_t nnames = 0;
        size_t nbases = 0;
        size_t nentries = 0;
        size_t nsub = 0;
        size_t a;

        // Submits register names in
        // order; stops, updates, and
        // workload conds must resolve.
        // A bare second submit fails:
        // only as: opens a new flight.
        memset(names, 0,
               sizeof(names));
        memset(bases, 0,
               sizeof(bases));
        for (a = 0; a < o->nphases;
             a++) {
            ExPhase *p;
            size_t k;

            p = &o->phases[a];
            for (k = 0;
                 k < p->nsteps; k++) {
                ExStep *s;

                s = &p->steps[k];
                if (s->kind ==
                    ST_SUBMIT) {
                    const char *eff;
                    long long add;

                    eff = s->has_as
                              ? s->as_name
                              : "submitted";
                    if (!s->has_as &&
                        nsub > 0) {
                        return (vbad(
                            v, s->line,
                            "second submit "
                            "needs as: "
                            "name"));
                    }
                    if (reg_has(
                            names, nnames,
                            bases, bcount,
                            nbases,
                            eff)) {
                        return (vbad(
                            v, s->line,
                            "duplicate "
                            "workload "
                            "name"));
                    }
                    add = s->has_count
                              ? s->count
                              : 1;
                    if (nentries + add >
                        (size_t)kMaxWl) {
                        return (vbad(
                            v, s->line,
                            "too many "
                            "workloads"));
                    }
                    (void)strcpy(
                        names[nnames],
                        eff);
                    nnames++;
                    if (s->has_count) {
                        (void)strcpy(
                            bases[nbases],
                            eff);
                        bcount[nbases] =
                            s->count;
                        nbases++;
                    }
                    nentries +=
                        (size_t)add;
                    nsub++;
                    continue;
                }
                if (s->kind ==
                    ST_BENCH) {
                    // Weld benchmarks
                    // mint no table
                    // names. Concord
                    // ones mint like a
                    // counted submit.
                    // Dispatch on submit
                    // cap, not SUT kind.
                    if (!kEpCap[s->
                                from_kind]
                             .submit) {
                        continue;
                    }
                    {
                        const char *eff;
                        long long add;
                        bool multi;

                        eff = s->as_name;
                        if (reg_has(
                                names,
                                nnames,
                                bases,
                                bcount,
                                nbases,
                                eff)) {
                            return (vbad(
                                v, s->line,
                                "duplicate "
                                "workload "
                                "name"));
                        }
                        if (s->has_ramp) {
                            add =
                                s->ramp_max_steps *
                                s->ramp_max_rate *
                                s->
                                    ramp_step_dur;
                            if (nentries +
                                    (size_t)
                                        add >
                                (size_t)
                                    kMaxWl) {
                                return (vbad(
                                    v, s->line,
                                    "too many "
                                    "workloads"));
                            }
                            (void)strcpy(
                                names[nnames],
                                eff);
                            nnames++;
                            (void)strcpy(
                                bases[nbases],
                                eff);
                            bcount[nbases] =
                                add;
                            nbases++;
                            nentries +=
                                (size_t)add;
                            nsub++;
                            continue;
                        } else {
                            multi =
                                s->has_dur ||
                                !s->has_count ||
                                s->count >
                                    1;
                            if (s->has_count &&
                                !s->has_dur) {
                                add =
                                    s->count;
                            } else if (
                                s->has_dur &&
                                !s->
                                    has_count) {
                                add =
                                    s->rate *
                                    s->
                                        duration_s;
                            } else {
                                add =
                                    s->count;
                            }
                            if (nentries +
                                    (size_t)
                                        add >
                                (size_t)
                                    kMaxWl) {
                                return (vbad(
                                    v, s->line,
                                    "too many "
                                    "workloads"));
                            }
                            (void)strcpy(
                                names[nnames],
                                eff);
                            nnames++;
                            if (multi) {
                                long long
                                    bc;

                                bc = add;
                                (void)strcpy(
                                    bases
                                        [nbases],
                                    eff);
                                bcount
                                    [nbases] =
                                    bc;
                                nbases++;
                            }
                            nentries +=
                                (size_t)add;
                            nsub++;
                            continue;
                        }
                    }
                }
                if (s->kind ==
                        ST_STOP &&
                    !reg_has(
                        names, nnames,
                        bases, bcount,
                        nbases, s->wl)) {
                    return (vbad(
                        v, s->line,
                        "stop with no "
                        "submitted "
                        "workload"));
                }
                if (s->kind ==
                        ST_UPDATE &&
                    !reg_has(
                        names, nnames,
                        bases, bcount,
                        nbases, s->wl)) {
                    return (vbad(
                        v, s->line,
                        "update with no "
                        "submitted "
                        "workload"));
                }
            }
            // Do steps run before the
            // poll, so a same-phase
            // submit counts here.
            if (cond_wl(&p->cond) &&
                !reg_has(
                    names, nnames,
                    bases, bcount,
                    nbases,
                    p->cond.wl)) {
                return (vbad(
                    v, p->cond.line,
                    "no submitted "
                    "workload"));
            }
        }
    }
    return (true);
}

// read_file slurps a path.
static bool read_file(const char *path,
                      char **out,
                      size_t *nout, VCtx *v) {
    FILE *fp;
    long end;
    size_t n;
    char *buf;

    *out = nullptr;
    *nout = 0;
    fp = fopen(path, "rb");
    if (fp == nullptr) {
        return (vbad(v, 0,
                     "cannot open file"));
    }
    if (fseek(fp, 0, SEEK_END) !=
        0) {
        (void)fclose(fp);
        return (vbad(v, 0,
                     "cannot seek file"));
    }
    end = ftell(fp);
    if (end < 0) {
        (void)fclose(fp);
        return (vbad(v, 0,
                     "cannot size file"));
    }
    if (fseek(fp, 0, SEEK_SET) !=
        0) {
        (void)fclose(fp);
        return (vbad(v, 0,
                     "cannot seek file"));
    }
    n = (size_t)end;
    buf = malloc(n + 1);
    if (buf == nullptr) {
        (void)fclose(fp);
        return (vbad(v, 0,
                     "no memory"));
    }
    if (n > 0 &&
        fread(buf, 1, n, fp) != n) {
        free(buf);
        (void)fclose(fp);
        return (vbad(v, 0,
                     "cannot read file"));
    }
    (void)fclose(fp);
    buf[n] = '\0';
    *out = buf;
    *nout = n;
    return (true);
}

// load_validated parses plus checks.
// Parse text truncates to fit why.
static bool load_validated(
    const char *path, ExFile **out,
    char *why, size_t why_n) {
    VCtx v;
    char *text = nullptr;
    size_t n = 0;
    SuiteDoc *doc = nullptr;
    SuiteError err = {0, 0, ""};
    ExFile *f = nullptr;

    *out = nullptr;
    v.path = path;
    v.why = why;
    v.why_n = why_n;
    if (path == nullptr) {
        (void)vbad(&v, 0,
                   "null path");
        return (false);
    }
    if (!read_file(path, &text, &n,
                   &v)) {
        return (false);
    }
    if (!suite_parse(text, n, &doc,
                     &err)) {
        char msg[512];
        int m;

        free(text);
        m = snprintf(msg, sizeof(msg),
                     "%s:%zu:%zu: %s",
                     path, err.line,
                     err.col,
                     err.message);
        ellipsize(msg, sizeof(msg), m);
        if (why != nullptr &&
            why_n > 0) {
            (void)snprintf(
                why, why_n, "%s", msg);
        }
        return (false);
    }
    free(text);
    f = calloc(1, sizeof(*f));
    if (f == nullptr) {
        suite_doc_free(doc);
        (void)vbad(&v, 0,
                   "no memory");
        return (false);
    }
    if (!val_top(doc, &v, f)) {
        suite_doc_free(doc);
        free_file(f);
        return (false);
    }
    suite_doc_free(doc);
    *out = f;
    if (why != nullptr &&
        why_n > 0) {
        why[0] = '\0';
    }
    return (true);
}

// suite_lint_file validates one suite
// file only, with file:line errors.
bool suite_lint_file(const char *path,
                     char *why,
                     size_t why_n) {
    ExFile *f = nullptr;

    if (!load_validated(path, &f,
                        why, why_n)) {
        return (false);
    }
    free_file(f);
    return (true);
}

// need_ok reports a needs verb known
// for the SUT. Unknown verbs are
// unmet needs, never hard errors.
static bool need_ok(int sut,
                    const char *s) {
    static const char *concord[] = {
        "workload.submit",
        "workload.list",
        "workload.stop",
        "workload.update",
        "node.list",
        "journal.assigned",
        "journal.agree",
        "journal.assigned_on",
        "journal.epoch",
        "journal.image",
        "journal.corrupt",
        "journal.has",
        "metrics.scrape",
        "disk.fill",
        "disk.readonly",
        "net.qos",
        "net.slow",
        "net.nomcast",
        "spawn.plain",
        "spawn.anchor",
        "spawn.skew",
        "spawn.bins",
    };
    static const char *weld[] = {
        "weld.pub",  "weld.echo",
        "weld.replay",
        "ros.peer",  "ros.echo",
        "ros.pub",   "relay.echo",
        "restart.fresh",
        "restart.preserve",
        "spawn.bins",
        "weld.log",  "ros.log",
        "metrics.scrape",
    };
    static const char *const *tabs[2] = {
        concord, weld,
    };
    static const size_t lens[2] = {
        sizeof(concord) /
            sizeof(concord[0]),
        sizeof(weld) /
            sizeof(weld[0]),
    };
    const char *const *tab = nullptr;
    size_t n = 0;
    size_t i = 0;
    int idx = 0;

    idx = sut < 0 || sut > 1 ? 0 : sut;
    tab = tabs[idx];
    n = lens[idx];
    for (i = 0; i < n; i++) {
        if (strcmp(s, tab[i]) == 0) {
            return (true);
        }
    }
    return (false);
}

// PollCtx feeds run_poll: the drop
// set plus the log statedir.
typedef struct {
    int drop[kMaxNodes];
    char statedir[kDirLen];
    bool weld;
} PollCtx;

// poll_drop drops packets on marked
// nodes, pumps the rest.
static bool poll_drop(void *ud,
                      int i) {
    PollCtx *c = ud;

    if (c == nullptr) {
        return (false);
    }
    if (i < 0 || i >= kMaxNodes) {
        return (false);
    }
    return (c->drop[i] != 0);
}

// poll_log saves one log chunk per
// node for marker greps plus tail
// forensics. Weld reuses the driver
// saver so observed reads agree.
static void poll_log(void *ud, int i,
                     const char *buf,
                     size_t len) {
    PollCtx *c = ud;
    char path[kPathLen];
    FILE *fp;

    if (c == nullptr ||
        buf == nullptr) {
        return;
    }
    if (i < 0 || i >= kMaxNodes) {
        return;
    }
    if (c->weld) {
        weld_log_save(c->statedir,
                      i, buf, len);
        return;
    }
    (void)snprintf(path,
                   sizeof(path),
                   "%s/node-%d.log",
                   c->statedir, i);
    fp = fopen(path, "ab");
    if (fp == nullptr) {
        return;
    }
    if (len > 0) {
        (void)fwrite(buf, 1, len,
                     fp);
    }
    (void)fclose(fp);
}

// log_path names a node log file.
static void log_path(char *dst,
                     size_t n,
                     const char *dir,
                     bool weld, int i) {
    if (weld) {
        (void)snprintf(dst, n,
                       "%s/weld-%d.log",
                       dir, i);
    } else {
        (void)snprintf(dst, n,
                       "%s/node-%d.log",
                       dir, i);
    }
}

// file_has reports needle in path,
// chunked with overlap so long logs
// still hit.
static bool file_has(const char *path,
                     const char *needle) {
    char buf[kFileBuf];
    size_t nlen;
    size_t carry = 0;
    FILE *fp;

    if (path == nullptr ||
        needle == nullptr) {
        return (false);
    }
    if (needle[0] == '\0') {
        return (true);
    }
    nlen = strlen(needle);
    if (nlen + 1 > sizeof(buf) / 2) {
        return (false);
    }
    fp = fopen(path, "rb");
    if (fp == nullptr) {
        return (false);
    }
    for (;;) {
        size_t k;
        size_t total;

        k = fread(buf + carry, 1,
                  sizeof(buf) -
                      carry - 1,
                  fp);
        total = carry + k;
        buf[total] = '\0';
        if (strstr(buf, needle) !=
            nullptr) {
            (void)fclose(fp);
            return (true);
        }
        if (k == 0) {
            break;
        }
        if (total >= nlen) {
            carry = nlen - 1;
            memmove(buf,
                    buf + total - carry,
                    carry);
        } else {
            carry = total;
        }
    }
    (void)fclose(fp);
    return (false);
}

// peers_has reports the full-mesh
// marker for count on node i, in
// both spacing variants the mesh
// verdict shape covers.
static bool peers_has(
    const char *dir, int i,
    long long count) {
    char path[kPathLen];
    char a[64];
    char b[64];

    log_path(path, sizeof(path),
             dir, false, i);
    (void)snprintf(a, sizeof(a),
                   "\"peers\":%lld",
                   count);
    (void)snprintf(b, sizeof(b),
                   "\"peers\": %lld",
                   count);
    if (file_has(path, a)) {
        return (true);
    }
    return (file_has(path, b));
}

// WlEnt names one live workload: the
// file name, the CLI id, the submit
// node, plus submit and first
// present stamps in ms since file
// start for latency metrics.
typedef struct {
    char name[kWlLen];
    char wid[kIdLen];
    int node;
    unsigned long long sub_ms;
    unsigned long long present_ms;
    bool metric_done;
} WlEnt;

// WlTab owns workload names plus
// counted bases. RunCtx owns one;
// EvalCtx borrows it for reads.
typedef struct {
    WlEnt ent[kMaxWl];
    size_t n;
    char bases[kMaxWl][kWlLen];
    long long bcount[kMaxWl];
    size_t nbase;
} WlTab;

// wl_resolve lists entry indices
// for a name into out (max cap):
// exact hits win; else every member
// of a counted base. Returns the
// hit count, 0 when unknown.
static int wl_resolve(const WlTab *t,
                      const char *name,
                      int *out, int max) {
    size_t i;
    size_t j;
    int n = 0;

    if (t == nullptr ||
        name == nullptr ||
        out == nullptr || max <= 0) {
        return (0);
    }
    for (i = 0; i < t->n; i++) {
        if (strcmp(t->ent[i].name,
                   name) != 0) {
            continue;
        }
        if (n >= max) {
            break;
        }
        out[n++] = (int)i;
    }
    if (n > 0) {
        return (n);
    }
    for (i = 0; i < t->nbase; i++) {
        if (strcmp(t->bases[i],
                   name) != 0) {
            continue;
        }
        for (j = 0; j < t->n; j++) {
            if (!reg_member(
                    t->bases[i],
                    t->bcount[i],
                    t->ent[j].name)) {
                continue;
            }
            if (n >= max) {
                break;
            }
            out[n++] = (int)j;
        }
    }
    return (n);
}

// EvalCtx holds query caches plus a
// borrow of the run workload table.
// plist caches list hits per entry
// per node; pms holds first present
// stamps; asg caches assigned per
// entry. Caches refresh on the
// query tick or when stale.
typedef struct {
    const ExFile *f;
    Run *r;
    char statedir[kDirLen];
    bool ros_on[kMaxNodes];
    WlTab *tab;
    time_t tcheck;
    unsigned long long tsample;
    bool plist[kMaxWl][kMaxNodes];
    bool list_set;
    unsigned long long pms[kMaxWl];
    bool asg[kMaxWl];
    bool asg_set;
    int mesh;
    bool mesh_set;
} EvalCtx;

// short_of stores the 8-char list id.
static void short_of(const char *wid,
                     char *out,
                     size_t n) {
    size_t i;

    for (i = 0; i + 1 < n && i < 8 &&
         wid[i] != '\0';
         i++) {
        out[i] = wid[i];
    }
    out[i] = '\0';
}

// refresh_lists polls workload lists
// for every live entry on every
// node, throttled by the caller to
// about one round per 2 s. First
// sightings stamp pms for the
// present latency metric.
static void refresh_lists(EvalCtx *e) {
    int n;
    int i;
    size_t h;

    if (e == nullptr ||
        e->tab == nullptr) {
        return;
    }
    n = (int)e->f->nodes;
    for (h = 0; h < e->tab->n; h++) {
        char shortid[kShortLen];

        short_of(e->tab->ent[h].wid,
                 shortid,
                 sizeof(shortid));
        for (i = 0; i < n; i++) {
            e->plist[h][i] =
                concord_list_has(
                    e->statedir, i,
                    shortid);
        }
        if (e->pms[h] != 0) {
            continue;
        }
        for (i = 0; i < n; i++) {
            if (e->plist[h][i]) {
                unsigned long long sub;

                e->pms[h] =
                    ev_now_ms();
                // Bridge into the row:
                // present.<name>.ms is
                // first sight minus
                // submit. Same-ms reads
                // 0, like bench sub-ms.
                sub = e->tab->ent[h]
                          .sub_ms;
                if (e->pms[h] >= sub) {
                    e->tab->ent[h]
                        .present_ms =
                        e->pms[h] - sub;
                }
                break;
            }
        }
    }
    e->list_set = true;
}

// refresh_assigned polls assigned
// anywhere for every live entry.
// Bad ids read false, never shell
// out.
static void refresh_assigned(
    EvalCtx *e) {
    size_t h;

    if (e == nullptr ||
        e->tab == nullptr) {
        return;
    }
    for (h = 0; h < e->tab->n; h++) {
        if (!wid_ok(
                e->tab->ent[h].wid)) {
            e->asg[h] = false;
            continue;
        }
        e->asg[h] =
            concord_assigned_anywhere(
                e->statedir,
                e->tab->ent[h].wid);
    }
    e->asg_set = true;
}

// refresh_mesh polls the CLI alive
// count on node 0 for concord, or
// counts live children for weld
// via kill probe; never reaps.
static void refresh_mesh(EvalCtx *e) {
    int n;
    int i;

    if (e->f->sut == kConcord) {
        e->mesh = concord_mesh_count(
            e->statedir, 0);
        e->mesh_set = true;
        return;
    }
    n = (int)e->f->nodes;
    e->mesh = 0;
    for (i = 0; i < n; i++) {
        if (e->r->pids[i] <= 0) {
            continue;
        }
        if (kill(e->r->pids[i], 0) ==
            0) {
            e->mesh++;
            continue;
        }
        if (errno == EPERM) {
            e->mesh++;
        }
    }
    e->mesh_set = true;
}

// observed_on reports text seen on
// node i for a kind: weld dual (weld
// log plus ROS log where a peer
// runs, legacy) or ROS only. Dispatch
// on capability, not SUT kind.
static bool observed_on(EvalCtx *e,
                        int kind, int i,
                        const char *text) {
    char path[kPathLen];

    if (kind < 0 || kind > 2) {
        return (false);
    }
    if (!kEpCap[kind].obs_weld &&
        !kEpCap[kind].obs_ros) {
        return (false);
    }
    if (!kEpCap[kind].obs_weld &&
        kEpCap[kind].obs_ros) {
        if (e == nullptr ||
            text == nullptr) {
            return (false);
        }
        if (i < 0 ||
            i >= kMaxNodes) {
            return (false);
        }
        (void)snprintf(
            path, sizeof(path),
            "%s/ros-%d.log",
            e->statedir, i);
        return (file_has(path, text));
    }
    log_path(path, sizeof(path),
             e->statedir, true, i);
    if (!file_has(path, text)) {
        return (false);
    }
    if (!e->ros_on[i]) {
        return (true);
    }
    (void)snprintf(path,
                   sizeof(path),
                   "%s/ros-%d.log",
                   e->statedir, i);
    return (file_has(path, text));
}

// kNilUuid marks unassigned slots
// in concord journals, as in the
// check/ readers.
static const char kNilUuid[] =
    "00000000-0000-0000-0000-"
    "000000000000";

// journal_tail stores the tail-1
// grep -o hit for pat in one node
// journal. Wid is hex-gated and the
// statedir charset-gated, so neither
// breaks the shell. False on miss.
static bool journal_tail(
    const char *dir, int node,
    const char *wid, const char *pat,
    char *out, size_t n) {
    char cmd[kCmdLen * 2];
    char buf[kLogChunk];
    FILE *fp;
    int m;

    if (!statedir_ok(dir) ||
        !wid_ok(wid) || node < 0 ||
        pat == nullptr ||
        out == nullptr || n == 0) {
        return (false);
    }
    m = snprintf(cmd, sizeof(cmd),
                 "grep '%s' %s/node%d/"
                 "concord/journal.jsonl "
                 "2>/dev/null | grep -o "
                 "'%s' | tail -1",
                 wid, dir, node, pat);
    if (m < 0 ||
        m >= (int)sizeof(cmd)) {
        return (false);
    }
    fp = popen(cmd, "r");
    if (fp == nullptr) {
        return (false);
    }
    if (fgets(buf, sizeof(buf),
              fp) == nullptr) {
        pclose(fp);
        return (false);
    }
    pclose(fp);
    if (!copy_text(out, n, buf)) {
        return (false);
    }
    return (true);
}

// read_assignment stores the latest
// AssignedNodeID for wid on node,
// mirroring check/concord_update.c.
// False when missing.
static bool read_assignment(
    const char *dir, int node,
    const char *wid, char *out,
    size_t n) {
    char buf[kLogChunk];
    char seg[kIdLen];

    if (!journal_tail(
            dir, node, wid,
            "\"AssignedNodeID\":"
            "\"[^\"]*\"",
            buf, sizeof(buf))) {
        return (false);
    }
    if (sscanf(buf,
               "\"AssignedNodeID\":"
               "\"%63[^\"]\"",
               seg) != 1) {
        return (false);
    }
    if (!copy_text(out, n, seg)) {
        return (false);
    }
    return (true);
}

// read_epoch stores the latest
// AssignmentEpoch for wid on node,
// mirroring check/concord_update.c.
// False when missing.
static bool read_epoch(
    const char *dir, int node,
    const char *wid,
    unsigned long long *out) {
    char buf[kLogChunk];
    unsigned long long v;

    if (out == nullptr) {
        return (false);
    }
    if (!journal_tail(
            dir, node, wid,
            "\"AssignmentEpoch\":"
            "[0-9]*",
            buf, sizeof(buf))) {
        return (false);
    }
    if (sscanf(buf,
               "\"AssignmentEpoch\":"
               "%llu",
               &v) != 1) {
        return (false);
    }
    *out = v;
    return (true);
}

// read_image stores the latest Image
// for wid on node, mirroring
// check/concord_update.c. False when
// missing.
static bool read_image(
    const char *dir, int node,
    const char *wid, char *out,
    size_t n) {
    char buf[kLogChunk];
    char img[128];

    if (!journal_tail(
            dir, node, wid,
            "\"Image\":\"[^\"]*\"",
            buf, sizeof(buf))) {
        return (false);
    }
    if (sscanf(buf,
               "\"Image\":\"%127"
               "[^\"]\"",
               img) != 1) {
        return (false);
    }
    if (!copy_text(out, n, img)) {
        return (false);
    }
    return (true);
}

// pace sleeps 1e9/rate ns between
// paced issues via nanosleep. Ask
// is exact; get is coarse: kernel
// hrtimer plus sched jitter (tens
// of us) plus per-issue pub cost
// (fork plus exec, ms) or submit
// cost (shell plus CLI, tens of
// ms) dominates, so high rates
// fall short. Duration loops break
// at the deadline and report the
// shortfall in overrun, never
// silent. Grace pumps at the 100
// ms run poll, not here.
// eval_cond checks one condition.
// Returns 1 true, 0 false, -1 error
// with text in ewhy.
static int eval_cond(const ExCond *c,
                     EvalCtx *e,
                     bool query, char *ewhy,
                     size_t n) {
    size_t i;

    switch (c->kind) {
    case CD_PEERS:
        for (i = 0; i < c->non;
             i++) {
            if (!peers_has(
                    e->statedir,
                    c->on[i], c->count)) {
                return (0);
            }
        }
        return (1);
    case CD_MARKER:
        if (c->any) {
            int n = (int)e->f->nodes;
            int k;

            for (k = 0; k < n; k++) {
                char path[kPathLen];

                log_path(path,
                         sizeof(path),
                         e->statedir,
                         e->f->sut ==
                                 kWeld,
                         k);
                if (file_has(
                        path,
                        c->marker)) {
                    return (1);
                }
            }
            return (0);
        }
        for (i = 0; i < c->non;
             i++) {
            char path[kPathLen];

            log_path(path,
                     sizeof(path),
                     e->statedir,
                     e->f->sut == kWeld,
                     c->on[i]);
            if (!file_has(path,
                          c->marker)) {
                return (0);
            }
        }
        return (1);
    case CD_PRESENT:
    case CD_ABSENT:
        {
            bool want = c->kind ==
                CD_PRESENT;

            if (c->workload) {
                int hits[kMaxWl];
                int nh;
                int h;

                nh = wl_resolve(
                    e->tab, c->wl, hits,
                    kMaxWl);
                if (nh == 0) {
                    (void)snprintf(
                        ewhy, n,
                        "no submitted "
                        "workload");
                    return (-1);
                }
                if (query ||
                    !e->list_set) {
                    refresh_lists(e);
                }
                for (h = 0; h < nh;
                     h++) {
                    for (i = 0;
                         i < c->non;
                         i++) {
                        bool has =
                            e->plist[hits
                                         [h]]
                                     [c->on
                                          [i]];

                        if (has !=
                            want) {
                            return (0);
                        }
                    }
                }
                return (1);
            }
            if (want) {
                for (i = 0; i < c->non;
                     i++) {
                    char path[kPathLen];

                    log_path(
                        path,
                        sizeof(path),
                        e->statedir,
                        true, c->on[i]);
                    if (!file_has(
                            path,
                            c->text)) {
                        return (0);
                    }
                }
                return (1);
            }
            for (i = 0; i < c->non;
                 i++) {
                char path[kPathLen];

                log_path(path,
                         sizeof(path),
                         e->statedir,
                         true, c->on[i]);
                if (file_has(path,
                             c->text)) {
                    return (0);
                }
            }
            return (1);
        }
    case CD_OBSERVED:
        if (observed_on(e,
                        c->obs_kind,
                        c->obs,
                        c->text)) {
            return (1);
        }
        return (0);
    case CD_ASSIGNED:
        {
            int hits[kMaxWl];
            int nh;
            int h;

            nh = wl_resolve(e->tab,
                            c->wl, hits,
                            kMaxWl);
            if (nh == 0) {
                (void)snprintf(
                    ewhy, n,
                    "no submitted "
                    "workload");
                return (-1);
            }
            if (query ||
                !e->asg_set) {
                refresh_assigned(e);
            }
            for (h = 0; h < nh; h++) {
                if (!e->asg[hits[h]]) {
                    return (0);
                }
            }
            return (1);
        }
    case CD_AGREE:
        {
            int hits[kMaxWl];
            int nh;
            int h;

            nh = wl_resolve(e->tab,
                            c->wl, hits,
                            kMaxWl);
            if (nh == 0) {
                (void)snprintf(
                    ewhy, n,
                    "no submitted "
                    "workload");
                return (-1);
            }
            for (h = 0; h < nh; h++) {
                char ref[kIdLen];
                int k;
                int n;

                n = (int)e->f->nodes;
                if (!read_assignment(
                        e->statedir, 0,
                        e->tab->ent[hits
                                       [h]]
                            .wid,
                        ref,
                        sizeof(ref)) ||
                    strcmp(
                        ref,
                        kNilUuid) ==
                        0) {
                    return (0);
                }
                for (k = 1; k < n;
                     k++) {
                    char seg[kIdLen];

                    if (!read_assignment(
                            e->statedir,
                            k,
                            e->tab->ent
                                [hits[h]]
                                .wid,
                            seg,
                            sizeof(
                                seg)) ||
                        strcmp(seg,
                               ref) !=
                            0) {
                        return (0);
                    }
                }
            }
            return (1);
        }
    case CD_ASSIGNED_ON:
        {
            int hits[kMaxWl];
            int nh;
            int h;

            nh = wl_resolve(e->tab,
                            c->wl, hits,
                            kMaxWl);
            if (nh == 0) {
                (void)snprintf(
                    ewhy, n,
                    "no submitted "
                    "workload");
                return (-1);
            }
            for (h = 0; h < nh; h++) {
                char seg[kIdLen];

                if (!read_assignment(
                        e->statedir,
                        c->node,
                        e->tab->ent[hits
                                       [h]]
                            .wid,
                        seg,
                        sizeof(seg)) ||
                    strcmp(
                        seg,
                        kNilUuid) ==
                        0) {
                    return (0);
                }
            }
            return (1);
        }
    case CD_EPOCH:
        {
            int hits[kMaxWl];
            int nh;
            int h;

            nh = wl_resolve(e->tab,
                            c->wl, hits,
                            kMaxWl);
            if (nh == 0) {
                (void)snprintf(
                    ewhy, n,
                    "no submitted "
                    "workload");
                return (-1);
            }
            for (h = 0; h < nh; h++) {
                unsigned long long v;

                if (!read_epoch(
                        e->statedir,
                        c->node,
                        e->tab->ent[hits
                                       [h]]
                            .wid,
                        &v)) {
                    return (0);
                }
                if (c->cmp == CMP_EQ &&
                    v != c->epoch) {
                    return (0);
                }
                if (c->cmp ==
                        CMP_GTE &&
                    v < c->epoch) {
                    return (0);
                }
            }
            return (1);
        }
    case CD_IMAGE:
        {
            int hits[kMaxWl];
            int nh;
            int h;

            nh = wl_resolve(e->tab,
                            c->wl, hits,
                            kMaxWl);
            if (nh == 0) {
                (void)snprintf(
                    ewhy, n,
                    "no submitted "
                    "workload");
                return (-1);
            }
            for (h = 0; h < nh; h++) {
                for (i = 0;
                     i < c->non; i++) {
                    char img[128];

                    if (!read_image(
                            e->statedir,
                            c->on[i],
                            e->tab->ent
                                [hits[h]]
                                .wid,
                            img,
                            sizeof(
                                img)) ||
                        strcmp(
                            img,
                            c->imgval) !=
                            0) {
                        return (0);
                    }
                }
            }
            return (1);
        }
    case CD_MESH:
        if (query || !e->mesh_set) {
            refresh_mesh(e);
        }
        // Exact match is fixed count.
        return (e->mesh ==
                        (int)c->count
                    ? 1
                    : 0);
    }
    return (-1);
}

// cond_desc names a condition.
static void cond_desc(const ExCond *c,
                      char *dst,
                      size_t n) {
    switch (c->kind) {
    case CD_PEERS:
        (void)snprintf(dst, n,
                       "peers %lld",
                       c->count);
        break;
    case CD_MARKER:
        (void)snprintf(dst, n,
                       "marker '%s'",
                       c->marker);
        break;
    case CD_PRESENT:
        (void)snprintf(dst, n,
                       "present");
        break;
    case CD_ABSENT:
        (void)snprintf(dst, n,
                       "absent");
        break;
    case CD_OBSERVED:
        (void)snprintf(dst, n,
                       "observed '%s'",
                       c->text);
        break;
    case CD_ASSIGNED:
        (void)snprintf(dst, n,
                       "assigned");
        break;
    case CD_AGREE:
        (void)snprintf(dst, n,
                       "agree");
        break;
    case CD_ASSIGNED_ON:
        (void)snprintf(dst, n,
                       "assigned_on %d",
                       c->node);
        break;
    case CD_EPOCH:
        (void)snprintf(dst, n,
                       "epoch %d",
                       c->node);
        break;
    case CD_IMAGE:
        (void)snprintf(dst, n,
                       "image '%s'",
                       c->imgval);
        break;
    case CD_MESH:
        (void)snprintf(dst, n,
                       "mesh_count %lld",
                       c->count);
        break;
    }
}

// cond_frac counts nodes now meeting
// c: num of den. False for global
// shapes with no node set. Callers
// run it on the 2 s query tick, so
// workload and mesh caches are
// fresh with no extra CLI polls.
static bool cond_frac(const ExCond *c,
                      EvalCtx *e,
                      long long *num,
                      long long *den) {
    long long got = 0;
    long long scope = 0;
    size_t i;
    int k;

    if (c == nullptr || e == nullptr ||
        num == nullptr ||
        den == nullptr) {
        return (false);
    }
    switch (c->kind) {
    case CD_PEERS:
        for (i = 0; i < c->non; i++) {
            if (peers_has(e->statedir,
                          c->on[i],
                          c->count)) {
                got++;
            }
        }
        scope = (long long)c->non;
        break;
    case CD_MARKER:
        if (c->any) {
            for (k = 0;
                 k < (int)e->f->nodes;
                 k++) {
                char path[kPathLen];

                log_path(path,
                         sizeof(path),
                         e->statedir,
                         e->f->sut ==
                                 kWeld,
                         k);
                if (file_has(
                        path,
                        c->marker)) {
                    got++;
                }
            }
            scope = e->f->nodes;
            break;
        }
        for (i = 0; i < c->non; i++) {
            char path[kPathLen];

            log_path(path,
                     sizeof(path),
                     e->statedir,
                     e->f->sut == kWeld,
                     c->on[i]);
            if (file_has(path,
                         c->marker)) {
                got++;
            }
        }
        scope = (long long)c->non;
        break;
    case CD_PRESENT:
    case CD_ABSENT:
        {
            bool want = c->kind ==
                CD_PRESENT;
            int hits[kMaxWl];
            int nh;

            if (!c->workload) {
                for (i = 0;
                     i < c->non; i++) {
                    char path[kPathLen];
                    bool has;

                    log_path(
                        path,
                        sizeof(path),
                        e->statedir,
                        true, c->on[i]);
                    has = file_has(
                        path, c->text);
                    if (has == want) {
                        got++;
                    }
                }
                scope =
                    (long long)c->non;
                break;
            }
            nh = wl_resolve(e->tab,
                            c->wl, hits,
                            kMaxWl);
            if (nh == 0 ||
                !e->list_set) {
                return (false);
            }
            for (i = 0; i < c->non;
                 i++) {
                bool ok = true;
                int h2;

                for (h2 = 0; h2 < nh;
                     h2++) {
                    bool has =
                        e->plist[hits
                                     [h2]]
                                 [c->on
                                      [i]];

                    if (has != want) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    got++;
                }
            }
            scope = (long long)c->non;
        }
        break;
    case CD_OBSERVED:
        got = observed_on(e,
                          c->obs_kind,
                          c->obs,
                          c->text)
                  ? 1
                  : 0;
        scope = 1;
        break;
    case CD_ASSIGNED:
        return (false);
    case CD_AGREE:
        {
            int hits[kMaxWl];
            char ref[kIdLen];
            int k;
            int n;

            if (wl_resolve(e->tab,
                           c->wl, hits,
                           kMaxWl) == 0) {
                return (false);
            }
            if (!read_assignment(
                    e->statedir, 0,
                    e->tab->ent[hits[0]]
                        .wid,
                    ref, sizeof(ref)) ||
                strcmp(ref,
                       kNilUuid) == 0) {
                got = 0;
                scope =
                    (long long)e->f
                        ->nodes;
                break;
            }
            n = (int)e->f->nodes;
            for (k = 0; k < n; k++) {
                char seg[kIdLen];

                if (!read_assignment(
                        e->statedir, k,
                        e->tab->ent[hits
                                       [0]]
                            .wid,
                        seg,
                        sizeof(seg))) {
                    continue;
                }
                if (strcmp(seg, ref) ==
                    0) {
                    got++;
                }
            }
            scope = (long long)n;
        }
        break;
    case CD_ASSIGNED_ON:
    case CD_EPOCH:
        {
            char ewhy[160];

            ewhy[0] = '\0';
            if (eval_cond(c, e, false,
                          ewhy,
                          sizeof(
                              ewhy)) >
                0) {
                got = 1;
            }
            scope = 1;
        }
        break;
    case CD_IMAGE:
        {
            int hits[kMaxWl];
            int nh;

            nh = wl_resolve(e->tab,
                            c->wl, hits,
                            kMaxWl);
            if (nh == 0) {
                return (false);
            }
            for (i = 0; i < c->non;
                 i++) {
                bool ok = true;
                int h2;

                for (h2 = 0; h2 < nh;
                     h2++) {
                    char img[128];

                    if (!read_image(
                            e->statedir,
                            c->on[i],
                            e->tab->ent
                                [hits[h2]]
                                .wid,
                            img,
                            sizeof(
                                img)) ||
                        strcmp(
                            img,
                            c->imgval) !=
                            0) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    got++;
                }
            }
            scope = (long long)c->non;
        }
        break;
    case CD_MESH:
        if (!e->mesh_set ||
            e->mesh < 0) {
            return (false);
        }
        got = e->mesh > (int)c->count
                  ? c->count
                  : (long long)e->mesh;
        scope = c->count;
        break;
    }
    if (scope <= 0) {
        return (false);
    }
    if (got < 0 || got > scope) {
        return (false);
    }
    *num = got;
    *den = scope;
    return (true);
}

// move_node sets the sim x of the
// node behind fds[idx], converted
// by the caller from meters to nm.
static bool move_node(Run *r, int idx,
                      int64_t x) {
    size_t k;

    if (r == nullptr) {
        return (false);
    }
    if (idx < 0 || idx >= r->n) {
        return (false);
    }
    for (k = 0; k < r->map.n; k++) {
        if (r->map.slot[k].fd ==
            r->fds[idx]) {
            Node *nd;

            nd = nodelist_find(
                &r->ctx.nodes,
                r->map.slot[k]
                    .node_id);
            if (nd == nullptr) {
                return (false);
            }
            nd->x_nm = x;
            return (true);
        }
    }
    return (false);
}

// RunCtx carries live run state:
// drop sets, workload table, paths.
// skip_fixed latches a submit id:
// request: the run ends as skip.
// ros_on marks nodes with a ROS
// peer, mirroring EvalCtx for the
// benchmark observe path.
typedef struct {
    Run *r;
    const SimNet *net;
    PollCtx *pc;
    const ExFile *f;
    const char *weld_bin;
    const char *statedir;
    const char *topic;
    int domain;
    WlTab tab;
    bool skip_fixed;
    bool ros_on[kMaxNodes];
    // tsample throttles live series
    // emission to kSampleGapNs; see
    // sample_live.
    unsigned long long tsample;
} RunCtx;

// step_desc names an applied step
// for the step event.
static void step_desc(const ExStep *s,
                      char *dst,
                      size_t n) {
    const char *eff;

    switch (s->kind) {
    case ST_DROP:
        (void)snprintf(dst, n,
                       "drop");
        break;
    case ST_HEAL:
        (void)snprintf(dst, n,
                       "heal");
        break;
    case ST_KILL:
        (void)snprintf(dst, n,
                       "kill %d",
                       s->node);
        break;
    case ST_RESTART:
        (void)snprintf(dst, n,
                       "restart %d",
                       s->node);
        break;
    case ST_MOVE:
        (void)snprintf(dst, n,
                       "move %d",
                       s->node);
        break;
    case ST_SUBMIT:
        eff = s->has_as ? s->as_name
                        : "submitted";
        (void)snprintf(dst, n,
                       "submit %s",
                       eff);
        break;
    case ST_STOP:
        (void)snprintf(dst, n,
                       "stop %s", s->wl);
        break;
    case ST_PUB:
        (void)snprintf(dst, n,
                       "pub %s",
                       s->text);
        break;
    case ST_ROSPUB:
        (void)snprintf(dst, n,
                       "ros_pub %s",
                       s->text);
        break;
    case ST_SLOW:
        (void)snprintf(dst, n,
                       "slow %lld",
                       s->slow_secs);
        break;
    case ST_UPDATE:
        (void)snprintf(dst, n,
                       "update %s",
                       s->wl);
        break;
    case ST_REPLAY:
        (void)snprintf(dst, n,
                       "replay %s",
                       s->file);
        break;
    case ST_DISKFILL:
        (void)snprintf(dst, n,
                       "diskfill %d",
                       s->node);
        break;
    case ST_DISKRO:
        (void)snprintf(dst, n,
                       "diskreadonly %d",
                       s->node);
        break;
    case ST_JCORRUPT:
        (void)snprintf(dst, n,
                       "journalcorrupt "
                       "%d",
                       s->node);
        break;
    case ST_BENCH:
        (void)snprintf(dst, n,
                       "benchmark");
        break;
    }
}

// do_submit_once issues one submit
// via the driver. Single submits
// and benchmark loops share it.
static void metric_u64(size_t ph,
                       const char *key,
                       unsigned long long
                           val);
static bool do_submit_once(
    RunCtx *c, int node,
    const char *image,
    const char *flags, char *wid,
    size_t wid_n) {
    if (c == nullptr ||
        image == nullptr ||
        wid == nullptr ||
        wid_n == 0) {
        return (false);
    }
    return (concord_submit(
        c->statedir, node, image,
        flags, wid, wid_n));
}

// do_pub_once issues one pub via
// the driver. Single pubs and the
// benchmark loop share it.
static bool do_pub_once(RunCtx *c,
                        int node,
                        const char *topic,
                        const char *text) {
    const char *t;

    if (c == nullptr ||
        text == nullptr) {
        return (false);
    }
    t = topic != nullptr ? topic
                         : c->topic;
    return (weld_pub(
        c->net, node, c->domain,
        c->weld_bin, t, text));
}

// do_ros_pub_once issues one ROS
// pub via the driver on node peer.
// Single ros_pub steps and the ROS
// benchmark from: path share it.
// Publishes through the distro of
// the peer beside node, falling
// back to the driver default when
// node runs bare.
static bool do_ros_pub_once(
    RunCtx *c, int node,
    const char *topic,
    const char *text) {
    char dom[16];
    const char *t;
    const char *distro = nullptr;
    int m;
    size_t i;

    if (c == nullptr ||
        text == nullptr) {
        return (false);
    }
    t = topic != nullptr ? topic
                         : c->topic;
    m = snprintf(dom, sizeof(dom),
                 "%d", c->domain);
    if (m < 0 ||
        m >= (int)sizeof(dom)) {
        return (false);
    }
    (void)setenv("ROS_DOMAIN_ID",
                 dom, 1);
    if (c->f != nullptr) {
        for (i = 0;
             i < c->f->nros; i++) {
            if (c->f->rosnode[i] ==
                (long long)node) {
                distro = c->f
                             ->rosdistro
                                 [i];
                break;
            }
        }
    }
    return (ros_pub_once(
        c->net, node, t, text,
        distro));
}

// bench_now_ns reports monotonic
// ns for latency stamps. Zero on
// clock failure, treated as miss.
static unsigned long long
bench_now_ns(void) {
    struct timespec ts;

    if (clock_gettime(
            CLOCK_MONOTONIC,
            &ts) != 0) {
        return (0);
    }
    return ((unsigned long long)ts
                .tv_sec *
            1000000000ULL +
            (unsigned long long)ts
                .tv_nsec);
}

// sample_slot reports true at most
// once per kSampleGapNs, updating
// last. First call always fires.
// Zero clock reads as due, never
// stuck. Shared by condition waits
// (EvalCtx tsample) and step loops
// (RunCtx tsample).
static bool sample_slot(
    unsigned long long *last) {
    unsigned long long now;

    if (last == nullptr) {
        return (false);
    }
    now = bench_now_ns();
    if (*last != 0 &&
        now - *last <
            kSampleGapNs) {
        return (false);
    }
    *last = now;
    return (true);
}

// sample_live ticks the sampler and
// emits flows plus resources at most
// once per kSampleGapNs. Call it from
// every paced or pumped loop so the
// series cover step execution, not
// just condition waits.
static void sample_live(RunCtx *c,
                        size_t ph) {
    if (c == nullptr ||
        c->r == nullptr) {
        return;
    }
    run_sample_tick(c->r);
    if (sample_slot(
            &c->tsample)) {
        auto_sample(c->r, ph);
    }
}

// pace_poll waits one 1/rate period
// while pumping TUNs, draining logs,
// and emitting live series on the
// sample throttle. Replaces blind
// pace sleeps in publish loops so
// sampling and log flow never stall
// mid-burst and cadence stays fixed.
// Rate below 1 returns at once. The
// sub-ms tail sleeps out instead of
// poll-spinning.
static void pace_poll(RunCtx *c,
                      size_t ph,
                      long long rate) {
    unsigned long long period;
    unsigned long long deadline;

    if (c == nullptr ||
        c->r == nullptr || rate < 1) {
        return;
    }
    period = 1000000000ULL /
             (unsigned long long)rate;
    deadline = bench_now_ns() +
               period;
    for (;;) {
        unsigned long long now =
            bench_now_ns();
        unsigned long long rem;
        int ms;

        if (now >= deadline) {
            break;
        }
        rem = deadline - now;
        if (rem < 1000000ULL) {
            struct timespec ts;

            ts.tv_sec = 0;
            ts.tv_nsec = (long)rem;
            (void)nanosleep(&ts,
                            nullptr);
            break;
        }
        ms = rem / 1000000ULL >
                     100
                 ? 100
                 : (int)(rem /
                         1000000ULL);
        run_poll_ms(c->r,
                    poll_drop,
                    poll_log, c->pc,
                    ms);
        sample_live(c, ph);
    }
}

// cmp_u64 orders latencies for
// qsort, ascending.
static int cmp_u64(const void *a,
                   const void *b) {
    unsigned long long x;
    unsigned long long y;

    x = *(const unsigned long long *)
        a;
    y = *(const unsigned long long *)
        b;
    if (x < y) {
        return (-1);
    }
    if (x > y) {
        return (1);
    }
    return (0);
}

// bench_pct picks sorted[n] at pct
// 0..100 by nearest rank: rank
// ceil(pct*n/100), idx rank-1.
// Empty reads 0, pct 0 reads min,
// n 1 reads sole. Pct times n
// fits: pct capped at 100, n at
// 1000 by lint, far below ULL.
static unsigned long long bench_pct(
    unsigned long long *sv, size_t n,
    int pct) {
    unsigned long long rank;
    size_t idx;

    if (sv == nullptr || n == 0) {
        return (0);
    }
    if (pct <= 0) {
        return (sv[0]);
    }
    if (pct > 100) {
        pct = 100;
    }
    rank = ((unsigned long long)pct *
            (unsigned long long)n +
            99ULL) / 100ULL;
    if (rank == 0) {
        rank = 1;
    }
    if (rank >
        (unsigned long long)n) {
        rank =
            (unsigned long long)n;
    }
    idx = (size_t)(rank - 1ULL);
    return (sv[idx]);
}

// bench_key emits one bench metric
// through the shared metric site,
// so bytes match phase.<i>.ms.
static void bench_key(size_t ph,
                      size_t k,
                      const char *leaf,
                      unsigned long long
                          val) {
    char key[96];

    (void)snprintf(key, sizeof(key),
                   "bench.%zu.%zu.%s",
                   ph, k, leaf);
    metric_u64(ph, key, val);
}

// scrape_cmp applies op to got vs want.
static bool scrape_cmp(const char *op,
                       double got,
                       double want) {
    if (op == nullptr) {
        return (false);
    }
    if (strcmp(op, "eq") == 0) {
        return (got == want);
    }
    if (strcmp(op, "ne") == 0) {
        return (got != want);
    }
    if (strcmp(op, "lt") == 0) {
        return (got < want);
    }
    if (strcmp(op, "le") == 0) {
        return (got <= want);
    }
    if (strcmp(op, "gt") == 0) {
        return (got > want);
    }
    if (strcmp(op, "ge") == 0) {
        return (got >= want);
    }
    return (false);
}

// scrape_sum parses Prometheus text,
// summing series whose base name
// matches metric. Base ends at
// brace or space; labels collapse
// into the sum. True when found.
static bool scrape_sum(const char *text,
                       const char *metric,
                       double *sum) {
    const char *p = nullptr;
    double acc = 0.0;
    bool hit = false;

    if (text == nullptr ||
        metric == nullptr ||
        sum == nullptr) {
        return (false);
    }
    p = text;
    while (*p != 0) {
        const char *e = nullptr;
        size_t ml = 0;
        char base[128];
        size_t bi = 0;
        const char *v = nullptr;
        double d = 0.0;
        char *end = nullptr;

        e = strchr(p, '\n');
        if (e == nullptr) {
            e = p + strlen(p);
        }
        while (*p == ' ' ||
               *p == '	') {
            p++;
        }
        if (*p == 0 || *p == '#' ||
            p >= e) {
            p = *e == 0 ? e : e + 1;
            continue;
        }
        ml = strlen(metric);
        if (strncmp(p, metric, ml) !=
            0) {
            p = *e == 0 ? e : e + 1;
            continue;
        }
        if (p[ml] != '{' &&
            p[ml] != ' ' &&
            p[ml] != '	') {
            p = *e == 0 ? e : e + 1;
            continue;
        }
        bi = 0;
        while (bi < ml &&
               bi + 1 < sizeof(base)) {
            base[bi] = p[bi];
            bi++;
        }
        base[bi] = 0;
        (void)base;
        v = p + ml;
        while (v < e && *v != ' ' &&
               *v != '	') {
            v++;
        }
        while (v < e &&
               (*v == ' ' ||
                *v == '	')) {
            v++;
        }
        if (v >= e) {
            p = *e == 0 ? e : e + 1;
            continue;
        }
        d = strtod(v, &end);
        if (end == v) {
            p = *e == 0 ? e : e + 1;
            continue;
        }
        acc += d;
        hit = true;
        p = *e == 0 ? e : e + 1;
    }
    if (!hit) {
        return (false);
    }
    *sum = acc;
    return (true);
}

// scrape_gate fetches the endpoint in
// the observe netns and checks the
// threshold. Emits scrape_ok 1/0.
// Unreachable or missing metric
// reads as gate fail, so the same
// YAML passes once the SUT exposes
// the endpoint.
static bool scrape_gate(RunCtx *c,
                        const ExStep *s,
                        size_t ph,
                        size_t k) {
    char url[256];
    char ns[80];
    char cmd[384];
    FILE *fp = nullptr;
    char *buf = nullptr;
    size_t cap = 65536;
    size_t len = 0;
    double sum = 0.0;
    bool ok = false;
    int m = 0;

    if (c == nullptr || s == nullptr ||
        c->net == nullptr) {
        return (false);
    }
    m = snprintf(url, sizeof(url),
                 "http://127.0.0.1:%lld%s",
                 s->exp_port,
                 s->exp_path);
    if (m < 0 ||
        (size_t)m >= sizeof(url)) {
        bench_key(ph, k,
                  "scrape_ok", 0ULL);
        return (false);
    }
    if (c->net->isolate_all == 0 &&
        s->obs == 0) {
        m = snprintf(cmd, sizeof(cmd),
                     "curl -s --max-time 5 "
                     "'%s'", url);
    } else {
        m = snprintf(ns, sizeof(ns),
                     "%s%d",
                     c->net->ns_prefix,
                     s->obs);
        if (m < 0 ||
            (size_t)m >= sizeof(ns)) {
            bench_key(ph, k,
                      "scrape_ok", 0ULL);
            return (false);
        }
        m = snprintf(cmd, sizeof(cmd),
                     "ip netns exec %s curl "
                     "-s --max-time 5 '%s'",
                     ns, url);
    }
    if (m < 0 ||
        (size_t)m >= sizeof(cmd)) {
        bench_key(ph, k,
                  "scrape_ok", 0ULL);
        return (false);
    }
    buf = calloc(1, cap);
    if (buf == nullptr) {
        bench_key(ph, k,
                  "scrape_ok", 0ULL);
        return (false);
    }
    fp = popen(cmd, "r");
    if (fp == nullptr) {
        free(buf);
        bench_key(ph, k,
                  "scrape_ok", 0ULL);
        return (false);
    }
    {
        size_t r = 0;

        r = fread(buf, 1, cap - 1, fp);
        len = r;
        buf[len] = 0;
    }
    (void)pclose(fp);
    if (!scrape_sum(buf, s->exp_metric,
                    &sum)) {
        free(buf);
        bench_key(ph, k,
                  "scrape_ok", 0ULL);
        return (false);
    }
    free(buf);
    ok = scrape_cmp(s->exp_op, sum,
                    s->exp_thresh);
    bench_key(ph, k, "scrape_ok",
              ok ? 1ULL : 0ULL);
    return (ok);
}

// bench_want reports intended
// issues: count bound, or rate by
// duration_s, or the lower when
// both bind. Rate 1..1e6 by dur
// 1..3600 fits ULL; lint caps the
// product, no wrap.
static unsigned long long bench_want(
    const ExStep *s) {
    unsigned long long by_rate;
    unsigned long long by_count;

    if (s == nullptr) {
        return (0);
    }
    by_rate = 0;
    by_count = 0;
    if (s->has_dur) {
        by_rate =
            (unsigned long long)s
                ->rate *
            (unsigned long long)s
                ->duration_s;
    }
    if (s->has_count) {
        by_count =
            (unsigned long long)s
                ->count;
    }
    if (s->has_count &&
        s->has_dur) {
        return (by_count < by_rate
                    ? by_count
                    : by_rate);
    }
    if (s->has_count) {
        return (by_count);
    }
    return (by_rate);
}

// tpl_expand writes template tpl to
// out, swapping {{seq}},
// {{send_ns}}, {{body}}, {{wid}}
// for the per-issue values. False
// on truncation or bad shape.
static bool tpl_expand(const char *tpl,
                       size_t seq,
                       unsigned long long
                           send_ns,
                       const char *body,
                       const char *wid,
                       char *out,
                       size_t n) {
    size_t o = 0;
    size_t i = 0;

    if (tpl == nullptr ||
        out == nullptr || n == 0) {
        return (false);
    }
    if (body == nullptr) {
        body = "";
    }
    if (wid == nullptr) {
        wid = "";
    }
    for (i = 0; tpl[i] != '\0';
         i++) {
        char nm[32];
        size_t k = 0;
        size_t j;
        const char *rep = nullptr;
        char num[32];

        if (tpl[i] != '{' ||
            tpl[i + 1] != '{') {
            if (o + 1 >= n) {
                return (false);
            }
            out[o++] = tpl[i];
            continue;
        }
        j = i + 2;
        for (; tpl[j] != '\0' &&
             k + 1 < sizeof(nm);
             j++) {
            if (tpl[j] == '}' &&
                tpl[j + 1] == '}') {
                break;
            }
            nm[k++] = tpl[j];
        }
        nm[k] = '\0';
        if (tpl[j] == '\0' ||
            tpl[j + 1] != '}') {
            return (false);
        }
        i = j + 1;
        if (strcmp(nm, "seq") ==
            0) {
            (void)snprintf(
                num, sizeof(num),
                "%zu", seq);
            rep = num;
        } else if (strcmp(
                       nm,
                       "send_ns") == 0) {
            (void)snprintf(
                num, sizeof(num),
                "%llu", send_ns);
            rep = num;
        } else if (strcmp(
                       nm, "body") ==
            0) {
            rep = body;
        } else if (strcmp(
                       nm, "wid") == 0) {
            rep = wid;
        } else {
            return (false);
        }
        {
            size_t rl = strlen(rep);

            if (o + rl + 1 > n) {
                return (false);
            }
            (void)memcpy(out + o,
                         rep, rl);
            o += rl;
        }
    }
    if (o + 1 > n) {
        return (false);
    }
    out[o] = '\0';
    return (true);
}

// bench_weld_pay builds the exact
// payload issued for seq j with
// send_ns: bench#seq#send_ns#body
// where body is N x bytes when
// payload_bytes is set, else user
// text. Same shape as the issue
// loop, so matching the full
// prefix cannot confuse user text
// holding another tag. Old
// two-field tags never build here.
static bool bench_weld_pay(
    const ExStep *s, size_t j,
    unsigned long long send_ns,
    char *out, size_t n) {
    char tag[64];
    int m;

    if (s == nullptr ||
        out == nullptr || n == 0) {
        return (false);
    }
    if (s->has_generic &&
        s->send_tpl[0] != '\0') {
        char body[kBenchPay];
        size_t k;

        if (s->has_pay) {
            size_t nb;

            nb = (size_t)
                s->pay_bytes;
            if (nb >=
                sizeof(body)) {
                return (false);
            }
            for (k = 0; k < nb;
                 k++) {
                body[k] = 'x';
            }
            body[nb] = '\0';
        } else {
            if (!copy_text(
                    body,
                    sizeof(body),
                    s->text)) {
                return (false);
            }
        }
        return (tpl_expand(
            s->send_tpl, j, send_ns,
            body, "", out, n));
    }
    m = snprintf(tag, sizeof(tag),
                 "bench#%zu#%llu#", j,
                 send_ns);
    if (m < 0 ||
        m >= (int)sizeof(tag)) {
        return (false);
    }
    if (s->has_pay) {
        size_t tl = strlen(tag);
        size_t k;

        if (tl + (size_t)
                s->pay_bytes +
            1 > n) {
            return (false);
        }
        (void)memcpy(out, tag, tl);
        for (k = 0;
             k < (size_t)
                 s->pay_bytes;
             k++) {
            out[tl + k] = 'x';
        }
        out[tl + (size_t)
                    s->pay_bytes] =
            '\0';
        return (true);
    }
    m = snprintf(out, n, "%s%s", tag,
                 s->text);
    if (m < 0 || m >= (int)n) {
        return (false);
    }
    return (true);
}

// bench_weld_seen reports the weld
// log plus ROS log holding needle,
// mirroring observed_on exactly.
static bool bench_weld_seen(
    RunCtx *c, int node,
    const char *needle) {
    char path[kPathLen];

    if (c == nullptr ||
        needle == nullptr) {
        return (false);
    }
    log_path(path, sizeof(path),
             c->statedir, true,
             node);
    if (!file_has(path, needle)) {
        return (false);
    }
    if (node < 0 ||
        node >= kMaxNodes ||
        !c->ros_on[node]) {
        return (true);
    }
    (void)snprintf(path,
                   sizeof(path),
                   "%s/ros-%d.log",
                   c->statedir,
                   node);
    return (file_has(path, needle));
}

// bench_obs_seen reports needle seen
// for an endpoint kind: weld dual
// (weld log plus ROS log where a
// peer runs, legacy) or ROS only.
// Dispatch on capability, not SUT
// kind; adding a kind is a table
// row.
static bool bench_obs_seen(
    RunCtx *c, int kind, int node,
    const char *needle) {
    char path[kPathLen];

    if (kind < 0 || kind > 2) {
        return (false);
    }
    if (needle == nullptr ||
        c == nullptr) {
        return (false);
    }
    if (!kEpCap[kind].obs_weld &&
        !kEpCap[kind].obs_ros) {
        return (false);
    }
    if (kEpCap[kind].obs_weld &&
        kEpCap[kind].obs_ros) {
        return (bench_weld_seen(
            c, node, needle));
    }
    if (!kEpCap[kind].obs_weld &&
        kEpCap[kind].obs_ros) {
        if (node < 0 ||
            node >= kMaxNodes) {
            return (false);
        }
        (void)snprintf(
            path, sizeof(path),
            "%s/ros-%d.log",
            c->statedir, node);
        return (file_has(path,
                         needle));
    }
    if (node < 0 ||
        node >= kMaxNodes) {
        return (false);
    }
    log_path(path, sizeof(path),
             c->statedir, true,
             node);
    return (file_has(path, needle));
}

// bench_conc_seen reports wid listed
// on any node by full id, so 8-char
// prefixes cannot collide. Output
// caps at OUT_LEN may still miss;
// the grace dwell covers it.
static bool bench_conc_seen(
    RunCtx *c, const char *wid) {
    int n;
    int i;

    if (c == nullptr ||
        wid == nullptr ||
        c->f == nullptr) {
        return (false);
    }
    if (wid[0] == '\0') {
        return (false);
    }
    n = (int)c->f->nodes;
    for (i = 0; i < n; i++) {
        if (concord_list_has(
                c->statedir, i,
                wid)) {
            return (true);
        }
    }
    return (false);
}

// bench_journal_seen reports the
// expect node journal holding field
// equal to value_tpl expanded with
// wid (plus seq, send_ns). False on
// any miss; nonsense sends with no
// wid never hit, so they read as
// 100 percent loss honestly.
static bool bench_journal_seen(
    RunCtx *c, int node,
    const char *field,
    const char *value_tpl,
    size_t seq,
    unsigned long long send_ns,
    const char *wid) {
    char want[256];
    char got[128];

    if (c == nullptr ||
        field == nullptr ||
        value_tpl == nullptr ||
        wid == nullptr) {
        return (false);
    }
    if (wid[0] == '\0') {
        return (false);
    }
    if (!tpl_expand(
            value_tpl, seq, send_ns,
            "", wid, want,
            sizeof(want))) {
        return (false);
    }
    if (strcmp(field,
               "Image") == 0) {
        if (!read_image(
                c->statedir, node,
                wid, got,
                sizeof(got))) {
            return (false);
        }
        return (strcmp(got, want) ==
                0);
    }
    if (strcmp(field,
               "AssignedNodeID") ==
        0) {
        if (!read_assignment(
                c->statedir, node,
                wid, got,
                sizeof(got))) {
            return (false);
        }
        return (strcmp(got, want) ==
                0);
    }
    {
        unsigned long long ev;
        unsigned long long wv;
        char *end = nullptr;

        if (!read_epoch(
                c->statedir, node,
                wid, &ev)) {
            return (false);
        }
        errno = 0;
        wv = strtoull(want, &end,
                      10);
        if (errno != 0 ||
            end == nullptr ||
            *end != '\0') {
            return (false);
        }
        return (ev == wv);
    }
}

// generic_miss reports a nonsense
// pair: weld-family send with a
// concord-family expect, or the
// reverse. Scrape never misses:
// it gates any family. True means
// always miss, so the step measures
// 100 percent loss through the
// same stats.
static bool generic_miss(
    const ExStep *s) {
    bool send_c;
    bool exp_c;

    if (s == nullptr ||
        !s->has_generic) {
        return (false);
    }
    if (strcmp(s->expect_via,
               "prometheus-scrape") ==
        0) {
        return (false);
    }
    send_c = strcmp(s->send_via,
                    "workload-submit") ==
        0;
    exp_c = strcmp(s->expect_via,
                   "journal-has") ==
            0 ||
        strcmp(s->expect_via,
               "workload-running") ==
            0;
    return (send_c != exp_c);
}

// bench_stats sorts hits ms ascending
// then nearest rank, and emits
// count, loss, p50, p99, max, then
// target plus overrun, all through
// the metric site only. Each id
// counts once; clock-zero misses.
// New keys ride the same site:
// rate_ach, per-message lat capped
// at 2000 with a truncated mark,
// plus hist buckets 0..9 under
// kHistEdge. Us leaves mirror ms:
// lat_us, hist_us, p50.us, p99.us,
// max.us under kHistUsEdge. Old
// leaves never move.
static void bench_stats(
    size_t ph, size_t k,
    unsigned long long *hits,
    unsigned long long *hits_us,
    size_t nhit,
    unsigned long long loss,
    unsigned long long total,
    unsigned long long target,
    unsigned long long overrun,
    unsigned long long rate_ach) {
    unsigned long long p50;
    unsigned long long p99;
    unsigned long long max;
    unsigned long long u50;
    unsigned long long u99;
    unsigned long long umax;
    unsigned long long hc[10];
    unsigned long long hu[10];
    size_t hi;
    size_t li;
    size_t cap;

    bench_key(ph, k, "rate_ach",
              rate_ach);
    cap = nhit > (size_t)kLatCap
              ? (size_t)kLatCap
              : nhit;
    for (li = 0; li < cap; li++) {
        bench_key(ph, k, "lat",
                  hits[li]);
    }
    for (li = 0; li < cap; li++) {
        bench_key(ph, k, "lat_us",
                  hits_us[li]);
    }
    if (nhit > (size_t)kLatCap) {
        bench_key(ph, k, "truncated",
                  1ULL);
    }
    for (hi = 0; hi < 10; hi++) {
        hc[hi] = 0;
        hu[hi] = 0;
    }
    for (hi = 0; hi < nhit; hi++) {
        int b;
        int u;

        b = hist_bucket(hits[hi]);
        if (b >= 0 && b < kHistN) {
            hc[(size_t)b]++;
        }
        u = hist_bucket_us(
            hits_us[hi]);
        if (u >= 0 && u < kHistN) {
            hu[(size_t)u]++;
        }
    }
    for (hi = 0; hi <
         (size_t)kHistN; hi++) {
        char leaf[16];

        if (hc[hi] == 0) {
            continue;
        }
        (void)snprintf(leaf,
                       sizeof(leaf),
                       "hist.%zu",
                       hi);
        bench_key(ph, k, leaf,
                  hc[hi]);
    }
    for (hi = 0; hi <
         (size_t)kHistN; hi++) {
        char leaf[16];

        if (hu[hi] == 0) {
            continue;
        }
        (void)snprintf(leaf,
                       sizeof(leaf),
                       "hist_us.%zu",
                       hi);
        bench_key(ph, k, leaf,
                  hu[hi]);
    }
    if (nhit > 1) {
        qsort(hits, nhit,
              sizeof(*hits),
              cmp_u64);
        qsort(hits_us, nhit,
              sizeof(*hits_us),
              cmp_u64);
    }
    p50 = bench_pct(hits, nhit, 50);
    p99 = bench_pct(hits, nhit, 99);
    max = nhit > 0 ? hits[nhit - 1]
                   : 0;
    u50 = bench_pct(hits_us, nhit,
                    50);
    u99 = bench_pct(hits_us, nhit,
                    99);
    umax = nhit > 0
               ? hits_us[nhit - 1]
               : 0;
    bench_key(ph, k, "count",
              total);
    bench_key(ph, k, "loss", loss);
    bench_key(ph, k, "p50.ms",
              p50);
    bench_key(ph, k, "p99.ms",
              p99);
    bench_key(ph, k, "max.ms",
              max);
    bench_key(ph, k, "p50.us",
              u50);
    bench_key(ph, k, "p99.us",
              u99);
    bench_key(ph, k, "max.us",
              umax);
    bench_key(ph, k, "target",
              target);
    bench_key(ph, k, "overrun",
              overrun);
}

// ramp_key emits one ramp step metric
// through the shared metric site, so
// bytes match phase.<i>.ms. Leaf set
// mirrors bench_stats summary only
// (no per-message lat/hist), keeping
// logs bounded: 10 steps times 2000
// lat would be 20k lines. Summary
// suffices for the ceiling choice.
static void ramp_key(size_t ph,
                     size_t k,
                     size_t step,
                     const char *leaf,
                     unsigned long long
                         val) {
    char key[96];

    (void)snprintf(key, sizeof(key),
                   "bench.%zu.%zu.ramp."
                   "%zu.%s",
                   ph, k, step, leaf);
    metric_u64(ph, key, val);
}

// ramp_stats sorts hits then emits
// count, loss, p50, p99, max, target,
// overrun plus rate_ach under the
// ramp prefix. No lat/hist, see
// ramp_key. Old bench leaves never
// move; ramp adds only. Us mirrors
// ms for sub-ms spread.
static void ramp_stats(
    size_t ph, size_t k, size_t step,
    unsigned long long *hits,
    unsigned long long *hits_us,
    size_t nhit,
    unsigned long long loss,
    unsigned long long total,
    unsigned long long target,
    unsigned long long overrun,
    unsigned long long rate_ach) {
    unsigned long long p50;
    unsigned long long p99;
    unsigned long long max;
    unsigned long long u50;
    unsigned long long u99;
    unsigned long long umax;

    if (nhit > 1) {
        qsort(hits, nhit,
              sizeof(*hits),
              cmp_u64);
        qsort(hits_us, nhit,
              sizeof(*hits_us),
              cmp_u64);
    }
    p50 = bench_pct(hits, nhit, 50);
    p99 = bench_pct(hits, nhit, 99);
    max = nhit > 0 ? hits[nhit - 1]
                   : 0;
    u50 = bench_pct(hits_us, nhit,
                    50);
    u99 = bench_pct(hits_us, nhit,
                    99);
    umax = nhit > 0
               ? hits_us[nhit - 1]
               : 0;
    ramp_key(ph, k, step, "count",
             total);
    ramp_key(ph, k, step, "loss",
             loss);
    ramp_key(ph, k, step, "p50.ms",
             p50);
    ramp_key(ph, k, step, "p99.ms",
             p99);
    ramp_key(ph, k, step, "max.ms",
             max);
    ramp_key(ph, k, step, "p50.us",
             u50);
    ramp_key(ph, k, step, "p99.us",
             u99);
    ramp_key(ph, k, step, "max.us",
             umax);
    ramp_key(ph, k, step, "target",
             target);
    ramp_key(ph, k, step, "overrun",
             overrun);
    ramp_key(ph, k, step, "rate_ach",
             rate_ach);
}

// run_bench_weld paces pubs with a
// bench#seq#send_ns#body tag, body
// is x fill or user text, then
// pumps to a 10 s grace while full
// payloads surface in the observed
// path. From ros:N publishes via
// ROS, else via weld. Echo runs a
// pong relay on echo_node for the
// step; reply topic is always
// <topic>/pong and RTT is reply
// minus send. Loss plus overrun
// are metrics, never step fails.
static bool run_bench_weld(
    const ExStep *s, RunCtx *c,
    size_t ph, size_t k, char *why,
    size_t n) {
    static unsigned long long pub
        [1000];
    static unsigned long long seen
        [1000];
    static unsigned long long hits
        [1000];
    static unsigned long long hits_us
        [1000];
    unsigned long long deadline;
    unsigned long long grace;
    size_t total = 0;
    size_t i;
    size_t nhit = 0;
    unsigned long long loss = 0;
    unsigned long long start;
    unsigned long long end;
    unsigned long long rate_ach;
    const char *topic;
    const char *base;
    char dom[16];
    bool relay_on = false;
    int m;

    if (s == nullptr || c == nullptr) {
        return (false);
    }
    topic = s->has_topic ? s->topic
                         : nullptr;
    base = s->has_topic ? s->topic
                        : c->topic;
    if (s->has_echo) {
        m = snprintf(dom,
                     sizeof(dom),
                     "%d", c->domain);
        if (m < 0 ||
            m >= (int)sizeof(dom)) {
            (void)snprintf(
                why, n,
                "bad domain");
            return (false);
        }
        (void)setenv(
            "ROS_DOMAIN_ID", dom,
            1);
        if (!weld_relay_start(
                c->r,
                s->echo_node,
                base,
                c->weld_bin)) {
            (void)snprintf(
                why, n,
                "relay start on "
                "node %d failed",
                s->echo_node);
            return (false);
        }
        relay_on = true;
    }
    if (kEpCap[s->from_kind].pub_ros) {
        m = snprintf(dom,
                     sizeof(dom),
                     "%d", c->domain);
        if (m < 0 ||
            m >= (int)sizeof(dom)) {
            if (relay_on) {
                weld_relay_stop(
                    c->r,
                    s->echo_node);
            }
            (void)snprintf(
                why, n,
                "bad domain");
            return (false);
        }
        (void)setenv(
            "ROS_DOMAIN_ID", dom,
            1);
    }
    start = bench_now_ns();
    if (s->has_dur) {
        // Dur 1..3600 s times 1e9
        // fits ULL, no wrap.
        deadline = start +
                   (unsigned long long)
                       s->duration_s *
                   1000000000ULL;
    } else {
        deadline = 0;
    }
    for (i = 0; i < 1000; i++) {
        char pay[kBenchPay];
        unsigned long long snd;
        bool ok;

        if (s->has_count &&
            total >=
                (size_t)s->count) {
            break;
        }
        if (s->has_dur &&
            bench_now_ns() >=
                deadline) {
            break;
        }
        if (total > 0) {
            pace_poll(c, ph,
                      s->rate);
        }
        snd = bench_now_ns();
        if (!bench_weld_pay(
                s, total, snd, pay,
                sizeof(pay))) {
            if (relay_on) {
                weld_relay_stop(
                    c->r,
                    s->echo_node);
            }
            (void)snprintf(
                why, n,
                "benchmark payload "
                "too long");
            return (false);
        }
        if (kEpCap[s->from_kind].pub_ros) {
            ok = do_ros_pub_once(
                c, s->node, topic,
                pay);
        } else if (kEpCap[s->from_kind]
                        .pub_weld) {
            ok = do_pub_once(
                c, s->node, topic,
                pay);
        } else {
            if (relay_on) {
                weld_relay_stop(
                    c->r,
                    s->echo_node);
            }
            (void)snprintf(
                why, n,
                "%s cannot publish: "
                "no pub verb",
                ep_name(
                    s->from_kind));
            return (false);
        }
        if (!ok) {
            if (relay_on) {
                weld_relay_stop(
                    c->r,
                    s->echo_node);
            }
            (void)snprintf(
                why, n,
                "benchmark pub from "
                "node %d failed",
                s->node);
            return (false);
        }
        pub[total] = snd;
        seen[total] = 0;
        total++;
        sample_live(c, ph);
    }
    end = bench_now_ns();
    if (end >= start &&
        end - start > 0) {
        rate_ach = (unsigned long long)
                       total *
                   1000000000ULL /
                   (end - start);
    } else if (total > 0) {
        rate_ach = (unsigned long long)
            total;
    } else {
        rate_ach = 0;
    }
    // Grace dwell uses the second
    // cap times 1e9 ns, fits ULL.
    grace = bench_now_ns() +
            (unsigned long long)
                kBenchGrace *
            1000000000ULL;
    for (;;) {
        size_t miss = 0;
        size_t j;

        for (j = 0; j < total; j++) {
            char pay[kBenchPay];

            if (seen[j] != 0) {
                continue;
            }
            if (generic_miss(s)) {
                miss++;
                continue;
            }
            if (!bench_weld_pay(
                    s, j, pub[j],
                    pay,
                    sizeof(pay))) {
                miss++;
                continue;
            }
            if (bench_obs_seen(
                    c, s->on_kind,
                    s->obs, pay)) {
                seen[j] =
                    bench_now_ns();
                continue;
            }
            miss++;
        }
        if (miss == 0) {
            break;
        }
        if (bench_now_ns() >=
            grace) {
            break;
        }
        if (!run_alive(c->r, -1)) {
            break;
        }
        run_poll(c->r, poll_drop,
                 poll_log, c->pc);
        if (sample_slot(
                &c->tsample)) {
            auto_sample(c->r, ph);
        }
    }
    if (relay_on) {
        weld_relay_stop(c->r,
                        s->echo_node);
    }
    {
        size_t j;
        size_t first;
        unsigned long long want;
        unsigned long long target;
        unsigned long long overrun;
        bool gate_ok = true;

        if (s->has_generic &&
            strcmp(s->expect_via,
                   "prometheus-scrape") ==
                0) {
            gate_ok = scrape_gate(
                c, s, ph, k);
        }
        if (!gate_ok) {
            want = bench_want(s);
            first = (size_t)s->warmup;
            if (first > total) {
                first = total;
            }
            target = 0;
            overrun = 0;
            if (want >
                (unsigned long long)first) {
                target = want -
                    (unsigned long long)first;
            }
            if (want >
                (unsigned long long)total) {
                overrun = 1;
            }
            loss = total - first;
            nhit = 0;
            bench_stats(ph, k, hits,
                        hits_us, nhit, loss,
                        total - first,
                        target, overrun,
                        rate_ach);
        } else {
        want = bench_want(s);
        first = (size_t)s->warmup;
        if (first > total) {
            first = total;
        }
        target = 0;
        overrun = 0;
        if (want >
            (unsigned long long)first) {
            target = want -
                (unsigned long long)first;
        }
        if (want >
            (unsigned long long)total) {
            overrun = 1;
        }
        for (j = first; j < total;
             j++) {
            if (seen[j] == 0 ||
                seen[j] < pub[j] ||
                pub[j] == 0) {
                loss++;
                continue;
            }
            hits[nhit] =
                (seen[j] - pub[j]) /
                1000000ULL;
            hits_us[nhit] =
                (seen[j] - pub[j]) /
                1000ULL;
            nhit++;
        }
        bench_stats(ph, k, hits,
                    hits_us, nhit, loss,
                    total - first,
                    target, overrun,
                    rate_ach);
        }
    }
    printf("suite: step benchmark "
           "%zu\n",
           total);
    fflush(stdout);
    return (true);
}

// run_bench_conc paces submits under
// one base name, then pumps to a 10
// s grace while full wids list. Loss
// plus overrun are metrics, never a
// fail.
static bool run_bench_conc(
    const ExStep *s, RunCtx *c,
    size_t ph, size_t k, char *why,
    size_t n) {
    static unsigned long long pub
        [100];
    static unsigned long long seen
        [100];
    static unsigned long long hits
        [100];
    static unsigned long long hits_us
        [100];
    static char wids[100][64];
    unsigned long long deadline;
    unsigned long long grace;
    size_t total = 0;
    size_t i;
    size_t nhit = 0;
    unsigned long long loss = 0;
    unsigned long long start;
    unsigned long long end;
    unsigned long long rate_ach;
    bool multi;

    if (s == nullptr || c == nullptr) {
        return (false);
    }
    multi = s->has_dur ||
            !s->has_count ||
            s->count > 1;
    start = bench_now_ns();
    if (s->has_dur) {
        // Same bound as weld.
        deadline = start +
                   (unsigned long long)
                       s->duration_s *
                   1000000000ULL;
    } else {
        deadline = 0;
    }
    for (i = 0; i < 100; i++) {
        WlEnt *e;
        char name[kWlLen];
        const char *fl;

        if (s->has_count &&
            total >=
                (size_t)s->count) {
            break;
        }
        if (s->has_dur &&
            bench_now_ns() >=
                deadline) {
            break;
        }
        if (c->tab.n >=
            (size_t)kMaxWl) {
            break;
        }
        if (total > 0) {
            pace_poll(c, ph,
                      s->rate);
        }
        fl = s->has_flags ? s->flags
                          : nullptr;
        e = &c->tab.ent[c->tab.n];
        if (!do_submit_once(
                c, s->node,
                s->image, fl, e->wid,
                sizeof(e->wid))) {
            (void)snprintf(
                why, n,
                "benchmark submit "
                "on node %d failed",
                s->node);
            return (false);
        }
        // Lint owns the fit; this
        // guard keeps truncation
        // warnings quiet, no shape.
        if (strlen(s->as_name) + 4 >
            sizeof(name)) {
            (void)snprintf(
                why, n,
                "workload name too "
                "long for count");
            return (false);
        }
        if (multi) {
            (void)snprintf(
                name, sizeof(name),
                "%s-%zu", s->as_name,
                total);
        } else {
            (void)snprintf(
                name, sizeof(name),
                "%s", s->as_name);
        }
        (void)snprintf(
            e->name, sizeof(e->name),
            "%s", name);
        e->node = s->node;
        e->sub_ms = ev_now_ms();
        e->present_ms = 0;
        e->metric_done = false;
        (void)snprintf(
            wids[total],
            sizeof(wids[total]),
            "%s", e->wid);
        c->tab.n++;
        printf("suite: step submit "
               "%s\n",
               e->wid);
        pub[total] = bench_now_ns();
        seen[total] = 0;
        total++;
        sample_live(c, ph);
    }
    end = bench_now_ns();
    if (end >= start &&
        end - start > 0) {
        rate_ach = (unsigned long long)
                       total *
                   1000000000ULL /
                   (end - start);
    } else if (total > 0) {
        rate_ach = (unsigned long long)
            total;
    } else {
        rate_ach = 0;
    }
    if (multi && total > 1) {
        WlTab *t = &c->tab;

        (void)snprintf(
            t->bases[t->nbase],
            sizeof(t->bases[t->nbase]),
            "%s", s->as_name);
        t->bcount[t->nbase] =
            (long long)total;
        t->nbase++;
    }
    // Grace dwell uses the second
    // cap times 1e9 ns, fits ULL.
    grace = bench_now_ns() +
            (unsigned long long)
                kBenchGrace *
            1000000000ULL;
    for (;;) {
        size_t miss = 0;
        size_t j;

        for (j = 0; j < total; j++) {
            if (seen[j] != 0) {
                continue;
            }
            if (generic_miss(s)) {
                miss++;
                continue;
            }
            if (s->has_generic &&
                strcmp(
                    s->expect_via,
                    "journal-has") ==
                    0) {
                if (bench_journal_seen(
                        c, s->obs,
                        s->exp_field,
                        s->exp_value, j,
                        pub[j],
                        wids[j])) {
                    seen[j] =
                        bench_now_ns();
                    continue;
                }
                miss++;
                continue;
            }
            if (bench_conc_seen(
                    c, wids[j])) {
                seen[j] =
                    bench_now_ns();
                continue;
            }
            miss++;
        }
        if (miss == 0) {
            break;
        }
        if (bench_now_ns() >=
            grace) {
            break;
        }
        if (!run_alive(c->r, -1)) {
            break;
        }
        run_poll(c->r, poll_drop,
                 poll_log, c->pc);
        if (sample_slot(
                &c->tsample)) {
            auto_sample(c->r, ph);
        }
    }
    {
        size_t j;
        size_t first;
        unsigned long long want;
        unsigned long long target;
        unsigned long long overrun;
        bool gate_ok = true;

        if (s->has_generic &&
            strcmp(s->expect_via,
                   "prometheus-scrape") ==
                0) {
            gate_ok = scrape_gate(
                c, s, ph, k);
        }
        if (!gate_ok) {
            want = bench_want(s);
            first = (size_t)s->warmup;
            if (first > total) {
                first = total;
            }
            target = 0;
            overrun = 0;
            if (want >
                (unsigned long long)first) {
                target = want -
                    (unsigned long long)first;
            }
            if (want >
                (unsigned long long)total) {
                overrun = 1;
            }
            loss = total - first;
            nhit = 0;
            bench_stats(ph, k, hits,
                        hits_us, nhit, loss,
                        total - first,
                        target, overrun,
                        rate_ach);
        } else {
        want = bench_want(s);
        first = (size_t)s->warmup;
        if (first > total) {
            first = total;
        }
        target = 0;
        overrun = 0;
        if (want >
            (unsigned long long)first) {
            target = want -
                (unsigned long long)first;
        }
        if (want >
            (unsigned long long)total) {
            overrun = 1;
        }
        for (j = first; j < total;
             j++) {
            if (seen[j] == 0 ||
                seen[j] < pub[j] ||
                pub[j] == 0) {
                loss++;
                continue;
            }
            hits[nhit] =
                (seen[j] - pub[j]) /
                1000000ULL;
            hits_us[nhit] =
                (seen[j] - pub[j]) /
                1000ULL;
            nhit++;
        }
        bench_stats(ph, k, hits,
                    hits_us, nhit, loss,
                    total - first,
                    target, overrun,
                    rate_ach);
        }
    }
    printf("suite: step benchmark "
           "%zu\n",
           total);
    fflush(stdout);
    return (true);
}

// run_ramp_weld runs an adaptive
// ceiling ladder: start_rate,
// plus step_rate per step, each step
// paced for step_duration_s, stop on
// loss_pct and/or p99_ms, plus hard
// backstops max_rate, max_steps and
// max_duration_s. Boundedness proof:
// loop i below max_steps (1..10) and
// elapsed below max_duration
// (1..3600 s) and rate at or below
// max_rate; each iter does one paced
// step bounded by step_duration
// (1..3600 s) plus 10 s grace; rate
// grows by step_rate at least 1, so
// it hits max_rate in finite steps,
// capped by max_steps; i plus elapsed
// plus rate all monotonic, so no
// infinite loop is possible.
static bool run_ramp_weld(
    const ExStep *s, RunCtx *c,
    size_t ph, size_t k, char *why,
    size_t n) {
    static unsigned long long rpub
        [1000];
    static unsigned long long rseen
        [1000];
    static unsigned long long rhits
        [1000];
    static unsigned long long rhits_us
        [1000];
    unsigned long long ramp_start;
    unsigned long long ceiling = 0;
    const char *reason = "max_steps";
    const char *topic = nullptr;
    const char *base = nullptr;
    char dom[16];
    bool relay_on = false;
    size_t step = 0;
    int m = 0;

    if (s == nullptr || c == nullptr) {
        return (false);
    }
    if (!kEpCap[s->from_kind].pub_weld &&
        !kEpCap[s->from_kind].pub_ros) {
        (void)snprintf(
            why, n,
            "%s cannot publish: no "
            "pub verb",
            ep_name(s->from_kind));
        return (false);
    }
    if (!kEpCap[s->on_kind].obs_weld &&
        !kEpCap[s->on_kind].obs_ros) {
        (void)snprintf(
            why, n,
            "%s cannot observe: no "
            "log verb",
            ep_name(s->on_kind));
        return (false);
    }
    topic = s->has_topic ? s->topic
                         : nullptr;
    base = s->has_topic ? s->topic
                        : c->topic;
    if (s->has_echo) {
        if (!kEpCap[s->echo_relay_kind]
                 .relay) {
            (void)snprintf(
                why, n,
                "%s cannot relay: no "
                "relay verb",
                ep_name(
                    s->echo_relay_kind));
            return (false);
        }
        m = snprintf(dom,
                     sizeof(dom),
                     "%d", c->domain);
        if (m < 0 ||
            m >= (int)sizeof(dom)) {
            (void)snprintf(
                why, n,
                "bad domain");
            return (false);
        }
        (void)setenv(
            "ROS_DOMAIN_ID", dom,
            1);
        if (!weld_relay_start(
                c->r,
                s->echo_node,
                base,
                c->weld_bin)) {
            (void)snprintf(
                why, n,
                "relay start on "
                "node %d failed",
                s->echo_node);
            return (false);
        }
        relay_on = true;
    }
    if (kEpCap[s->from_kind].pub_ros) {
        m = snprintf(dom,
                     sizeof(dom),
                     "%d", c->domain);
        if (m < 0 ||
            m >= (int)sizeof(dom)) {
            if (relay_on) {
                weld_relay_stop(
                    c->r,
                    s->echo_node);
            }
            (void)snprintf(
                why, n,
                "bad domain");
            return (false);
        }
        (void)setenv(
            "ROS_DOMAIN_ID", dom,
            1);
    }
    ramp_start = bench_now_ns();
    for (step = 0;
         step <
         (size_t)s->ramp_max_steps;
         step++) {
        unsigned long long rate = 0;
        unsigned long long want = 0;
        unsigned long long deadline =
            0;
        unsigned long long grace = 0;
        unsigned long long start = 0;
        unsigned long long end = 0;
        unsigned long long rach = 0;
        unsigned long long loss = 0;
        unsigned long long over = 0;
        unsigned long long targ = 0;
        size_t total = 0;
        size_t nhit = 0;
        size_t j = 0;
        size_t i = 0;

        rate = (unsigned long long)
                s->ramp_start +
            (unsigned long long)step *
                (unsigned long long)
                    s->ramp_step;
        if (rate >
            (unsigned long long)
                s->ramp_max_rate) {
            rate = (unsigned long long)
                s->ramp_max_rate;
        }
        if (bench_now_ns() - ramp_start >=
            (unsigned long long)
                s->ramp_max_dur *
                1000000000ULL) {
            reason = "max_duration";
            break;
        }
        want = rate *
            (unsigned long long)
                s->ramp_step_dur;
        start = bench_now_ns();
        deadline = start +
            (unsigned long long)
                s->ramp_step_dur *
                1000000000ULL;
        for (i = 0; i < 1000; i++) {
            char pay[kBenchPay];
            unsigned long long snd = 0;
            bool ok = false;

            if (total >= (size_t)want) {
                break;
            }
            if (bench_now_ns() >=
                deadline) {
                break;
            }
            if (total > 0) {
                pace_poll(c, ph,
                            (long long)rate);
            }
            snd = bench_now_ns();
            if (!bench_weld_pay(
                    s, total, snd,
                    pay,
                    sizeof(pay))) {
                if (relay_on) {
                    weld_relay_stop(
                        c->r,
                        s->echo_node);
                }
                (void)snprintf(
                    why, n,
                    "benchmark payload "
                    "too long");
                return (false);
            }
            if (kEpCap[s->from_kind]
                     .pub_ros) {
                ok = do_ros_pub_once(
                    c, s->node, topic,
                    pay);
            } else {
                ok = do_pub_once(
                    c, s->node, topic,
                    pay);
            }
            if (!ok) {
                if (relay_on) {
                    weld_relay_stop(
                        c->r,
                        s->echo_node);
                }
                (void)snprintf(
                    why, n,
                    "benchmark pub "
                    "from node %d "
                    "failed",
                    s->node);
                return (false);
            }
            rpub[total] = snd;
            rseen[total] = 0;
            total++;
            sample_live(c, ph);
        }
        end = bench_now_ns();
        if (end >= start &&
            end - start > 0) {
            rach = (unsigned long long)
                    total *
                1000000000ULL /
                (end - start);
        } else if (total > 0) {
            rach = (unsigned long long)
                total;
        } else {
            rach = 0;
        }
        grace = bench_now_ns() +
            (unsigned long long)
                kBenchGrace *
                1000000000ULL;
        for (;;) {
            size_t miss = 0;

            for (j = 0; j < total;
                 j++) {
                char pay[kBenchPay];

                if (rseen[j] != 0) {
                    continue;
                }
                if (generic_miss(s)) {
                    miss++;
                    continue;
                }
                if (!bench_weld_pay(
                        s, j, rpub[j],
                        pay,
                        sizeof(pay))) {
                    miss++;
                    continue;
                }
                if (bench_obs_seen(
                        c, s->on_kind,
                        s->obs,
                        pay)) {
                    rseen[j] =
                        bench_now_ns();
                    continue;
                }
                miss++;
            }
            if (miss == 0) {
                break;
            }
            if (bench_now_ns() >=
                grace) {
                break;
            }
            if (!run_alive(c->r, -1)) {
                break;
            }
            run_poll(c->r, poll_drop,
                     poll_log, c->pc);
            if (sample_slot(
                    &c->tsample)) {
                auto_sample(c->r, ph);
            }
        }
        targ = want;
        over = want > (unsigned long long)
                       total
            ? 1
            : 0;
        for (j = 0; j < total; j++) {
            if (rseen[j] == 0 ||
                rseen[j] < rpub[j] ||
                rpub[j] == 0) {
                loss++;
                continue;
            }
            rhits[nhit] =
                (rseen[j] - rpub[j]) /
                1000000ULL;
            rhits_us[nhit] =
                (rseen[j] - rpub[j]) /
                1000ULL;
            nhit++;
        }
        if (s->has_generic &&
            strcmp(s->expect_via,
                   "prometheus-scrape") ==
                0 &&
            !scrape_gate(c, s, ph, k)) {
            loss = total;
            nhit = 0;
        }
        ramp_stats(ph, k, step,
                   rhits, rhits_us,
                   nhit, loss,
                   total, targ, over,
                   rach);
        {
            unsigned long long lpct = 0;
            unsigned long long p99 = 0;

            if (total > 0) {
                lpct = loss * 100ULL /
                    (unsigned long long)
                        total;
            } else {
                lpct = 100;
            }
            p99 = bench_pct(rhits,
                            nhit, 99);
            if (s->ramp_has_loss &&
                lpct >
                    (unsigned long long)
                        s->ramp_loss_pct) {
                reason = "loss";
                break;
            }
            if (s->ramp_has_p99 &&
                p99 >
                    (unsigned long long)
                        s->ramp_p99) {
                reason = "p99";
                break;
            }
        }
        ceiling = rate;
        if (rate >=
            (unsigned long long)
                s->ramp_max_rate) {
            reason = "max_rate";
            break;
        }
        if (step + 1 >=
            (size_t)
                s->ramp_max_steps) {
            reason = "max_steps";
            break;
        }
        if (bench_now_ns() - ramp_start >=
            (unsigned long long)
                s->ramp_max_dur *
                1000000000ULL) {
            reason = "max_duration";
            break;
        }
        if (rate ==
            (unsigned long long)
                s->ramp_max_rate) {
            reason = "max_rate";
            break;
        }
    }
    if (relay_on) {
        weld_relay_stop(c->r,
                        s->echo_node);
    }
    bench_key(ph, k, "ceiling.rate",
              ceiling);
    {
        char msg[kEscLen];

        (void)snprintf(
            msg, sizeof(msg),
            "ceiling limited by %s",
            reason);
        emit_ev("marker", ph, msg,
                (long long)ceiling,
                0);
    }
    printf("suite: step benchmark "
           "ramp %zu steps ceiling "
           "%llu\n",
           step, ceiling);
    fflush(stdout);
    return (true);
}

// run_ramp_conc runs the same ladder
// for the concord submit family: each
// step submits rate times duration
// workloads under one base, pumps to
// grace while full wids list, same
// stop plus backstop rules. Total
// issues stay below kMaxWl by lint,
// so the table never overflows. Same
// boundedness proof as weld: steps,
// time and rate all monotonic.
static bool run_ramp_conc(
    const ExStep *s, RunCtx *c,
    size_t ph, size_t k, char *why,
    size_t n) {
    static unsigned long long rpub
        [100];
    static unsigned long long rseen
        [100];
    static unsigned long long rhits
        [100];
    static unsigned long long rhits_us
        [100];
    static char rwids[100][64];
    unsigned long long ramp_start = 0;
    unsigned long long ceiling = 0;
    const char *reason = "max_steps";
    size_t step = 0;
    size_t glob = 0;

    if (s == nullptr || c == nullptr) {
        return (false);
    }
    if (!kEpCap[s->from_kind].submit) {
        (void)snprintf(
            why, n,
            "%s cannot submit: no "
            "submit verb",
            ep_name(s->from_kind));
        return (false);
    }
    ramp_start = bench_now_ns();
    for (step = 0;
         step <
         (size_t)s->ramp_max_steps;
         step++) {
        unsigned long long rate = 0;
        unsigned long long want = 0;
        unsigned long long deadline =
            0;
        unsigned long long grace = 0;
        unsigned long long start = 0;
        unsigned long long end = 0;
        unsigned long long rach = 0;
        unsigned long long loss = 0;
        unsigned long long over = 0;
        unsigned long long targ = 0;
        size_t total = 0;
        size_t nhit = 0;
        size_t j = 0;
        size_t i = 0;

        rate = (unsigned long long)
                s->ramp_start +
            (unsigned long long)step *
                (unsigned long long)
                    s->ramp_step;
        if (rate >
            (unsigned long long)
                s->ramp_max_rate) {
            rate = (unsigned long long)
                s->ramp_max_rate;
        }
        if (bench_now_ns() - ramp_start >=
            (unsigned long long)
                s->ramp_max_dur *
                1000000000ULL) {
            reason = "max_duration";
            break;
        }
        want = rate *
            (unsigned long long)
                s->ramp_step_dur;
        start = bench_now_ns();
        deadline = start +
            (unsigned long long)
                s->ramp_step_dur *
                1000000000ULL;
        for (i = 0; i < 100; i++) {
            WlEnt *e = nullptr;
            char name[kWlLen];
            const char *fl = nullptr;

            if (total >= (size_t)want) {
                break;
            }
            if (bench_now_ns() >=
                deadline) {
                break;
            }
            if (c->tab.n >=
                (size_t)kMaxWl) {
                break;
            }
            if (total > 0) {
                pace_poll(c, ph,
                            (long long)rate);
            }
            fl = s->has_flags ? s->flags
                              : nullptr;
            e = &c->tab.ent[c->tab.n];
            if (!do_submit_once(
                    c, s->node,
                    s->image, fl, e->wid,
                    sizeof(e->wid))) {
                (void)snprintf(
                    why, n,
                    "benchmark submit "
                    "on node %d failed",
                    s->node);
                return (false);
            }
            if (strlen(s->as_name) + 4 >
                sizeof(name)) {
                (void)snprintf(
                    why, n,
                    "workload name too "
                    "long for count");
                return (false);
            }
            (void)snprintf(
                name, sizeof(name),
                "%s-%zu", s->as_name,
                glob);
            (void)snprintf(
                e->name, sizeof(e->name),
                "%s", name);
            e->node = s->node;
            e->sub_ms = ev_now_ms();
            e->present_ms = 0;
            e->metric_done = false;
            (void)snprintf(
                rwids[total],
                sizeof(rwids[total]),
                "%s", e->wid);
            c->tab.n++;
            glob++;
            printf("suite: step submit "
                   "%s\n",
                   e->wid);
            rpub[total] = bench_now_ns();
            rseen[total] = 0;
            total++;
            sample_live(c, ph);
        }
        end = bench_now_ns();
        if (end >= start &&
            end - start > 0) {
            rach = (unsigned long long)
                    total *
                1000000000ULL /
                (end - start);
        } else if (total > 0) {
            rach = (unsigned long long)
                total;
        } else {
            rach = 0;
        }
        grace = bench_now_ns() +
            (unsigned long long)
                kBenchGrace *
                1000000000ULL;
        for (;;) {
            size_t miss = 0;

            for (j = 0; j < total;
                 j++) {
                if (rseen[j] != 0) {
                    continue;
                }
                if (generic_miss(s)) {
                    miss++;
                    continue;
                }
                if (s->has_generic &&
                    strcmp(
                        s->expect_via,
                        "journal-has") ==
                        0) {
                    if (bench_journal_seen(
                            c, s->obs,
                            s->exp_field,
                            s->exp_value,
                            j,
                            rpub[j],
                            rwids[j])) {
                        rseen[j] =
                            bench_now_ns();
                        continue;
                    }
                    miss++;
                    continue;
                }
                if (bench_conc_seen(
                        c,
                        rwids[j])) {
                    rseen[j] =
                        bench_now_ns();
                    continue;
                }
                miss++;
            }
            if (miss == 0) {
                break;
            }
            if (bench_now_ns() >=
                grace) {
                break;
            }
            if (!run_alive(c->r, -1)) {
                break;
            }
            run_poll(c->r, poll_drop,
                     poll_log, c->pc);
            if (sample_slot(
                    &c->tsample)) {
                auto_sample(c->r, ph);
            }
        }
        targ = want;
        over = want > (unsigned long long)
                       total
            ? 1
            : 0;
        for (j = 0; j < total; j++) {
            if (rseen[j] == 0 ||
                rseen[j] < rpub[j] ||
                rpub[j] == 0) {
                loss++;
                continue;
            }
            rhits[nhit] =
                (rseen[j] - rpub[j]) /
                1000000ULL;
            rhits_us[nhit] =
                (rseen[j] - rpub[j]) /
                1000ULL;
            nhit++;
        }
        if (s->has_generic &&
            strcmp(s->expect_via,
                   "prometheus-scrape") ==
                0 &&
            !scrape_gate(c, s, ph, k)) {
            loss = total;
            nhit = 0;
        }
        ramp_stats(ph, k, step,
                   rhits, rhits_us,
                   nhit, loss,
                   total, targ, over,
                   rach);
        {
            unsigned long long lpct = 0;
            unsigned long long p99 = 0;

            if (total > 0) {
                lpct = loss * 100ULL /
                    (unsigned long long)
                        total;
            } else {
                lpct = 100;
            }
            p99 = bench_pct(rhits,
                            nhit, 99);
            if (s->ramp_has_loss &&
                lpct >
                    (unsigned long long)
                        s->ramp_loss_pct) {
                reason = "loss";
                break;
            }
            if (s->ramp_has_p99 &&
                p99 >
                    (unsigned long long)
                        s->ramp_p99) {
                reason = "p99";
                break;
            }
        }
        ceiling = rate;
        if (rate >=
            (unsigned long long)
                s->ramp_max_rate) {
            reason = "max_rate";
            break;
        }
        if (step + 1 >=
            (size_t)
                s->ramp_max_steps) {
            reason = "max_steps";
            break;
        }
        if (bench_now_ns() - ramp_start >=
            (unsigned long long)
                s->ramp_max_dur *
                1000000000ULL) {
            reason = "max_duration";
            break;
        }
        if (rate ==
            (unsigned long long)
                s->ramp_max_rate) {
            reason = "max_rate";
            break;
        }
    }
    {
        WlTab *t = nullptr;

        if (glob > 1) {
            t = &c->tab;
            (void)snprintf(
                t->bases[t->nbase],
                sizeof(
                    t->bases[t->nbase]),
                "%s", s->as_name);
            t->bcount[t->nbase] =
                (long long)glob;
            t->nbase++;
        }
    }
    bench_key(ph, k, "ceiling.rate",
              ceiling);
    {
        char msg[kEscLen];

        (void)snprintf(
            msg, sizeof(msg),
            "ceiling limited by %s",
            reason);
        emit_ev("marker", ph, msg,
                (long long)ceiling,
                0);
    }
    printf("suite: step benchmark "
           "ramp %zu steps ceiling "
           "%llu\n",
           step, ceiling);
    fflush(stdout);
    return (true);
}

// apply_step runs one do step on
// phase entry, in file order. Ph
// plus k name the metric slot for
// benchmark stats lines.
static bool apply_step(const ExStep *s,
                       RunCtx *c,
                       size_t ph,
                       size_t k,
                       char *why,
                       size_t n) {
    size_t i;

    switch (s->kind) {
    case ST_DROP:
        if (s->has_fault) {
            TunQos q;
            int k;

            memset(&q, 0,
                   sizeof(q));
            q.loss01 = s->loss;
            q.base_delay_ns =
                s->delay_ns;
            q.jitter_ns =
                s->jitter_ns;
            q.kbps = s->kbps;
            q.dup_pct = s->dup_pct;
            for (k = 0;
                 k < c->r->n; k++) {
                bool hit = false;
                size_t j;

                if (s->dir ==
                    DIR_BOTH) {
                    hit = true;
                } else {
                    bool listed =
                        s->all;

                    for (j = 0;
                         j < s->nidx;
                         j++) {
                        if (s->idx[j] ==
                            k) {
                            listed =
                                true;
                            break;
                        }
                    }
                    hit = s->dir ==
                                  DIR_OUT
                              ? listed
                              : !listed;
                }
                if (!hit) {
                    continue;
                }
                q.seed =
                    (uint64_t)(k + 1);
                tun_qos_set(
                    &c->r->map,
                    c->r->fds[k], &q);
            }
            printf("suite: step drop\n");
            fflush(stdout);
            return (true);
        }
        {
            int k;
            int j;

            // Out keeps the legacy
            // egress marks; in arms
            // directed ingress edges
            // from every other fd into
            // each listed fd; both arms
            // both (marks plus ingress
            // edges; egress edges stay
            // redundant under the
            // marks). Fan-out ignores
            // directed edges per tun.
            if (s->dir != DIR_IN) {
                for (k = 0;
                     k < c->r->n;
                     k++) {
                    if (!is_listed(
                            s, k)) {
                        continue;
                    }
                    c->pc->drop[k] =
                        1;
                }
            }
            if (s->dir != DIR_OUT) {
                for (k = 0;
                     k < c->r->n;
                     k++) {
                    if (!is_listed(
                            s, k)) {
                        continue;
                    }
                    for (j = 0;
                         j < c->r->n;
                         j++) {
                        if (j == k) {
                            continue;
                        }
                        tun_drop_set(
                            &c->r->map,
                            c->r->fds[j],
                            c->r->fds[k],
                            true);
                    }
                }
            }
            printf("suite: step drop\n");
            fflush(stdout);
            return (true);
        }
    case ST_HEAL:
        {
            int k;
            int j;

            if (s->all) {
                for (k = 0;
                     k < c->r->n;
                     k++) {
                    c->pc->drop[k] =
                        0;
                }
            } else {
                for (i = 0;
                     i < s->nidx;
                     i++) {
                    c->pc->drop[s->idx
                                     [i]] =
                        0;
                }
            }
            // Heal clears every pump
            // fault at once: all qos
            // slots plus all directed
            // unicast edges.
            for (k = 0;
                 k < c->r->n; k++) {
                tun_qos_clear(
                    &c->r->map,
                    c->r->fds[k]);
            }
            for (k = 0;
                 k < c->r->n; k++) {
                for (j = 0;
                     j < c->r->n;
                     j++) {
                    if (j == k) {
                        continue;
                    }
                    tun_drop_clear(
                        &c->r->map,
                        c->r->fds[k],
                        c->r->fds[j],
                        false);
                }
            }
            printf("suite: step heal\n");
            fflush(stdout);
            return (true);
        }
    case ST_KILL:
        if (c->f->sut == kWeld) {
            if (!weld_kill(c->r,
                           s->node,
                           s->sig)) {
                (void)snprintf(
                    why, n,
                    "kill node %d "
                    "failed",
                    s->node);
                return (false);
            }
            run_note_dead(c->r,
                          s->node);
        } else {
            if (!concord_kill(
                    c->r, s->node,
                    s->sig)) {
                (void)snprintf(
                    why, n,
                    "kill node %d "
                    "failed",
                    s->node);
                return (false);
            }
        }
        printf("suite: step kill %d\n",
               s->node);
        fflush(stdout);
        return (true);
    case ST_RESTART:
        if (c->f->sut == kWeld) {
            WeldDurability d =
                s->dur == kDurFresh
                    ? WELD_FRESH
                    : WELD_PRESERVE;

            if (!weld_restart(
                    c->r, s->node,
                    c->domain,
                    c->weld_bin,
                    c->topic, d)) {
                (void)snprintf(
                    why, n,
                    "restart node %d "
                    "failed",
                    s->node);
                return (false);
            }
        } else {
            ConcordDurability d;

            switch (s->dur) {
            case kDurFresh:
                d = CONCORD_FRESH;
                break;
            case kDurKeep:
                d = CONCORD_PRESERVE;
                break;
            case kDurRot:
                d = CONCORD_REKEY_ROTATE;
                break;
            case kDurWipe:
                d = CONCORD_KEY_WIPE;
                break;
            default:
                d = CONCORD_WIPE_STATE;
                break;
            }
            if (!concord_restart(
                    c->r, s->node,
                    d)) {
                (void)snprintf(
                    why, n,
                    "restart node %d "
                    "failed",
                    s->node);
                return (false);
            }
        }
        printf("suite: step restart "
               "%d\n",
               s->node);
        fflush(stdout);
        return (true);
    case ST_MOVE:
        {
            int64_t x;

            x = (int64_t)(s->xm *
                          1000000000.0);
            if (!move_node(c->r,
                           s->node,
                           x)) {
                (void)snprintf(
                    why, n,
                    "move node %d "
                    "failed",
                    s->node);
                return (false);
            }
            printf("suite: step move "
                   "%d\n",
                   s->node);
            fflush(stdout);
            return (true);
        }
    case ST_SUBMIT:
        {
            const char *fl;
            const char *eff;
            long long total;
            long long k;

            if (s->has_id) {
                (void)snprintf(
                    why, n,
                    "fixed ids "
                    "unsupported");
                c->skip_fixed = true;
                return (true);
            }
            eff = s->has_as
                      ? s->as_name
                      : "submitted";
            if (!s->has_as &&
                c->tab.n > 0) {
                (void)snprintf(
                    why, n,
                    "second submit "
                    "needs as: name");
                return (false);
            }
            {
                int tmp[kMaxWl];

                if (wl_resolve(
                        &c->tab, eff,
                        tmp,
                        kMaxWl) > 0) {
                    (void)snprintf(
                        why, n,
                        "duplicate "
                        "workload %s",
                        eff);
                    return (false);
                }
            }
            total = s->has_count
                        ? s->count
                        : 1;
            if ((size_t)total +
                    c->tab.n >
                (size_t)kMaxWl) {
                (void)snprintf(
                    why, n,
                    "too many "
                    "workloads");
                return (false);
            }
            if (s->has_count &&
                strlen(eff) + 4 >
                    (size_t)kWlLen) {
                (void)snprintf(
                    why, n,
                    "workload name too "
                    "long for count");
                return (false);
            }
            fl = s->has_flags
                     ? s->flags
                     : nullptr;
            for (k = 0; k < total;
                 k++) {
                WlEnt *e;
                char name[kWlLen];

                if (k > 0 &&
                    s->has_rate) {
                    pace_poll(c, ph,
                              s->rate);
                }
                e = &c->tab.ent[c->tab
                                     .n];
                if (!do_submit_once(
                        c, s->node,
                        s->image, fl,
                        e->wid,
                        sizeof(
                            e->wid))) {
                    (void)snprintf(
                        why, n,
                        "submit on node "
                        "%d failed",
                        s->node);
                    return (false);
                }
                if (total > 1) {
                    (void)snprintf(
                        name,
                        sizeof(name),
                        "%s-%lld", eff,
                        k);
                } else {
                    (void)snprintf(
                        name,
                        sizeof(name),
                        "%s", eff);
                }
                (void)snprintf(
                    e->name,
                    sizeof(e->name),
                    "%s", name);
                e->node = s->node;
                e->sub_ms =
                    ev_now_ms();
                e->present_ms = 0;
                e->metric_done =
                    false;
                c->tab.n++;
                printf("suite: step "
                       "submit %s\n",
                       e->wid);
            }
            if (total > 1) {
                WlTab *t = &c->tab;

                (void)snprintf(
                    t->bases[t->nbase],
                    sizeof(
                        t->bases[t
                            ->nbase]),
                    "%s", eff);
                t->bcount[t->nbase] =
                    total;
                t->nbase++;
            }
            fflush(stdout);
            return (true);
        }
    case ST_STOP:
        {
            int hits[kMaxWl];
            int nh;
            int h;

            nh = wl_resolve(
                &c->tab, s->wl, hits,
                kMaxWl);
            if (nh == 0) {
                (void)snprintf(
                    why, n,
                    "stop with no "
                    "submitted workload");
                return (false);
            }
            for (h = 0; h < nh; h++) {
                WlEnt *e;

                e = &c->tab.ent[hits
                                     [h]];
                if (!concord_stop(
                        c->statedir,
                        e->node,
                        e->wid)) {
                    (void)snprintf(
                        why, n,
                        "stop %s failed",
                        e->wid);
                    return (false);
                }
            }
            printf("suite: step stop\n");
            fflush(stdout);
            return (true);
        }
    case ST_PUB:
        {
            const char *t;

            if (!kEpCap[s->from_kind]
                     .pub_weld) {
                (void)snprintf(
                    why, n,
                    "%s cannot publish: "
                    "no pub verb",
                    ep_name(
                        s->from_kind));
                return (false);
            }
            t = s->has_topic
                    ? s->topic
                    : nullptr;
            if (!do_pub_once(
                    c, s->node, t,
                    s->text)) {
                (void)snprintf(
                    why, n,
                    "pub from node %d "
                    "failed",
                    s->node);
                return (false);
            }
            printf("suite: step pub "
                   "'%s'\n",
                   s->text);
            fflush(stdout);
            return (true);
        }
    case ST_ROSPUB:
        {
            const char *t;
            unsigned long long dl;
            size_t total = 0;
            size_t i;

            if (!kEpCap[s->from_kind]
                     .pub_ros) {
                (void)snprintf(
                    why, n,
                    "%s cannot publish: "
                    "no pub verb",
                    ep_name(
                        s->from_kind));
                return (false);
            }

            t = s->has_topic
                    ? s->topic
                    : nullptr;
            if (s->has_dur) {
                dl = bench_now_ns() +
                     (unsigned long long)
                         s->duration_s *
                     1000000000ULL;
            } else {
                dl = 0;
            }
            for (i = 0;
                 i < (size_t)
                     kBenchWeld;
                 i++) {
                char pay[kBenchPay];
                unsigned long long snd;

                if (s->has_count &&
                    total >=
                        (size_t)
                            s->count) {
                    break;
                }
                if (s->has_dur &&
                    bench_now_ns() >=
                        dl) {
                    break;
                }
                if (total > 0) {
                    pace_poll(c, ph,
                              s->rate);
                }
                snd = bench_now_ns();
                if (!bench_weld_pay(
                        s, total, snd,
                        pay,
                        sizeof(pay))) {
                    (void)snprintf(
                        why, n,
                        "ros_pub "
                        "payload too "
                        "long");
                    return (false);
                }
                if (!do_ros_pub_once(
                        c, s->node, t,
                        pay)) {
                    (void)snprintf(
                        why, n,
                        "ros_pub from "
                        "node %d "
                        "failed",
                        s->node);
                    return (false);
                }
                total++;
            }
            printf("suite: step "
                   "ros_pub %zu\n",
                   total);
            fflush(stdout);
            return (true);
        }
    case ST_SLOW:
        {
            TunQos q;
            int k;

            memset(&q, 0,
                   sizeof(q));
            q.slow_secs =
                (int)s->slow_secs;
            for (k = 0;
                 k < c->r->n; k++) {
                bool hit = s->all;
                size_t j;

                for (j = 0;
                     j < s->nidx;
                     j++) {
                    if (s->idx[j] ==
                        k) {
                        hit = true;
                        break;
                    }
                }
                if (!hit) {
                    continue;
                }
                tun_qos_set(
                    &c->r->map,
                    c->r->fds[k], &q);
            }
            printf("suite: step slow "
                   "%lld\n",
                   s->slow_secs);
            fflush(stdout);
            return (true);
        }
    case ST_UPDATE:
        {
            int hits[kMaxWl];
            int nh;
            int h;

            if (c->f->sut == kWeld) {
                (void)snprintf(
                    why, n,
                    "update is "
                    "concord-only");
                return (false);
            }
            nh = wl_resolve(
                &c->tab, s->wl, hits,
                kMaxWl);
            if (nh == 0) {
                (void)snprintf(
                    why, n,
                    "update with no "
                    "submitted workload");
                return (false);
            }
            for (h = 0; h < nh; h++) {
                WlEnt *e;

                e = &c->tab.ent[hits
                                     [h]];
                if (!concord_update(
                        c->statedir,
                        e->node,
                        e->wid,
                        s->image)) {
                    (void)snprintf(
                        why, n,
                        "update %s "
                        "failed",
                        e->wid);
                    return (false);
                }
            }
            printf("suite: step update "
                   "%s\n",
                   s->wl);
            fflush(stdout);
            return (true);
        }
    case ST_REPLAY:
        {
            const char *t;
            FILE *fp;
            char line[1025];
            size_t nmsg = 0;

            if (c->f->sut != kWeld) {
                (void)snprintf(
                    why, n,
                    "replay is "
                    "weld-only");
                return (false);
            }
            t = s->has_topic
                    ? s->topic
                    : c->topic;
            fp = fopen(s->file,
                       "r");
            if (fp == nullptr) {
                (void)snprintf(
                    why, n,
                    "replay cannot open "
                    "%s",
                    s->file);
                return (false);
            }
            while (fgets(
                       line,
                       sizeof(line),
                       fp) != nullptr) {
                size_t m;

                m = strlen(line);
                if (m > 0 &&
                    line[m - 1] ==
                        '\n') {
                    line[--m] = '\0';
                }
                if (m > 0 &&
                    line[m - 1] ==
                        '\r') {
                    line[--m] = '\0';
                }
                if (m == 0) {
                    continue;
                }
                if (m + 1 >=
                    sizeof(line)) {
                    (void)fclose(fp);
                    (void)snprintf(
                        why, n,
                        "replay line too "
                        "long");
                    return (false);
                }
                if (!do_pub_once(
                        c, s->node, t,
                        line)) {
                    (void)fclose(fp);
                    (void)snprintf(
                        why, n,
                        "replay publish "
                        "from node %d "
                        "failed",
                        s->node);
                    return (false);
                }
                nmsg++;
            }
            (void)fclose(fp);
            printf("suite: step replay "
                   "%s %zu\n",
                   s->file, nmsg);
            fflush(stdout);
            return (true);
        }
    case ST_DISKFILL:
        if (c->f->sut == kWeld) {
            (void)snprintf(
                why, n,
                "diskfill is "
                "concord-only");
            return (false);
        }
        if (!concord_disk_fill(
                c->r, s->node,
                s->mb)) {
            (void)snprintf(
                why, n,
                "diskfill node %d "
                "failed",
                s->node);
            return (false);
        }
        printf("suite: step diskfill "
               "%d %llu\n",
               s->node, s->mb);
        fflush(stdout);
        return (true);
    case ST_DISKRO:
        if (c->f->sut == kWeld) {
            (void)snprintf(
                why, n,
                "diskreadonly is "
                "concord-only");
            return (false);
        }
        if (!concord_disk_readonly(
                c->r, s->node,
                s->ro)) {
            (void)snprintf(
                why, n,
                "diskreadonly node "
                "%d failed",
                s->node);
            return (false);
        }
        printf("suite: step "
               "diskreadonly %d %s\n",
               s->node,
               s->ro ? "on" : "off");
        fflush(stdout);
        return (true);
    case ST_JCORRUPT:
        if (c->f->sut == kWeld) {
            (void)snprintf(
                why, n,
                "journalcorrupt is "
                "concord-only");
            return (false);
        }
        if (!concord_journal_corrupt(
                c->r, s->node)) {
            (void)snprintf(
                why, n,
                "journalcorrupt node "
                "%d failed",
                s->node);
            return (false);
        }
        printf("suite: step "
               "journalcorrupt %d\n",
               s->node);
        fflush(stdout);
        return (true);
    case ST_BENCH:
        if (s->has_ramp) {
            if (kEpCap[s->from_kind]
                     .submit) {
                return (run_ramp_conc(
                    s, c, ph, k, why,
                    n));
            }
            if (!kEpCap[s->from_kind]
                     .pub_weld &&
                !kEpCap[s->from_kind]
                     .pub_ros) {
                (void)snprintf(
                    why, n,
                    "%s cannot "
                    "publish: no pub "
                    "verb",
                    ep_name(
                        s->from_kind));
                return (false);
            }
            return (run_ramp_weld(
                s, c, ph, k, why, n));
        }
        if (kEpCap[s->from_kind]
                 .submit) {
            return (run_bench_conc(
                s, c, ph, k, why, n));
        }
        if (kEpCap[s->from_kind]
                 .pub_weld ||
            kEpCap[s->from_kind]
                .pub_ros) {
            return (run_bench_weld(
                s, c, ph, k, why,
                n));
        }
        (void)snprintf(
            why, n,
            "%s cannot publish: no "
            "pub verb",
            ep_name(s->from_kind));
        return (false);
    }
    (void)snprintf(why, n,
                   "bad step");
    return (false);
}

// kPollMs mirrors the run poll wait:
// 100 ms per ready-TUN round.
static constexpr int kPollMs = 100;

// kPollChunk mirrors the run log
// chunk: 2048 bytes per read.
static constexpr int kPollChunk = 2048;

// run_poll_nomcast mirrors run_poll
// but pumps through
// tun_pump_fd_nomcast, so multicast
// goes blind while unicast gossip,
// sync, and Noise keep flowing. Dead
// TUN discard plus the log drain copy
// run_poll exactly, using only
// public Run fields; run/ stays
// untouched. Used for phases with
// multicast: off only.
static int run_poll_nomcast(
    RunCtx *c) {
    Run *r;
    char buf[kPollChunk];
    int hit;
    int i;

    if (c == nullptr ||
        c->r == nullptr ||
        c->pc == nullptr) {
        return (-1);
    }
    r = c->r;
    hit = poll(r->p,
               (nfds_t)(2 * r->n),
               kPollMs);
    if (hit <= 0) {
        return (hit);
    }
    for (i = 0; i < r->n; i++) {
        bool d;
        ssize_t k;

        if (!(r->p[i].revents &
              POLLIN)) {
            continue;
        }
        if (i == r->dead) {
            k = read(r->fds[i], buf,
                     sizeof(buf));
            (void)k;
            continue;
        }
        d = poll_drop(c->pc, i);
        if (d) {
            k = read(r->fds[i], buf,
                     sizeof(buf));
            (void)k;
            continue;
        }
        tun_pump_fd_nomcast(
            &r->map, r->fds[i],
            &r->ctx.grid,
            &r->ctx.radio,
            &r->ctx.nodes);
    }
    for (i = 0; i < r->n; i++) {
        ssize_t k;

        if (!(r->p[r->n + i].revents &
              POLLIN)) {
            continue;
        }
        if (r->logfds[i] < 0) {
            continue;
        }
        k = read(r->logfds[i], buf,
                 sizeof(buf) - 1);
        if (k <= 0) {
            continue;
        }
        buf[k] = '\0';
        poll_log(c->pc, i, buf,
                 (size_t)k);
    }
    return (hit);
}

// emit_frac prints one sub-phase
// line for phase ph: num of den.
// Bad shapes stay silent; progress
// never fails the run.
static void emit_frac(size_t ph,
                      long long num,
                      long long den) {
    if (den <= 0) {
        return;
    }
    if (num < 0 || num > den) {
        return;
    }
    printf("suite: frac %zu %lld "
           "%lld\n",
           ph, num, den);
    fflush(stdout);
}

// auto_sample re-emits the teardown
// flow plus resource set mid-run;
// defined with the dumps it shares.
static void auto_sample(Run *r,
                        size_t ph);

// run_until polls until condition
// holds, then waits hold_s. Flap
// resets latch; timeout is strict.
// Emits frac lines from real events
// only, see suite/exec.h.
static bool run_until(
    const ExPhase *p, size_t phidx,
    RunCtx *c, EvalCtx *e,
    time_t file_start, char *why,
    size_t n) {
    time_t start;
    time_t hold0 = 0;
    bool sat = false;
    long long lastn = -1;
    long long lastd = -1;
    long long lasthe = -1;

    start = time(nullptr);
    for (;;) {
        bool q;
        int v;
        char ewhy[160];

        if (p->nomcast) {
            run_poll_nomcast(c);
        } else {
            run_poll(c->r, poll_drop,
                     poll_log, c->pc);
        }
        run_sample_tick(c->r);
        if (!run_alive(c->r, -1)) {
            (void)snprintf(
                why, n,
                "phase '%s': no child "
                "alive",
                p->name);
            return (false);
        }
        if (time(nullptr) -
                file_start >=
            c->f->timeout_s) {
            (void)snprintf(
                why, n,
                "phase '%s': file "
                "timeout",
                p->name);
            return (false);
        }
        if (time(nullptr) - start >=
            p->timeout_s) {
            char desc[300];

            cond_desc(&p->cond, desc,
                      sizeof(desc));
            emit_ev("cond", phidx,
                    desc, 0, 0);
            (void)snprintf(
                why, n,
                "phase '%s': until "
                "timeout on %s",
                p->name, desc);
            return (false);
        }
        q = run_throttle(&e->tcheck,
                         kQueryGap);
        ewhy[0] = '\0';
        v = eval_cond(&p->cond, e, q,
                      ewhy,
                      sizeof(ewhy));
        if (v < 0) {
            (void)snprintf(
                why, n,
                "phase '%s': %s",
                p->name, ewhy);
            return (false);
        }
        if (q) {
            long long fn = 0;
            long long fd = 0;

            if (cond_frac(&p->cond, e,
                          &fn, &fd) &&
                (fn != lastn ||
                 fd != lastd)) {
                emit_frac(phidx, fn,
                          fd);
                lastn = fn;
                lastd = fd;
            }
            // Samples ride their own 250 ms
            // throttle, decoupled from
            // queries: the call below
            // re-emits the teardown key
            // set, so the series gains
            // shape even in short
            // phases. Step loops emit
            // through sample_live, so
            // series never stall mid
            // step.
            if (sample_slot(
                    &e->tsample)) {
                auto_sample(c->r, phidx);
            }
        }
        if (v == 0 && sat) {
            sat = false;
            hold0 = 0;
            lasthe = -1;
        }
        if (v > 0 && !sat) {
            char desc[300];

            sat = true;
            hold0 = 0;
            lasthe = -1;
            cond_desc(&p->cond, desc,
                      sizeof(desc));
            if (p->cond.kind ==
                CD_MARKER) {
                long long fn = 0;
                long long fd = 0;

                if (!cond_frac(
                        &p->cond, e,
                        &fn, &fd)) {
                    fn = 0;
                }
                emit_ev("marker",
                        phidx,
                        p->cond.marker,
                        fn, 0);
            }
            if (p->hold_s == 0) {
                emit_ev("cond", phidx,
                        desc, 1, 0);
                return (true);
            }
        }
        if (sat) {
            time_t now;
            long long el;

            if (run_hold(&hold0,
                         (int)p->hold_s)) {
                char desc[300];

                cond_desc(&p->cond,
                          desc,
                          sizeof(desc));
                emit_ev("cond", phidx,
                        desc, 1, 0);
                return (true);
            }
            now = time(nullptr);
            el = (long long)(now -
                             hold0);
            if (el < 0) {
                el = 0;
            }
            if (el > p->hold_s) {
                el = p->hold_s;
            }
            if (el != lasthe) {
                emit_frac(phidx, el,
                          p->hold_s);
                lasthe = el;
            }
        }
    }
}

// run_sustain polls the whole phase
// timeout; any violation fails at
// once, holding all of it passes.
// Emits elapsed of timeout_s about
// once per second, see exec.h.
static bool run_sustain(
    const ExPhase *p, size_t phidx,
    RunCtx *c, EvalCtx *e,
    time_t file_start, char *why,
    size_t n) {
    time_t start;
    long long lasthe = -1;
    bool seen = false;

    start = time(nullptr);
    for (;;) {
        bool q;
        int v;
        char ewhy[160];
        long long el;

        if (p->nomcast) {
            run_poll_nomcast(c);
        } else {
            run_poll(c->r, poll_drop,
                     poll_log, c->pc);
        }
        run_sample_tick(c->r);
        if (!run_alive(c->r, -1)) {
            (void)snprintf(
                why, n,
                "phase '%s': no child "
                "alive",
                p->name);
            return (false);
        }
        if (time(nullptr) -
                file_start >=
            c->f->timeout_s) {
            (void)snprintf(
                why, n,
                "phase '%s': file "
                "timeout",
                p->name);
            return (false);
        }
        el = (long long)(time(nullptr) -
                         start);
        if (el < 0) {
            el = 0;
        }
        if (el > p->timeout_s) {
            el = p->timeout_s;
        }
        if (el != lasthe) {
            emit_frac(phidx, el,
                      p->timeout_s);
            lasthe = el;
        }
        if (time(nullptr) - start >=
            p->timeout_s) {
            // One last read: a break on
            // the exact-expiry poll
            // still fails the phase.
            ewhy[0] = '\0';
            v = eval_cond(&p->cond, e,
                          false, ewhy,
                          sizeof(ewhy));
            if (v < 0) {
                (void)snprintf(
                    why, n,
                    "phase '%s': %s",
                    p->name, ewhy);
                return (false);
            }
            if (!v) {
                char desc[300];

                cond_desc(&p->cond,
                          desc,
                          sizeof(desc));
                emit_ev("cond", phidx,
                        desc, 0, 0);
                (void)snprintf(
                    why, n,
                    "phase '%s': sustain "
                    "violated on %s",
                    p->name, desc);
                return (false);
            }
            {
                char desc[300];

                cond_desc(&p->cond,
                          desc,
                          sizeof(desc));
                emit_ev("cond", phidx,
                        desc, 1, 0);
            }
            return (true);
        }
        q = run_throttle(&e->tcheck,
                         kQueryGap);
        ewhy[0] = '\0';
        v = eval_cond(&p->cond, e, q,
                      ewhy,
                      sizeof(ewhy));
        if (v < 0) {
            (void)snprintf(
                why, n,
                "phase '%s': %s",
                p->name, ewhy);
            return (false);
        }
        // Samples ride their own 250 ms
        // throttle, decoupled from
        // queries; same keys as
        // teardown by construction
        // (auto_sample calls the same
        // two dumps). Step loops emit
        // through sample_live.
        if (sample_slot(
                &e->tsample)) {
            auto_sample(c->r, phidx);
        }
        if (v > 0 &&
            p->cond.kind == CD_MARKER &&
            !seen) {
            long long fn = 0;
            long long fd = 0;

            seen = true;
            if (!cond_frac(&p->cond,
                           e, &fn,
                           &fd)) {
                fn = 0;
            }
            emit_ev("marker", phidx,
                    p->cond.marker, fn,
                    0);
        }
        if (!v) {
            char desc[300];

            cond_desc(&p->cond, desc,
                      sizeof(desc));
            (void)snprintf(
                why, n,
                "phase '%s': sustain "
                "violated on %s",
                p->name, desc);
            return (false);
        }
    }
}

// ForCtx carries forensics paths
// plus the metric phase for the
// aside-dir manifest key.
typedef struct {
    char statedir[kDirLen];
    long long nodes;
    bool weld;
    size_t ph;
} ForCtx;

// forensics_tail prints the last
// bytes of one log to stderr.
static void forensics_tail(
    const char *path) {
    char buf[kLogChunk];
    long end;
    long off;
    size_t n;
    FILE *fp;

    fp = fopen(path, "rb");
    if (fp == nullptr) {
        fprintf(stderr, "no %s\n",
                path);
        return;
    }
    if (fseek(fp, 0, SEEK_END) !=
        0) {
        (void)fclose(fp);
        return;
    }
    end = ftell(fp);
    if (end < 0) {
        (void)fclose(fp);
        return;
    }
    off = end > (long)sizeof(buf)
              ? end - (long)sizeof(buf)
              : 0;
    if (fseek(fp, off, SEEK_SET) !=
        0) {
        (void)fclose(fp);
        return;
    }
    n = fread(buf, 1, sizeof(buf),
              fp);
    (void)fclose(fp);
    fprintf(stderr, "--- %s\n", path);
    if (n > 0) {
        (void)fwrite(buf, 1, n,
                     stderr);
    }
    fprintf(stderr, "\n");
}

// metric_str emits one text-valued
// metric event (paths, names). d1
// reads 0: the msg text is the value,
// there is no integral mirror.
static void metric_str(size_t ph,
                       const char *key,
                       const char *val) {
    char msg[kEscLen];

    (void)snprintf(msg, sizeof(msg),
                   "%s=%s", key, val);
    emit_ev("metric", ph, msg, 0, 0);
}

// rel_ok gates manifest values: short
// relative paths of safe chars only.
// Rejects absolute paths, parent
// escapes, and empties, so emitted
// links cannot leave the log dir.
static bool rel_ok(const char *s) {
    size_t i;

    if (s == nullptr ||
        s[0] == '\0') {
        return (false);
    }
    if (s[0] == '/') {
        return (false);
    }
    if (strstr(s, "..") != nullptr) {
        return (false);
    }
    for (i = 0; s[i] != '\0'; i++) {
        char ch = s[i];

        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '.' ||
            ch == '-' || ch == '/') {
            continue;
        }
        return (false);
    }
    return (true);
}

// suite_forensics tails error logs
// and preserves journals beside the
// run statedir before the wipe,
// mirroring check/ forensics. The
// statedir reaches the shell, so an
// ungated value returns with no copy.
static void suite_forensics(
    void *ud) {
    ForCtx *f = ud;
    char dir[kPathLen];
    char cmd[kCmdLen * 2];
    char num[32];
    long long i;
    long now;
    int m;

    if (f == nullptr) {
        return;
    }
    if (!statedir_ok(f->statedir)) {
        return;
    }
    for (i = 0; i < f->nodes; i++) {
        char path[kPathLen];

        log_path(path, sizeof(path),
                 f->statedir, f->weld,
                 (int)i);
        forensics_tail(path);
        if (f->weld) {
            (void)snprintf(
                path, sizeof(path),
                "%s/ros-%d.log",
                f->statedir, (int)i);
            forensics_tail(path);
        }
    }
    // One stamp names the aside dir
    // and the manifest below; a single
    // read keeps the two in step.
    now = (long)time(nullptr);
    (void)snprintf(
        dir, sizeof(dir), "%s-fail/"
        "suite-%ld",
        f->statedir, now);
    (void)snprintf(num, sizeof(num),
                   "%lld", f->nodes);
    if (f->weld) {
        m = snprintf(cmd,
                     sizeof(cmd),
                     "mkdir -p %s && i=0 "
                     "&& while [ $i -lt "
                     "%s ]; do cp %s/"
                     "weld-$i.log %s/ "
                     "2>/dev/null; cp %s/"
                     "ros-$i.log %s/ "
                     "2>/dev/null; "
                     "i=$((i+1)); done",
                     dir, num,
                     f->statedir, dir,
                     f->statedir, dir);
    } else {
        m = snprintf(cmd,
                     sizeof(cmd),
                     "mkdir -p %s && i=0 "
                     "&& while [ $i -lt "
                     "%s ]; do mkdir -p "
                     "%s/node$i/concord "
                     "&& cp %s/node$i/"
                     "concord/"
                     "journal.jsonl %s/"
                     "node$i/concord/ "
                     "2>/dev/null; cp %s/"
                     "node$i/concord/"
                     "config.json %s/"
                     "node$i/concord/ "
                     "2>/dev/null; cp %s/"
                     "node-$i.log %s/ "
                     "2>/dev/null; "
                     "i=$((i+1)); done",
                     dir, num, dir,
                     f->statedir, dir,
                     f->statedir, dir,
                     f->statedir, dir);
    }
    if (m < 0 ||
        m >= (int)sizeof(cmd)) {
        return;
    }
    if (system(cmd) != 0) {
        // Best effort forensics.
    }
    fprintf(stderr,
            "suite: forensics at %s\n",
            dir);
    // Manifest: name the aside dir
    // when it landed, so the report
    // can link preserved journals.
    // Bare name only, never a path;
    // gated on the dir existing.
    if (now > 0 &&
        access(dir, F_OK) == 0) {
        char base[64];

        (void)snprintf(base,
                       sizeof(base),
                       "suite-%ld", now);
        if (rel_ok(base)) {
            metric_str(f->ph,
                       "forensics.dir",
                       base);
        }
    }
}

// stop_ros ends started ROS peers.
static void stop_ros(WeldRos *peers,
                     size_t n) {
    size_t i;

    for (i = 0; i < n; i++) {
        (void)weld_ros_peer_stop(
            &peers[i]);
    }
}

// file_verdict emits one file-level
// verdict event: 0 pass, 1 fail,
// 2 skip. Phase index stays 0: the
// verdict covers the whole file.
static void file_verdict(long long v,
                         const char *msg) {
    emit_ev("verdict", 0, msg, v,
            0);
}

// metric_u64 emits one metric event
// with an integral value in d1.
static void metric_u64(size_t ph,
                       const char *key,
                       unsigned long long
                           val) {
    char msg[kEscLen];

    (void)snprintf(msg, sizeof(msg),
                   "%s=%llu", key,
                   val);
    emit_ev("metric", ph, msg,
            (long long)val, 0);
}

// sanitize_tag maps src into dst
// for path use: alnum plus _ . -
// keep, all else read as _. Empty
// reads as run. Always terminates.
static void sanitize_tag(
    const char *src, char *dst,
    size_t n) {
    size_t o = 0;
    size_t i = 0;

    if (dst == nullptr || n == 0) {
        return;
    }
    if (src == nullptr) {
        src = "";
    }
    for (i = 0; src[i] != '\0'; i++) {
        char ch = src[i];

        if (o + 1 >= n) {
            break;
        }
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '_' || ch == '.' ||
            ch == '-') {
            dst[o++] = ch;
        } else {
            dst[o++] = '_';
        }
    }
    if (o == 0) {
        if (n < 4) {
            dst[0] = '\0';
            return;
        }
        dst[0] = 'r';
        dst[1] = 'u';
        dst[2] = 'n';
        dst[3] = '\0';
        return;
    }
    dst[o] = '\0';
}

// snap_dir builds the per-node
// profile dir under base: profile
// scope tag node id. False on bad
// args or truncation.
static bool snap_dir(char *dst,
                     size_t n,
                     const char *base,
                     const char *tag,
                     int node) {
    int m = 0;

    if (dst == nullptr || n == 0) {
        return (false);
    }
    if (base == nullptr ||
        base[0] == '\0') {
        return (false);
    }
    if (tag == nullptr ||
        tag[0] == '\0') {
        return (false);
    }
    if (node < 0) {
        return (false);
    }
    m = snprintf(dst, n,
                 "%s/profile/%s/"
                 "node%d",
                 base, tag, node);
    if (m < 0 || (size_t)m >= n) {
        return (false);
    }
    return (true);
}

// auto_snapshot runs the driver
// snapshot helper per node. The
// helper lands rusage plus proc
// data; no perf record and no
// sleep run here. Outdir redirects
// into the run log dir the parent
// exports, which persists; without
// it the statedir fallback still
// feeds the ok metric before the
// wipe. Best effort, never fails.
static void auto_snapshot(Run *r,
                          bool weld,
                          const char *statedir,
                          const char *tag,
                          size_t ph) {
    const char *logd = nullptr;
    char scope[kNameLen + 16];
    int i = 0;

    if (r == nullptr) {
        return;
    }
    if (statedir == nullptr) {
        return;
    }
    if (tag == nullptr) {
        return;
    }
    (void)snprintf(scope,
                   sizeof(scope),
                   "%s-%d", tag,
                   (int)getpid());
    logd = getenv("RSUITE_LOG_DIR");
    for (i = 0; i < r->n; i++) {
        char dir[kPathLen];
        char key[64];
        bool ok = false;

        if (snap_dir(dir,
                     sizeof(dir),
                     logd, scope, i) ||
            snap_dir(dir,
                     sizeof(dir),
                     statedir, scope,
                     i)) {
            if (weld) {
                ok = weld_profile_snapshot(
                    r, i, dir);
            } else {
                ok = concord_profile_snapshot(
                    r, i, dir);
            }
        }
        (void)snprintf(key,
                       sizeof(key),
                       "profile.node%d.ok",
                       i);
        metric_u64(ph, key,
                   ok ? 1ULL : 0ULL);
    }
}

// FlowPrev remembers the last total
// per directed node pair in this
// process, so the same tick can also
// emit the per-interval delta. run
// tags the owner: a new Run restarts
// counters at zero, which reads as a
// fresh baseline, never negative.
typedef struct {
    bool have;
    const Run *run;
    TunFlow last;
} FlowPrev;
static FlowPrev g_flow_prev[kMaxNodes]
                             [kMaxNodes];

// flow_delta diffs one counter cell:
// a fresh baseline (first sight, new
// Run, or map reset) reads delta as
// current. Cells saturate at max
// (tun sadd), so they never wrap;
// a read below stored only means the
// map reset, and also reads fresh.
static unsigned long long flow_delta(
    unsigned long long cur,
    unsigned long long prev,
    bool fresh) {
    if (fresh) {
        return (cur);
    }
    if (cur < prev) {
        return (cur);
    }
    return (cur - prev);
}

// flow_cell emits one flow leaf total
// plus its per-interval delta beside
// it. Totals keep exact old bytes;
// deltas ride the same 2 s tick the
// sampler already runs on.
static void flow_cell(size_t ph,
                      int i, int j,
                      const char *leaf,
                      unsigned long long cur,
                      unsigned long long d) {
    char key[64];

    (void)snprintf(key, sizeof(key),
                   "flow.%d.to.%d.%s",
                   i, j, leaf);
    metric_u64(ph, key, cur);
    if (d == 0) {
        return;
    }
    (void)snprintf(key, sizeof(key),
                   "flow.%d.to.%d.%s."
                   "delta",
                   i, j, leaf);
    metric_u64(ph, key, d);
}

// flow_pair emits one directed pair:
// each nonzero total keeps its exact
// old line, plus the delta beside it.
// First sight diffs from a zero
// baseline, so delta equals current.
// Pairs past the pair cap never reach
// here, so they emit nothing and hold
// no baseline.
static void flow_pair(size_t ph,
                      Run *r, int i,
                      int j,
                      const TunFlow *fl) {
    FlowPrev *pe = nullptr;
    bool fresh = true;
    unsigned long long pp = 0;
    unsigned long long pb = 0;
    unsigned long long pr = 0;
    unsigned long long po = 0;

    if (r == nullptr ||
        fl == nullptr) {
        return;
    }
    if (i >= 0 && i < kMaxNodes &&
        j >= 0 && j < kMaxNodes) {
        pe = &g_flow_prev[i][j];
        fresh = !pe->have ||
            pe->run != r;
        if (!fresh) {
            pp = pe->last.pkts;
            pb = pe->last.bytes;
            pr = pe->last
                     .radio_drops;
            po = pe->last
                     .policy_drops;
        }
    }
    if (fl->pkts != 0) {
        flow_cell(ph, i, j, "pkts",
                  fl->pkts,
                  flow_delta(fl->pkts,
                             pp, fresh));
    }
    if (fl->bytes != 0) {
        flow_cell(ph, i, j, "bytes",
                  fl->bytes,
                  flow_delta(fl->bytes,
                             pb, fresh));
    }
    if (fl->radio_drops != 0) {
        flow_cell(ph, i, j,
                  "radio_drops",
                  fl->radio_drops,
                  flow_delta(
                      fl->radio_drops,
                      pr, fresh));
    }
    if (fl->policy_drops != 0) {
        flow_cell(ph, i, j,
                  "policy_drops",
                  fl->policy_drops,
                  flow_delta(
                      fl->policy_drops,
                      po, fresh));
    }
    if (pe != nullptr) {
        pe->last = *fl;
        pe->have = true;
        pe->run = r;
    }
}

// auto_flows dumps nonzero tun flow
// cells per directed pair through
// the shared metric site, so the
// report needs no parser change.
// Reads only, no syscalls. Caps
// pairs, then marks truncated.
static void auto_flows(Run *r,
                       size_t ph) {
    int kept = 0;
    bool cut = false;
    int i = 0;

    if (r == nullptr) {
        return;
    }
    for (i = 0; i < r->n && !cut;
         i++) {
        int from = r->fds[i];
        int j = 0;

        if (from < 0) {
            continue;
        }
        for (j = 0; j < r->n; j++) {
            TunFlow fl;
            int to = 0;

            if (i == j) {
                continue;
            }
            if (kept >= kAutoPairs) {
                cut = true;
                break;
            }
            to = r->fds[j];
            if (to < 0) {
                continue;
            }
            memset(&fl, 0,
                   sizeof(fl));
            if (!tun_stats_get(&r->map,
                               from, to,
                               &fl)) {
                continue;
            }
            if (fl.pkts == 0 &&
                fl.bytes == 0 &&
                fl.radio_drops == 0 &&
                fl.policy_drops == 0) {
                continue;
            }
            kept++;
            flow_pair(ph, r, i, j,
                      &fl);
        }
    }
    if (cut) {
        metric_u64(ph,
                   "flow.truncated",
                   1ULL);
    }
}

// kMaxHz caps sane tick rates: sysconf
// values are tiny; past it the .secs
// lines stay silent rather than risk
// wrap in the hundredths scale-up.
static constexpr int kMaxHz = 1000000;

// clk_hz reports scheduler ticks per
// second, read once per process and
// cached. Nonpositive means sysconf
// failed; callers keep ticks only.
static long clk_hz(void) {
    static long hz = 0;
    static bool set = false;

    if (!set) {
        set = true;
        hz = sysconf(_SC_CLK_TCK);
    }
    return (hz);
}

// metric_secs emits one decimal
// seconds metric from a tick count:
// whole plus two digits by integer
// math, no float in the print path.
// Ticks come from run_sample_cpu, hz
// from clk_hz. d1 carries the total
// centiseconds, the integral mirror
// of the text.
static void metric_secs(size_t ph,
                        const char *key,
                        unsigned long long t,
                        long hz) {
    char msg[kEscLen];
    unsigned long long s;
    unsigned long long h;

    if (hz <= 0) {
        return;
    }
    s = t / (unsigned long long)hz;
    h = (t % (unsigned long long)hz) *
        100ULL /
        (unsigned long long)hz;
    (void)snprintf(msg, sizeof(msg),
                   "%s=%llu.%02llu",
                   key, s, h);
    emit_ev("metric", ph, msg,
            (long long)(s * 100ULL +
                        h),
            0);
}

// auto_res dumps the run sample
// store per node through the shared
// metric site: sample counts,
// dropped counts, peak rss, cpu
// totals, latest rss and io. Reads
// only, no syscalls. Best effort
// throughout.
static void auto_res(Run *r,
                     size_t ph) {
    int i = 0;

    if (r == nullptr) {
        return;
    }
    for (i = 0; i < r->n; i++) {
        char key[64];
        unsigned long long v = 0;
        unsigned long long w = 0;
        RunSample sm;

        (void)snprintf(key,
                       sizeof(key),
                       "res.node%d."
                       "samples",
                       i);
        metric_u64(ph, key,
            (unsigned long long)
                run_sample_count(r,
                                 i));
        (void)snprintf(key,
                       sizeof(key),
                       "res.node%d."
                       "samples_dropped",
                       i);
        metric_u64(ph, key,
            (unsigned long long)
                run_sample_dropped(
                    r, i));
        if (run_sample_peak(r, i,
                            &v)) {
            (void)snprintf(
                key, sizeof(key),
                "res.node%d."
                "peakrss.mb",
                i);
            metric_u64(ph, key,
                       v / 1048576ULL);
        }
        if (run_sample_cpu(r, i, &v,
                           &w)) {
            long hz;

            (void)snprintf(
                key, sizeof(key),
                "res.node%d.cpu."
                "utime",
                i);
            metric_u64(ph, key, v);
            (void)snprintf(
                key, sizeof(key),
                "res.node%d.cpu."
                "stime",
                i);
            metric_u64(ph, key, w);
            // Seconds beside ticks on
            // the same sample. Ticks
            // stay byte-identical; hz
            // failure keeps ticks only.
            hz = clk_hz();
            if (hz > 0 &&
                hz <= kMaxHz) {
                (void)snprintf(
                    key, sizeof(key),
                    "res.node%d.cpu."
                    "user.secs",
                    i);
                metric_secs(ph, key, v,
                            hz);
                (void)snprintf(
                    key, sizeof(key),
                    "res.node%d.cpu."
                    "sys.secs",
                    i);
                metric_secs(ph, key, w,
                            hz);
            }
        }
        memset(&sm, 0, sizeof(sm));
        if (run_sample_get(r, i,
                           &sm)) {
            (void)snprintf(
                key, sizeof(key),
                "res.node%d.rss."
                "bytes",
                i);
            metric_u64(ph, key,
                       sm.rss_bytes);
            (void)snprintf(
                key, sizeof(key),
                "res.node%d.rchar",
                i);
            metric_u64(ph, key,
                       sm.rchar);
            (void)snprintf(
                key, sizeof(key),
                "res.node%d.wchar",
                i);
            metric_u64(ph, key,
                       sm.wchar);
        }
    }
}

// auto_sample re-emits the teardown
// flow plus resource set mid-run so
// each series gains shape. Same keys
// and site as auto_closeout: it calls
// the same two dumps through the same
// metric_u64 print site with the same
// flushed delivery. The closeout dump
// stays the final point. Cost per
// call is bounded: flows cap at
// kAutoPairs pairs (eight lines each:
// four totals plus four deltas)
// plus one truncated mark; res costs
// O(n) nodes at nine lines each. In
// memory counter reads only: no
// sleeps, no new syscalls past the
// getters, no new threads; poll
// timing never moves. Callers gate
// it on the 250 ms sample throttle
// (sample_slot, sample_live), never
// per tick.
static void auto_sample(Run *r,
                        size_t ph) {
    if (r == nullptr) {
        return;
    }
    auto_flows(r, ph);
    auto_res(r, ph);
}

// auto_manifest emits linkable
// per-node snapshot paths relative
// to the log dir, teardown only. The
// scope matches auto_snapshot, so the
// paths name files it just wrote. The
// statedir fallback (no log dir) is
// wiped, so it never emits. Each path
// is gated on the file existing and
// on rel_ok: never dead links, never
// escapes.
static void auto_manifest(Run *r,
                          const char *tag,
                          size_t ph) {
    const char *logd;
    char scope[kNameLen + 16];
    int i;

    if (r == nullptr ||
        tag == nullptr) {
        return;
    }
    logd = getenv("RSUITE_LOG_DIR");
    if (logd == nullptr ||
        logd[0] == '\0') {
        return;
    }
    (void)snprintf(scope,
                   sizeof(scope),
                   "%s-%d", tag,
                   (int)getpid());
    for (i = 0; i < r->n; i++) {
        char rel[kPathLen];
        char full[kPathLen * 2];
        char key[64];
        int m;

        m = snprintf(rel, sizeof(rel),
                     "profile/%s/node%d/"
                     "snapshot.txt",
                     scope, i);
        if (m < 0 ||
            (size_t)m >= sizeof(rel)) {
            continue;
        }
        if (!rel_ok(rel)) {
            continue;
        }
        m = snprintf(full,
                     sizeof(full),
                     "%s/%s", logd,
                     rel);
        if (m < 0 ||
            (size_t)m >= sizeof(full)) {
            continue;
        }
        if (access(full, F_OK) != 0) {
            continue;
        }
        (void)snprintf(key,
                       sizeof(key),
                       "profile.node%d."
                       "path",
                       i);
        metric_str(ph, key, rel);
    }
}

// auto_closeout snapshots every
// node and dumps flows plus ring
// data before teardown wipes the
// statedir. Order here is load
// bearing: snapshot, metrics,
// manifest, then the forensics hook
// and the wipe inside run_teardown.
// Cost stays bounded: per node one
// helper call (file writes plus
// proc reads, no sleep or perf
// from us) plus O(1) metrics;
// flows cost n squared map queries
// at two linear fd scans each,
// all in memory. No sleeps, no
// perf, no network on this path.
// Best effort, never fails.
static void auto_closeout(Run *r,
                          const ExFile *f,
                          bool weld) {
    char tag[kNameLen];
    size_t ph = 0;

    if (r == nullptr) {
        return;
    }
    if (f == nullptr) {
        return;
    }
    if (f->nphases > 0) {
        ph = f->nphases - 1;
    }
    sanitize_tag(f->name, tag,
                 sizeof(tag));
    auto_snapshot(r, weld,
                  r->net.statedir,
                  tag, ph);
    auto_flows(r, ph);
    auto_res(r, ph);
    auto_manifest(r, tag, ph);
}

// suite_run_file validates then runs
// one suite file to pass, fail, or
// skip with a reason in why. The run
// is always fully isolated (no host
// footprint); C scenarios keep the
// host default.
SuiteVerdict suite_run_file(
    const char *path,
    const SuiteOpts *opts, char *why,
    size_t why_n) {
    ExFile *f = nullptr;
    const char *weld_bin = nullptr;
    const char *concord_bin = nullptr;
    int want_run = 0;
    size_t i;

    ev_start();
    if (why != nullptr &&
        why_n > 0) {
        why[0] = '\0';
    }
    if (opts != nullptr) {
        want_run = opts->run_id;
        weld_bin = opts->weld_bin;
        concord_bin =
            opts->concord_bin;
    }
    if (!load_validated(path, &f,
                        why, why_n)) {
        file_verdict(1, why);
        return (SUITE_FAIL);
    }
    for (i = 0; i < f->nneeds; i++) {
        if (!need_ok(f->sut,
                     f->needs[i])) {
            if (why != nullptr &&
                why_n > 0) {
                (void)snprintf(
                    why, why_n,
                    "skip unmet need: "
                    "%s",
                    f->needs[i]);
            }
            printf("suite: skip %s "
                   "unmet need %s\n",
                   f->name,
                   f->needs[i]);
            file_verdict(2, why);
            free_file(f);
            return (SUITE_SKIP);
        }
    }
    if (f->sut == kConcord) {
        const char *bin;

        bin = concord_bin != nullptr
                  ? concord_bin
                  : "./bin/concord";
        if (access(bin, X_OK) != 0) {
            if (why != nullptr &&
                why_n > 0) {
                (void)snprintf(
                    why, why_n,
                    "skip no %s", bin);
            }
            printf("suite: skip %s "
                   "no %s\n",
                   f->name, bin);
            file_verdict(2, why);
            free_file(f);
            return (SUITE_SKIP);
        }
    } else {
        if (weld_skip_unless_bin(
                weld_bin)) {
            const char *bin;

            bin = weld_bin != nullptr
                      ? weld_bin
                      : WELD_BIN_DEFAULT;
            if (why != nullptr &&
                why_n > 0) {
                (void)snprintf(
                    why, why_n,
                    "skip no %s", bin);
            }
            printf("suite: skip %s "
                   "no %s\n",
                   f->name, bin);
            file_verdict(2, why);
            free_file(f);
            return (SUITE_SKIP);
        }
        if (weld_skip_unless_docker()) {
            if (why != nullptr &&
                why_n > 0) {
                (void)snprintf(
                    why, why_n,
                    "skip no docker");
            }
            printf("suite: skip %s "
                   "no docker\n",
                   f->name);
            file_verdict(2, why);
            free_file(f);
            return (SUITE_SKIP);
        }
    }
    {
        SimNet net;
        Run r;
        RunResult setup;
        PollCtx pc;
        RunCtx c;
        EvalCtx e;
        WeldRos peers[kMaxNodes];
        size_t npeers = 0;
        char topic[128];
        time_t file_start;
        int rid = want_run;
        bool failed = false;

        if (rid == 0 &&
            !sim_run_acquire(&rid)) {
            if (why != nullptr &&
                why_n > 0) {
                (void)snprintf(
                    why, why_n,
                    "run acquire "
                    "failed");
            }
            file_verdict(1, why);
            free_file(f);
            return (SUITE_FAIL);
        }
        if (!sim_net_try_for_run(
                rid, &net)) {
            if (why != nullptr &&
                why_n > 0) {
                (void)snprintf(
                    why, why_n,
                    "bad run id %d",
                    rid);
            }
            file_verdict(1, why);
            free_file(f);
            return (SUITE_FAIL);
        }
        // Executor runs fully
        // isolated: no host footprint.
        // C scenarios keep the default.
        net.isolate_all = 1;
        if (f->sut == kWeld &&
            !f->has_topic) {
            pid_t pid;
            time_t secs;

            pid = getpid();
            secs = time(nullptr);
            if (secs ==
                (time_t)-1) {
                if (why != nullptr &&
                    why_n > 0) {
                    (void)snprintf(
                        why, why_n,
                        "no wall time");
                }
                file_verdict(1, why);
                free_file(f);
                return (SUITE_FAIL);
            }
            (void)snprintf(
                topic, sizeof(topic),
                "/chatter_run%d_%d_%ld"
                "_%u",
                rid, (int)pid,
                (long)secs,
                ++topic_seq);
        } else if (f->sut == kWeld) {
            (void)snprintf(
                topic, sizeof(topic),
                "%s", f->topic);
        } else {
            topic[0] = '\0';
        }
        setup = run_setup_isolated(
            &r, &net,
            (int)f->nodes);
        if (setup == RUN_SKIP) {
            int sv = errno;

            if (why != nullptr &&
                why_n > 0) {
                (void)snprintf(
                    why, why_n,
                    "skip (%s)",
                    strerror(sv));
            }
            printf("suite: skip %s "
                   "(%s)\n",
                   f->name,
                   strerror(sv));
            file_verdict(2, why);
            free_file(f);
            return (SUITE_SKIP);
        }
        if (setup != RUN_OK) {
            if (why != nullptr &&
                why_n > 0) {
                (void)snprintf(
                    why, why_n,
                    "setup failed");
            }
            file_verdict(1, why);
            free_file(f);
            return (SUITE_FAIL);
        }
        memset(&pc, 0, sizeof(pc));
        if (!copy_text(
                pc.statedir,
                sizeof(pc.statedir),
                net.statedir)) {
            run_teardown(&r, true,
                         nullptr,
                         nullptr);
            if (why != nullptr &&
                why_n > 0) {
                (void)snprintf(
                    why, why_n,
                    "statedir too "
                    "long");
            }
            file_verdict(1, why);
            free_file(f);
            return (SUITE_FAIL);
        }
        pc.weld = f->sut == kWeld;
        memset(&c, 0, sizeof(c));
        c.r = &r;
        c.net = &net;
        c.pc = &pc;
        c.f = f;
        c.weld_bin = weld_bin;
        c.statedir = net.statedir;
        c.topic = topic;
        c.domain = (int)f->domain;
        memset(&e, 0, sizeof(e));
        e.f = f;
        e.r = &r;
        e.tab = &c.tab;
        if (!copy_text(
                e.statedir,
                sizeof(e.statedir),
                net.statedir)) {
            run_teardown(&r, true,
                         nullptr,
                         nullptr);
            if (why != nullptr &&
                why_n > 0) {
                (void)snprintf(
                    why, why_n,
                    "statedir too "
                    "long");
            }
            file_verdict(1, why);
            free_file(f);
            return (SUITE_FAIL);
        }
        printf("suite: start %s "
               "sut=%s nodes=%lld\n",
               f->name,
               f->sut == kWeld
                   ? "weld"
                   : "concord",
               f->nodes);
        if (f->sut == kConcord) {
            ConcordSpawn v;
            const char *arg = nullptr;
            int anode = 0;

            v = CONCORD_SPAWN_PLAIN;
            if (f->has_spawn) {
                if (f->variant ==
                    kSkew) {
                    v = CONCORD_SPAWN_SKEW;
                    anode = (int)
                        f->snode;
                    arg = f->offset;
                } else if (f->variant ==
                           kAnchor) {
                    v =
                        CONCORD_SPAWN_ANCHOR;
                    anode = (int)
                        f->snode;
                }
            }
            if (!f->has_bins) {
                bool spawned;

                // A nonzero cap boots
                // through spawn_ex; the
                // zero cap keeps the
                // plain call exactly.
                if (f->disk_cap_mb ==
                    0) {
                    spawned =
                        concord_spawn(
                            &r, v,
                            anode, arg);
                } else {
                    spawned =
                        concord_spawn_ex(
                            &r, (int)v,
                            anode, arg,
                            nullptr,
                            f->disk_cap_mb);
                }
                if (!spawned) {
                    run_teardown(
                        &r, true,
                        nullptr,
                        nullptr);
                    if (why !=
                            nullptr &&
                        why_n > 0) {
                        (void)snprintf(
                            why, why_n,
                            "spawn failed");
                    }
                    printf("suite: fail "
                           "%s spawn\n",
                           f->name);
                    file_verdict(1,
                                 why);
                    free_file(f);
                    return (SUITE_FAIL);
                }
            } else {
                int k;

                // Mixed versions: every
                // bins binary must
                // exist, like the
                // default binary check.
                for (k = 0;
                     k < (int)f->nodes;
                     k++) {
                    if (!f->bin_set[k]) {
                        continue;
                    }
                    if (access(
                            f->bins[k],
                            X_OK) == 0) {
                        continue;
                    }
                    run_teardown(
                        &r, true,
                        nullptr,
                        nullptr);
                    if (why !=
                            nullptr &&
                        why_n > 0) {
                        (void)snprintf(
                            why, why_n,
                            "skip no %s",
                            f->bins[k]);
                    }
                    printf("suite: skip "
                           "%s no %s\n",
                           f->name,
                           f->bins[k]);
                    file_verdict(2,
                                 why);
                    free_file(f);
                    return (SUITE_SKIP);
                }
                // No per-node spawn verb
                // exists: boot default,
                // then preserve-restart
                // each listed node into
                // its binary.
                if (!concord_spawn_ex(
                        &r, (int)v,
                        anode, arg,
                        nullptr,
                        f->disk_cap_mb)) {
                    run_teardown(
                        &r, true,
                        nullptr,
                        nullptr);
                    if (why !=
                            nullptr &&
                        why_n > 0) {
                        (void)snprintf(
                            why, why_n,
                            "spawn failed");
                    }
                    printf("suite: fail "
                           "%s spawn\n",
                           f->name);
                    file_verdict(1,
                                 why);
                    free_file(f);
                    return (SUITE_FAIL);
                }
                for (k = 0;
                     k < (int)f->nodes;
                     k++) {
                    if (!f->bin_set[k]) {
                        continue;
                    }
                    if (concord_restart_ex(
                            &r, k,
                            (int)CONCORD_PRESERVE,
                            f->bins[k],
                            0)) {
                        continue;
                    }
                    run_teardown(
                        &r, true,
                        nullptr,
                        nullptr);
                    if (why !=
                            nullptr &&
                        why_n > 0) {
                        (void)snprintf(
                            why, why_n,
                            "spawn bins "
                            "node %d "
                            "failed",
                            k);
                    }
                    printf("suite: fail "
                           "%s spawn\n",
                           f->name);
                    file_verdict(1,
                                 why);
                    free_file(f);
                    return (SUITE_FAIL);
                }
            }
        } else {
            int k;

            // Mixed weld versions ride
            // the per-node bin spawn.
            for (k = 0;
                 k < (int)f->nodes;
                 k++) {
                const char *bb;

                bb = f->bin_set[k]
                         ? f->bins[k]
                         : weld_bin;
                if (f->bin_set[k] &&
                    access(bb, X_OK) !=
                        0) {
                    run_teardown(
                        &r, true,
                        nullptr,
                        nullptr);
                    if (why !=
                            nullptr &&
                        why_n > 0) {
                        (void)snprintf(
                            why, why_n,
                            "skip no %s",
                            bb);
                    }
                    printf("suite: skip "
                           "%s no %s\n",
                           f->name, bb);
                    file_verdict(2,
                                 why);
                    free_file(f);
                    return (SUITE_SKIP);
                }
                if (!weld_spawn_node(
                        &r, k,
                        (int)f->domain,
                        bb,
                        topic)) {
                    run_teardown(
                        &r, true,
                        nullptr,
                        nullptr);
                    if (why !=
                            nullptr &&
                        why_n > 0) {
                        (void)snprintf(
                            why, why_n,
                            "spawn node "
                            "%d failed",
                            k);
                    }
                    printf("suite: "
                           "fail %s "
                           "spawn\n",
                           f->name);
                    file_verdict(1,
                                 why);
                    free_file(f);
                    return (
                        SUITE_FAIL);
                }
            }
            memset(peers, 0,
                   sizeof(peers));
            for (i = 0;
                 i < f->nros; i++) {
                if (!weld_ros_peer_start(
                        &r,
                        &peers[npeers],
                        (int)f->rosnode
                            [i],
                        f->rosdistro
                            [i],
                        (int)f->domain,
                        topic)) {
                    stop_ros(peers,
                             npeers);
                    run_teardown(
                        &r, true,
                        nullptr,
                        nullptr);
                    if (why !=
                            nullptr &&
                        why_n > 0) {
                        (void)snprintf(
                            why, why_n,
                            "ros peer %zu "
                            "failed",
                            i);
                    }
                    printf("suite: "
                           "fail %s ros "
                           "peer\n",
                           f->name);
                    file_verdict(1,
                                 why);
                    free_file(f);
                    return (
                        SUITE_FAIL);
                }
                e.ros_on[f->rosnode
                             [i]] =
                    true;
                c.ros_on[f->rosnode
                             [i]] =
                    true;
                npeers++;
            }
        }
        file_start = time(nullptr);
        for (i = 0;
             i < f->nphases && !failed;
             i++) {
            ExPhase *p;
            size_t k;
            unsigned long long pst;
            char pbegin[kNameLen + 8];

            p = &f->phases[i];
            printf("suite: phase %zu "
                   "%s begin\n",
                   i, p->name);
            fflush(stdout);
            pst = ev_now_ms();
            (void)snprintf(
                pbegin, sizeof(pbegin),
                "begin %s", p->name);
            emit_ev("phase", i, pbegin,
                    0, 0);
            for (k = 0;
                 k < p->nsteps;
                 k++) {
                char swhy[256];
                char sdesc[kEscLen];

                swhy[0] = '\0';
                if (!apply_step(
                        &p->steps[k],
                        &c, i, k, swhy,
                        sizeof(swhy))) {
                    if (why !=
                            nullptr &&
                        why_n > 0) {
                        (void)snprintf(
                            why, why_n,
                            "phase '%s': "
                            "%s",
                            p->name,
                            swhy);
                    }
                    printf("suite: "
                           "fail %s "
                           "phase '%s' "
                           "step\n",
                           f->name,
                           p->name);
                    failed = true;
                    break;
                }
                step_desc(&p->steps[k],
                          sdesc,
                          sizeof(sdesc));
                emit_ev("step", i,
                        sdesc,
                        (long long)k,
                        0);
                emit_frac(i,
                          (long long)(k +
                                      1),
                          (long long)p
                              ->nsteps);
                if (c.skip_fixed) {
                    break;
                }
            }
            if (failed) {
                break;
            }
            if (c.skip_fixed) {
                if (why != nullptr &&
                    why_n > 0) {
                    (void)snprintf(
                        why, why_n,
                        "fixed ids "
                        "unsupported");
                }
                printf("suite: skip %s "
                       "fixed ids "
                       "unsupported\n",
                       f->name);
                file_verdict(2, why);
                if (f->sut == kWeld) {
                    stop_ros(peers,
                             npeers);
                }
                auto_closeout(&r, f,
                              f->sut == kWeld);
                {
                    ForCtx fc;

                    memset(&fc, 0,
                           sizeof(fc));
                    (void)snprintf(
                        fc.statedir,
                        sizeof(
                            fc.statedir),
                        "%s",
                        net.statedir);
                    fc.nodes = f->nodes;
                    fc.weld =
                        f->sut == kWeld;
                    fc.ph = f->nphases > 0
                        ? f->nphases - 1
                        : 0;
                    run_teardown(
                        &r, false,
                        suite_forensics,
                        &fc);
                }
                free_file(f);
                return (SUITE_SKIP);
            }
            e.tcheck = 0;
            e.tsample = 0;
            e.list_set = false;
            e.mesh_set = false;
            e.asg_set = false;
            {
                char pwhy[512];
                bool ok;

                pwhy[0] = '\0';
                if (p->has_until) {
                    ok = run_until(
                        p, i, &c, &e,
                        file_start,
                        pwhy,
                        sizeof(pwhy));
                } else {
                    ok = run_sustain(
                        p, i, &c, &e,
                        file_start,
                        pwhy,
                        sizeof(pwhy));
                }
                if (!ok) {
                    if (why !=
                            nullptr &&
                        why_n > 0) {
                        (void)snprintf(
                            why, why_n,
                            "%s", pwhy);
                    }
                    printf("suite: "
                           "fail %s "
                           "phase '%s'\n",
                           f->name,
                           p->name);
                    failed = true;
                    break;
                }
            }
            {
                unsigned long long
                    pend;
                char pdone[kNameLen +
                           8];
                char mkey[64];
                size_t h;
                struct rusage ru;

                pend = ev_now_ms();
                (void)snprintf(
                    pdone,
                    sizeof(pdone),
                    "done %s", p->name);
                (void)snprintf(
                    mkey, sizeof(mkey),
                    "phase.%zu.ms", i);
                metric_u64(i, mkey,
                           pend >= pst
                               ? pend -
                                 pst
                               : 0);
                if (getrusage(
                        RUSAGE_SELF,
                        &ru) == 0 &&
                    ru.ru_maxrss > 0) {
                    metric_u64(
                        i, "maxrss.mb",
                        (unsigned long long)
                                ru.ru_maxrss /
                            1024ULL);
                }
                for (h = 0;
                     h < c.tab.n; h++) {
                    WlEnt *e2;
                    char lkey[kWlLen +
                              16];

                    e2 = &c.tab.ent[h];
                    if (e2->present_ms ==
                            0 ||
                        e2->metric_done) {
                        continue;
                    }
                    e2->metric_done =
                        true;
                    (void)snprintf(
                        lkey,
                        sizeof(lkey),
                        "present.%s.ms",
                        e2->name);
                    metric_u64(
                        i, lkey,
                        e2->present_ms);
                }
                emit_ev("phase", i,
                        pdone,
                        (long long)(pend >=
                                            pst
                                        ? pend -
                                          pst
                                        : 0),
                        0);
            }
            printf("suite: phase %zu "
                   "%s done\n",
                   i, p->name);
            fflush(stdout);
        }
        {
            ForCtx fc;

            if (f->sut == kWeld) {
                stop_ros(peers,
                         npeers);
            }
            auto_closeout(&r, f,
                          f->sut == kWeld);
            memset(&fc, 0,
                   sizeof(fc));
            (void)snprintf(
                fc.statedir,
                sizeof(fc.statedir),
                "%s", net.statedir);
            fc.nodes = f->nodes;
            fc.weld =
                f->sut == kWeld;
            fc.ph = f->nphases > 0
                ? f->nphases - 1
                : 0;
            run_teardown(&r, failed,
                         suite_forensics,
                         &fc);
            if (failed) {
                char vmsg[256];

                printf("suite: fail "
                       "%s\n",
                       f->name);
                (void)snprintf(
                    vmsg, sizeof(vmsg),
                    "fail %s",
                    why != nullptr
                        ? why
                        : f->name);
                file_verdict(1, vmsg);
                free_file(f);
                return (SUITE_FAIL);
            }
            {
                char vmsg[kNameLen +
                          8];

                printf("suite: pass "
                       "%s\n",
                       f->name);
                (void)snprintf(
                    vmsg, sizeof(vmsg),
                    "pass %s", f->name);
                file_verdict(0, vmsg);
            }
            if (why != nullptr &&
                why_n > 0) {
                why[0] = '\0';
            }
            free_file(f);
            return (SUITE_PASS);
        }
    }
}
