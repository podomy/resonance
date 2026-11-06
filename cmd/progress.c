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
// into a +N more files tail line.
// Done rows always show after the
// tail.
static constexpr int MaxRows = 8;

// Cap on a full line with inks.
static constexpr int LineCap = 4096;

// Fallback width when winsize is
// unknown; rows adapt to real width.
static constexpr int Fallback = 80;

// Cap on cached rows; matches the
// MaxJobs window in the parent.
static constexpr int CacheCap = 64;

// Colors, reset, all ANSI.
static const char Yellow[] =
    "\x1b[33m";
static const char Green[] =
    "\x1b[32m";
static const char Red[] =
    "\x1b[31m";
static const char Dim[] =
    "\x1b[2m";
static const char Reset[] =
    "\x1b[0m";

// Lines in the prior block, for the
// cursor up repaint. Zero on first
// draw or after a dumb run.
static int Prev = 0;

// One cached row: copies plus ints
// for the dirty compare.
typedef struct {
    char base[512];
    char phase[512];
    int live;
    int ph_idx;
    int ph_total;
    long fr_num;
    long fr_den;
    long fr_idx;
    int done;
    int dkind;
} CacheRow;

static CacheRow Cache[64];
static int CacheN = 0;
static int CacheTail = 0;
static int HaveCache = 0;

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
// erase unless mute, then text plus
// newline. Mute prints text plus
// newline only, never CR. Text is
// cut to width; colors wrap outside.
static void put_line(const char *t,
    int dumb) {
    if (dumb == 0) {
        fputs("\r\x1b[K", stdout);
    }
    fputs(t, stdout);
    fputc('\n', stdout);
}

// Cuts the middle of s to width,
// keeping head and tail visible with
// ... between; short lines stay whole.
// Rows keep base start, phase end,
// plus stage [i/n] and frac tails.
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

// Colors a plain ok row: state word
// green, rest plain. Falls back to
// plain when too short.
static void ok_wrap(const char *pl,
    char *out, size_t n) {
    if (strlen(pl) < 2) {
        snprintf(out, n, "%.4000s",
            pl);
        return;
    }
    snprintf(out, n,
        "%s%.2s%s%.4000s", Green,
        pl, Reset, pl + 2);
}

// Colors a plain not ok row: state
// word red, rest plain. Falls back to
// plain when too short.
static void notok_wrap(
    const char *pl, char *out,
    size_t n) {
    if (strlen(pl) < 6) {
        snprintf(out, n, "%.4000s",
            pl);
        return;
    }
    snprintf(out, n,
        "%s%.6s%s%.4000s", Red,
        pl, Reset, pl + 6);
}

// Colors a plain skip row: state
// word dim, rest plain. Falls back
// to plain when too short.
static void skip_wrap(const char *pl,
    char *out, size_t n) {
    if (strlen(pl) < 4) {
        snprintf(out, n, "%.4000s",
            pl);
        return;
    }
    snprintf(out, n,
        "%s%.4s%s%.4000s", Dim,
        pl, Reset, pl + 4);
}

// Cleans a total: negatives read as
// unknown zero.
static int clean_tot(int tot) {
    if (tot < 0) {
        return (0);
    }
    return (tot);
}

// Cleans an index into 0..tot when
// known, else zero.
static int clean_idx(int idx,
    int tot) {
    if (tot <= 0) {
        return (0);
    }
    if (idx < 0) {
        return (0);
    }
    if (idx > tot) {
        return (tot);
    }
    return (idx);
}

// Cleans a frac den: negatives read
// as zero, meaning no frac.
static long clean_den(long den) {
    if (den < 0) {
        return (0);
    }
    return (den);
}

// Cleans a frac num into 0..den when
// den is past zero, else zero.
static long clean_num(long num,
    long den) {
    if (den <= 0) {
        return (0);
    }
    if (num < 0) {
        return (0);
    }
    if (num > den) {
        return (den);
    }
    return (num);
}

