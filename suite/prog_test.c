#define _GNU_SOURCE
#include "cmd/progress.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

// Progress view gates plus drawing.
// Drawing capture goes to a scratch
// file, stdout restored after.

// Flag gates: any tap or json flag
// kills the view; plain follows
// the tty.
static void pg_want(void) {
    assert(prog_want(1, 0) == 0);
    assert(prog_want(0, 1) == 0);
    assert(prog_want(1, 1) == 0);
    assert(prog_want(0, 0) ==
           (isatty(STDOUT_FILENO) !=
            0));
}

// Color gates: any NO_COLOR value
// or a dumb TERM disables color.
static void pg_color(void) {
    const char *onc;
    const char *otm;
    char bnc[128];
    char btm[128];
    bool hnc;
    bool htm;
    int m;

    onc = getenv("NO_COLOR");
    otm = getenv("TERM");
    hnc = onc != nullptr;
    htm = otm != nullptr;
    if (hnc) {
        m = snprintf(bnc,
                     sizeof(bnc),
                     "%s", onc);
        assert(m > 0 &&
               m < (int)sizeof(
                       bnc));
    }
    if (htm) {
        m = snprintf(btm,
                     sizeof(btm),
                     "%s", otm);
        assert(m > 0 &&
               m < (int)sizeof(
                       btm));
    }
    assert(setenv("NO_COLOR", "1",
                  1) == 0);
    assert(prog_color() == 0);
    assert(unsetenv("NO_COLOR") ==
           0);
    assert(setenv("TERM", "dumb",
                  1) == 0);
    assert(prog_color() == 0);
    if (hnc) {
        assert(setenv("NO_COLOR",
                      bnc, 1) == 0);
    } else {
        assert(unsetenv(
                   "NO_COLOR") == 0);
    }
    if (htm) {
        assert(setenv("TERM", btm,
                      1) == 0);
    } else {
        assert(unsetenv("TERM") ==
               0);
    }
}

// Inits a live view with all fields.
static void mk_live(ProgView *v,
    const char *base,
    const char *phase, int idx,
    int tot) {
    v->base = base;
    v->phase = phase;
    v->live = 1;
    v->ph_idx = idx;
    v->ph_total = tot;
    v->fr_num = 0;
    v->fr_den = 0;
    v->fr_idx = 0;
    v->done = 0;
    v->dkind = 0;
}

// Inits a done view with a verdict:
// 0 pass, 1 fail, 2 skip.
static void mk_done(ProgView *v,
    const char *base,
    const char *phase, int kind) {
    v->base = base;
    v->phase = phase;
    v->live = 0;
    v->ph_idx = 0;
    v->ph_total = 0;
    v->fr_num = 0;
    v->fr_den = 0;
    v->fr_idx = 0;
    v->done = 1;
    v->dkind = kind;
}

// Draws rows into a scratch file:
// empty draws stay silent, known
// rows show stage [idx+1/total],
// suite prefixes strip, nulls hold.
static void pg_draw(void) {
    static const char cap[] =
        "/tmp/opencode/prog-cap"
        ".txt";
    ProgView live;
    ProgView unk;
    ProgView dead;
    ProgView nulls;
    FILE *fp;
    int saved;
    char buf[4096];
    size_t n;

    assert(setenv("NO_COLOR", "1",
                  1) == 0);
    (void)remove(cap);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0);
    fp = fopen(cap, "w");
    assert(fp != nullptr);
    assert(dup2(fileno(fp),
                STDOUT_FILENO) >= 0);
    prog_draw(nullptr, 0);
    mk_live(&dead, "a.yaml",
        "mesh", 0, 2);
    dead.live = 0;
    prog_draw(&dead, 1);
    mk_live(&live, "b.yaml",
        "mesh", 0, 2);
    prog_draw(&live, 1);
    mk_live(&unk, "c.yaml",
        "suite: up", 0, 0);
    prog_draw(&unk, 1);
    mk_live(&nulls, nullptr,
        nullptr, -5, -5);
    prog_draw(&nulls, 1);
    fflush(stdout);
    assert(dup2(saved,
                STDOUT_FILENO) >= 0);
    assert(close(saved) == 0);
    assert(fclose(fp) == 0);
    fp = fopen(cap, "rb");
    assert(fp != nullptr);
    n = fread(buf, 1,
              sizeof(buf) - 1,
              fp);
    assert(fclose(fp) == 0);
    buf[n] = '\0';
    assert(strstr(buf, "RUNNING") !=
           nullptr);
    assert(strstr(buf, "[1/2]") !=
           nullptr);
    assert(strstr(buf, "stage") !=
           nullptr);
    assert(strstr(buf, "b.yaml") !=
           nullptr);
    assert(strstr(buf, "suite:") ==
           nullptr);
    assert(remove(cap) == 0);
    assert(unsetenv("NO_COLOR") ==
           0);
}

