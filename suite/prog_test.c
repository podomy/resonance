#define _GNU_SOURCE
#include "cmd/progress.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

// Draws rows into a scratch file:
// empty draws stay silent, known
// rows show [idx/total], suite
// prefixes strip, nulls hold.
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
    dead.base = "a.yaml";
    dead.phase = "mesh";
    dead.live = 0;
    dead.ph_idx = 0;
    dead.ph_total = 2;
    prog_draw(&dead, 1);
    live.base = "b.yaml";
    live.phase = "mesh";
    live.live = 1;
    live.ph_idx = 0;
    live.ph_total = 2;
    prog_draw(&live, 1);
    unk.base = "c.yaml";
    unk.phase = "suite: up";
    unk.live = 1;
    unk.ph_idx = 0;
    unk.ph_total = 0;
    prog_draw(&unk, 1);
    nulls.base = nullptr;
    nulls.phase = nullptr;
    nulls.live = 1;
    nulls.ph_idx = -5;
    nulls.ph_total = -5;
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
    assert(strstr(buf, "[0/2]") !=
           nullptr);
    assert(strstr(buf, "b.yaml") !=
           nullptr);
    assert(strstr(buf, "suite:") ==
           nullptr);
    assert(remove(cap) == 0);
    assert(unsetenv("NO_COLOR") ==
           0);
}

// Runs all checks. Returns 0.
int main(void) {
    pg_want();
    pg_color();
    pg_draw();
    printf("prog_test: ok\n");
    return (0);
}
