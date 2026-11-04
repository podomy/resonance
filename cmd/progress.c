#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "progress.h"

// Max live rows drawn; rest fold
// into a +N more tail line.
static constexpr int MaxRows = 8;

// Cap on a full line with inks.
static constexpr int LineCap = 4096;

// Fallback width when winsize is
// unknown; rows adapt to real width.
static constexpr int Fallback = 80;

// Colors, reset, all ANSI.
static const char Yellow[] =
    "\x1b[33m";
static const char Reset[] =
    "\x1b[0m";

// Lines in the prior block, for the
// cursor up repaint. Zero on first
// draw or after a dumb run.
static int Prev = 0;

// True when the progress view may
// run: tty stdout, no --tap/--json.
int prog_want(int tap_flag,
    int json_flag) {
    if (tap_flag != 0) {
        return (0);
    }
    if (json_flag != 0) {
        return (0);
    }
    if (isatty(STDOUT_FILENO)
        == 0) {
        return (0);
    }
    return (1);
}

// Width of stdout in columns, with
// fallback when unknown or zero.
static int term_width(void) {
    struct winsize ws;
    memset(&ws, 0, sizeof ws);
    if (ioctl(STDOUT_FILENO,
        TIOCGWINSZ, &ws) != 0) {
        return (Fallback);
    }
    if (ws.ws_col == 0) {
        return (Fallback);
    }
    return ((int)ws.ws_col);
}

// True when TERM says dumb: skips
// all escape sequences.
static int term_dumb(void) {
    const char *t = getenv("TERM");
    if (t == nullptr) {
        return (0);
    }
    return (strcmp(t, "dumb")
        == 0);
}

// True when no escapes may print:
// dumb term or NO_COLOR set to any
// value. Keeps NO_COLOR runs free
// of cursor and erase codes too.
static int no_esc(void) {
    if (getenv("NO_COLOR") !=
        nullptr) {
        return (1);
    }
    return (term_dumb());
}

// True when ANSI colors may print.
// NO_COLOR set to any value, even
// empty, disables color. Dumb terms
// disable color too.
int prog_color(void) {
    if (getenv("NO_COLOR") !=
        nullptr) {
        return (0);
    }
    if (term_dumb() != 0) {
        return (0);
    }
    return (1);
}

// Skips a suite: prefix plus blanks;
// returns the display phase text.
static const char *strip_suite(
    const char *s) {
    if (s == nullptr) {
        return ("");
    }
    if (strncmp(s, "suite:", 6)
        == 0) {
        s += 6;
        while (*s == ' ' ||
            *s == '\t') {
            s++;
        }
    }
    if (*s == 0) {
        return ("");
    }
    return (s);
}

// Prints one block line: CR plus
// erase unless dumb, then text and
// newline. Text is already cut to
// width; colors wrap outside.
static void put_line(const char *t,
    int dumb) {
    if (dumb != 0) {
        fputc('\r', stdout);
    } else {
        fputs("\r\x1b[K", stdout);
    }
    fputs(t, stdout);
    fputc('\n', stdout);
}

// Cuts the middle of s to width,
// keeping head and tail visible with
// ... between; short lines stay whole.
// Rows keep base start, phase end, and
// the [i/n] tail; the head keeps its
// counts head and ETA tail.
static void mid_cut(char *s, int w) {
    size_t n = strlen(s);
    size_t k = 0;
    size_t h = 0;
    if (w <= 0) {
        return;
    }
    if (n <= (size_t)w) {
        return;
    }
    if (w <= 3) {
        s[w] = 0;
        return;
    }
    k = (size_t)w - 3;
    h = k / 2 + k % 2;
    memmove(s + h + 3,
        s + n - (k - h), k - h);
    memcpy(s + h, "...", 3);
    s[w] = 0;
}

// Colors a plain RUNNING row: state
// word yellow, rest plain. Falls back
// to plain when too short.
static void run_wrap(const char *pl,
    char *out, size_t n) {
    if (strlen(pl) < 7) {
        snprintf(out, n, "%.4000s",
            pl);
        return;
    }
    snprintf(out, n,
        "%s%.7s%s%.4000s", Yellow,
        pl, Reset, pl + 7);
}

// Draws the block: live rows capped,
// then tail. No head line, no counts,
// no bars, no percent, no elapsed, no
// spinners; rows repaint in place by
// cursor up; dumb or NO_COLOR terms
// scroll as before with zero escapes
// at all. Known rows append 0-based
// [idx/total]; unknown rows read as
// phase only.
void prog_draw(ProgView *vs, int n) {
    char plain[LineCap];
    char shown[LineCap];
    int width = term_width();
    int mute = no_esc();
    int color = prog_color();
    int live = 0;
    int made = 0;
    int cur = 0;
    int tail = 0;
    for (int i = 0; i < n; i++) {
        if (vs[i].live == 0) {
            continue;
        }
        live++;
    }
    made = live;
    if (made > MaxRows) {
        made = MaxRows;
    }
    tail = live - made;
    cur = made;
    if (tail > 0) {
        cur++;
    }
    if (mute == 0 && Prev > 0) {
        fprintf(stdout, "\x1b[%dA",
            Prev);
    }
    made = 0;
    for (int i = 0; i < n &&
        made < MaxRows; i++) {
        const char *ds = nullptr;
        const char *bs = nullptr;
        int idx = 0;
        int tot = 0;
        if (vs[i].live == 0) {
            continue;
        }
        bs = vs[i].base;
        if (bs == nullptr) {
            bs = "";
        }
        ds = strip_suite(
            vs[i].phase);
        idx = vs[i].ph_idx;
        tot = vs[i].ph_total;
        if (idx < 0) {
            idx = 0;
        }
        if (tot < 0) {
            tot = 0;
        }
        if (tot > 0 && idx > tot) {
            idx = tot;
        }
        if (tot > 0) {
            snprintf(plain,
                sizeof plain,
                "RUNNING  %.800s  "
                "%.800s [%d/%d]", bs,
                ds, idx, tot);
        } else {
            snprintf(plain,
                sizeof plain,
                "RUNNING  %.1000s  "
                "%.1000s", bs, ds);
        }
        mid_cut(plain, width);
        if (color != 0) {
            run_wrap(plain, shown,
                sizeof shown);
        } else {
            snprintf(shown,
                sizeof shown,
                "%.4000s", plain);
        }
        put_line(shown, mute);
        made++;
    }
    if (tail > 0) {
        snprintf(plain,
            sizeof plain,
            "... +%d more", tail);
        mid_cut(plain, width);
        snprintf(shown,
            sizeof shown, "%.4000s",
            plain);
        put_line(shown, mute);
    }
    if (mute == 0) {
        for (int k = cur; k < Prev;
            k++) {
            put_line("", mute);
        }
        Prev = cur;
    }
    fflush(stdout);
}