// Identical redraw emits nothing:
// second draw keeps file size.
static void pg_dirty(void) {
    static const char cap[] =
        "/tmp/opencode/prog-dirty"
        ".txt";
    ProgView v;
    FILE *fp;
    int saved;
    off_t a = 0;
    off_t b = 0;
    off_t c = 0;

    assert(setenv("NO_COLOR", "1",
                  1) == 0);
    (void)remove(cap);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0);
    fp = fopen(cap, "w");
    assert(fp != nullptr);
    assert(dup2(fileno(fp),
                STDOUT_FILENO) >= 0);
    mk_live(&v, "dirty.yaml",
        "mesh", 0, 2);
    prog_draw(&v, 1);
    fflush(stdout);
    a = lseek(STDOUT_FILENO, 0,
        SEEK_CUR);
    assert(a > 0);
    prog_draw(&v, 1);
    fflush(stdout);
    b = lseek(STDOUT_FILENO, 0,
        SEEK_CUR);
    assert(b == a);
    v.phase = "mesh2";
    prog_draw(&v, 1);
    fflush(stdout);
    c = lseek(STDOUT_FILENO, 0,
        SEEK_CUR);
    assert(c > b);
    assert(dup2(saved,
                STDOUT_FILENO) >= 0);
    assert(close(saved) == 0);
    assert(fclose(fp) == 0);
    assert(remove(cap) == 0);
    assert(unsetenv("NO_COLOR") ==
           0);
}

// Done rows carry colors: ok green,
// not ok red, skip dim.
static void pg_done(void) {
    static const char cap[] =
        "/tmp/opencode/prog-done"
        ".txt";
    const char *onc;
    const char *otm;
    char bnc[128];
    char btm[128];
    bool hnc;
    bool htm;
    int m;
    ProgView vs[3];
    FILE *fp;
    int saved;
    char buf[8192];
    size_t n;

    onc = getenv("NO_COLOR");
    otm = getenv("TERM");
    hnc = onc != nullptr;
    htm = otm != nullptr;
    if (hnc) {
        m = snprintf(bnc,
            sizeof bnc, "%s", onc);
        assert(m > 0 &&
            m < (int)sizeof bnc);
    }
    if (htm) {
        m = snprintf(btm,
            sizeof btm, "%s", otm);
        assert(m > 0 &&
            m < (int)sizeof btm);
    }
    assert(unsetenv("NO_COLOR") ==
        0);
    assert(setenv("TERM", "xterm",
        1) == 0);
    assert(prog_color() == 1);
    (void)remove(cap);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0);
    fp = fopen(cap, "w");
    assert(fp != nullptr);
    assert(dup2(fileno(fp),
        STDOUT_FILENO) >= 0);
    mk_done(&vs[0], "p.yaml",
        "mesh", 0);
    mk_done(&vs[1], "f.yaml",
        "mesh", 1);
    mk_done(&vs[2], "s.yaml",
        "mesh", 2);
    prog_draw(vs, 3);
    fflush(stdout);
    assert(dup2(saved,
        STDOUT_FILENO) >= 0);
    assert(close(saved) == 0);
    assert(fclose(fp) == 0);
    fp = fopen(cap, "rb");
    assert(fp != nullptr);
    n = fread(buf, 1,
        sizeof buf - 1, fp);
    assert(fclose(fp) == 0);
    buf[n] = '\0';
    assert(strstr(buf, "ok") !=
        nullptr);
    assert(strstr(buf, "not ok") !=
        nullptr);
    assert(strstr(buf, "skip") !=
        nullptr);
    assert(strstr(buf,
        "\x1b[32m") != nullptr);
    assert(strstr(buf,
        "\x1b[31m") != nullptr);
    assert(strstr(buf,
        "\x1b[2m") != nullptr);
    assert(remove(cap) == 0);
    if (hnc) {
        assert(setenv("NO_COLOR",
            bnc, 1) == 0);
    } else {
        assert(unsetenv(
            "NO_COLOR") == 0);
    }
    if (htm) {
        assert(setenv("TERM", btm,
            1) == 0);
    } else {
        assert(unsetenv("TERM") ==
            0);
    }
}

