#include "suite/parse.h"

#include <assert.h>
#include <string.h>

// Parses text. Asserts success, returns doc.
static SuiteDoc *parse_ok(const char *text) {
    SuiteDoc *doc = nullptr;
    SuiteError err = {0, 0, ""};
    bool good = suite_parse(text, strlen(text),
                            &doc, &err);
    assert(good);
    assert(doc != nullptr);
    assert(doc->root != nullptr);
    return (doc);
}

// Parses text. Asserts failure at line.
static void parse_bad(const char *text,
                      size_t line) {
    SuiteDoc *doc = nullptr;
    SuiteError err = {0, 0, ""};
    bool good = suite_parse(text, strlen(text),
                            &doc, &err);
    assert(!good);
    assert(doc == nullptr);
    assert(err.line == line);
    assert(err.message[0] != '\0');
}

// Parses text. Asserts failure spot.
static void parse_bad_at(
    const char *text, size_t line,
    size_t col) {
    SuiteDoc *doc = nullptr;
    SuiteError err = {0, 0, ""};
    bool good = suite_parse(
        text, strlen(text), &doc,
        &err);
    assert(!good);
    assert(doc == nullptr);
    assert(err.line == line);
    assert(err.col == col);
    assert(err.message[0] != '\0');
}

// Finds key. Asserts present.
static SuiteNode *need_key(SuiteNode *map,
                           const char *key) {
    SuiteNode *nd = suite_map_find(map, key);
    assert(nd != nullptr);
    return (nd);
}

// Asserts scalar text.
static void need_str(SuiteNode *nd,
                     const char *want) {
    const char *got = suite_scalar_str(nd);
    assert(got != nullptr);
    assert(strcmp(got, want) == 0);
}

// Asserts scalar integer.
static void need_int(SuiteNode *nd,
                     long long want) {
    long long got = 0;
    assert(suite_scalar_int(nd, &got));
    assert(got == want);
}

