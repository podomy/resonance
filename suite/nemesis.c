#define _GNU_SOURCE
#include "suite/nemesis.h"

#include "suite/parse.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Bounds for profile checks.
static constexpr int kMinNd = 2;
static constexpr int kMaxNd = 5;
static constexpr int kMinPh = 2;
static constexpr int kMaxPh = 12;
static constexpr int kMaxTo = 3600;
static constexpr int kFileTo = 60;
static constexpr int kMinTo = 10;
static constexpr int kMaxPTo = 40;
static constexpr int kMeshTo = 30;
static constexpr int kMaxPass = 4;
static constexpr int kMaxTry = 512;

// Rng keeps splitmix state.
// Self-contained so nemesis
// links without rng/: one
// less owner to coordinate
// plus stable bytes across
// trees.
typedef struct {
    uint64_t s;
} NemRng;

// Seeds the splitmix state.
static void rng_seed(NemRng *r,
                      uint64_t seed) {
    r->s = seed + 1u;
}

// Next splitmix64 output.
static uint64_t rng_next(NemRng *r) {
    uint64_t z;

    r->s += 0x9e3779b97f4a7c15ull;
    z = r->s;
    z = (z ^ (z >> 30)) *
        0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 27)) *
        0x94d049bb133111ebull;
    return (z ^ (z >> 31));
}

// Range 0..n-1. Zero when n
// is zero.
static uint64_t rng_range(NemRng *r,
                           uint64_t n) {
    if (n == 0u) {
        return (0u);
    }
    return (rng_next(r) % n);
}

// Int in lo..hi inclusive.
static int rng_int(NemRng *r, int lo,
                   int hi) {
    uint64_t span;
    uint64_t v;

    span = (uint64_t)(hi - lo + 1);
    v = rng_range(r, span);
    return (lo + (int)v);
}

// True when s is concord.
static bool sut_conc(const char *s) {
    return (strcmp(s, "concord") ==
            0);
}

// True when s is weld.
static bool sut_weld(const char *s) {
    return (strcmp(s, "weld") == 0);
}

// Copies sut safely. False
// when missing terminator.
static bool sut_copy(const NemProfile *p,
                      char *out) {
    size_t i;

    for (i = 0; i < 16u; i++) {
        out[i] = p->sut[i];
        if (p->sut[i] == '\0') {
            return (true);
        }
    }
    return (false);
}

// Validates a profile. False
// on any bad field.
static bool profile_ok(
    const NemProfile *p) {
    char sut[16];

    if (p == nullptr) {
        return (false);
    }
    if (!sut_copy(p, sut)) {
        return (false);
    }
    if (!sut_conc(sut) &&
        !sut_weld(sut)) {
        return (false);
    }
    if (p->nodes < kMinNd ||
        p->nodes > kMaxNd) {
        return (false);
    }
    if (p->nphases < kMinPh ||
        p->nphases > kMaxPh) {
        return (false);
    }
    if (p->max_drop < 0 ||
        p->max_kill < 0 ||
        p->max_restart < 0) {
        return (false);
    }
    return (true);
}

// Picks a node index.
static int pick_node(NemRng *r,
                     int nodes) {
    return (rng_int(r, 0, nodes - 1));
}

// Picks a phase timeout.
static int pick_pto(NemRng *r) {
    return (rng_int(r, kMinTo,
                    kMaxPTo));
}

// Emits the file header.
static bool emit_head(FILE *fp,
                       const NemProfile *p,
                       int conc) {
    char sut[16];
    unsigned long long sh;
    int fto;

    if (!sut_copy(p, sut)) {
        return (false);
    }
    sh = (unsigned long long)
        (p->seed & 0xffffffffull);
    fto = p->nphases * kFileTo;
    if (fto < 1 || fto > kMaxTo) {
        return (false);
    }
    if (conc != 0) {
        if (fprintf(fp,
                    "name: nemesis-c-"
                    "%08llx\n",
                    sh) < 0) {
            return (false);
        }
    } else {
        if (fprintf(fp,
                    "name: nemesis-w-"
                    "%08llx\n",
                    sh) < 0) {
            return (false);
        }
    }
    if (fputs("version: 1\n", fp) <
        0) {
        return (false);
    }
    if (fprintf(fp, "sut: %s\n",
                conc != 0 ? "concord"
                          : "weld") < 0) {
        return (false);
    }
    if (fprintf(fp, "nodes: %d\n",
                p->nodes) < 0) {
        return (false);
    }
    if (conc == 0) {
        if (fputs("domain: 64\n",
                  fp) < 0) {
            return (false);
        }
    }
    if (fprintf(fp,
                "timeout_s: %d\n"
                "phases:\n",
                fto) < 0) {
        return (false);
    }
    return (true);
}

