#include "suite/nemesis.h"

#include "suite/parse.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Caps for test buffers.
static constexpr int kBuf = 8192;

// Reads a file. Asserts ok.
static void must_read(const char *path,
                       char *out,
                       size_t *nout) {
    FILE *fp;
    long end;
    size_t n;

    fp = fopen(path, "rb");
    assert(fp != nullptr);
    assert(fseek(fp, 0, SEEK_END) ==
           0);
    end = ftell(fp);
    assert(end >= 0);
    assert(fseek(fp, 0, SEEK_SET) ==
           0);
    n = (size_t)end;
    assert(n < (size_t)kBuf);
    assert(fread(out, 1, n, fp) ==
           n);
    assert(fclose(fp) == 0);
    out[n] = '\0';
    *nout = n;
}

// True when files match.
static bool files_same(
    const char *a, const char *b) {
    char ta[kBuf];
    char tb[kBuf];
    size_t na = 0u;
    size_t nb = 0u;

    must_read(a, ta, &na);
    must_read(b, tb, &nb);
    if (na != nb) {
        return (false);
    }
    return (memcmp(ta, tb, na) ==
            0);
}

// Counts phases via parse.
static size_t phase_count(
    const char *path) {
    char tx[kBuf];
    size_t n = 0u;
    SuiteDoc *doc = nullptr;
    SuiteError err = {0, 0, ""};
    SuiteNode *phs = nullptr;
    size_t c = 0u;

    must_read(path, tx, &n);
    assert(suite_parse(tx, n, &doc,
                       &err));
    phs = suite_map_find(doc->root,
                         "phases");
    assert(phs != nullptr);
    c = suite_list_len(phs);
    suite_doc_free(doc);
    return (c);
}

// True when file has marker.
static bool file_has(
    const char *path,
    const char *mark) {
    char tx[kBuf];
    size_t n = 0u;

    must_read(path, tx, &n);
    return (strstr(tx, mark) !=
            nullptr);
}

// Stub: fail when marker hit.
static int stub_mark(
    const char *path, void *ud) {
    const char *m;
    char tx[kBuf];
    FILE *fp;
    long end;
    size_t n;

    m = ud;
    fp = fopen(path, "rb");
    if (fp == nullptr) {
        return (-1);
    }
    if (fseek(fp, 0, SEEK_END) !=
        0) {
        (void)fclose(fp);
        return (-1);
    }
    end = ftell(fp);
    if (end < 0) {
        (void)fclose(fp);
        return (-1);
    }
    if (fseek(fp, 0, SEEK_SET) !=
        0) {
        (void)fclose(fp);
        return (-1);
    }
    n = (size_t)end;
    if (n >= (size_t)kBuf) {
        (void)fclose(fp);
        return (-1);
    }
    if (n > 0u &&
        fread(tx, 1, n, fp) != n) {
        (void)fclose(fp);
        return (-1);
    }
    (void)fclose(fp);
    tx[n] = '\0';
    if (strstr(tx, m) != nullptr) {
        return (1);
    }
    return (0);
}

// Stub: fail on mark, else skip.
static int stub_skip(
    const char *path, void *ud) {
    int v;

    v = stub_mark(path, ud);
    if (v == 0) {
        return (2);
    }
    return (v);
}

// Stub: always errors.
static int stub_err(
    const char *path, void *ud) {
    (void)path;
    (void)ud;
    return (7);
}

// Makes a base profile.
static NemProfile base_prof(
    uint64_t seed, int nodes,
    int nph, const char *sut) {
    NemProfile p;

    memset(&p, 0, sizeof(p));
    p.seed = seed;
    p.nodes = nodes;
    p.nphases = nph;
    p.max_drop = 2;
    p.max_kill = 2;
    p.max_restart = 2;
    p.allow_submit = 1;
    p.allow_stop = 1;
    (void)snprintf(p.sut,
                   sizeof(p.sut),
                   "%s", sut);
    return (p);
}

// Checks mesh gate first.
static void need_gate(
    const char *path, int nodes,
    int conc) {
    char tx[kBuf];
    size_t n = 0u;
    SuiteDoc *doc = nullptr;
    SuiteError err = {0, 0, ""};
    SuiteNode *phs = nullptr;
    SuiteNode *p0 = nullptr;
    SuiteNode *u = nullptr;

    must_read(path, tx, &n);
    assert(suite_parse(tx, n, &doc,
                       &err));
    phs = suite_map_find(doc->root,
                         "phases");
    assert(phs != nullptr);
    assert(suite_list_len(phs) >=
           1u);
    p0 = suite_list_at(phs, 0);
    assert(p0 != nullptr);
    assert(suite_map_find(
               p0, "do") == nullptr);
    u = suite_map_find(p0,
                       "until");
    assert(u != nullptr);
    if (conc != 0) {
        SuiteNode *pe;
        long long c = 0;

        pe = suite_map_find(u,
                            "peers");
        assert(pe != nullptr);
        assert(suite_scalar_int(
            pe, &c));
        assert(c == nodes);
    } else {
        SuiteNode *mc;
        long long c = 0;

        mc = suite_map_find(
            u, "mesh_count");
        assert(mc != nullptr);
        assert(suite_scalar_int(
            mc, &c));
        assert(c == nodes);
    }
    suite_doc_free(doc);
}