// Runs parser checks. Returns 0.
int main(void) {
    SuiteDoc *d = parse_ok("server:\n"
                           "  host: example\n"
                           "  port: 8080\n"
                           "  neg: -7\n"
                           "  ratio: 1.5\n");
    SuiteNode *srv = need_key(d->root, "server");
    need_str(need_key(srv, "host"), "example");
    need_int(need_key(srv, "port"), 8080);
    need_int(need_key(srv, "neg"), -7);
    need_str(need_key(srv, "ratio"), "1.5");
    long long v = 0;
    SuiteNode *host = need_key(srv, "host");
    assert(!suite_scalar_int(host, &v));
    SuiteNode *ratio = need_key(srv, "ratio");
    assert(!suite_scalar_int(ratio, &v));
    assert(suite_map_find(srv, "nope") ==
           nullptr);
    assert(suite_list_len(srv) == 0);
    assert(suite_scalar_str(srv) == nullptr);
    suite_doc_free(d);

    d = parse_ok("phases:\n"
                 "  - name: build\n"
                 "    run: make all\n"
                 "  - name: test\n"
                 "    run: make check\n");
    SuiteNode *ph = need_key(d->root, "phases");
    assert(suite_list_len(ph) == 2);
    SuiteNode *p0 = suite_list_at(ph, 0);
    SuiteNode *p1 = suite_list_at(ph, 1);
    assert(p0 != nullptr);
    assert(p1 != nullptr);
    assert(suite_list_at(ph, 2) == nullptr);
    need_str(need_key(p0, "name"), "build");
    need_str(need_key(p0, "run"), "make all");
    need_str(need_key(p1, "name"), "test");
    need_str(need_key(p1, "run"), "make check");
    suite_doc_free(d);

    d = parse_ok("n: 42\n"
                 "s: hello\n"
                 "q1: \"hi there\"\n"
                 "q2: 'it''s'\n"
                 "esc: \"a\\nb\"\n"
                 "colon: \"a: b\"\n"
                 "big: 9999999999999999999999\n");
    need_int(need_key(d->root, "n"), 42);
    need_str(need_key(d->root, "n"), "42");
    need_str(need_key(d->root, "s"), "hello");
    SuiteNode *s = need_key(d->root, "s");
    assert(!suite_scalar_int(s, &v));
    need_str(need_key(d->root, "q1"),
             "hi there");
    need_str(need_key(d->root, "q2"), "it's");
    need_str(need_key(d->root, "esc"), "a\nb");
    need_str(need_key(d->root, "colon"),
             "a: b");
    SuiteNode *big = need_key(d->root, "big");
    need_str(big, "9999999999999999999999");
    assert(!suite_scalar_int(big, &v));
    suite_doc_free(d);

    d = parse_ok("# top\n"
                 "\n"
                 "a: 1 # one\n"
                 "   \n"
                 "# mid\n"
                 "tags:\n"
                 "  - x\n"
                 "  - y # why\n"
                 "b: 2\n");
    need_int(need_key(d->root, "a"), 1);
    need_int(need_key(d->root, "b"), 2);
    SuiteNode *tags = need_key(d->root, "tags");
    assert(suite_list_len(tags) == 2);
    need_str(suite_list_at(tags, 0), "x");
    need_str(suite_list_at(tags, 1), "y");
    suite_doc_free(d);

    d = parse_ok("jobs:\n"
                 "- a\n"
                 "- b\n");
    SuiteNode *jobs = need_key(d->root, "jobs");
    assert(suite_list_len(jobs) == 2);
    need_str(suite_list_at(jobs, 0), "a");
    need_str(suite_list_at(jobs, 1), "b");
    suite_doc_free(d);

    d = parse_ok("- a\n- b\n");
    assert(d->root->kind == SUITE_LIST);
    assert(suite_list_len(d->root) == 2);
    suite_doc_free(d);

    d = parse_ok("a: 1\r\nb: 2\r\n");
    need_int(need_key(d->root, "a"), 1);
    need_int(need_key(d->root, "b"), 2);
    suite_doc_free(d);

    d = parse_ok("");
    assert(d->root->kind == SUITE_MAP);
    assert(suite_map_find(d->root, "a") ==
           nullptr);
    suite_doc_free(d);

    d = parse_ok("# only\n   \n");
    assert(d->root->kind == SUITE_MAP);
    suite_doc_free(d);

    d = nullptr;
    assert(suite_parse("a: 1\n", 5, &d, nullptr));
    assert(d != nullptr);
    need_int(need_key(d->root, "a"), 1);
    suite_doc_free(d);

    assert(suite_map_find(nullptr, "a") ==
           nullptr);
    assert(suite_list_len(nullptr) == 0);
    assert(suite_list_at(nullptr, 0) ==
           nullptr);
    assert(suite_scalar_str(nullptr) == nullptr);
    assert(!suite_scalar_int(nullptr, &v));
    suite_doc_free(nullptr);

    parse_bad("a: 1\n\tb: 2\n", 2);
    parse_bad("a:\n \tb: 1\n", 2);
    parse_bad("a: &x 1\n", 1);
    parse_bad("a: *x\n", 1);
    parse_bad("a: !t\n", 1);
    parse_bad("a: {x}\n", 1);
    parse_bad("a: [1]\n", 1);
    parse_bad("a: x}\n", 1);
    parse_bad("a: x]\n", 1);
    parse_bad("%YAML 1.2\na: 1\n", 1);
    parse_bad("a: |\n  t\n", 1);
    parse_bad("a: >\n  t\n", 1);
    parse_bad("a: 1\na: 2\n", 2);
    parse_bad("  a: 1\nb: 2\n", 2);
    parse_bad("a:\n  x: 1\n   y: 2\n", 3);
    parse_bad("a: 1\n  b: 2\n", 2);
    parse_bad("a: \"x\n", 1);
    parse_bad("a: 'x\n", 1);
    parse_bad("a: \"\\q\"\n", 1);
    parse_bad("? foo\n", 1);
    parse_bad("k: - x\n", 1);
    parse_bad("- a\nb: 1\n", 2);

    parse_bad_at("a: \"x\n", 1, 4);
    parse_bad_at(": 1\n", 1, 1);
    parse_bad_at("\"\": 1\n", 1, 1);
    parse_bad_at("a: 1\rb: 2\n", 1,
                 5);
    parse_bad_at("a: 1\t\n", 1, 5);
    parse_bad_at("\"a\": 1\na: 2\n",
                 2, 1);
    parse_bad_at("a:\n  x: 1\n  x: 2\n",
                 3, 3);

    d = parse_ok("a: 1   \n"
                 "b: 2\r\n"
                 "c: \"a#b\"\n"
                 "e: \"a\tb\"\n");
    need_int(need_key(d->root, "a"), 1);
    need_int(need_key(d->root, "b"), 2);
    need_str(need_key(d->root, "c"),
             "a#b");
    need_str(need_key(d->root, "e"),
             "a\tb");
    suite_doc_free(d);

    d = parse_ok("a:\n"
                 "  x: 1\n"
                 "b:\n"
                 "  x: 2\n"
                 "\"q k\": 3\n"
                 "'s': 4\n");
    SuiteNode *am =
        need_key(d->root, "a");
    SuiteNode *bm =
        need_key(d->root, "b");
    need_int(need_key(am, "x"), 1);
    need_int(need_key(bm, "x"), 2);
    need_int(need_key(d->root, "q k"),
             3);
    need_int(need_key(d->root, "s"),
             4);
    suite_doc_free(d);

    d = parse_ok("- \"k\": 1\n"
                 "- 'a: b': 2\n");
    assert(d->root->kind == SUITE_LIST);
    assert(suite_list_len(d->root) ==
           2);
    need_str(need_key(
                 suite_list_at(d->root,
                               0),
                 "k"),
             "1");
    need_str(need_key(
                 suite_list_at(d->root,
                               1),
                 "a: b"),
             "2");
    suite_doc_free(d);

    d = parse_ok("p: +42\n"
                 "z: 007\n"
                 "m: -0\n");
    need_int(need_key(d->root, "p"),
             42);
    need_str(need_key(d->root, "p"),
             "+42");
    need_int(need_key(d->root, "z"),
             7);
    need_int(need_key(d->root, "m"),
             0);
    suite_doc_free(d);
    d = parse_ok("top: 9223372036854775807\n"
                 "e: \"\"\n");
    need_int(need_key(d->root, "top"),
             9223372036854775807LL);
    SuiteNode *ee =
        need_key(d->root, "e");
    need_str(ee, "");
    assert(!suite_scalar_int(ee, &v));
    assert(!suite_scalar_int(nullptr,
                             nullptr));
    assert(!suite_parse("a: 1\n", 5,
                        nullptr,
                        nullptr));
    suite_doc_free(d);
    d = parse_ok("x: 9223372036854775808\n");
    assert(!suite_scalar_int(
        need_key(d->root, "x"), &v));
    suite_doc_free(d);
    d = parse_ok("y: 1\n");
    need_int(need_key(d->root, "y"),
             1);
    suite_doc_free(d);

    {
        char deep[8192] = {0};
        size_t off = 0;
        int i;

        for (i = 0; i < 80; i++) {
            int k;

            for (k = 0; k < i; k++) {
                deep[off++] = ' ';
            }
            deep[off++] = 'k';
            deep[off++] = ':';
            deep[off++] = '\n';
        }
        parse_bad_at(deep, 65, 65);
    }
    {
        char lvl[512] = {0};
        size_t lo = 0;
        int i;

        for (i = 0; i < 9; i++) {
            int k;

            for (k = 0; k < i; k++) {
                lvl[lo++] = ' ';
            }
            lvl[lo++] = 'k';
            lvl[lo++] = ':';
            lvl[lo++] = '\n';
        }
        for (i = 0; i < 9; i++) {
            lvl[lo++] = ' ';
        }
        lvl[lo++] = 'k';
        lvl[lo++] = ':';
        lvl[lo++] = ' ';
        lvl[lo++] = '1';
        lvl[lo++] = '\n';
        d = parse_ok(lvl);
        SuiteNode *w = d->root;
        for (i = 0; i < 10; i++) {
            w = need_key(w, "k");
        }
        need_int(w, 1);
        suite_doc_free(d);
    }
    {
        static char big[9000];

        memset(big, 'y',
               sizeof(big) - 1);
        parse_bad_at(big, 1, 1);
    }

    d = parse_ok("t1: \"a\\tb\"\n"
                 "t2: \"a\\\\b\"\n"
                 "t3: \"a\\\"b\"\n"
                 "t4: \"a\\'b\"\n"
                 "t5: \"a\\rb\"\n");
    need_str(need_key(d->root, "t1"),
             "a\tb");
    need_str(need_key(d->root, "t2"),
             "a\\b");
    need_str(need_key(d->root, "t3"),
             "a\"b");
    need_str(need_key(d->root, "t4"),
             "a'b");
    need_str(need_key(d->root, "t5"),
             "a\rb");
    suite_doc_free(d);

    parse_bad("a{b: 1\n", 1);
    parse_bad("a}b: 1\n", 1);
    parse_bad("a[b: 1\n", 1);
    parse_bad("a]b: 1\n", 1);
    parse_bad("&k: 1\n", 1);
    parse_bad("*k: 1\n", 1);
    parse_bad("!k: 1\n", 1);
    parse_bad("@k: 1\n", 1);
    parse_bad("`k: 1\n", 1);
    parse_bad("a:\t1\n", 1);
    parse_bad_at("a:\t1\n", 1, 3);

    d = parse_ok("a: b#c\n");
    need_str(need_key(d->root, "a"),
             "b#c");
    suite_doc_free(d);

    d = parse_ok("top:\n"
                 "  - name: a\n"
                 "    vals:\n"
                 "      - x\n"
                 "      - y\n");
    {
        SuiteNode *tp =
            need_key(d->root, "top");

        assert(suite_list_len(tp) ==
               1);
        SuiteNode *it0 =
            suite_list_at(tp, 0);

        need_str(need_key(it0,
                          "name"),
                 "a");
        SuiteNode *vs =
            need_key(it0, "vals");

        assert(suite_list_len(vs) ==
               2);
        need_str(suite_list_at(vs,
                               0),
                 "x");
        need_str(suite_list_at(vs,
                               1),
                 "y");
    }
    suite_doc_free(d);

    {
        static char ok[8194];
        size_t k = 0;
        size_t j;

        ok[k++] = 'k';
        ok[k++] = ':';
        ok[k++] = ' ';
        for (j = 0; j < 8189; j++) {
            ok[k++] = 'v';
        }
        ok[k++] = '\n';
        ok[k] = '\0';
        d = parse_ok(ok);
        SuiteNode *kk =
            need_key(d->root, "k");

        assert(strlen(
                   suite_scalar_str(
                       kk)) == 8189);
        suite_doc_free(d);
    }

    parse_bad("hello\n", 1);
    parse_bad("-\n", 1);
    parse_bad("- \n", 1);

    static const char raw[] = {
        'k', ':', ' ', '1', '\n', 'b',
        ':', ' ', '\0', 'x', '\n'};
    SuiteDoc *ddoc = nullptr;
    SuiteError derr = {0, 0, ""};
    bool good =
        suite_parse(raw, sizeof(raw),
                    &ddoc, &derr);
    assert(!good);
    assert(ddoc == nullptr);
    assert(derr.line == 2);
    assert(derr.col == 4);
    assert(derr.message[0] != '\0');

    parse_bad("a: \"x\" y\n", 1);
    d = parse_ok("a: b:c\n");
    need_str(need_key(d->root, "a"),
             "b:c");
    suite_doc_free(d);
    parse_bad("a: b : c\n", 1);
    parse_bad("-x\n", 1);
    d = parse_ok("l:\n"
                 "  - \"a b\"\n");
    need_str(suite_list_at(
                 need_key(d->root,
                          "l"),
                 0),
             "a b");
    suite_doc_free(d);
    d = parse_ok("l:\n"
                 "  - k: 1\n"
                 "    j: 2\n");
    {
        SuiteNode *it =
            suite_list_at(
                need_key(d->root,
                         "l"),
                0);

        need_int(need_key(it, "k"),
                 1);
        need_int(need_key(it, "j"),
                 2);
    }
    suite_doc_free(d);
    parse_bad("l:\n"
              "  - a\n"
              "   - b\n",
              3);
    d = parse_ok("a: 42 # comment\n");
    need_int(need_key(d->root, "a"),
             42);
    suite_doc_free(d);
    parse_bad("a: b\x01"
              "c\n",
              1);
    parse_bad("key:\n", 1);
    d = parse_ok("q: \"42\"\n");
    need_int(need_key(d->root, "q"),
             42);
    suite_doc_free(d);
    d = parse_ok("h: 0x10\n");
    need_str(need_key(d->root, "h"),
             "0x10");
    assert(!suite_scalar_int(
        need_key(d->root, "h"), &v));
    suite_doc_free(d);
    {
        static char b64[4096];
        size_t off = 0;
        int i;

        for (i = 0; i < 63; i++) {
            int k;

            for (k = 0; k < i; k++) {
                b64[off++] = ' ';
            }
            b64[off++] = 'k';
            b64[off++] = ':';
            b64[off++] = '\n';
        }
        for (i = 0; i < 63; i++) {
            b64[off++] = ' ';
        }
        b64[off++] = 'k';
        b64[off++] = ':';
        b64[off++] = ' ';
        b64[off++] = '1';
        b64[off++] = '\n';
        b64[off] = '\0';
        d = parse_ok(b64);
        SuiteNode *dep = d->root;
        int j;

        for (j = 0; j < 64; j++) {
            dep = need_key(dep, "k");
        }
        need_int(dep, 1);
        suite_doc_free(d);
    }
    d = parse_ok("l:\n"
                 "  - []\n");
    {
        SuiteNode *ll =
            need_key(d->root, "l");
        SuiteNode *it =
            suite_list_at(ll, 0);

        assert(suite_list_len(ll) ==
               1);
        assert(it != nullptr);
        assert(it->kind ==
               SUITE_LIST);
        assert(suite_list_len(it) ==
               0);
    }
    suite_doc_free(d);
    d = parse_ok("l:\n"
                 "  - [ ]\n");
    {
        SuiteNode *ll =
            need_key(d->root, "l");

        assert(suite_list_len(ll) ==
               1);
        assert(suite_list_at(ll,
                             0)->kind ==
               SUITE_LIST);
    }
    suite_doc_free(d);
    return (0);
}