// Emits the mesh gate first.
static bool emit_mesh(FILE *fp,
                       int nodes,
                       int conc) {
    if (fputs("  - name: mesh\n",
              fp) < 0) {
        return (false);
    }
    if (conc != 0) {
        if (fprintf(fp,
                    "    until:\n"
                    "      peers: %d\n"
                    "      on: all\n",
                    nodes) < 0) {
            return (false);
        }
    } else {
        if (fprintf(fp,
                    "    until:\n"
                    "      mesh_count:"
                    " %d\n",
                    nodes) < 0) {
            return (false);
        }
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                kMeshTo) < 0) {
        return (false);
    }
    return (true);
}

// Emits a dwell with no steps.
static bool emit_dwell(FILE *fp,
                        const char *nm,
                        int nodes,
                        int conc,
                        NemRng *r) {
    int to = pick_pto(r);

    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (conc != 0) {
        if (fprintf(fp,
                    "    until:\n"
                    "      peers: %d\n"
                    "      on: all\n",
                    nodes) < 0) {
            return (false);
        }
    } else {
        if (fprintf(fp,
                    "    until:\n"
                    "      mesh_count:"
                    " %d\n",
                    nodes) < 0) {
            return (false);
        }
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Emits a drop fault phase.
static bool emit_drop(FILE *fp,
                       const char *nm,
                       int nodes, int nd,
                       NemRng *r) {
    int to = pick_pto(r);

    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (fputs("    do:\n"
              "      - drop:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          - %d\n", nd) <
        0) {
        return (false);
    }
    if (fprintf(fp,
                "    until:\n"
                "      mesh_count:"
                " %d\n",
                nodes) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Emits a heal phase.
static bool emit_heal(FILE *fp,
                       const char *nm,
                       int nodes, int nd,
                       NemRng *r) {
    int to = pick_pto(r);

    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (fputs("    do:\n"
              "      - heal:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          - %d\n", nd) <
        0) {
        return (false);
    }
    if (fprintf(fp,
                "    until:\n"
                "      mesh_count:"
                " %d\n",
                nodes) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Emits kill plus fresh restart.
static bool emit_kill(FILE *fp,
                       const char *nm,
                       int nodes,
                       NemRng *r) {
    int nd = pick_node(r, nodes);
    int to = pick_pto(r);
    int term;

    term = (int)rng_range(r, 2u);
    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (fputs("    do:\n"
              "      - kill:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          node: %d\n",
                nd) < 0) {
        return (false);
    }
    if (fprintf(
            fp,
            "          signal: %s\n",
            term == 0 ? "KILL"
                      : "TERM") < 0) {
        return (false);
    }
    if (fputs("      - restart:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          node: %d\n",
                nd) < 0) {
        return (false);
    }
    if (fputs("          durability:"
              " fresh\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "    until:\n"
                "      mesh_count:"
                " %d\n",
                nodes) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Emits a concord submit phase.
static bool emit_submit(FILE *fp,
                         const char *nm,
                         NemRng *r,
                         int nodes) {
    int nd = pick_node(r, nodes);
    int to = pick_pto(r);

    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (fputs("    do:\n"
              "      - submit:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          node: %d\n",
                nd) < 0) {
        return (false);
    }
    if (fputs("          image:"
              " probe\n",
              fp) < 0) {
        return (false);
    }
    if (fputs("    until:\n"
              "      assigned:\n"
              "        workload:"
              " submitted\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Emits submit plus stop in one
// phase for tiny files.
static bool emit_substop(FILE *fp,
                          const char *nm,
                          NemRng *r) {
    int to = pick_pto(r);

    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (fputs("    do:\n"
              "      - submit:\n"
              "          node: 0\n"
              "          image:"
              " probe\n"
              "      - stop:\n"
              "          workload:"
              " submitted\n",
              fp) < 0) {
        return (false);
    }
    if (fputs("    until:\n"
              "      mesh_count:"
              " 1\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Emits a concord stop phase.
static bool emit_stop(FILE *fp,
                       const char *nm,
                       int nodes,
                       NemRng *r) {
    int to = pick_pto(r);

    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (fputs("    do:\n"
              "      - stop:\n"
              "          workload:"
              " submitted\n",
              fp) < 0) {
        return (false);
    }
    if (fputs("    until:\n"
              "      absent:\n"
              "        workload:"
              " submitted\n"
              "        on: all\n",
              fp) < 0) {
        return (false);
    }
    (void)nodes;
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Makes a weld pub text.
static void pub_text(NemRng *r,
                     char *out,
                     size_t n) {
    unsigned long long v;

    v = (unsigned long long)
        rng_next(r);
    (void)snprintf(out, n,
                   "wmsg%04llx",
                   v & 0xffffull);
}

// Emits a weld pub phase.
static bool emit_pub(FILE *fp,
                      const char *nm,
                      int nodes,
                      NemRng *r) {
    char tx[32];
    int from;
    int on;
    int to;

    pub_text(r, tx, sizeof(tx));
    from = pick_node(r, nodes);
    on = pick_node(r, nodes);
    to = pick_pto(r);
    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (fputs("    do:\n"
              "      - pub:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          from: %d\n",
                from) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          text: %s\n",
                tx) < 0) {
        return (false);
    }
    if (fputs("    until:\n"
              "      observed:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "        on: %d\n", on) <
        0) {
        return (false);
    }
    if (fprintf(fp,
                "        text: %s\n",
                tx) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Emits weld drop plus pub.
static bool emit_wdrop(FILE *fp,
                        const char *nm,
                        int nodes, int nd,
                        NemRng *r) {
    char tx[32];
    int from;
    int on;
    int to;

    pub_text(r, tx, sizeof(tx));
    from = pick_node(r, nodes);
    on = pick_node(r, nodes);
    to = pick_pto(r);
    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (fputs("    do:\n"
              "      - drop:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          - %d\n", nd) <
        0) {
        return (false);
    }
    if (fputs("      - pub:\n", fp) <
        0) {
        return (false);
    }
    if (fprintf(fp,
                "          from: %d\n",
                from) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          text: %s\n",
                tx) < 0) {
        return (false);
    }
    if (fputs("    until:\n"
              "      observed:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "        on: %d\n", on) <
        0) {
        return (false);
    }
    if (fprintf(fp,
                "        text: %s\n",
                tx) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Emits weld heal plus pub.
static bool emit_wheal(FILE *fp,
                        const char *nm,
                        int nodes, int nd,
                        NemRng *r) {
    char tx[32];
    int from;
    int on;
    int to;

    pub_text(r, tx, sizeof(tx));
    from = pick_node(r, nodes);
    on = pick_node(r, nodes);
    to = pick_pto(r);
    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (fputs("    do:\n"
              "      - heal:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          - %d\n", nd) <
        0) {
        return (false);
    }
    if (fputs("      - pub:\n", fp) <
        0) {
        return (false);
    }
    if (fprintf(fp,
                "          from: %d\n",
                from) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          text: %s\n",
                tx) < 0) {
        return (false);
    }
    if (fputs("    until:\n"
              "      observed:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "        on: %d\n", on) <
        0) {
        return (false);
    }
    if (fprintf(fp,
                "        text: %s\n",
                tx) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Emits weld kill plus pub.
static bool emit_wkill(FILE *fp,
                        const char *nm,
                        int nodes,
                        NemRng *r) {
    char tx[32];
    int nd;
    int from;
    int on;
    int to;

    pub_text(r, tx, sizeof(tx));
    nd = pick_node(r, nodes);
    from = pick_node(r, nodes);
    on = pick_node(r, nodes);
    to = pick_pto(r);
    if (fprintf(fp,
                "  - name: %s\n", nm) <
        0) {
        return (false);
    }
    if (fputs("    do:\n"
              "      - kill:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          node: %d\n",
                nd) < 0) {
        return (false);
    }
    if (fputs("          signal:"
              " KILL\n",
              fp) < 0) {
        return (false);
    }
    if (fputs("      - restart:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          node: %d\n",
                nd) < 0) {
        return (false);
    }
    if (fputs("          durability:"
              " fresh\n",
              fp) < 0) {
        return (false);
    }
    if (fputs("      - pub:\n", fp) <
        0) {
        return (false);
    }
    if (fprintf(fp,
                "          from: %d\n",
                from) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "          text: %s\n",
                tx) < 0) {
        return (false);
    }
    if (fputs("    until:\n"
              "      observed:\n",
              fp) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "        on: %d\n", on) <
        0) {
        return (false);
    }
    if (fprintf(fp,
                "        text: %s\n",
                tx) < 0) {
        return (false);
    }
    if (fprintf(fp,
                "    timeout_s: %d\n",
                to) < 0) {
        return (false);
    }
    return (true);
}

// Builds a phase name p01...
static void phase_nm(int idx,
                     char *out,
                     size_t n) {
    (void)snprintf(out, n, "p%02d",
                   idx);
}

// Generates a concord file.
static bool gen_conc(FILE *fp,
                      const NemProfile *p,
                      NemRng *r) {
    int drop = p->max_drop;
    int kill = p->max_kill;
    int rest = p->max_restart;
    int want_sub;
    int want_stop;
    int sub_at;
    int stop_at;
    int pend = -1;
    int i;

    want_sub = p->allow_submit != 0;
    want_stop = want_sub &&
        p->allow_stop != 0;
    sub_at = want_sub ? 1 : -1;
    stop_at = -1;
    if (want_stop) {
        if (p->nphases == 2) {
            stop_at = 1;
        } else {
            stop_at = p->nphases - 1;
        }
    }
    if (sub_at == 1 && stop_at == 1) {
        char nm[16];

        phase_nm(1, nm, sizeof(nm));
        if (!emit_substop(fp, nm,
                           r)) {
            return (false);
        }
        return (true);
    }
    for (i = 1; i < p->nphases;
         i++) {
        char nm[16];

        phase_nm(i, nm, sizeof(nm));
        if (i == sub_at) {
            if (!emit_submit(
                    fp, nm, r,
                    p->nodes)) {
                return (false);
            }
            continue;
        }
        if (i == stop_at) {
            if (!emit_stop(fp, nm,
                            p->nodes,
                            r)) {
                return (false);
            }
            continue;
        }
        if (pend >= 0) {
            if (!emit_heal(fp, nm,
                            p->nodes,
                            pend, r)) {
                return (false);
            }
            pend = -1;
            continue;
        }
        {
            uint64_t pick;

            pick = rng_range(r, 4u);
            if (pick == 0u &&
                drop > 0) {
                int nd;

                nd = pick_node(
                    r, p->nodes);
                if (!emit_drop(
                        fp, nm,
                        p->nodes, nd,
                        r)) {
                    return (false);
                }
                drop--;
                pend = nd;
                continue;
            }
            if (pick == 1u &&
                kill > 0 && rest > 0) {
                if (!emit_kill(
                        fp, nm,
                        p->nodes, r)) {
                    return (false);
                }
                kill--;
                rest--;
                continue;
            }
            if (!emit_dwell(
                    fp, nm, p->nodes,
                    1, r)) {
                return (false);
            }
        }
    }
    return (true);
}

// Generates a weld file.
static bool gen_weld(FILE *fp,
                      const NemProfile *p,
                      NemRng *r) {
    int drop = p->max_drop;
    int kill = p->max_kill;
    int rest = p->max_restart;
    int pend = -1;
    int i;

    for (i = 1; i < p->nphases;
         i++) {
        char nm[16];

        phase_nm(i, nm, sizeof(nm));
        if (pend >= 0) {
            if (!emit_wheal(
                    fp, nm, p->nodes,
                    pend, r)) {
                return (false);
            }
            pend = -1;
            continue;
        }
        {
            uint64_t pick;

            pick = rng_range(r, 4u);
            if (pick == 0u &&
                drop > 0) {
                int nd;

                nd = pick_node(
                    r, p->nodes);
                if (!emit_wdrop(
                        fp, nm,
                        p->nodes, nd,
                        r)) {
                    return (false);
                }
                drop--;
                pend = nd;
                continue;
            }
            if (pick == 1u &&
                kill > 0 && rest > 0) {
                if (!emit_wkill(
                        fp, nm,
                        p->nodes, r)) {
                    return (false);
                }
                kill--;
                rest--;
                continue;
            }
            if (!emit_pub(fp, nm,
                           p->nodes,
                           r)) {
                return (false);
            }
        }
    }
    return (true);
}

// Writes a valid suite file
// deterministically from seed.
bool nemesis_gen(const NemProfile *p,
                 const char *outpath) {
    FILE *fp = nullptr;
    NemRng r;
    char sut[16];
    int conc;

    if (!profile_ok(p)) {
        return (false);
    }
    if (outpath == nullptr) {
        return (false);
    }
    if (!sut_copy(p, sut)) {
        return (false);
    }
    conc = sut_conc(sut) ? 1 : 0;
    fp = fopen(outpath, "w");
    if (fp == nullptr) {
        return (false);
    }
    rng_seed(&r, p->seed);
    if (!emit_head(fp, p, conc)) {
        (void)fclose(fp);
        return (false);
    }
    if (!emit_mesh(fp, p->nodes,
                   conc)) {
        (void)fclose(fp);
        return (false);
    }
    if (conc != 0) {
        if (!gen_conc(fp, p, &r)) {
            (void)fclose(fp);
            return (false);
        }
    } else {
        if (!gen_weld(fp, p, &r)) {
            (void)fclose(fp);
            return (false);
        }
    }
    if (fclose(fp) != 0) {
        return (false);
    }
    return (true);
}

// Reads a file into memory.
static bool read_all(const char *path,
                      char **out,
                      size_t *nout) {
    FILE *fp = nullptr;
    long end = 0;
    size_t n = 0;
    char *tx = nullptr;

    *out = nullptr;
    *nout = 0;
    if (path == nullptr) {
        return (false);
    }
    fp = fopen(path, "rb");
    if (fp == nullptr) {
        return (false);
    }
    if (fseek(fp, 0, SEEK_END) !=
        0) {
        (void)fclose(fp);
        return (false);
    }
    end = ftell(fp);
    if (end < 0) {
        (void)fclose(fp);
        return (false);
    }
    if (fseek(fp, 0, SEEK_SET) !=
        0) {
        (void)fclose(fp);
        return (false);
    }
    n = (size_t)end;
    tx = malloc(n + 1u);
    if (tx == nullptr) {
        (void)fclose(fp);
        return (false);
    }
    if (n > 0u &&
        fread(tx, 1, n, fp) != n) {
        free(tx);
        (void)fclose(fp);
        return (false);
    }
    (void)fclose(fp);
    tx[n] = '\0';
    *out = tx;
    *nout = n;
    return (true);
}

// Writes memory to a file.
static bool write_all(const char *path,
                       const char *tx,
                       size_t n) {
    FILE *fp = nullptr;

    if (path == nullptr ||
        tx == nullptr) {
        return (false);
    }
    fp = fopen(path, "wb");
    if (fp == nullptr) {
        return (false);
    }
    if (n > 0u &&
        fwrite(tx, 1, n, fp) != n) {
        (void)fclose(fp);
        return (false);
    }
    if (fclose(fp) != 0) {
        return (false);
    }
    return (true);
}

// Builds line start offsets.
static bool line_starts(
    const char *tx, size_t n,
    size_t **out, size_t *nl) {
    size_t cnt = 1u;
    size_t *tab = nullptr;
    size_t k = 0u;
    size_t i = 0u;

    *out = nullptr;
    *nl = 0;
    if (tx == nullptr) {
        return (false);
    }
    if (n == 0u) {
        tab = malloc(sizeof(*tab));
        if (tab == nullptr) {
            return (false);
        }
        tab[0] = 0u;
        *out = tab;
        *nl = 1u;
        return (true);
    }
    for (i = 0u; i < n; i++) {
        if (tx[i] == '\n' &&
            i + 1u < n) {
            cnt++;
        }
    }
    tab = malloc(cnt *
                 sizeof(*tab));
    if (tab == nullptr) {
        return (false);
    }
    tab[0] = 0u;
    k = 1u;
    for (i = 0u; i < n; i++) {
        if (tx[i] == '\n' &&
            i + 1u < n) {
            tab[k] = i + 1u;
            k++;
        }
    }
    *out = tab;
    *nl = k;
    return (true);
}

// Offset of 1-based line.
static size_t line_off(
    size_t *tab, size_t nl,
    size_t len, size_t line) {
    if (tab == nullptr) {
        return (len);
    }
    if (line < 1u) {
        return (0u);
    }
    if (line > nl) {
        return (len);
    }
    return (tab[line - 1u]);
}

// True when step holds verb.
static bool step_is(SuiteNode *st,
                    const char *verb) {
    if (st == nullptr ||
        verb == nullptr) {
        return (false);
    }
    if (st->kind != SUITE_MAP) {
        return (false);
    }
    if (st->u.map.len != 1u) {
        return (false);
    }
    return (strcmp(
                st->u.map.items[0].key,
                verb) == 0);
}

// True when cond needs a prior
// submit: assigned or present
// or absent over workload.
static bool cond_needs(
    SuiteNode *ph) {
    SuiteNode *u = nullptr;
    SuiteNode *t = nullptr;
    SuiteNode *c = nullptr;
    SuiteNode *in = nullptr;
    SuiteNode *w = nullptr;
    const char *s = nullptr;

    if (ph == nullptr) {
        return (false);
    }
    u = suite_map_find(ph, "until");
    t = suite_map_find(ph,
                       "sustain");
    c = u != nullptr ? u : t;
    if (c == nullptr) {
        return (false);
    }
    if (c->kind != SUITE_MAP) {
        return (false);
    }
    if (suite_map_find(
            c, "assigned") !=
        nullptr) {
        return (true);
    }
    in = suite_map_find(c,
                        "present");
    if (in == nullptr) {
        in = suite_map_find(
            c, "absent");
    }
    if (in == nullptr ||
        in->kind != SUITE_MAP) {
        return (false);
    }
    w = suite_map_find(in,
                       "workload");
    if (w == nullptr) {
        return (false);
    }
    s = suite_scalar_str(w);
    if (s == nullptr) {
        return (false);
    }
    return (strcmp(s, "submit") ==
                0 ||
            strcmp(s, "submitted") ==
                0);
}

// Checks structural validity:
// parseable, required keys,
// one cond, unique names, no
// dangling stop or workload.
static bool trial_valid(
    const char *tx, size_t n) {
    SuiteDoc *doc = nullptr;
    SuiteError err = {0, 0, ""};
    SuiteNode *root = nullptr;
    SuiteNode *phs = nullptr;
    size_t np = 0u;
    size_t i = 0u;
    bool saw = false;

    if (tx == nullptr) {
        return (false);
    }
    if (!suite_parse(tx, n, &doc,
                     &err)) {
        return (false);
    }
    root = doc->root;
    if (root == nullptr ||
        root->kind != SUITE_MAP) {
        suite_doc_free(doc);
        return (false);
    }
    if (suite_map_find(root,
                       "name") ==
        nullptr) {
        suite_doc_free(doc);
        return (false);
    }
    if (suite_map_find(root,
                       "version") ==
        nullptr) {
        suite_doc_free(doc);
        return (false);
    }
    if (suite_map_find(root,
                       "sut") ==
        nullptr) {
        suite_doc_free(doc);
        return (false);
    }
    if (suite_map_find(root,
                       "nodes") ==
        nullptr) {
        suite_doc_free(doc);
        return (false);
    }
    if (suite_map_find(root,
                       "timeout_s") ==
        nullptr) {
        suite_doc_free(doc);
        return (false);
    }
    phs = suite_map_find(root,
                         "phases");
    if (phs == nullptr ||
        phs->kind != SUITE_LIST) {
        suite_doc_free(doc);
        return (false);
    }
    np = suite_list_len(phs);
    if (np == 0u) {
        suite_doc_free(doc);
        return (false);
    }
    for (i = 0u; i < np; i++) {
        SuiteNode *ph;
        SuiteNode *nm;
        SuiteNode *u;
        SuiteNode *t;
        SuiteNode *d;
        size_t j = 0u;
        const char *s = nullptr;

        ph = suite_list_at(phs, i);
        if (ph == nullptr ||
            ph->kind != SUITE_MAP) {
            suite_doc_free(doc);
            return (false);
        }
        nm = suite_map_find(ph,
                            "name");
        s = suite_scalar_str(nm);
        if (s == nullptr ||
            s[0] == '\0') {
            suite_doc_free(doc);
            return (false);
        }
        for (j = 0u; j < i; j++) {
            SuiteNode *q;
            const char *a = nullptr;
            const char *b = nullptr;

            q = suite_list_at(phs,
                              j);
            a = suite_scalar_str(
                suite_map_find(q,
                               "name"));
            b = s;
            if (a != nullptr &&
                strcmp(a, b) == 0) {
                suite_doc_free(doc);
                return (false);
            }
        }
        u = suite_map_find(ph,
                           "until");
        t = suite_map_find(
            ph, "sustain");
        if ((u == nullptr) ==
            (t == nullptr)) {
            suite_doc_free(doc);
            return (false);
        }
        if (suite_map_find(
                ph, "timeout_s") ==
            nullptr) {
            suite_doc_free(doc);
            return (false);
        }
        d = suite_map_find(ph,
                           "do");
        if (d != nullptr) {
            size_t k = 0u;

            if (d->kind !=
                SUITE_LIST) {
                suite_doc_free(doc);
                return (false);
            }
            for (k = 0u;
                 k <
                 suite_list_len(d);
                 k++) {
                SuiteNode *st;

                st = suite_list_at(
                    d, k);
                if (st == nullptr ||
                    st->kind !=
                        SUITE_MAP ||
                    st->u.map.len !=
                        1u) {
                    suite_doc_free(
                        doc);
                    return (false);
                }
                if (step_is(
                        st,
                        "submit")) {
                    saw = true;
                    continue;
                }
                if (step_is(
                        st, "stop") &&
                    !saw) {
                    suite_doc_free(
                        doc);
                    return (false);
                }
            }
        }
        if (cond_needs(ph) &&
            !saw) {
            // Same-phase submit
            // counts, so check
            // steps first above
            // then cond here.
            suite_doc_free(doc);
            return (false);
        }
        // Same-phase submit
        // already sets saw,
        // later phases see it.
    }
    suite_doc_free(doc);
    return (true);
}

// Next phase key line after ln.
static size_t next_key_after(
    SuiteNode *ph, size_t ln) {
    size_t best = 0u;
    size_t i = 0u;

    if (ph == nullptr ||
        ph->kind != SUITE_MAP) {
        return (0u);
    }
    for (i = 0u;
         i < ph->u.map.len; i++) {
        size_t kl;

        kl = ph->u.map.items[i]
                 .key_line;
        if (kl <= ln) {
            continue;
        }
        if (best == 0u ||
            kl < best) {
            best = kl;
        }
    }
    return (best);
}

// Do key line or zero.
static size_t do_key_line(
    SuiteNode *ph) {
    size_t i = 0u;

    if (ph == nullptr ||
        ph->kind != SUITE_MAP) {
        return (0u);
    }
    for (i = 0u;
         i < ph->u.map.len; i++) {
        if (strcmp(ph->u.map.items[i]
                       .key,
                   "do") == 0) {
            return (ph->u.map.items[i]
                        .key_line);
        }
    }
    return (0u);
}

// Builds a trial without phase
// j. Caller frees out.
static bool trial_no_phase(
    const char *tx, size_t n,
    size_t *tab, size_t nl,
    SuiteNode *phs, size_t j,
    char **out, size_t *nout) {
    SuiteNode *me = nullptr;
    SuiteNode *nx = nullptr;
    size_t np = 0u;
    size_t a = 0u;
    size_t b = 0u;
    char *tt = nullptr;

    *out = nullptr;
    *nout = 0;
    np = suite_list_len(phs);
    me = suite_list_at(phs, j);
    if (me == nullptr) {
        return (false);
    }
    a = line_off(tab, nl, n,
                 me->line);
    if (j + 1u < np) {
        nx = suite_list_at(
            phs, j + 1u);
        if (nx == nullptr) {
            return (false);
        }
        b = line_off(tab, nl, n,
                     nx->line);
    } else {
        b = n;
    }
    if (a > b || b > n) {
        return (false);
    }
    tt = malloc(n - (b - a) + 1u);
    if (tt == nullptr) {
        return (false);
    }
    memcpy(tt, tx, a);
    memcpy(tt + a, tx + b,
           n - b);
    tt[n - (b - a)] = '\0';
    *out = tt;
    *nout = n - (b - a);
    return (true);
}

// Builds a trial without step
// k of phase j. Caller frees.
static bool trial_no_step(
    const char *tx, size_t n,
    size_t *tab, size_t nl,
    SuiteNode *phs, size_t j,
    size_t k, char **out,
    size_t *nout) {
    SuiteNode *ph = nullptr;
    SuiteNode *d = nullptr;
    SuiteNode *me = nullptr;
    SuiteNode *nx = nullptr;
    size_t ns = 0u;
    size_t a = 0u;
    size_t b = 0u;
    char *tt = nullptr;

    *out = nullptr;
    *nout = 0;
    ph = suite_list_at(phs, j);
    if (ph == nullptr) {
        return (false);
    }
    d = suite_map_find(ph, "do");
    if (d == nullptr) {
        return (false);
    }
    ns = suite_list_len(d);
    me = suite_list_at(d, k);
    if (me == nullptr) {
        return (false);
    }
    a = line_off(tab, nl, n,
                 me->line);
    if (k + 1u < ns) {
        nx = suite_list_at(
            d, k + 1u);
        if (nx == nullptr) {
            return (false);
        }
        b = line_off(tab, nl, n,
                     nx->line);
    } else {
        size_t post;

        post = next_key_after(
            ph, me->line);
        if (post != 0u) {
            b = line_off(tab, nl,
                         n, post);
        } else {
            size_t np;

            np = suite_list_len(
                phs);
            if (j + 1u < np) {
                SuiteNode *jp;

                jp = suite_list_at(
                    phs, j + 1u);
                if (jp ==
                    nullptr) {
                    return (false);
                }
                b = line_off(
                    tab, nl, n,
                    jp->line);
            } else {
                b = n;
            }
        }
    }
    if (ns == 1u) {
        size_t dk;

        dk = do_key_line(ph);
        if (dk == 0u) {
            return (false);
        }
        a = line_off(tab, nl, n,
                     dk);
    }
    if (a > b || b > n) {
        return (false);
    }
    tt = malloc(n - (b - a) + 1u);
    if (tt == nullptr) {
        return (false);
    }
    memcpy(tt, tx, a);
    memcpy(tt + a, tx + b,
           n - b);
    tt[n - (b - a)] = '\0';
    *out = tt;
    *nout = n - (b - a);
    return (true);
}

// Calls oracle on a temp file.
// Sets bad on oracle errors.
static int call_oracle(
    NemOracle o, const char *tx,
    size_t n, void *ud,
    bool *bad) {
    char tmpl[64];
    int fd = -1;
    FILE *fp = nullptr;
    int v = -1;

    *bad = true;
    (void)snprintf(
        tmpl, sizeof(tmpl),
        "/tmp/opencode/nem-%d-"
        "XXXXXX",
        (int)getpid());
    fd = mkstemp(tmpl);
    if (fd < 0) {
        return (-1);
    }
    fp = fdopen(fd, "w");
    if (fp == nullptr) {
        (void)close(fd);
        (void)unlink(tmpl);
        return (-1);
    }
    if (n > 0u &&
        fwrite(tx, 1, n, fp) != n) {
        (void)fclose(fp);
        (void)unlink(tmpl);
        return (-1);
    }
    if (fclose(fp) != 0) {
        (void)unlink(tmpl);
        return (-1);
    }
    v = o(tmpl, ud);
    (void)unlink(tmpl);
    if (v < 0 || v > 2) {
        *bad = true;
        return (-1);
    }
    *bad = false;
    return (v);
}

// Shrinks while oracle fails.
// Bounded, deterministic.
bool nemesis_minimize(
    const char *yamlpath,
    NemOracle oracle, void *ud,
    const char *outpath) {
    char *cur = nullptr;
    size_t cn = 0u;
    int v = -1;
    int pass = 0;
    int tries = 0;

    if (yamlpath == nullptr ||
        oracle == nullptr ||
        outpath == nullptr) {
        return (false);
    }
    if (!read_all(yamlpath, &cur,
                  &cn)) {
        return (false);
    }
    if (!trial_valid(cur, cn)) {
        free(cur);
        return (false);
    }
    v = oracle(yamlpath, ud);
    if (v < 0 || v > 2) {
        free(cur);
        return (false);
    }
    if (v != 1) {
        free(cur);
        return (false);
    }
    for (pass = 0; pass < kMaxPass;
         pass++) {
        bool moved = false;
        SuiteDoc *doc = nullptr;
        SuiteError err = {0, 0, ""};
        SuiteNode *phs = nullptr;
        size_t np = 0u;
        size_t j = 0u;

        if (tries >= kMaxTry) {
            break;
        }
        if (!suite_parse(cur, cn,
                         &doc, &err)) {
            free(cur);
            return (false);
        }
        phs = suite_map_find(
            doc->root, "phases");
        if (phs == nullptr) {
            suite_doc_free(doc);
            free(cur);
            return (false);
        }
        np = suite_list_len(phs);
        // Drop phases last to
        // first, never phase 0.
        for (j = np; j > 1u; j--) {
            size_t idx;
            char *tt = nullptr;
            size_t tn = 0u;
            size_t *tab = nullptr;
            size_t nl = 0u;
            bool bad = false;
            int w = -1;

            idx = j - 1u;
            if (tries >= kMaxTry) {
                break;
            }
            tries++;
            if (!line_starts(
                    cur, cn, &tab,
                    &nl)) {
                suite_doc_free(doc);
                free(cur);
                return (false);
            }
            if (!trial_no_phase(
                    cur, cn, tab, nl,
                    phs, idx, &tt,
                    &tn)) {
                free(tab);
                continue;
            }
            free(tab);
            if (!trial_valid(tt,
                             tn)) {
                free(tt);
                continue;
            }
            w = call_oracle(
                oracle, tt, tn, ud,
                &bad);
            if (bad) {
                free(tt);
                suite_doc_free(
                    doc);
                free(cur);
                return (false);
            }
            if (w == 1) {
                free(cur);
                cur = tt;
                cn = tn;
                moved = true;
                // Re-parse after
                // accept; indices
                // shift, so stop
                // this sweep.
                break;
            }
            free(tt);
        }
        suite_doc_free(doc);
        if (moved) {
            pass = -1;
            continue;
        }
        break;
    }
    for (pass = 0; pass < kMaxPass;
         pass++) {
        bool moved = false;

        if (tries >= kMaxTry) {
            break;
        }
        {
            SuiteDoc *doc = nullptr;
            SuiteError err = {0, 0,
                              ""};
            SuiteNode *phs = nullptr;
            size_t np = 0u;
            size_t j = 0u;

            if (!suite_parse(
                    cur, cn, &doc,
                    &err)) {
                free(cur);
                return (false);
            }
            phs = suite_map_find(
                doc->root,
                "phases");
            if (phs == nullptr) {
                suite_doc_free(
                    doc);
                free(cur);
                return (false);
            }
            np = suite_list_len(
                phs);
            for (j = np; j > 0u;
                 j--) {
                size_t idx;
                SuiteNode *ph;
                SuiteNode *d;
                size_t ns = 0u;
                size_t k = 0u;

                idx = j - 1u;
                ph = suite_list_at(
                    phs, idx);
                if (ph ==
                    nullptr) {
                    continue;
                }
                d = suite_map_find(
                    ph, "do");
                if (d ==
                    nullptr) {
                    continue;
                }
                ns = suite_list_len(
                    d);
                for (k = ns; k > 0u;
                     k--) {
                    size_t sk;
                    char *tt = nullptr;
                    size_t tn = 0u;
                    size_t *tab =
                        nullptr;
                    size_t nl = 0u;
                    bool bad = false;
                    int w = -1;

                    sk = k - 1u;
                    if (tries >=
                        kMaxTry) {
                        break;
                    }
                    tries++;
                    if (!line_starts(
                            cur, cn,
                            &tab,
                            &nl)) {
                        suite_doc_free(
                            doc);
                        free(cur);
                        return (false);
                    }
                    if (!trial_no_step(
                            cur, cn,
                            tab, nl,
                            phs, idx,
                            sk, &tt,
                            &tn)) {
                        free(tab);
                        continue;
                    }
                    free(tab);
                    if (!trial_valid(
                            tt, tn)) {
                        free(tt);
                        continue;
                    }
                    w = call_oracle(
                        oracle, tt, tn,
                        ud, &bad);
                    if (bad) {
                        free(tt);
                        suite_doc_free(
                            doc);
                        free(cur);
                        return (
                            false);
                    }
                    if (w == 1) {
                        free(cur);
                        cur = tt;
                        cn = tn;
                        moved = true;
                        break;
                    }
                    free(tt);
                }
                if (moved) {
                    break;
                }
            }
            suite_doc_free(doc);
        }
        if (!moved) {
            break;
        }
    }
    {
        bool ok;

        ok = write_all(outpath, cur,
                       cn);
        free(cur);
        return (ok);
    }
}