// Checks structural validity.
static void need_struct(
    const char *path) {
    char tx[kBuf];
    size_t n = 0u;
    SuiteDoc *doc = nullptr;
    SuiteError err = {0, 0, ""};
    SuiteNode *phs = nullptr;
    size_t np = 0u;
    size_t i = 0u;

    must_read(path, tx, &n);
    assert(suite_parse(tx, n, &doc,
                       &err));
    assert(doc->root->kind ==
           SUITE_MAP);
    phs = suite_map_find(doc->root,
                         "phases");
    assert(phs != nullptr);
    np = suite_list_len(phs);
    assert(np >= 1u);
    for (i = 0u; i < np; i++) {
        SuiteNode *ph;
        SuiteNode *nm;
        SuiteNode *u;
        SuiteNode *t;
        size_t j = 0u;

        ph = suite_list_at(phs, i);
        assert(ph != nullptr);
        nm = suite_map_find(ph,
                            "name");
        assert(suite_scalar_str(
                   nm) != nullptr);
        u = suite_map_find(ph,
                           "until");
        t = suite_map_find(
            ph, "sustain");
        assert((u == nullptr) !=
               (t == nullptr));
        assert(suite_map_find(
                   ph,
                   "timeout_s") !=
               nullptr);
        for (j = 0u; j < i; j++) {
            const char *a;
            const char *b;

            a = suite_scalar_str(
                suite_map_find(
                    suite_list_at(
                        phs, j),
                    "name"));
            b = suite_scalar_str(
                nm);
            assert(a != nullptr);
            assert(b != nullptr);
            assert(strcmp(a, b) !=
                   0);
        }
    }
    suite_doc_free(doc);
}

// Writes a canned 5-phase file.
static void write_canned(
    const char *path) {
    FILE *fp;

    fp = fopen(path, "w");
    assert(fp != nullptr);
    assert(fputs(
               "name: canned\n"
               "version: 1\n"
               "sut: concord\n"
               "nodes: 3\n"
               "timeout_s: 300\n"
               "phases:\n"
               "  - name: mesh\n"
               "    until:\n"
               "      peers: 3\n"
               "      on: all\n"
               "    timeout_s: 30\n"
               "  - name: p01\n"
               "    until:\n"
               "      mesh_count: 3\n"
               "    timeout_s: 10\n"
               "  - name: p02\n"
               "    until:\n"
               "      marker:"
               " keepfail\n"
               "      on: all\n"
               "    timeout_s: 10\n"
               "  - name: p03\n"
               "    until:\n"
               "      mesh_count: 3\n"
               "    timeout_s: 10\n"
               "  - name: p04\n"
               "    do:\n"
               "      - drop:\n"
               "          - 1\n"
               "    until:\n"
               "      mesh_count: 3\n"
               "    timeout_s: 10\n",
               fp) >= 0);
    assert(fclose(fp) == 0);
}

// Tests determinism.
static void test_determ(void) {
    NemProfile p;
    const char *a;
    const char *b;

    a = "/tmp/opencode/nem-a.yaml";
    b = "/tmp/opencode/nem-b.yaml";
    p = base_prof(42u, 3, 5,
                  "concord");
    assert(nemesis_gen(&p, a));
    assert(nemesis_gen(&p, b));
    assert(files_same(a, b));
    p = base_prof(99u, 2, 4,
                  "weld");
    assert(nemesis_gen(&p, a));
    assert(nemesis_gen(&p, b));
    assert(files_same(a, b));
    (void)remove(a);
    (void)remove(b);
}