// Frac rows append num/den when den
// is past zero.
static void pg_frac(void) {
    static const char cap[] =
        "/tmp/opencode/prog-frac"
        ".txt";
    ProgView v;
    FILE *fp;
    int saved;
    char buf[4096];
    size_t n;

    assert(setenv("NO_COLOR", "1",
        1) == 0);
    (void)remove(cap);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0);
    fp = fopen(cap, "w");
    assert(fp != nullptr);
    assert(dup2(fileno(fp),
        STDOUT_FILENO) >= 0);
    mk_live(&v, "frac.yaml",
        "mesh", 1, 4);
    v.fr_num = 3;
    v.fr_den = 10;
    v.fr_idx = 1;
    prog_draw(&v, 1);
    fflush(stdout);
    assert(dup2(saved,
        STDOUT_FILENO) >= 0);
    assert(close(saved) == 0);
    assert(fclose(fp) == 0);
    fp = fopen(cap, "rb");
    assert(fp != nullptr);
    n = fread(buf, 1,
        sizeof buf - 1, fp);
    assert(fclose(fp) == 0);
    buf[n] = '\0';
    assert(strstr(buf, "3/10") !=
        nullptr);
    assert(strstr(buf, "frac") !=
        nullptr);
    assert(strstr(buf, "[2/4]") !=
        nullptr);
    assert(strstr(buf, "stage") !=
        nullptr);
    assert(remove(cap) == 0);
    assert(unsetenv("NO_COLOR") ==
        0);
}

// Mute runs never emit CR bytes:
// text plus newline only.
static void pg_mute(void) {
    static const char cap[] =
        "/tmp/opencode/prog-mute"
        ".txt";
    ProgView v;
    FILE *fp;
    int saved;
    char buf[4096];
    size_t n;
    size_t k = 0;

    assert(setenv("NO_COLOR", "1",
        1) == 0);
    (void)remove(cap);
    fflush(stdout);
    saved = dup(STDOUT_FILENO);
    assert(saved >= 0);
    fp = fopen(cap, "w");
    assert(fp != nullptr);
    assert(dup2(fileno(fp),
        STDOUT_FILENO) >= 0);
    mk_live(&v, "mute.yaml",
        "mesh", 0, 2);
    prog_draw(&v, 1);
    fflush(stdout);
    assert(dup2(saved,
        STDOUT_FILENO) >= 0);
    assert(close(saved) == 0);
    assert(fclose(fp) == 0);
    fp = fopen(cap, "rb");
    assert(fp != nullptr);
    n = fread(buf, 1,
        sizeof buf - 1, fp);
    assert(fclose(fp) == 0);
    for (k = 0; k < n; k++) {
        assert(buf[k] != '\r');
    }
    assert(n > 0);
    assert(remove(cap) == 0);
    assert(unsetenv("NO_COLOR") ==
        0);
}

// Runs all checks. Returns 0.
int main(void) {
    pg_want();
    pg_color();
    pg_draw();
    pg_dirty();
    pg_frac();
    pg_mute();
    pg_done();
    printf("prog_test: ok\n");
    return (0);
}