// Draws the block, dirty only: skips
// all output when live set, phase
// texts, frac values, and tail count
// match the prior draw; Prev stays
// when skipping. Live rows cap, then
// tail, then done rows once each. No
// head line, no counts, no bars, no
// percent, no elapsed, no spinners;
// rows repaint in place by cursor up;
// dumb or NO_COLOR terms scroll with
// text plus newline only, never CR.
// Known live rows append 1-based
// stage [idx+1/total] plus frac
// num/den when den is past zero;
// unknown live rows read as phase
// only plus frac when set. Done rows
// read as ok green, not ok red, skip
// dim.
void prog_draw(ProgView *vs, int n) {
    char plain[LineCap];
    char shown[LineCap];
    int width = term_width();
    int mute = no_esc();
    int color = prog_color();
    int live = 0;
    int ndone = 0;
    int made = 0;
    int cur = 0;
    int tail = 0;
    int i = 0;
    int k = 0;
    if (vs == nullptr) {
        n = 0;
    }
    if (n < 0) {
        n = 0;
    }
    if (n > CacheCap) {
        n = CacheCap;
    }
    for (i = 0; i < n; i++) {
        if (vs[i].done != 0) {
            ndone++;
            continue;
        }
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
    if (HaveCache != 0 &&
        CacheN == n &&
        CacheTail == tail) {
        int same = 1;
        for (i = 0; i < n; i++) {
            const char *bs = nullptr;
            const char *ds = nullptr;
            int idx = 0;
            int tot = 0;
            long nm = 0;
            long dn = 0;
            long fi = 0;
            int lv = 0;
            int dnw = 0;
            int dkw = 0;
            bs = vs[i].base;
            if (bs == nullptr) {
                bs = "";
            }
            ds = strip_suite(
                vs[i].phase);
            tot = clean_tot(
                vs[i].ph_total);
            idx = clean_idx(
                vs[i].ph_idx, tot);
            dn = clean_den(
                vs[i].fr_den);
            nm = clean_num(
                vs[i].fr_num, dn);
            if (dn == 0) {
                nm = 0;
                fi = 0;
            } else {
                fi = vs[i].fr_idx;
            }
            lv = vs[i].live != 0;
            if (vs[i].done != 0) {
                dnw = 1;
                lv = 0;
            }
            dkw = vs[i].dkind;
            if (strcmp(bs,
                Cache[i].base) != 0) {
                same = 0;
                break;
            }
            if (strcmp(ds,
                Cache[i].phase)
                != 0) {
                same = 0;
                break;
            }
            if (lv !=
                Cache[i].live) {
                same = 0;
                break;
            }
            if (idx !=
                Cache[i].ph_idx) {
                same = 0;
                break;
            }
            if (tot !=
                Cache[i].ph_total) {
                same = 0;
                break;
            }
            if (nm !=
                Cache[i].fr_num) {
                same = 0;
                break;
            }
            if (dn !=
                Cache[i].fr_den) {
                same = 0;
                break;
            }
            if (fi !=
                Cache[i].fr_idx) {
                same = 0;
                break;
            }
            if (dnw !=
                Cache[i].done) {
                same = 0;
                break;
            }
            if (dkw !=
                Cache[i].dkind) {
                same = 0;
                break;
            }
        }
        if (same != 0) {
            return;
        }
    }
    if (mute == 0 && Prev > 0) {
        fprintf(stdout, "\x1b[%dA",
            Prev);
    }
    made = 0;
    for (i = 0; i < n &&
        made < MaxRows; i++) {
        const char *ds = nullptr;
        const char *bs = nullptr;
        int idx = 0;
        int tot = 0;
        long nm = 0;
        long dn = 0;
        if (vs[i].done != 0) {
            continue;
        }
        if (vs[i].live == 0) {
            continue;
        }
        bs = vs[i].base;
        if (bs == nullptr) {
            bs = "";
        }
        ds = strip_suite(
            vs[i].phase);
        tot = clean_tot(
            vs[i].ph_total);
        idx = clean_idx(
            vs[i].ph_idx, tot);
        dn = clean_den(
            vs[i].fr_den);
        nm = clean_num(
            vs[i].fr_num, dn);
        if (tot > 0) {
            if (dn > 0) {
                snprintf(plain,
                    sizeof plain,
                    "RUNNING  %.400s  "
                    "%.400s stage "
                    "[%d/%d] frac "
                    "%ld/%ld", bs, ds,
                    idx + 1, tot, nm,
                    dn);
            } else {
                snprintf(plain,
                    sizeof plain,
                    "RUNNING  %.800s  "
                    "%.800s stage "
                    "[%d/%d]",
                    bs, ds, idx + 1,
                    tot);
            }
        } else {
            if (dn > 0) {
                snprintf(plain,
                    sizeof plain,
                    "RUNNING  %.800s  "
                    "%.800s frac "
                    "%ld/%ld",
                    bs, ds, nm, dn);
            } else {
                snprintf(plain,
                    sizeof plain,
                    "RUNNING  %.1000s  "
                    "%.1000s", bs, ds);
            }
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
            "... +%d more files",
            tail);
        mid_cut(plain, width);
        snprintf(shown,
            sizeof shown, "%.4000s",
            plain);
        put_line(shown, mute);
    }
    for (i = 0; i < n; i++) {
        const char *ds = nullptr;
        const char *bs = nullptr;
        int kind = 0;
        if (vs[i].done == 0) {
            continue;
        }
        bs = vs[i].base;
        if (bs == nullptr) {
            bs = "";
        }
        ds = strip_suite(
            vs[i].phase);
        kind = vs[i].dkind;
        if (kind == 0) {
            snprintf(plain,
                sizeof plain,
                "ok  %.1000s  "
                "%.1000s", bs, ds);
        } else if (kind == 2) {
            snprintf(plain,
                sizeof plain,
                "skip  %.1000s  "
                "%.1000s", bs, ds);
        } else {
            snprintf(plain,
                sizeof plain,
                "not ok  %.800s  "
                "%.800s", bs, ds);
        }
        mid_cut(plain, width);
        if (color != 0) {
            if (kind == 0) {
                ok_wrap(plain,
                    shown,
                    sizeof shown);
            } else if (
                kind == 2) {
                skip_wrap(plain,
                    shown,
                    sizeof shown);
            } else {
                notok_wrap(plain,
                    shown,
                    sizeof shown);
            }
        } else {
            snprintf(shown,
                sizeof shown,
                "%.4000s", plain);
        }
        put_line(shown, mute);
    }
    cur = made;
    if (tail > 0) {
        cur++;
    }
    cur += ndone;
    if (mute == 0) {
        for (k = cur; k < Prev;
            k++) {
            put_line("", mute);
        }
        Prev = cur;
    }
    for (i = 0; i < n; i++) {
        const char *bs = nullptr;
        const char *ds = nullptr;
        int idx = 0;
        int tot = 0;
        long nm = 0;
        long dn = 0;
        long fi = 0;
        int lv = 0;
        int dnw = 0;
        bs = vs[i].base;
        if (bs == nullptr) {
            bs = "";
        }
        ds = strip_suite(
            vs[i].phase);
        tot = clean_tot(
            vs[i].ph_total);
        idx = clean_idx(
            vs[i].ph_idx, tot);
        dn = clean_den(
            vs[i].fr_den);
        nm = clean_num(
            vs[i].fr_num, dn);
        if (dn == 0) {
            nm = 0;
            fi = 0;
        } else {
            fi = vs[i].fr_idx;
        }
        lv = vs[i].live != 0;
        if (vs[i].done != 0) {
            dnw = 1;
            lv = 0;
        }
        snprintf(Cache[i].base,
            sizeof Cache[i].base,
            "%.500s", bs);
        snprintf(Cache[i].phase,
            sizeof Cache[i].phase,
            "%.500s", ds);
        Cache[i].live = lv;
        Cache[i].ph_idx = idx;
        Cache[i].ph_total = tot;
        Cache[i].fr_num = nm;
        Cache[i].fr_den = dn;
        Cache[i].fr_idx = fi;
        Cache[i].done = dnw;
        Cache[i].dkind =
            vs[i].dkind;
    }
    CacheN = n;
    CacheTail = tail;
    HaveCache = 1;
    fflush(stdout);
}