// Tests profile rejects.
static void test_reject(void) {
    NemProfile p;
    const char *o;

    o = "/tmp/opencode/nem-r.yaml";
    p = base_prof(1u, 1, 5,
                  "concord");
    assert(!nemesis_gen(&p, o));
    p = base_prof(1u, 6, 5,
                  "concord");
    assert(!nemesis_gen(&p, o));
    p = base_prof(1u, 3, 1,
                  "concord");
    assert(!nemesis_gen(&p, o));
    p = base_prof(1u, 3, 99,
                  "concord");
    assert(!nemesis_gen(&p, o));
    p = base_prof(1u, 3, 5,
                  "bogus");
    assert(!nemesis_gen(&p, o));
    p = base_prof(1u, 3, 5,
                  "concord");
    p.max_drop = -1;
    assert(!nemesis_gen(&p, o));
    assert(!nemesis_gen(nullptr,
                        o));
    p = base_prof(1u, 3, 5,
                  "concord");
    assert(!nemesis_gen(&p,
                        nullptr));
}

// Tests mesh gate first.
static void test_gate(void) {
    NemProfile p;
    const char *o;

    o = "/tmp/opencode/nem-g.yaml";
    p = base_prof(7u, 3, 5,
                  "concord");
    assert(nemesis_gen(&p, o));
    need_gate(o, 3, 1);
    need_struct(o);
    p = base_prof(7u, 2, 4,
                  "weld");
    assert(nemesis_gen(&p, o));
    need_gate(o, 2, 0);
    need_struct(o);
    (void)remove(o);
}

// Tests verb gating plus caps.
static void test_verbs(void) {
    NemProfile p;
    const char *o;

    o = "/tmp/opencode/nem-v.yaml";
    p = base_prof(5u, 3, 6,
                  "concord");
    p.allow_submit = 0;
    p.allow_stop = 1;
    assert(nemesis_gen(&p, o));
    assert(!file_has(o, "submit:"));
    assert(!file_has(o, "stop:"));
    need_struct(o);
    p = base_prof(5u, 3, 6,
                  "concord");
    p.allow_stop = 0;
    assert(nemesis_gen(&p, o));
    assert(!file_has(o, "stop:"));
    need_struct(o);
    p = base_prof(5u, 3, 6,
                  "concord");
    p.max_drop = 0;
    p.max_kill = 0;
    p.max_restart = 0;
    assert(nemesis_gen(&p, o));
    assert(!file_has(o, "drop:"));
    assert(!file_has(o, "kill:"));
    assert(!file_has(o,
                     "restart:"));
    need_struct(o);
    (void)remove(o);
}

// Tests minimizer shrinks.
static void test_shrink(void) {
    const char *in;
    const char *out;
    size_t before = 0u;
    size_t after = 0u;

    in = "/tmp/opencode/nem-i.yaml";
    out = "/tmp/opencode/nem-o.yaml";
    write_canned(in);
    before = phase_count(in);
    assert(before == 5u);
    assert(stub_mark(in,
                     "keepfail") ==
           1);
    assert(nemesis_minimize(
        in, stub_mark, "keepfail",
        out));
    after = phase_count(out);
    assert(after < before);
    assert(file_has(out,
                    "keepfail"));
    assert(stub_mark(out,
                     "keepfail") ==
           1);
    need_struct(out);
    need_gate(out, 3, 1);
    (void)remove(in);
    (void)remove(out);
}

// Tests skip stops shrink.
static void test_skip(void) {
    const char *in;
    const char *out;

    in = "/tmp/opencode/nem-s.yaml";
    out = "/tmp/opencode/nem-t.yaml";
    write_canned(in);
    assert(nemesis_minimize(
        in, stub_skip, "keepfail",
        out));
    assert(file_has(out,
                    "keepfail"));
    need_struct(out);
    (void)remove(in);
    (void)remove(out);
}

// Tests minimizer errors.
static void test_merr(void) {
    const char *in;
    const char *out;
    FILE *fp;

    in = "/tmp/opencode/nem-e.yaml";
    out = "/tmp/opencode/nem-f.yaml";
    assert(!nemesis_minimize(
        "/tmp/opencode/nope.yaml",
        stub_mark, "keepfail",
        out));
    assert(!nemesis_minimize(
        in, nullptr, "keepfail",
        out));
    assert(!nemesis_minimize(
        in, stub_mark, "keepfail",
        nullptr));
    assert(!nemesis_minimize(
        nullptr, stub_mark,
        "keepfail", out));
    write_canned(in);
    assert(!nemesis_minimize(
        in, stub_err, nullptr,
        out));
    fp = fopen(in, "w");
    assert(fp != nullptr);
    assert(fputs("bogus: [unclosed\n",
                 fp) >= 0);
    assert(fclose(fp) == 0);
    assert(!nemesis_minimize(
        in, stub_mark, "keepfail",
        out));
    (void)remove(in);
    (void)remove(out);
}

// Runs all tests. Returns 0.
int main(void) {
    test_determ();
    test_reject();
    test_gate();
    test_verbs();
    test_shrink();
    test_skip();
    test_merr();
    return (0);
}
