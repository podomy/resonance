// rsuite: lint, run, list, doctor
// for YAML suites.
//
// Consumes the exec contract
// only; real logic lives in
// suite/exec.c, written apart.
// Types here match it word
// for word. Moved from
// suite/main.c; TAP and JSON
// bytes are unchanged.
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "progress.h"
#include "sim/sim.h"
#include "suite/exec.h"
#include "suite/nemesis.h"
#include "suite/parse.h"

// Cap on why text per file.
static constexpr int WhyCap = 4096;

// Cap on parallel children.
static constexpr int MaxJobs = 64;

// Grace seconds of TERM before the
// run loop escalates to KILL.
static constexpr int KillGrace = 2;

// Cap on a kept phase line.
static constexpr int PhaseCap = 256;

// Prefix len of suite phase lines:
// suite plus phase plus spaces.
static constexpr int PhasePre = 13;

// Prefix len of suite frac notes:
// suite plus frac, edge checked past.
static constexpr int FracPre = 11;

// Cap on pending line bytes.
static constexpr int PendCap = 1024;

// Cap on a log path.
static constexpr int PathCap = 4096;

// Poll tick ms; sets frame rate.
static constexpr int TickMs = 100;

// Child exit code for a skip.
static constexpr int SkipCode = 2;

// Child exit when sh is missing.
static constexpr int NoShell = 127;

// Log name retry cap on clash.
static constexpr int NameTry = 100;

// Cap on phases kept per file; more
// reads as unknown, never crashes.
static constexpr int PhaseKeep = 10000;

// Exits: ok, failure, bad usage.
static constexpr int ExOk = 0;
static constexpr int ExFail = 1;
static constexpr int ExUse = 2;

// Cap on minimize oracle calls;
// past it minimize fails loudly.
static constexpr int OracleCap =
    256;

// Cap on report events kept; past
// it the timeline truncates.
static constexpr int EvCap = 8192;

// Cap on an event message kept.
static constexpr int MsgCap = 512;

// Cap on an event kind token.
static constexpr int KindCap = 16;

// Cap on report logs per dir.
static constexpr int DirCap = 1024;

// Cap on distinct phases scanned
// per key for the split series.
static constexpr int PhScan = 64;

// Cap on file refs kept per log;
// extra refs note and skip.
static constexpr int FileCap = 32;

// Cap on one kept file ref path.
static constexpr int FileLen = 256;

// Cap on a phase display name.
static constexpr int PhName = 64;

// SHA-256 digest bytes.
static constexpr int ShaLen = 32;

// ANSI inks for the tty end panel;
// empty when NO_COLOR or dumb.
static const char InkBold[] =
    "\x1b[1m";
static const char InkGreen[] =
    "\x1b[32m";
static const char InkRed[] =
    "\x1b[31m";
static const char InkDim[] =
    "\x1b[2m";
static const char InkReset[] =
    "\x1b[0m";

// Raised by SIGINT/SIGTERM; the run
// loop kills live children next tick.
static volatile sig_atomic_t SawSig =
    0;

// First-tick time of SawSig; past
// KillGrace the loop sends KILL.
static double SigAt = 0.0;

// Notes a shutdown signal; the run
// loop does the killing, no work here.
static void on_sig(int sig) {
    (void)sig;
    SawSig = 1;
}

// Catches SIGINT/SIGTERM for the run
// loop. 1 when both install.
static int sig_watch(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sig;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa,
        nullptr) != 0) {
        return (0);
    }
    if (sigaction(SIGTERM, &sa,
        nullptr) != 0) {
        return (0);
    }
    return (1);
}

// Restores default dispatch; forked
// children call this first so Ctrl-C
// still kills them directly.
static void sig_def(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa,
        nullptr);
    sigaction(SIGTERM, &sa,
        nullptr);
}

// One file slot in the run window.
// pdone marks a just done slot whose
// verdict row still needs one draw;
// pkind holds 0 pass, 1 fail, 2 skip.
typedef struct {
    pid_t pid;
    int lg;
    int rs;
    FILE *log;
    const char *file;
    const char *base;
    SuiteVerdict v;
    int live;
    int pdone;
    int pkind;
    int grp;
    size_t plen;
    int ptrunc;
    char why[WhyCap];
    char phase[PhaseCap];
    char pend[PendCap];
    char logpath[PathCap];
    char **ph_names;
    size_t ph_total;
    size_t ph_idx;
    size_t ph_next;
    long fr_num;
    long fr_den;
    size_t fr_idx;
    double w0;
    double wall;
    long rsskb;
} Slot;

// True when strings match.
static bool streq(const char *a,
    const char *b) {
    return (strcmp(a, b) == 0);
}

// True for --jobs=N spelling.
static bool is_jobs_eq(const char *a) {
    if (strncmp(a, "--jobs=", 7)
        != 0) {
        return (false);
    }
    return (true);
}

// Parse job count; caps at max with
// a warning. False on bad text;
// leading space fails like 2x does.
static bool jobs_of(const char *s,
    int *out) {
    char *end = nullptr;
    long v = 0;
    if (s[0] == 0) {
        return (false);
    }
    if (isspace(
        (unsigned char)s[0]) != 0) {
        return (false);
    }
    v = strtol(s, &end, 10);
    if (end == s) {
        return (false);
    }
    if (*end != 0) {
        return (false);
    }
    if (v <= 0) {
        return (false);
    }
    if (v > MaxJobs) {
        fprintf(stderr, "rsuite: "
            "--jobs %ld clamped to "
            "%d\n", v, MaxJobs);
        v = MaxJobs;
    }
    *out = (int)v;
    return (true);
}

// True for --log-dir=D spelling.
static bool is_logdir_eq(
    const char *a) {
    if (strncmp(a, "--log-dir=",
        10) != 0) {
        return (false);
    }
    return (true);
}

// Shows global help; fwd for use_err.
static void show_use(FILE *out);

// Reports msg plus full usage; the
// shared usage-error exit path, so
// errors name the cause first.
static int use_err(const char *msg) {
    fprintf(stderr, "rsuite: %s\n",
        msg);
    show_use(stderr);
    return (ExUse);
}

// Prints the shared exit code line.
static void show_exits(FILE *out) {
    fputs("exits: 0 ok, 1 suite",
        out);
    fputs(" failure, 2 bad usage.\n",
        out);
}

// Show usage text on out.
static void show_use(FILE *out) {
    fputs("usage: rsuite lint", out);
    fputs(" <file.yaml>...\n", out);
    fputs("       rsuite run", out);
    fputs(" [--jobs N]", out);
    fputs(" [--log-dir D]", out);
    fputs(" [--tap|--json]", out);
    fputs(" <file.yaml>...\n", out);
    fputs("       rsuite list", out);
    fputs(" <file.yaml>...\n", out);
    fputs("       rsuite doctor\n",
        out);
    fputs("       rsuite nemesis",
        out);
    fputs(" [opts] -o FILE\n",
        out);
    fputs("       rsuite minimize",
        out);
    fputs(" FILE [-o OUT]\n", out);
    fputs("       rsuite report",
        out);
    fputs(" [FILE.log|DIR]\n",
        out);
    fputs("       rsuite bundle",
        out);
    fputs(" FILE.yaml", out);
    fputs(" [--out DIR]\n", out);
    fputs("       rsuite --help\n",
        out);
    fputs("\n", out);
    show_exits(out);
    fputs("run flags precede files;",
        out);
    fputs(" one child per file.",
        out);
    fputs(" Logs land in a fresh",
        out);
    fputs(" per-run dir under",
        out);
    fputs(" /var/tmp and persist;",
        out);
    fputs(" nothing cleans them.",
        out);
    fputs(" Shows live rows with phase",
        out);
    fputs(" stage [i/n], 1-based, plus",
        out);
    fputs(" file counts on each row,",
        out);
    fputs(" tty only, never piped;",
        out);
    fputs(" the two flags conflict.\n",
        out);
    fputs("list parses only, needs",
        out);
    fputs(" no privs; list success",
        out);
    fputs(" does not imply lint",
        out);
    fputs(" success. doctor exits 0",
        out);
    fputs(" with the report even",
        out);
    fputs(" when lines say MISSING,",
        out);
    fputs(" 1 only on probe error.\n",
        out);
}

// Shows lint help only.
static void show_lint_use(FILE *out) {
    fputs("usage: rsuite lint", out);
    fputs(" <file.yaml>...\n", out);
    fputs("\n", out);
    show_exits(out);
    fputs("validates files without",
        out);
    fputs(" running them; prints",
        out);
    fputs(" path:line errors on",
        out);
    fputs(" stderr.\n", out);
}

// Shows run help only.
static void show_run_use(FILE *out) {
    fputs("usage: rsuite run", out);
    fputs(" [--jobs N]", out);
    fputs(" [--log-dir D]", out);
    fputs(" [--tap|--json]", out);
    fputs(" <file.yaml>...\n", out);
    fputs("\n", out);
    show_exits(out);
    fputs("flags precede files;",
        out);
    fputs(" trailing flags are",
        out);
    fputs(" usage errors. --jobs",
        out);
    fputs(" caps at 64 with a",
        out);
    fputs(" warning; --tap and",
        out);
    fputs(" --json conflict. One",
        out);
    fputs(" child per file; Ctrl-C",
        out);
    fputs(" stops launch and kills",
        out);
    fputs(" live children: TERM",
        out);
    fputs(" first, KILL past grace;",
        out);
    fputs(" own group per child",
        out);
    fputs(" plus pid fallback,",
        out);
    fputs(" no orphans. Rows show",
        out);
    fputs(" phase stage [i/n],",
        out);
    fputs(" 1-based.",
        out);
    fputs(" Logs",
        out);
    fputs(" persist per file in the",
        out);
    fputs(" run dir; nothing cleans",
        out);
    fputs(" them; missing logs",
        out);
    fputs(" report (no log). Each",
        out);
    fputs(" file keeps per-node",
        out);
    fputs(" snapshots plus flow",
        out);
    fputs(" counters in its log;",
        out);
    fputs(" the run dir gains",
        out);
    fputs(" report.html. Run dir",
        out);
    fputs(" opens 0755, logs and",
        out);
    fputs(" report read 0644; sudo",
        out);
    fputs(" runs chown to",
        out);
    fputs(" $SUDO_USER; secrets",
        out);
    fputs(" need --log-dir on a",
        out);
    fputs(" private path.\n", out);
}

// Shows list help only.
static void show_list_use(FILE *out) {
    fputs("usage: rsuite list", out);
    fputs(" <file.yaml>...\n", out);
    fputs("\n", out);
    show_exits(out);
    fputs("parses only, needs no",
        out);
    fputs(" privs; success does not",
        out);
    fputs(" imply lint success.",
        out);
    fputs(" Missing keys warn on",
        out);
    fputs(" stderr; spaced names",
        out);
    fputs(" print quoted.\n", out);
}

// Shows doctor help only.
static void show_doctor_use(FILE *out) {
    fputs("usage: rsuite doctor\n",
        out);
    fputs("\n", out);
    show_exits(out);
    fputs("reports deps; exits 0",
        out);
    fputs(" with the report even",
        out);
    fputs(" when lines say MISSING,",
        out);
    fputs(" 1 only on probe error.",
        out);
    fputs(" Creating netns and TUN",
        out);
    fputs(" ids needs root; missing",
        out);
    fputs(" deps inform, not fail.\n",
        out);
}

// Fill why from errno text.
static void why_errno(char *why) {
    snprintf(why, (size_t)WhyCap,
        "%s", strerror(errno));
}

// Print s with no newlines; CR maps
// to space like LF.
static void flat_why(const char *s) {
    for (; *s != 0; s++) {
        if (*s == '\n' ||
            *s == '\r') {
            fputc(' ', stdout);
        } else {
            fputc(*s, stdout);
        }
    }
}

// Print s quoted for JSON.
static void json_str(FILE *out,
    const char *s) {
    fputc('"', out);
    for (; *s != 0; s++) {
        unsigned char c =
            (unsigned char)*s;
        if (c == '"') {
            fputs("\\\"", out);
        } else if (c == '\\') {
            fputs("\\\\", out);
        } else if (c == '\n') {
            fputs("\\n", out);
        } else if (c == '\t') {
            fputs("\\t", out);
        } else if (c == '\r') {
            fputs("\\r", out);
        } else if (c < 0x20) {
            fprintf(out, "\\u%04x",
                (unsigned int)c);
        } else {
            fputc(c, out);
        }
    }
    fputc('"', out);
}

// Print one TAP result line.
static void tap_line(int idx,
    const char *path, SuiteVerdict v,
    const char *why) {
    if (v == SUITE_PASS) {
        printf("ok %d %s\n", idx,
            path);
        return;
    }
    if (v == SUITE_SKIP) {
        printf("ok %d %s # SKIP ",
            idx, path);
        flat_why(why);
        fputc('\n', stdout);
        return;
    }
    printf("not ok %d %s - ", idx,
        path);
    flat_why(why);
    fputc('\n', stdout);
}

// Print one JSON result line.
static void json_line(
    const char *path, SuiteVerdict v,
    const char *why) {
    const char *s = "fail";
    if (v == SUITE_PASS) {
        s = "pass";
    } else if (v == SUITE_SKIP) {
        s = "skip";
    }
    fputs("{\"file\":", stdout);
    json_str(stdout, path);
    fputs(",\"verdict\":\"", stdout);
    fputs(s, stdout);
    fputs("\",\"why\":", stdout);
    json_str(stdout, why);
    fputs("}\n", stdout);
}

// Print one result in TAP/JSON.
static void show_one(int json_flag,
    int idx, const char *path,
    SuiteVerdict v,
    const char *why) {
    if (json_flag != 0) {
        json_line(path, v, why);
        return;
    }
    tap_line(idx, path, v, why);
}

// Print TAP summary counts. Colors
// counts on a color tty run only;
// piped and flagged runs stay plain.
static void tap_sum(int pass,
    int fail, int skip, int prog) {
    if (prog != 0 &&
        prog_color() != 0) {
        printf("%s# pass=%s%d%s%s",
            InkBold, InkGreen, pass,
            InkReset, InkBold);
        printf(" fail=%s%d%s%s",
            InkRed, fail, InkReset,
            InkBold);
        printf(" skip=%s%d%s\n",
            InkDim, skip, InkReset);
        return;
    }
    printf("# pass=%d fail=%d", pass,
        fail);
    printf(" skip=%d\n", skip);
}

// Print JSON summary line, plus a
// resources object: summed wall
// seconds and peak child megabytes.
// Shape stays fixed; values vary
// run to run.
static void json_sum(int pass,
    int fail, int skip, double wall,
    long rssmb) {
    printf("{\"summary\":{\"pass\":"
        "%d", pass);
    printf(",\"fail\":%d", fail);
    printf(",\"skip\":%d", skip);
    printf(",\"resources\":"
        "{\"wall_s\":%.3f,"
        "\"maxrss_mb\":%ld}}\n",
        wall, rssmb);
}

// Print summary in TAP/JSON. JSON
// never colors; TAP colors only on
// a color tty run, else stays plain.
static void show_sum(int json_flag,
    int pass, int fail, int skip,
    int prog, double wall,
    long rssmb) {
    if (json_flag != 0) {
        json_sum(pass, fail, skip,
            wall, rssmb);
        return;
    }
    tap_sum(pass, fail, skip, prog);
}

// Prints one resource comment per
// file in TAP output only. # lines
// are TAP-legal and parsers ignore
// them, so result and summary bytes
// stay unchanged; piped runs keep
// the same shape while values vary.
static void show_res(char **files,
    Slot *sl, int n) {
    for (int i = 0; i < n; i++) {
        long mb = sl[i].rsskb /
            1024;
        printf("# resources: "
            "wall=%.3f maxrss=%ld "
            "%s\n", sl[i].wall, mb,
            files[i]);
    }
}

// Tally verdict to counters.
static void tally(SuiteVerdict v,
    int *pass, int *fail, int *skip) {
    if (v == SUITE_PASS) {
        (*pass)++;
        return;
    }
    if (v == SUITE_SKIP) {
        (*skip)++;
        return;
    }
    (*fail)++;
}

// Drain fd into buf; NUL terms.
static void drain(int fd, char *buf) {
    size_t got = 0;
    for (;;) {
        size_t want =
            (size_t)(WhyCap - 1) -
            got;
        ssize_t r = read(fd,
            buf + got, want);
        if (r == 0) {
            break;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        got += (size_t)r;
        if (got >=
            (size_t)(WhyCap - 1)) {
            break;
        }
    }
    buf[got] = 0;
}

// Base name after last slash.
static const char *base_of(
    const char *p) {
    const char *b =
        strrchr(p, '/');
    if (b == nullptr) {
        return (p);
    }
    return (b + 1);
}

// Monotonic seconds, for ETA sums
// and the KILL grace clock.
static double now_s(void) {
    struct timespec ts = {0, 0};
    if (clock_gettime(
        CLOCK_MONOTONIC, &ts)
        != 0) {
        return (0.0);
    }
    return ((double)ts.tv_sec +
        (double)ts.tv_nsec / 1e9);
}

// Run dir mode: mkdtemp gives 0700,
// and file:// readers need +x to
// enter, so force 0755, not umask.
static constexpr int DirMode = 0755;

// Run file mode: logs, report, and
// bundle files read for all, forced
// past umask like the dir.
static constexpr int FileMode = 0644;

// Warned once when a creation-time
// chmod misses; the end walk retries
// it, so one note suffices.
static int ChmodWarned = 0;

// Notes one chmod miss on stderr,
// once per process; never fails.
static void chmod_note(
    const char *p) {
    if (ChmodWarned != 0) {
        return;
    }
    ChmodWarned = 1;
    fprintf(stderr, "rsuite: "
        "cannot chmod %s: %s\n", p,
        strerror(errno));
}

// Parses a strict uid/gid: digits
// only, no junk, fits 32 bits.
// False on garbage, never crashes.
static bool parse_id(const char *s,
    unsigned long *out) {
    unsigned long v = 0;
    char *end = nullptr;
    size_t k = 0;
    if (s == nullptr ||
        out == nullptr) {
        return (false);
    }
    if (s[0] == 0) {
        return (false);
    }
    for (k = 0; s[k] != 0; k++) {
        if (isdigit(
            (unsigned char)s[k])
            == 0) {
            return (false);
        }
    }
    errno = 0;
    v = strtoul(s, &end, 10);
    if (end == s || *end != 0) {
        return (false);
    }
    if (errno != 0) {
        return (false);
    }
    if (v >
        (unsigned long)UINT32_MAX) {
        return (false);
    }
    *out = v;
    return (true);
}

// Reads the sudo caller from env:
// needs SUDO_USER plus numeric
// SUDO_UID/SUDO_GID, and they must
// differ from euid/egid, else there
// is nothing to chown. Garbage
// reads as absent, never crashes.
static bool sudo_owner(uid_t *u,
    gid_t *g) {
    const char *n = getenv(
        "SUDO_USER");
    const char *su = getenv(
        "SUDO_UID");
    const char *sg = getenv(
        "SUDO_GID");
    unsigned long v = 0;
    unsigned long w = 0;
    if (u == nullptr ||
        g == nullptr) {
        return (false);
    }
    if (n == nullptr || n[0] == 0) {
        return (false);
    }
    if (su == nullptr ||
        sg == nullptr) {
        return (false);
    }
    if (!parse_id(su, &v)) {
        return (false);
    }
    if (!parse_id(sg, &w)) {
        return (false);
    }
    if ((uid_t)v == geteuid() &&
        (gid_t)w == getegid()) {
        return (false);
    }
    *u = (uid_t)v;
    *g = (gid_t)w;
    return (true);
}

// Warns once for a fixup miss; the
// run verdict never moves for perms.
static void fix_warn(int *w,
    const char *p) {
    if (*w != 0) {
        return;
    }
    *w = 1;
    fprintf(stderr, "rsuite: "
        "cannot fix %s: %s\n", p,
        strerror(errno));
}

// Fixes one run tree path: dirs to
// 0755, files to 0644, plus chown
// when own says sudo. Links skip,
// so no escape; misses warn once.
static void fix_path(const char *p,
    uid_t u, gid_t g, int own,
    int *w) {
    struct stat st;
    DIR *d = nullptr;
    if (lstat(p, &st) != 0) {
        fix_warn(w, p);
        return;
    }
    if (S_ISLNK(st.st_mode)) {
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        struct dirent *e = nullptr;
        if (chmod(p, DirMode)
            != 0) {
            fix_warn(w, p);
        }
        if (own != 0 &&
            chown(p, u, g) != 0) {
            fix_warn(w, p);
        }
        d = opendir(p);
        if (d == nullptr) {
            fix_warn(w, p);
            return;
        }
        for (;;) {
            char full[PathCap];
            int r = 0;
            e = readdir(d);
            if (e == nullptr) {
                break;
            }
            if (streq(e->d_name,
                ".") ||
                streq(e->d_name,
                    "..")) {
                continue;
            }
            r = snprintf(full,
                sizeof full, "%s/%s",
                p, e->d_name);
            if (r < 0 ||
                (size_t)r >=
                    sizeof full) {
                fix_warn(w, p);
                continue;
            }
            fix_path(full, u, g,
                own, w);
        }
        closedir(d);
        return;
    }
    if (S_ISREG(st.st_mode)) {
        if (chmod(p, FileMode)
            != 0) {
            fix_warn(w, p);
        }
        if (own != 0 &&
            chown(p, u, g) != 0) {
            fix_warn(w, p);
        }
        return;
    }
    if (own != 0 &&
        chown(p, u, g) != 0) {
        fix_warn(w, p);
    }
}

// Opens run outputs for reading.
// Rule: explicit 0755/0644 always,
// chown to $SUDO_USER only under
// sudo. Logs may hold SUT output,
// so world-readable is the tradeoff
// for a clickable report link;
// secret runs pass --log-dir on a
// private path, whose dir modes
// stay theirs. Warns once, never
// fails the run.
static void fix_run(const char *d) {
    uid_t u = 0;
    gid_t g = 0;
    int own = 0;
    int w = 0;
    if (d == nullptr) {
        return;
    }
    own = sudo_owner(&u, &g) ? 1 : 0;
    fix_path(d, u, g, own, &w);
}

// Makes a fresh per-run log dir with
// mkdtemp, then chmods 0755: mkdtemp
// gives 0700, and file:// readers
// need +x to enter. Warns, 1 still.
static int make_logdir(char *out,
    size_t n) {
    int r = snprintf(out, n,
        "/var/tmp/"
        "resonance-rsuite-XXXXXX");
    if (r < 0 || (size_t)r >= n) {
        return (0);
    }
    if (mkdtemp(out) == nullptr) {
        return (0);
    }
    if (chmod(out, DirMode) != 0) {
        chmod_note(out);
    }
    return (1);
}

// Creates dir for --log-dir; EEXIST
// is fine. Stats back and rejects
// non-directories instead of
// degrading. 1 on success.
static int ensure_logdir(
    const char *dir) {
    struct stat st;
    if (mkdir(dir, 0755) != 0 &&
        errno != EEXIST) {
        return (0);
    }
    if (stat(dir, &st) != 0) {
        return (0);
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return (0);
    }
    return (1);
}

// Joins dir/base with opt tag plus
// .log; tag -1 means none. 1 when
// the result fits the path cap.
static int log_join(char *dst,
    const char *dir,
    const char *base, int tag) {
    size_t dn = strlen(dir);
    size_t bn = strlen(base);
    size_t tn = 0;
    size_t need = 0;
    size_t pos = 0;
    char tb[32];
    if (tag >= 0) {
        int r = snprintf(tb,
            sizeof tb, "-%d", tag);
        if (r < 0 ||
            (size_t)r >=
                sizeof tb) {
            return (0);
        }
        tn = (size_t)r;
    }
    need = dn + 1 + bn + tn + 4;
    if (need >= (size_t)PathCap) {
        return (0);
    }
    memcpy(dst + pos, dir, dn);
    pos += dn;
    dst[pos++] = '/';
    memcpy(dst + pos, base, bn);
    pos += bn;
    if (tag >= 0) {
        memcpy(dst + pos, tb,
            tn);
        pos += tn;
    }
    memcpy(dst + pos, ".log", 5);
    return (1);
}

// Opens a fresh per-file log with
// O_EXCL; numeric suffix on clash.
// Null log keeps the run; bytes are
// still phase scanned.
static void log_open(Slot *s,
    const char *logdir) {
    int k = 0;
    s->log = nullptr;
    s->logpath[0] = 0;
    if (logdir == nullptr) {
        return;
    }
    for (k = -1; k < NameTry; k++) {
        int fd = -1;
        FILE *f = nullptr;
        if (log_join(s->logpath,
            logdir, s->base, k)
            == 0) {
            s->logpath[0] = 0;
            return;
        }
        fd = open(s->logpath,
            O_WRONLY | O_CREAT |
                O_EXCL,
            FileMode);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            s->logpath[0] = 0;
            return;
        }
        // fopen follows umask; force
        // 0644 now, past root or 077.
        if (fchmod(fd, FileMode)
            != 0) {
            chmod_note(
                s->logpath);
        }
        f = fdopen(fd, "w");
        if (f == nullptr) {
            close(fd);
            s->logpath[0] = 0;
            return;
        }
        s->log = f;
        return;
    }
    s->logpath[0] = 0;
}

// Inits one empty run slot.
static void slot_init(Slot *s) {
    s->pid = -1;
    s->lg = -1;
    s->rs = -1;
    s->log = nullptr;
    s->file = nullptr;
    s->base = nullptr;
    s->why[0] = 0;
    s->logpath[0] = 0;
    s->pend[0] = 0;
    s->plen = 0;
    s->ptrunc = 0;
    s->v = SUITE_FAIL;
    s->live = 0;
    s->pdone = 0;
    s->pkind = 1;
    s->grp = 0;
    s->ph_names = nullptr;
    s->ph_total = 0;
    s->ph_idx = 0;
    s->ph_next = 0;
    s->fr_num = 0;
    s->fr_den = 0;
    s->fr_idx = 0;
    s->w0 = 0.0;
    s->wall = 0.0;
    s->rsskb = 0;
    memcpy(s->phase, "starting",
        9);
}

// Copies pending line to phase; call
// only when phase_advance moved.
static void phase_set(Slot *s) {
    size_t k = strlen(s->pend);
    size_t cap =
        (size_t)PhaseCap - 1;
    if (k > cap) {
        k = cap;
    }
    memcpy(s->phase, s->pend, k);
    s->phase[k] = 0;
}

// True when word is a phase edge:
// begin or done close a phase line.
static bool is_edge(
    const char *w) {
    if (strcmp(w, "begin") == 0) {
        return (true);
    }
    if (strcmp(w, "done") == 0) {
        return (true);
    }
    return (false);
}

// True when line opens a frac note:
// suite: frac with end or blank
// next. Malformed tails still count;
// the parser drops them below, and
// the row keeps old phase text.
static bool is_frac(
    const char *line) {
    char c = 0;
    if (line == nullptr) {
        return (false);
    }
    if (strncmp(line, "suite: frac",
        (size_t)FracPre) != 0) {
        return (false);
    }
    c = line[FracPre];
    if (c == 0 || c == ' ' ||
        c == '\t') {
        return (true);
    }
    return (false);
}

// Parses suite: frac i num den; all
// plain non-negative longs, den past
// zero, num within den, blanks only
// past. False means ignore the line.
static bool frac_parse(
    const char *line, long *pi,
    long *pn, long *pd) {
    const char *p = nullptr;
    long i = 0;
    long n2 = 0;
    long d = 0;
    int used = 0;
    int k = 0;
    if (line == nullptr ||
        pi == nullptr || pn == nullptr
        || pd == nullptr) {
        return (false);
    }
    if (!is_frac(line)) {
        return (false);
    }
    p = line + FracPre;
    if (sscanf(p, "%ld %ld %ld %n",
        &i, &n2, &d, &used) != 3) {
        return (false);
    }
    if (i < 0 || n2 < 0 || d <= 0) {
        return (false);
    }
    if (n2 > d) {
        return (false);
    }
    for (k = used; p[k] != 0; k++) {
        if (p[k] != ' ' &&
            p[k] != '\t') {
            return (false);
        }
    }
    *pi = i;
    *pn = n2;
    *pd = d;
    return (true);
}

// Stores one frac note beside phase
// idx; rows show stage [idx+1/total]
// from it. Out of range or malformed
// lines just miss, never crash.
static void frac_advance(Slot *s,
    const char *line) {
    long i = 0;
    long n2 = 0;
    long d = 0;
    if (s == nullptr) {
        return;
    }
    if (line == nullptr) {
        return;
    }
    if (s->ph_total == 0) {
        return;
    }
    if (s->ph_names == nullptr) {
        return;
    }
    if (!frac_parse(line, &i, &n2,
        &d)) {
        return;
    }
    if ((size_t)i >= s->ph_total) {
        return;
    }
    s->fr_idx = (size_t)i;
    s->fr_num = n2;
    s->fr_den = d;
}

// Reconciles stored frac on a phase
// edge: done means full, begin keeps
// early frac for the new idx else
// clears the stale old-phase note.
static void phase_frac(Slot *s,
    int done) {
    if (s == nullptr) {
        return;
    }
    if (done != 0) {
        s->fr_idx = s->ph_idx;
        s->fr_num = 1;
        s->fr_den = 1;
        return;
    }
    if (s->fr_den == 0) {
        return;
    }
    if (s->fr_idx != s->ph_idx) {
        s->fr_num = 0;
        s->fr_den = 0;
        s->fr_idx = 0;
    }
}

// Advances slot phase index when
// line is the next expected phase:
// suite: phase <i> <name> begin or
// done. Unknown or repeated names
// never move it back. Entering
// phase 0 keeps idx 0, so a 1-file
// 4-phase run steps 0, 1, 2, 3 over
// 4, then the done file is 4/4.
// Done sets stored frac full, begin
// clears the stale old-phase note.
// True when the line advanced the
// phase; only then may the caller
// refresh the row text, so replayed
// or foreign suite: lines never
// flicker it.
static bool phase_advance(Slot *s,
    const char *line) {
    const char *rest = nullptr;
    const char *exp = nullptr;
    char core[PendCap];
    size_t rn = 0;
    char *end = nullptr;
    long v = 0;
    int done = 0;
    if (s == nullptr) {
        return (false);
    }
    if (line == nullptr) {
        return (false);
    }
    if (s->ph_total == 0) {
        return (false);
    }
    if (s->ph_names == nullptr) {
        return (false);
    }
    if (s->ph_next >=
        s->ph_total) {
        return (false);
    }
    if (strncmp(line,
        "suite: phase ",
        (size_t)PhasePre) != 0) {
        return (false);
    }
    rest = line + PhasePre;
    while (*rest == ' ' ||
        *rest == '\t') {
        rest++;
    }
    if (*rest == 0) {
        return (false);
    }
    exp = s->ph_names[
        s->ph_next];
    if (exp == nullptr) {
        return (false);
    }
    if (exp[0] == 0) {
        return (false);
    }
    snprintf(core, sizeof core,
        "%s", rest);
    rn = strlen(core);
    while (rn > 0 &&
        (core[rn - 1] == ' ' ||
        core[rn - 1] == '\t')) {
        rn--;
        core[rn] = 0;
    }
    if (rn == 0) {
        return (false);
    }
    {
        size_t at = rn;
        while (at > 0 &&
            core[at - 1] != ' ' &&
            core[at - 1] != '\t') {
            at--;
        }
        if (at < rn &&
            is_edge(core + at)) {
            size_t en = at;
            done = streq(core + at,
                "done") ? 1 : 0;
            while (en > 0 &&
                (core[en - 1] ==
                    ' ' ||
                core[en - 1] ==
                    '\t')) {
                en--;
            }
            core[en] = 0;
            rn = en;
            if (rn == 0) {
                return (false);
            }
        }
    }
    if (strcmp(core, exp) == 0) {
        s->ph_idx = s->ph_next;
        s->ph_next++;
        phase_frac(s, done);
        return (true);
    }
    end = nullptr;
    v = strtol(core, &end, 10);
    (void)v;
    if (end == core) {
        return (false);
    }
    if (*end != ' ' &&
        *end != '\t') {
        return (false);
    }
    while (*end == ' ' ||
        *end == '\t') {
        end++;
    }
    if (strcmp(end, exp) != 0) {
        return (false);
    }
    s->ph_idx = s->ph_next;
    s->ph_next++;
    phase_frac(s, done);
        return (true);
}

// Feeds pipe bytes to the log and
// keeps the last suite: line seen.
static void feed_bytes(Slot *s,
    const char *buf, size_t n) {
    size_t w = 0;
    size_t i = 0;
    if (s->log != nullptr &&
        n > 0) {
        w = fwrite(buf, 1, n,
            s->log);
        (void)w;
    }
    for (i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') {
            s->pend[s->plen] = 0;
            if (strncmp(s->pend,
                "suite:", 6)
                == 0) {
                if (is_frac(
                    s->pend)) {
                    frac_advance(s,
                        s->pend);
                } else if (
                    phase_advance(s,
                        s->pend)) {
                    phase_set(s);
                }
            }
            s->plen = 0;
            s->ptrunc = 0;
            continue;
        }
        if (s->ptrunc != 0) {
            continue;
        }
        if (s->plen + 1 >=
            (size_t)PendCap) {
            s->ptrunc = 1;
            continue;
        }
        s->pend[s->plen++] = c;
    }
}

// Flushes a trailing partial line.
static void finish_feed(Slot *s) {
    if (s->plen == 0) {
        return;
    }
    s->pend[s->plen] = 0;
    if (strncmp(s->pend,
        "suite:", 6) == 0) {
        if (is_frac(s->pend)) {
            frac_advance(s,
                s->pend);
        } else if (phase_advance(s,
            s->pend)) {
            phase_set(s);
        }
    }
    s->plen = 0;
    s->ptrunc = 0;
}

// Runs one file through the shared
// suite entry, filling why; returns
// the child exit code for the
// verdict. Forked run children and
// the minimize oracle both call it,
// so one path serves every real run.
static int run_file_code(
    const char *path, char *why,
    size_t n) {
    SuiteOpts o = {0};
    SuiteVerdict v;
    if (n == 0) {
        return (ExFail);
    }
    memset(why, 0, n);
    v = suite_run_file(path, &o,
        why, n);
    why[n - 1] = 0;
    if (v == SUITE_PASS) {
        return (ExOk);
    }
    if (v == SUITE_SKIP) {
        return (SkipCode);
    }
    return (ExFail);
}

// Child: stdio onto log pipe, run
// the file, send why on result
// pipe, then exit with the code.
static void child_main(
    const char *path, int lgw,
    int rsw) {
    char why[WhyCap];
    int code = ExFail;
    size_t len = 0;
    size_t off = 0;
    sig_def();
    if (dup2(lgw, STDOUT_FILENO)
        < 0) {
        _exit(ExFail);
    }
    if (dup2(lgw, STDERR_FILENO)
        < 0) {
        _exit(ExFail);
    }
    if (lgw != STDOUT_FILENO &&
        lgw != STDERR_FILENO) {
        close(lgw);
    }
    setvbuf(stdout, nullptr, _IOLBF,
        0);
    code = run_file_code(path, why,
        sizeof why);
    fflush(stdout);
    fflush(stderr);
    len = strlen(why);
    while (off < len) {
        ssize_t w = write(rsw,
            why + off, len - off);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        off += (size_t)w;
    }
    close(rsw);
    _exit(code);
}

// Starts one file slot. Missing or
// unpipeable files fail with no
// child, exactly as before.
static void launch_one(Slot *s,
    const char *path,
    const char *logdir) {
    int lg[2] = {-1, -1};
    int rs[2] = {-1, -1};
    int fl = 0;
    pid_t c = -1;
    s->file = path;
    s->base = base_of(path);
    if (access(path, R_OK) != 0) {
        why_errno(s->why);
        return;
    }
    if (pipe(lg) != 0) {
        why_errno(s->why);
        return;
    }
    if (pipe(rs) != 0) {
        why_errno(s->why);
        close(lg[0]);
        close(lg[1]);
        return;
    }
    fcntl(lg[0], F_SETFD,
        FD_CLOEXEC);
    fcntl(lg[1], F_SETFD,
        FD_CLOEXEC);
    fcntl(rs[0], F_SETFD,
        FD_CLOEXEC);
    fcntl(rs[1], F_SETFD,
        FD_CLOEXEC);
    fl = fcntl(lg[0], F_GETFL,
        0);
    if (fl >= 0) {
        fcntl(lg[0], F_SETFL,
            fl | O_NONBLOCK);
    }
    log_open(s, logdir);
    c = fork();
    if (c < 0) {
        why_errno(s->why);
        close(lg[0]);
        close(lg[1]);
        close(rs[0]);
        close(rs[1]);
        if (s->log != nullptr) {
            fclose(s->log);
            s->log = nullptr;
        }
        return;
    }
    if (c == 0) {
        (void)setpgid(0, 0);
        close(lg[0]);
        close(rs[0]);
        child_main(path, lg[1],
            rs[1]);
        _exit(ExFail);
    }
    close(lg[1]);
    close(rs[1]);
    s->pid = c;
    if (setpgid(c, c) == 0) {
        s->grp = 1;
    }
    s->lg = lg[0];
    s->rs = rs[0];
    s->live = 1;
    s->w0 = now_s();
}

// Reads ready pipe bytes into one
// slot; stops at EOF or EAGAIN.
static void read_avail(Slot *s) {
    char tmp[WhyCap];
    for (;;) {
        ssize_t r = read(s->lg,
            tmp, sizeof tmp);
        if (r == 0) {
            return;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        feed_bytes(s, tmp,
            (size_t)r);
    }
}

// Kills leftover group members
// after a child is reaped; fast
// when the group is gone. TERM
// first, KILL past KillGrace, pid
// fallback when group is gone.
static void slot_orphan(pid_t pg,
    int grp) {
    int k = 0;
    int max = 0;
    struct timespec ts;
    if (pg <= 0) {
        return;
    }
    if (grp == 0) {
        return;
    }
    if (kill(-pg, 0) != 0) {
        return;
    }
    (void)kill(-pg, SIGTERM);
    if (kill(-pg, 0) != 0) {
        return;
    }
    (void)kill(pg, SIGTERM);
    max = KillGrace * 10;
    ts.tv_sec = 0;
    ts.tv_nsec = 100000000L;
    for (k = 0; k < max; k++) {
        nanosleep(&ts, nullptr);
        if (kill(-pg, 0) != 0) {
            return;
        }
    }
    (void)kill(-pg, SIGKILL);
    (void)kill(pg, SIGKILL);
}

// Finishes one reaped slot: last
// pipe bytes, log close, why drain,
// group orphan kill, verdict from
// the exit code. Marks pdone once so
// the next progress draw shows one
// done row; pkind holds 0 pass, 1
// fail, 2 skip.
static void finish_slot(Slot *s,
    int st) {
    pid_t pg = -1;
    int grp = 0;
    pg = s->pid;
    grp = s->grp;
    s->live = 0;
    s->pid = -1;
    s->grp = 0;
    if (s->w0 > 0.0) {
        struct rusage ru;
        s->wall = now_s() - s->w0;
        memset(&ru, 0, sizeof ru);
        if (getrusage(
            RUSAGE_CHILDREN,
            &ru) == 0) {
            s->rsskb = ru.ru_maxrss;
        }
    }
    read_avail(s);
    finish_feed(s);
    if (s->lg >= 0) {
        close(s->lg);
        s->lg = -1;
    }
    if (s->log != nullptr) {
        fclose(s->log);
        s->log = nullptr;
    }
    if (s->rs >= 0) {
        drain(s->rs, s->why);
        close(s->rs);
        s->rs = -1;
    }
    slot_orphan(pg, grp);
    if (!WIFEXITED(st)) {
        s->v = SUITE_FAIL;
        s->pdone = 1;
        s->pkind = 1;
        if (s->why[0] == 0) {
            snprintf(s->why,
                WhyCap,
                "child killed");
        }
        return;
    }
    if (WEXITSTATUS(st) == ExOk) {
        s->v = SUITE_PASS;
        s->pdone = 1;
        s->pkind = 0;
    } else if (WEXITSTATUS(st)
        == SkipCode) {
        s->v = SUITE_SKIP;
        s->pdone = 1;
        s->pkind = 2;
    } else {
        s->v = SUITE_FAIL;
        s->pdone = 1;
        s->pkind = 1;
    }
}

// Reaps exited children; drains
// pipes, closes logs, tallies.
static void reap_all(Slot *sl,
    int n, int *live, int *done,
    int *pass, int *fail, int *skip) {
    for (;;) {
        int st = 0;
        pid_t w = waitpid(-1,
            &st, WNOHANG);
        int k = -1;
        if (w == 0) {
            return;
        }
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        for (int i = 0; i < n;
            i++) {
            if (sl[i].pid == w) {
                k = i;
                break;
            }
        }
        if (k < 0) {
            continue;
        }
        finish_slot(&sl[k], st);
        (*live)--;
        (*done)++;
        tally(sl[k].v, pass, fail,
            skip);
    }
}

// Polls live pipes once, feeds new
// bytes, then reaps any exits.
static void poll_step(Slot *sl,
    int n, int *live, int *done,
    int *pass, int *fail, int *skip) {
    struct pollfd pf[MaxJobs];
    int who[MaxJobs];
    int m = 0;
    int r = 0;
    for (int i = 0; i < n; i++) {
        if (sl[i].live == 0) {
            continue;
        }
        if (m >= MaxJobs) {
            break;
        }
        pf[m].fd = sl[i].lg;
        pf[m].events = POLLIN;
        pf[m].revents = 0;
        who[m] = i;
        m++;
    }
    if (m <= 0) {
        return;
    }
    r = poll(pf, (nfds_t)m,
        TickMs);
    if (r > 0) {
        for (int k = 0; k < m;
            k++) {
            if ((pf[k].revents &
                (POLLIN |
                    POLLHUP |
                    POLLERR))
                != 0) {
                read_avail(
                    &sl[who[k]]);
            }
        }
    }
    reap_all(sl, n, live, done,
        pass, fail, skip);
}

// Loads phase names; fwd, defined
// near read_all it reuses.
static void phase_load(Slot *s,
    const char *path);

// Frees phase names; fwd, defined
// near read_all it reuses.
static void phase_free(Slot *s);

// Renders per-run report.html; fwd,
// defined near html_report below.
static void auto_html(
    const char *dir, int prog);

// Signals one live slot: its own
// group when set, else its pid.
// Negative pid targets the group,
// covering grandchildren, so no
// orphans keep netns or TUN. TERM
// first, KILL past grace. Group
// kill falls back to pid kill when
// the group is gone.
static void slot_sig(Slot *s,
    int sig) {
    pid_t p = -1;
    if (s->live == 0) {
        return;
    }
    if (s->pid <= 0) {
        return;
    }
    p = s->pid;
    if (s->grp != 0) {
        if (kill(-p, sig) == 0) {
            return;
        }
        (void)kill(p, sig);
        return;
    }
    (void)kill(p, sig);
}

// Draws one progress snapshot from
// the live slots plus just done ones
// in input order. No bars, no percent,
// no elapsed, no spinners; live rows
// show phase plus 1-based stage
// [idx+1/total] plus frac num/den
// when den is past zero, idx maxing
// at total. Done rows carry one
// verdict each and clear so they show
// exactly once.
static void prog_show(Slot *sl,
    int n) {
    ProgView vs[MaxJobs];
    int at[MaxJobs];
    int m = 0;
    for (int i = 0; i < n; i++) {
        size_t tot = 0;
        size_t idx = 0;
        int known = 0;
        if (sl[i].pdone != 0) {
            if (m >= MaxJobs) {
                break;
            }
            vs[m].base =
                sl[i].base;
            vs[m].phase =
                sl[i].phase;
            vs[m].live = 0;
            vs[m].ph_idx = 0;
            vs[m].ph_total = 0;
            vs[m].fr_num = 0;
            vs[m].fr_den = 0;
            vs[m].fr_idx = 0;
            vs[m].done = 1;
            vs[m].dkind =
                sl[i].pkind;
            at[m] = i;
            m++;
            continue;
        }
        if (sl[i].live == 0) {
            continue;
        }
        if (m >= MaxJobs) {
            break;
        }
        vs[m].base = sl[i].base;
        vs[m].phase = sl[i].phase;
        vs[m].live = 1;
        vs[m].ph_idx = 0;
        vs[m].ph_total = 0;
        vs[m].fr_num =
            sl[i].fr_num;
        vs[m].fr_den =
            sl[i].fr_den;
        vs[m].fr_idx =
            (long)sl[i].fr_idx;
        vs[m].done = 0;
        vs[m].dkind = 0;
        tot = sl[i].ph_total;
        idx = sl[i].ph_idx;
        known = 0;
        if (tot > 0 &&
            sl[i].ph_names !=
                nullptr &&
            tot <=
                (size_t)PhaseKeep &&
            idx <= tot) {
            known = 1;
        }
        if (known != 0) {
            vs[m].ph_total =
                (int)tot;
            vs[m].ph_idx =
                (int)idx;
        }
        at[m] = i;
        m++;
    }
    prog_draw(vs, m);
    for (int j = 0; j < m; j++) {
        if (vs[j].done != 0) {
            sl[at[j]].pdone = 0;
        }
    }
}

// Lists failed files with logs;
// progress mode only, so piped
// TAP stays byte-identical. Empty
// logs read as (no log), same as
// logs never opened. Red on a color
// tty run, plain otherwise.
static void show_fail_logs(int n,
    char **files, Slot *sl) {
    int any = 0;
    int color = prog_color();
    for (int i = 0; i < n; i++) {
        struct stat st;
        const char *lp = nullptr;
        if (sl[i].v != SUITE_FAIL) {
            continue;
        }
        if (any == 0) {
            printf("# failures:\n");
            any = 1;
        }
        lp = sl[i].logpath;
        if (lp[0] == 0 ||
            stat(lp, &st) != 0 ||
            st.st_size == 0) {
            lp = "(no log)";
        }
        if (color != 0) {
            printf("%s# %s log %s%s\n",
                InkRed, files[i], lp,
                InkReset);
            continue;
        }
        printf("# %s log %s\n",
            files[i], lp);
    }
}

// Runs files always forked; 1 when
// any fail.
//
// Uniform model: every file runs
// in exactly one child, even at
// jobs=1, so the parent keeps sole
// ownership of its display stream
// and one code path serves serial
// and parallel runs. Each child
// gets two pipes: a stdio pipe the
// child dup2-redirects before the
// run, whose bytes the parent logs
// and phase-scans, plus a result
// pipe carrying the why text while
// the exit code carries pass/skip
// /fail. Logs persist per file in
// the run log dir. Non-tty output
// and any --tap/--json output keep
// the exact old TAP/JSON bytes.
static int run_forked(int n,
    char **files, int jobs,
    int tap_flag, int json_flag,
    const char *log_dir) {
    Slot *sl = calloc((size_t)n,
        sizeof *sl);
    char logdir[PathCap];
    const char *ld = nullptr;
    int auto_dir = 0;
    int next = 0;
    int live = 0;
    int done = 0;
    int pass = 0;
    int fail = 0;
    int skip = 0;
    int prog = 0;
    if (sl == nullptr) {
        fputs("rsuite: no mem\n",
            stderr);
        return (ExFail);
    }
    SawSig = 0;
    SigAt = 0.0;
    if (sig_watch() == 0) {
        fputs("rsuite: no signal "
            "watch\n", stderr);
    }
    if (log_dir != nullptr) {
        if (ensure_logdir(
            log_dir) != 0) {
            ld = log_dir;
        } else {
            fprintf(stderr,
                "rsuite: no log dir"
                " %s: %s\n", log_dir,
                strerror(errno));
        }
    } else if (make_logdir(logdir,
        sizeof logdir) != 0) {
        ld = logdir;
        auto_dir = 1;
    } else {
        fprintf(stderr,
            "rsuite: no log dir: "
            "%s\n",
            strerror(errno));
    }
    // Exports the run log dir for
    // forked children: the executor
    // redirects per-node snapshots
    // into it, so they outlive the
    // statedir wipe. No output, so
    // TAP and JSON bytes keep shape.
    if (ld != nullptr) {
        setenv("RSUITE_LOG_DIR", ld,
            1);
    } else {
        unsetenv("RSUITE_LOG_DIR");
    }
    for (int i = 0; i < n; i++) {
        slot_init(&sl[i]);
    }
    for (int i = 0; i < n; i++) {
        phase_load(&sl[i],
            files[i]);
    }
    prog = prog_want(tap_flag,
        json_flag);
    while (next < n || live > 0) {
        if (SawSig != 0) {
            double at = now_s();
            int sig = SIGTERM;
            if (SigAt == 0.0) {
                SigAt = at;
            }
            if (at - SigAt >=
                (double)KillGrace) {
                sig = SIGKILL;
            }
            for (int i = 0; i < n;
                i++) {
                slot_sig(&sl[i],
                    sig);
            }
        }
        while (next < n &&
            live < jobs &&
            SawSig == 0) {
            launch_one(&sl[next],
                files[next], ld);
            if (sl[next].live
                != 0) {
                live++;
            } else {
                tally(sl[next].v,
                    &pass, &fail,
                    &skip);
                done++;
            }
            next++;
        }
        if (live <= 0) {
            break;
        }
        poll_step(sl, n, &live,
            &done, &pass, &fail,
            &skip);
        if (prog != 0) {
            prog_show(sl, n);
        }
    }
    if (SawSig != 0) {
        for (int i = next; i < n;
            i++) {
            snprintf(sl[i].why,
                WhyCap,
                "interrupted");
            sl[i].v = SUITE_FAIL;
            tally(sl[i].v, &pass,
                &fail, &skip);
            done++;
        }
    }
    if (prog != 0) {
        fputc('\n', stdout);
    }
    for (int i = 0; i < n; i++) {
        show_one(json_flag, i + 1,
            files[i], sl[i].v,
            sl[i].why);
    }
    {
        double wsum = 0.0;
        long rmax = 0;
        for (int i = 0; i < n;
            i++) {
            wsum += sl[i].wall;
            if (sl[i].rsskb >
                rmax) {
                rmax =
                    sl[i].rsskb;
            }
        }
        show_sum(json_flag, pass,
            fail, skip, prog, wsum,
            rmax / 1024);
        if (json_flag == 0) {
            show_res(files, sl, n);
        }
    }
    if (prog != 0) {
        show_fail_logs(n, files,
            sl);
    }
    // Renders per-run report.html
    // once every log is final, via
    // the shared HTML path. Best
    // effort: never moves verdicts.
    if (ld != nullptr) {
        auto_html(ld, prog);
    }
    // Opens the fresh run dir for
    // reading; user --log-dir keeps
    // its own dir modes, so only the
    // auto dir walks here.
    if (auto_dir != 0 &&
        ld != nullptr) {
        fix_run(ld);
    }
    for (int i = 0; i < n; i++) {
        phase_free(&sl[i]);
    }
    free(sl);
    if (fail > 0) {
        return (ExFail);
    }
    return (ExOk);
}

// Run the run subcommand.
static int do_run_args(int argc,
    char **argv) {
    int jobs = 1;
    int tap_flag = 0;
    int json_flag = 0;
    const char *log_dir = nullptr;
    int i = 2;
    while (i < argc) {
        const char *a = argv[i];
        if (streq(a, "--tap")) {
            tap_flag = 1;
            i++;
        } else if (streq(a,
            "--json")) {
            json_flag = 1;
            i++;
        } else if (streq(a,
            "--jobs")) {
            if (i + 1 >= argc) {
                return (use_err(
                    "run needs a "
                    "value after "
                    "--jobs"));
            }
            if (!jobs_of(argv[i +
                1], &jobs)) {
                fprintf(stderr,
                    "rsuite: bad "
                    "--jobs value "
                    "%s\n",
                    argv[i + 1]);
                show_use(stderr);
                return (ExUse);
            }
            i += 2;
        } else if (is_jobs_eq(a)) {
            if (!jobs_of(a + 7,
                &jobs)) {
                fprintf(stderr,
                    "rsuite: bad "
                    "--jobs value "
                    "%s\n", a + 7);
                show_use(stderr);
                return (ExUse);
            }
            i++;
        } else if (streq(a,
            "--log-dir")) {
            if (i + 1 >= argc) {
                return (use_err(
                    "run needs a "
                    "value after "
                    "--log-dir"));
            }
            if (argv[i + 1][0] ==
                0) {
                return (use_err(
                    "run needs a "
                    "non-empty "
                    "--log-dir"));
            }
            log_dir = argv[i + 1];
            i += 2;
        } else if (is_logdir_eq(a)) {
            if (a[10] == 0) {
                return (use_err(
                    "run needs a "
                    "non-empty "
                    "--log-dir"));
            }
            log_dir = a + 10;
            i++;
        } else if (streq(a,
            "--help") ||
            streq(a, "-h")) {
            show_run_use(stdout);
            return (ExOk);
        } else if (a[0] == '-') {
            fprintf(stderr,
                "rsuite: unknown "
                "flag %s\n", a);
            show_use(stderr);
            return (ExUse);
        } else {
            break;
        }
    }
    if (tap_flag != 0 &&
        json_flag != 0) {
        return (use_err("--tap and"
            " --json conflict"));
    }
    if (i >= argc) {
        return (use_err("run needs"
            " at least one file"));
    }
    for (int k = i; k < argc;
        k++) {
        if (argv[k][0] == '-') {
            fprintf(stderr,
                "rsuite: flag %s "
                "after files; flags"
                " precede files\n",
                argv[k]);
            show_use(stderr);
            return (ExUse);
        }
    }
    return (run_forked(argc - i,
        argv + i, jobs, tap_flag,
        json_flag, log_dir));
}

// Lint files; 1 when any fail. why
// already carries path:line, so it
// prints alone, never re-prefixed.
static int do_lint(int n,
    char **files) {
    char why[WhyCap];
    int bad = 0;
    for (int i = 0; i < n; i++) {
        bool ok;
        memset(why, 0,
            sizeof why);
        ok = suite_lint_file(
            files[i], why,
            sizeof why);
        if (ok) {
            printf("%s: OK\n",
                files[i]);
            continue;
        }
        bad = ExFail;
        if (why[0] == 0) {
            fprintf(stderr, "%s: "
                "invalid\n",
                files[i]);
            continue;
        }
        fprintf(stderr, "%s\n",
            why);
    }
    return (bad);
}

// Reads a whole file; caller frees
// text. 1 on success, errno kept.
static int read_all(
    const char *path, char **out,
    size_t *nout) {
    FILE *f = fopen(path, "r");
    long n = 0;
    char *tx = nullptr;
    size_t got = 0;
    *out = nullptr;
    *nout = 0;
    if (f == nullptr) {
        return (0);
    }
    if (fseek(f, 0, SEEK_END)
        != 0) {
        fclose(f);
        return (0);
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return (0);
    }
    if (fseek(f, 0, SEEK_SET)
        != 0) {
        fclose(f);
        return (0);
    }
    tx = malloc((size_t)n + 1);
    if (tx == nullptr) {
        fclose(f);
        return (0);
    }
    got = fread(tx, 1, (size_t)n,
        f);
    fclose(f);
    if (got != (size_t)n) {
        free(tx);
        return (0);
    }
    tx[n] = 0;
    *out = tx;
    *nout = (size_t)n;
    return (1);
}

// Scalar text for key, or nullptr.
static const char *map_text(
    SuiteNode *root, const char *key) {
    SuiteNode *v = nullptr;
    if (root == nullptr) {
        return (nullptr);
    }
    v = suite_map_find(root, key);
    if (v == nullptr) {
        return (nullptr);
    }
    return (suite_scalar_str(v));
}

// Frees phase names held by a slot;
// safe on unknown or empty slots.
static void phase_free(Slot *s) {
    size_t k = 0;
    if (s == nullptr) {
        return;
    }
    if (s->ph_names == nullptr) {
        s->ph_total = 0;
        s->ph_idx = 0;
        s->ph_next = 0;
        return;
    }
    for (k = 0; k < s->ph_total;
        k++) {
        free(s->ph_names[k]);
    }
    free(s->ph_names);
    s->ph_names = nullptr;
    s->ph_total = 0;
    s->ph_idx = 0;
    s->ph_next = 0;
}

// Loads ordered phase names for path
// into the slot; unknown stays zero
// total with null names, never crash,
// never guess: unreadable, unparse,
// missing or empty phases, or any
// blank name all mean unknown.
static void phase_load(Slot *s,
    const char *path) {
    char *tx = nullptr;
    size_t tn = 0;
    SuiteDoc *doc = nullptr;
    SuiteError se;
    SuiteNode *root = nullptr;
    SuiteNode *phs = nullptr;
    size_t cnt = 0;
    char **names = nullptr;
    size_t k = 0;
    s->ph_names = nullptr;
    s->ph_total = 0;
    s->ph_idx = 0;
    s->ph_next = 0;
    if (path == nullptr) {
        return;
    }
    memset(&se, 0, sizeof se);
    if (read_all(path, &tx, &tn)
        == 0) {
        return;
    }
    if (!suite_parse(tx, tn, &doc,
        &se)) {
        free(tx);
        return;
    }
    root = doc->root;
    if (root == nullptr) {
        suite_doc_free(doc);
        free(tx);
        return;
    }
    phs = suite_map_find(root,
        "phases");
    if (phs == nullptr) {
        suite_doc_free(doc);
        free(tx);
        return;
    }
    cnt = suite_list_len(phs);
    if (cnt == 0) {
        suite_doc_free(doc);
        free(tx);
        return;
    }
    if (cnt >
        (size_t)PhaseKeep) {
        suite_doc_free(doc);
        free(tx);
        return;
    }
    names = calloc(cnt,
        sizeof *names);
    if (names == nullptr) {
        suite_doc_free(doc);
        free(tx);
        return;
    }
    for (k = 0; k < cnt; k++) {
        SuiteNode *it =
            suite_list_at(phs, k);
        const char *nm = nullptr;
        char *cp = nullptr;
        if (it != nullptr) {
            nm = map_text(it,
                "name");
        }
        if (nm == nullptr ||
            nm[0] == 0) {
            size_t j = 0;
            for (j = 0; j < k;
                j++) {
                free(names[j]);
            }
            free(names);
            suite_doc_free(doc);
            free(tx);
            return;
        }
        cp = strdup(nm);
        if (cp == nullptr) {
            size_t j = 0;
            for (j = 0; j < k;
                j++) {
                free(names[j]);
            }
            free(names);
            suite_doc_free(doc);
            free(tx);
            return;
        }
        names[k] = cp;
    }
    suite_doc_free(doc);
    free(tx);
    s->ph_names = names;
    s->ph_total = cnt;
    s->ph_idx = 0;
    s->ph_next = 0;
}

// True when s needs quoting: blank
// or holding space, quote, or slash.
static bool needs_quote(
    const char *s) {
    if (s[0] == 0) {
        return (true);
    }
    for (; *s != 0; s++) {
        if (*s == ' ' ||
            *s == '\t' ||
            *s == '"' ||
            *s == '\\') {
            return (true);
        }
    }
    return (false);
}

// Prints s raw, or double quoted
// when it holds spaces.
static void print_word(
    const char *s) {
    if (!needs_quote(s)) {
        fputs(s, stdout);
        return;
    }
    fputc('"', stdout);
    for (; *s != 0; s++) {
        if (*s == '"' ||
            *s == '\\') {
            fputc('\\', stdout);
        }
        fputc(*s, stdout);
    }
    fputc('"', stdout);
}

// Warns when key is absent; list still
// prints, but lint would reject.
static void warn_absent(
    SuiteNode *root,
    const char *path,
    const char *key) {
    if (suite_map_find(root,
        key) == nullptr) {
        fprintf(stderr,
            "%s: warning: no '%s';"
            " lint fails\n", path,
            key);
    }
}

// Prints the nodes count or dash.
static void print_nodes(
    SuiteNode *nd) {
    long long v = 0;
    const char *t = nullptr;
    if (nd == nullptr) {
        fputs("nodes=-", stdout);
        return;
    }
    if (suite_scalar_int(nd, &v)) {
        printf("nodes=%lld", v);
        return;
    }
    t = suite_scalar_str(nd);
    if (t != nullptr) {
        printf("nodes=%s", t);
        return;
    }
    fputs("nodes=-", stdout);
}

// Prints phase count and names.
static void print_phases(
    SuiteNode *phs) {
    size_t cnt = 0;
    size_t k = 0;
    if (phs != nullptr) {
        cnt = suite_list_len(
            phs);
    }
    printf("phases=%zu [", cnt);
    for (k = 0; k < cnt; k++) {
        SuiteNode *it =
            suite_list_at(phs, k);
        const char *nm = nullptr;
        if (it != nullptr) {
            nm = map_text(it,
                "name");
        }
        if (k > 0) {
            fputs(", ", stdout);
        }
        if (nm != nullptr) {
            print_word(nm);
        } else {
            fputs("-", stdout);
        }
    }
    fputc(']', stdout);
}

// Prints one file summary via the
// parse API only. False on any
// read or parse failure. Warns on
// missing keys: list success does
// not imply lint success.
static bool list_one(
    const char *path) {
    char *tx = nullptr;
    size_t tn = 0;
    SuiteDoc *doc = nullptr;
    SuiteError se;
    SuiteNode *root = nullptr;
    const char *nm = nullptr;
    const char *sut = nullptr;
    memset(&se, 0, sizeof se);
    if (read_all(path, &tx, &tn)
        == 0) {
        fprintf(stderr, "%s: %s\n",
            path,
            strerror(errno));
        return (false);
    }
    if (!suite_parse(tx, tn, &doc,
        &se)) {
        fprintf(stderr,
            "%s:%zu:%zu: %s\n",
            path, se.line,
            se.col, se.message);
        free(tx);
        return (false);
    }
    root = doc->root;
    warn_absent(root, path,
        "name");
    warn_absent(root, path,
        "version");
    warn_absent(root, path, "sut");
    warn_absent(root, path,
        "nodes");
    warn_absent(root, path,
        "timeout_s");
    warn_absent(root, path,
        "phases");
    nm = map_text(root, "name");
    sut = map_text(root, "sut");
    printf("%s: ", path);
    fputs("name=", stdout);
    if (nm != nullptr) {
        print_word(nm);
    } else {
        fputc('-', stdout);
    }
    fputc(' ', stdout);
    fputs("sut=", stdout);
    if (sut != nullptr) {
        print_word(sut);
    } else {
        fputc('-', stdout);
    }
    fputc(' ', stdout);
    print_nodes(suite_map_find(
        root, "nodes"));
    fputc(' ', stdout);
    print_phases(suite_map_find(
        root, "phases"));
    fputc('\n', stdout);
    suite_doc_free(doc);
    free(tx);
    return (true);
}

// Lists suite files; 1 when any
// fail to parse. Needs no privs.
static int do_list(int n,
    char **files) {
    int bad = 0;
    for (int i = 0; i < n; i++) {
        if (!list_one(files[i])) {
            bad = ExFail;
        }
    }
    return (bad);
}

// True when path allows mode.
static int acc_ok(const char *path,
    int mode) {
    if (access(path, mode)
        == 0) {
        return (1);
    }
    return (0);
}

// Runs cmd via sh, stdout and
// stderr nulled. 1 when it exits
// zero, 0 when missing/failing,
// -1 on fork or wait error.
static int probe_ok(const char *cmd) {
    pid_t c = fork();
    int st = 0;
    if (c < 0) {
        return (-1);
    }
    if (c == 0) {
        int fd = open("/dev/null",
            O_WRONLY);
        if (fd >= 0) {
            dup2(fd,
                STDOUT_FILENO);
            dup2(fd,
                STDERR_FILENO);
            if (fd >
                STDERR_FILENO) {
                close(fd);
            }
        }
        execlp("sh", "sh", "-c",
            cmd, (char *)nullptr);
        _exit(NoShell);
    }
    for (;;) {
        pid_t w = waitpid(c,
            &st, 0);
        if (w >= 0) {
            break;
        }
        if (errno != EINTR) {
            return (-1);
        }
    }
    if (!WIFEXITED(st)) {
        return (0);
    }
    if (WEXITSTATUS(st) == 0) {
        return (1);
    }
    return (0);
}

// Prints one check line: OK when
// st is 1, MISSING when 0, ERROR
// on internal probe failure.
static void doc_line(
    const char *what, int st,
    const char *hint) {
    printf("%s: ", what);
    if (st > 0) {
        printf("OK\n");
        return;
    }
    if (st == 0) {
        printf("MISSING %s\n",
            hint);
        return;
    }
    printf("ERROR %s\n", hint);
}

// Resolves a sibling of the bin dir:
// exe dir plus leaf, or the CWD
// ./bin fallback when /proc/self
// /exe readlink fails.
static void exe_sib(char *dst,
    size_t n, const char *leaf) {
    char exe[PathCap];
    char *s = nullptr;
    size_t dn = 0;
    size_t ln = 0;
    ssize_t r = readlink(
        "/proc/self/exe", exe,
        sizeof exe - 1);
    if (r <= 0) {
        snprintf(dst, n,
            "./bin/%s", leaf);
        return;
    }
    exe[r] = 0;
    s = strrchr(exe, '/');
    if (s == nullptr) {
        snprintf(dst, n,
            "./bin/%s", leaf);
        return;
    }
    *s = 0;
    dn = strlen(exe);
    ln = strlen(leaf);
    if (dn + 1 + ln + 1 > n) {
        snprintf(dst, n,
            "./bin/%s", leaf);
        return;
    }
    memcpy(dst, exe, dn);
    dst[dn] = '/';
    memcpy(dst + dn + 1, leaf,
        ln);
    dst[dn + 1 + ln] = 0;
}

// Runs doctor checks and prints one
// OK/MISSING line each. Always 0
// with the report; 1 only when a
// probe itself errors, since
// missing deps inform, not fail.
// Lock path mirrors sim/sim.c
// RUN_LOCK_PATH; ids probe via
// sim_run_probe.
static int do_doctor(void) {
    char conc[PathCap];
    char weld[PathCap];
    int err = 0;
    int r = 0;
    uid_t id = geteuid();
    exe_sib(conc, sizeof conc,
        "concord");
    exe_sib(weld, sizeof weld,
        "weld");
    doc_line("euid root",
        id == 0 ? 1 : 0,
        "(not root)");
    doc_line("/dev/net/tun",
        acc_ok("/dev/net/tun",
            R_OK | W_OK),
        "(no tun access)");
    r = probe_ok("ip netns list");
    if (r < 0) {
        err = 1;
    }
    doc_line("netns list; create"
        " needs root", r,
        "(ip missing)");
    doc_line(conc,
        acc_ok(conc, X_OK),
        "(not built)");
    doc_line(weld,
        acc_ok(weld, X_OK),
        "(not built)");
    r = probe_ok("docker info");
    if (r < 0) {
        err = 1;
    }
    doc_line("docker daemon", r,
        "(daemon down)");
    doc_line("run lock",
        acc_ok("/var/tmp/"
            "resonance-runs.lock",
            R_OK | W_OK),
        "(missing or denied)");
    doc_line("run ids",
        sim_run_probe(),
        "(ids exhausted)");
    doc_line("lock dir /var/tmp",
        acc_ok("/var/tmp",
            W_OK | X_OK),
        "(not writable)");
    if (err != 0) {
        return (ExFail);
    }
    return (ExOk);
}

// Parses a strict u64; false on bad
// text, trailing junk, or overflow.
static bool nem_u64(const char *s,
    unsigned long long *out) {
    char *end = nullptr;
    unsigned long long v = 0;
    if (s[0] == 0) {
        return (false);
    }
    if (isspace(
        (unsigned char)s[0]) != 0) {
        return (false);
    }
    errno = 0;
    v = strtoull(s, &end, 10);
    if (end == s) {
        return (false);
    }
    if (*end != 0) {
        return (false);
    }
    if (errno != 0) {
        return (false);
    }
    *out = v;
    return (true);
}

// Parses a strict long; false on bad
// text, trailing junk, or overflow.
static bool nem_num(const char *s,
    long *out) {
    char *end = nullptr;
    long v = 0;
    if (s[0] == 0) {
        return (false);
    }
    if (isspace(
        (unsigned char)s[0]) != 0) {
        return (false);
    }
    errno = 0;
    v = strtol(s, &end, 10);
    if (end == s) {
        return (false);
    }
    if (*end != 0) {
        return (false);
    }
    if (errno != 0) {
        return (false);
    }
    *out = v;
    return (true);
}

// True for --opt and --opt= forms.
static bool is_opt(const char *a,
    const char *opt) {
    size_t n = strlen(opt);
    if (strcmp(a, opt) == 0) {
        return (true);
    }
    if (strncmp(a, opt, n) == 0 &&
        a[n] == '=') {
        return (true);
    }
    return (false);
}

// Takes a value flag in --opt V or
// --opt=V form, moving i past both
// words. Call only on a match.
static const char *opt_val(
    const char *a, const char *opt,
    char **argv, int argc, int *i) {
    size_t n = strlen(opt);
    if (strcmp(a, opt) == 0) {
        if (*i + 1 >= argc) {
            return ("");
        }
        (*i)++;
        return (argv[*i]);
    }
    return (a + n + 1);
}

// Shows nemesis help only.
static void show_nemesis_use(
    FILE *out) {
    fputs("usage: rsuite nemesis",
        out);
    fputs(" --seed N --nodes N",
        out);
    fputs(" --phases N", out);
    fputs(" --sut concord|weld",
        out);
    fputs(" [--max-drop N]", out);
    fputs(" [--max-kill N]", out);
    fputs(" [--max-restart N]",
        out);
    fputs(" [--no-submit]", out);
    fputs(" [--no-stop] -o FILE\n",
        out);
    fputs("\n", out);
    show_exits(out);
    fputs("writes a generated YAML",
        out);
    fputs(" and prints its path.",
        out);
    fputs(" =N spellings work for",
        out);
    fputs(" every value flag.",
        out);
    fputs(" Defaults: seed 1,",
        out);
    fputs(" nodes 3, phases 3,",
        out);
    fputs(" sut concord, max-drop",
        out);
    fputs(" 1, max-kill 1,",
        out);
    fputs(" max-restart 1, submit",
        out);
    fputs(" and stop allowed.",
        out);
    fputs(" nodes and phases need 1",
        out);
    fputs(" or more; max flags need",
        out);
    fputs(" 0 or more; sut is",
        out);
    fputs(" concord or weld; -o is",
        out);
    fputs(" required.\n", out);
}

// Runs nemesis: fills a NemProfile
// from flags, calls the generator,
// prints the path. 0 on success, 1
// when generation fails, 2 on bad
// usage.
static int do_nemesis(int argc,
    char **argv) {
    NemProfile p;
    unsigned long long seed = 1;
    long nodes = 3;
    long phases = 3;
    long drop = 1;
    long kill = 1;
    long restart = 1;
    const char *sut = "concord";
    const char *out = nullptr;
    int allow_submit = 1;
    int allow_stop = 1;
    int i = 2;
    memset(&p, 0, sizeof p);
    while (i < argc) {
        const char *a = argv[i];
        const char *v = nullptr;
        unsigned long long t = 0;
        long n2 = 0;
        if (streq(a, "--help") ||
            streq(a, "-h")) {
            show_nemesis_use(
                stdout);
            return (ExOk);
        }
        if (streq(a,
            "--no-submit")) {
            allow_submit = 0;
            i++;
            continue;
        }
        if (streq(a, "--no-stop")) {
            allow_stop = 0;
            i++;
            continue;
        }
        if (is_opt(a, "--seed")) {
            if (streq(a,
                "--seed") &&
                i + 1 >= argc) {
                return (use_err(
                    "nemesis needs a"
                    " value after "
                    "--seed"));
            }
            v = opt_val(a,
                "--seed", argv,
                argc, &i);
            if (!nem_u64(v, &t)) {
                fprintf(stderr,
                    "rsuite: bad "
                    "--seed value "
                    "%s\n", v);
                show_use(stderr);
                return (ExUse);
            }
            seed = t;
            i++;
            continue;
        }
        if (is_opt(a, "--nodes")) {
            if (streq(a,
                "--nodes") &&
                i + 1 >= argc) {
                return (use_err(
                    "nemesis needs a"
                    " value after "
                    "--nodes"));
            }
            v = opt_val(a,
                "--nodes", argv,
                argc, &i);
            if (!nem_num(v, &n2)) {
                fprintf(stderr,
                    "rsuite: bad "
                    "--nodes value "
                    "%s\n", v);
                show_use(stderr);
                return (ExUse);
            }
            nodes = n2;
            i++;
            continue;
        }
        if (is_opt(a, "--phases")) {
            if (streq(a,
                "--phases") &&
                i + 1 >= argc) {
                return (use_err(
                    "nemesis needs a"
                    " value after "
                    "--phases"));
            }
            v = opt_val(a,
                "--phases", argv,
                argc, &i);
            if (!nem_num(v, &n2)) {
                fprintf(stderr,
                    "rsuite: bad "
                    "--phases value "
                    "%s\n", v);
                show_use(stderr);
                return (ExUse);
            }
            phases = n2;
            i++;
            continue;
        }
        if (is_opt(a,
            "--max-drop")) {
            if (streq(a,
                "--max-drop") &&
                i + 1 >= argc) {
                return (use_err(
                    "nemesis needs a"
                    " value after "
                    "--max-drop"));
            }
            v = opt_val(a,
                "--max-drop", argv,
                argc, &i);
            if (!nem_num(v, &n2)) {
                fprintf(stderr,
                    "rsuite: bad "
                    "--max-drop "
                    "value %s\n", v);
                show_use(stderr);
                return (ExUse);
            }
            drop = n2;
            i++;
            continue;
        }
        if (is_opt(a,
            "--max-kill")) {
            if (streq(a,
                "--max-kill") &&
                i + 1 >= argc) {
                return (use_err(
                    "nemesis needs a"
                    " value after "
                    "--max-kill"));
            }
            v = opt_val(a,
                "--max-kill", argv,
                argc, &i);
            if (!nem_num(v, &n2)) {
                fprintf(stderr,
                    "rsuite: bad "
                    "--max-kill "
                    "value %s\n", v);
                show_use(stderr);
                return (ExUse);
            }
            kill = n2;
            i++;
            continue;
        }
        if (is_opt(a,
            "--max-restart")) {
            if (streq(a,
                "--max-restart") &&
                i + 1 >= argc) {
                return (use_err(
                    "nemesis needs a"
                    " value after "
                    "--max-restart"));
            }
            v = opt_val(a,
                "--max-restart",
                argv, argc, &i);
            if (!nem_num(v, &n2)) {
                fprintf(stderr,
                    "rsuite: bad "
                    "--max-restart "
                    "value %s\n", v);
                show_use(stderr);
                return (ExUse);
            }
            restart = n2;
            i++;
            continue;
        }
        if (is_opt(a, "--sut")) {
            if (streq(a, "--sut") &&
                i + 1 >= argc) {
                return (use_err(
                    "nemesis needs a"
                    " value after "
                    "--sut"));
            }
            v = opt_val(a,
                "--sut", argv,
                argc, &i);
            if (!streq(v,
                "concord") &&
                !streq(v, "weld")) {
                fprintf(stderr,
                    "rsuite: bad "
                    "--sut value "
                    "%s\n", v);
                show_use(stderr);
                return (ExUse);
            }
            sut = v;
            i++;
            continue;
        }
        if (streq(a, "-o")) {
            if (i + 1 >= argc) {
                return (use_err(
                    "nemesis needs a"
                    " value after "
                    "-o"));
            }
            out = argv[i + 1];
            i += 2;
            continue;
        }
        if (strncmp(a, "-o", 2) ==
            0 && a[2] != 0) {
            out = a + 2;
            i++;
            continue;
        }
        fprintf(stderr,
            "rsuite: unexpected "
            "arg %s\n", a);
        show_use(stderr);
        return (ExUse);
    }
    if (out == nullptr ||
        out[0] == 0) {
        return (use_err("nemesis "
            "needs -o FILE"));
    }
    if (nodes < 1) {
        return (use_err("nemesis "
            "nodes needs 1 or "
            "more"));
    }
    if (phases < 1) {
        return (use_err("nemesis "
            "phases needs 1 or "
            "more"));
    }
    if (drop < 0 || kill < 0 ||
        restart < 0) {
        return (use_err("nemesis "
            "max flags need 0 or "
            "more"));
    }
    p.seed = (uint64_t)seed;
    p.nodes = (int)nodes;
    p.nphases = (int)phases;
    p.max_drop = (int)drop;
    p.max_kill = (int)kill;
    p.max_restart = (int)restart;
    p.allow_submit = allow_submit;
    p.allow_stop = allow_stop;
    if (streq(sut, "weld")) {
        memcpy(p.sut, "weld", 5);
    } else {
        memcpy(p.sut, "concord",
            8);
    }
    if (!nemesis_gen(&p, out)) {
        fprintf(stderr, "rsuite: "
            "nemesis generation "
            "failed\n");
        return (ExFail);
    }
    printf("%s\n", out);
    return (ExOk);
}

// Oracle state: call count plus
// the bound trip flag.
typedef struct {
    int n;
    int tripped;
} OracleSt;

// Runs path in a forked child via
// run_file_code, the entry run
// children share; maps the exit to
// oracle 0 pass, 1 fail, 2 skip.
// Child stdio goes to null so only
// minimize lines print.
static int oracle_run(
    const char *path) {
    char why[WhyCap];
    pid_t c = -1;
    int code = 1;
    c = fork();
    if (c < 0) {
        return (1);
    }
    if (c == 0) {
        int rc = ExFail;
        int fd = -1;
        sig_def();
        fd = open("/dev/null",
            O_WRONLY);
        if (fd >= 0) {
            dup2(fd,
                STDOUT_FILENO);
            dup2(fd,
                STDERR_FILENO);
            if (fd >
                STDERR_FILENO) {
                close(fd);
            }
        }
        rc = run_file_code(path,
            why, sizeof why);
        _exit(rc);
    }
    for (;;) {
        int st = 0;
        pid_t w = waitpid(c, &st,
            0);
        if (w >= 0) {
            if (!WIFEXITED(st)) {
                code = 1;
            } else if (
                WEXITSTATUS(st) ==
                ExOk) {
                code = 0;
            } else if (
                WEXITSTATUS(st) ==
                SkipCode) {
                code = 2;
            } else {
                code = 1;
            }
            break;
        }
        if (errno != EINTR) {
            code = 1;
            break;
        }
    }
    return (code);
}

// Minimizer oracle: counts calls,
// fails loudly past OracleCap, else
// runs the candidate. 0 pass, 1
// fail, 2 skip per the contract.
static int oracle_cb(
    const char *yamlpath, void *ud) {
    OracleSt *st = ud;
    if (st == nullptr) {
        return (1);
    }
    if (yamlpath == nullptr) {
        return (1);
    }
    if (st->n >= OracleCap) {
        if (st->tripped == 0) {
            st->tripped = 1;
            fprintf(stderr,
                "rsuite: minimize "
                "oracle bound %d "
                "exceeded\n",
                OracleCap);
        }
        return (1);
    }
    st->n++;
    return (oracle_run(yamlpath));
}

// Counts phases and do steps in path
// via the parse API; false when the
// file will not parse.
static bool count_ps(
    const char *path, size_t *nph,
    size_t *nst) {
    char *tx = nullptr;
    size_t tn = 0;
    SuiteDoc *doc = nullptr;
    SuiteError se;
    SuiteNode *root = nullptr;
    SuiteNode *phs = nullptr;
    size_t cnt = 0;
    size_t steps = 0;
    size_t k = 0;
    memset(&se, 0, sizeof se);
    if (read_all(path, &tx, &tn)
        == 0) {
        return (false);
    }
    if (!suite_parse(tx, tn, &doc,
        &se)) {
        free(tx);
        return (false);
    }
    root = doc->root;
    if (root != nullptr) {
        phs = suite_map_find(root,
            "phases");
    }
    if (phs != nullptr) {
        cnt = suite_list_len(phs);
    }
    for (k = 0; k < cnt; k++) {
        SuiteNode *it =
            suite_list_at(phs, k);
        SuiteNode *dox = nullptr;
        if (it == nullptr) {
            continue;
        }
        dox = suite_map_find(it,
            "do");
        if (dox != nullptr) {
            steps += suite_list_len(
                dox);
        }
    }
    suite_doc_free(doc);
    free(tx);
    *nph = cnt;
    *nst = steps;
    return (true);
}

// Shows minimize help only.
static void show_minimize_use(
    FILE *out) {
    fputs("usage: rsuite minimize",
        out);
    fputs(" FILE [-o OUT]\n", out);
    fputs("\n", out);
    show_exits(out);
    fputs("shrinks a failing YAML",
        out);
    fputs(" and prints the shrunken",
        out);
    fputs(" path plus phase and",
        out);
    fputs(" step counts before and",
        out);
    fputs(" after. Default OUT",
        out);
    fputs(" swaps a .yaml tail for",
        out);
    fputs(" .min.yaml, else appends",
        out);
    fputs(" .min.yaml. Each oracle",
        out);
    fputs(" call runs the candidate",
        out);
    fputs(" in a forked child",
        out);
    fputs(" through the entry run",
        out);
    fputs(" children share. At most",
        out);
    fputs(" 256 oracle calls; past",
        out);
    fputs(" it minimize exits 1.",
        out);
    fputs(" Steps count do items",
        out);
    fputs(" per phase. Needs root",
        out);
    fputs(" like run.\n", out);
}

// Runs minimize: checks the input
// parses, loops via the minimizer
// with a forked oracle, prints the
// path plus counts. 0 on success,
// 1 on failure or bound trip, 2 on
// bad usage.
static int do_minimize(int argc,
    char **argv) {
    const char *in = nullptr;
    const char *out = nullptr;
    char dflt[PathCap];
    OracleSt st;
    size_t bph = 0;
    size_t bst = 0;
    size_t aph = 0;
    size_t ast = 0;
    int i = 2;
    st.n = 0;
    st.tripped = 0;
    while (i < argc) {
        const char *a = argv[i];
        if (streq(a, "--help") ||
            streq(a, "-h")) {
            show_minimize_use(
                stdout);
            return (ExOk);
        }
        if (streq(a, "-o")) {
            if (i + 1 >= argc) {
                return (use_err(
                    "minimize needs "
                    "a value after "
                    "-o"));
            }
            out = argv[i + 1];
            i += 2;
            continue;
        }
        if (strncmp(a, "-o", 2) ==
            0 && a[2] != 0) {
            out = a + 2;
            i++;
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr,
                "rsuite: unknown "
                "flag %s\n", a);
            show_use(stderr);
            return (ExUse);
        }
        if (in != nullptr) {
            return (use_err(
                "minimize takes one"
                " file"));
        }
        in = a;
        i++;
    }
    if (in == nullptr) {
        return (use_err("minimize "
            "needs a file"));
    }
    if (out == nullptr) {
        size_t n = strlen(in);
        if (n + 10 >=
            (size_t)PathCap) {
            return (use_err(
                "minimize path too"
                " long"));
        }
        memcpy(dflt, in, n + 1);
        if (n > 5 && strcmp(
            dflt + n - 5, ".yaml")
            == 0) {
            memcpy(dflt + n - 5,
                ".min.yaml", 10);
        } else {
            memcpy(dflt + n,
                ".min.yaml", 10);
        }
        out = dflt;
    }
    if (out[0] == 0) {
        return (use_err("minimize "
            "needs a non-empty "
            "-o"));
    }
    if (!count_ps(in, &bph,
        &bst)) {
        fprintf(stderr, "rsuite: "
            "minimize cannot parse "
            "%s\n", in);
        return (ExFail);
    }
    if (!nemesis_minimize(in,
        oracle_cb, &st, out)) {
        if (st.tripped != 0) {
            fprintf(stderr,
                "rsuite: minimize "
                "failed: oracle "
                "bound %d "
                "exceeded\n",
                OracleCap);
        } else {
            fprintf(stderr,
                "rsuite: minimize "
                "failed\n");
        }
        return (ExFail);
    }
    if (st.tripped != 0) {
        fprintf(stderr,
            "rsuite: minimize "
            "failed: oracle bound "
            "%d exceeded\n",
            OracleCap);
        return (ExFail);
    }
    if (!count_ps(out, &aph,
        &ast)) {
        fprintf(stderr, "rsuite: "
            "minimize output "
            "unreadable %s\n",
            out);
        return (ExFail);
    }
    printf("%s\n", out);
    printf("phases: %zu -> %zu\n",
        bph, aph);
    printf("steps: %zu -> %zu\n",
        bst, ast);
    return (ExOk);
}

// One parsed suite: ev line; seq
// keeps the sort stable.
typedef struct {
    unsigned long long t;
    unsigned long long seq;
    int ph;
    int has_ph;
    char k[KindCap];
    char msg[MsgCap];
} Ev;

// Hex digit value; -1 when bad.
static int ev_hexval(char c) {
    if (c >= '0' && c <= '9') {
        return (c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (c - 'A' + 10);
    }
    return (-1);
}

// Surrogate and plane bounds for
// \u decoding; pairs make astral.
static constexpr int SurHi0 =
    0xD800;
static constexpr int SurHi1 =
    0xDBFF;
static constexpr int SurLo0 =
    0xDC00;
static constexpr int SurLo1 =
    0xDFFF;
static constexpr int UniBase =
    0x10000;
static constexpr int UniSh = 10;

// Cap on hex digits in \uXXXX.
static constexpr int EvHex = 4;

// Parses 4 hex at s into cp;
// false on short or bad input.
static bool ev_hex4(const char *s,
    unsigned int *cp) {
    unsigned int v = 0;
    int k = 0;
    if (s == nullptr) {
        return (false);
    }
    if (cp == nullptr) {
        return (false);
    }
    for (k = 0; k < EvHex; k++) {
        int d = 0;
        if (s[k] == 0) {
            return (false);
        }
        d = ev_hexval(s[k]);
        if (d < 0) {
            return (false);
        }
        v = v * 16u +
            (unsigned int)d;
    }
    *cp = v;
    return (true);
}

// Appends cp as UTF-8; drops past
// the cap, never overruns.
static void ev_utf8(char *dst,
    size_t n, size_t *k,
    unsigned int cp) {
    char tmp[4];
    size_t m = 0;
    size_t i = 0;
    if (dst == nullptr) {
        return;
    }
    if (k == nullptr) {
        return;
    }
    if (cp < 128u) {
        tmp[0] = (char)cp;
        m = 1;
    } else if (cp < 2048u) {
        tmp[0] = (char)(192u |
            (cp >> 6u));
        tmp[1] = (char)(128u |
            (cp & 63u));
        m = 2;
    } else if (cp < 65536u) {
        tmp[0] = (char)(224u |
            (cp >> 12u));
        tmp[1] = (char)(128u |
            ((cp >> 6u) & 63u));
        tmp[2] = (char)(128u |
            (cp & 63u));
        m = 3;
    } else {
        tmp[0] = (char)(240u |
            (cp >> 18u));
        tmp[1] = (char)(128u |
            ((cp >> 12u) & 63u));
        tmp[2] = (char)(128u |
            ((cp >> 6u) & 63u));
        tmp[3] = (char)(128u |
            (cp & 63u));
        m = 4;
    }
    for (i = 0; i < m; i++) {
        if (*k + 1 < n) {
            dst[*k] = tmp[i];
            (*k)++;
        }
    }
}

// Copies m bytes at s into dst;
// drops past cap, never overruns.
static void ev_lit(char *dst,
    size_t n, size_t *k,
    const char *s, size_t m) {
    size_t i = 0;
    if (dst == nullptr) {
        return;
    }
    if (k == nullptr) {
        return;
    }
    if (s == nullptr) {
        return;
    }
    for (i = 0; i < m; i++) {
        if (*k + 1 < n) {
            dst[*k] = s[i];
            (*k)++;
        }
    }
}

// Copies a quoted token at *pp past
// the opening quote into dst, unesc
// aping the producer alphabet: \"
// \\ \n \r \t and \uXXXX as UTF-8
// with surrogate pairs; malformed
// stays literal, never overruns.
// *pp ends past the close quote.
// False when never closes.
static bool ev_quoted(
    const char **pp, char *dst,
    size_t n) {
    const char *p = nullptr;
    size_t k = 0;
    if (pp == nullptr) {
        return (false);
    }
    if (dst == nullptr) {
        return (false);
    }
    if (n == 0) {
        return (false);
    }
    p = *pp;
    if (p == nullptr) {
        return (false);
    }
    if (p[0] != '"') {
        return (false);
    }
    p++;
    while (p[0] != 0 &&
        p[0] != '"') {
        unsigned int hi = 0;
        unsigned int lo = 0;
        if (p[0] != '\\') {
            if (k + 1 < n) {
                dst[k++] = p[0];
            }
            p++;
            continue;
        }
        if (p[1] == 0) {
            if (k + 1 < n) {
                dst[k++] = '\\';
            }
            p++;
            continue;
        }
        if (p[1] == '"' ||
            p[1] == '\\') {
            if (k + 1 < n) {
                dst[k++] = p[1];
            }
            p += 2;
            continue;
        }
        if (p[1] == 'n') {
            if (k + 1 < n) {
                dst[k++] = '\n';
            }
            p += 2;
            continue;
        }
        if (p[1] == 'r') {
            if (k + 1 < n) {
                dst[k++] = '\r';
            }
            p += 2;
            continue;
        }
        if (p[1] == 't') {
            if (k + 1 < n) {
                dst[k++] = '\t';
            }
            p += 2;
            continue;
        }
        if (p[1] != 'u') {
            if (k + 1 < n) {
                dst[k++] = '\\';
            }
            p++;
            continue;
        }
        if (!ev_hex4(p + 2, &hi) ||
            hi == 0) {
            if (k + 1 < n) {
                dst[k++] = '\\';
            }
            p++;
            continue;
        }
        if (hi >=
            (unsigned int)SurHi0 &&
            hi <=
            (unsigned int)SurHi1) {
            if (p[6] == '\\' &&
                p[7] == 'u' &&
                ev_hex4(p + 8,
                &lo) &&
                lo >=
                (unsigned int)SurLo0 &&
                lo <=
                (unsigned int)SurLo1) {
                unsigned int cp =
                    (unsigned int)
                    UniBase +
                    ((hi -
                    (unsigned int)
                    SurHi0) <<
                    (unsigned int)
                    UniSh) +
                    (lo -
                    (unsigned int)
                    SurLo0);
                ev_utf8(dst, n, &k,
                    cp);
                p += 12;
                continue;
            }
            ev_lit(dst, n, &k, p,
                6);
            p += 6;
            continue;
        }
        if (hi >=
            (unsigned int)SurLo0 &&
            hi <=
            (unsigned int)SurLo1) {
            ev_lit(dst, n, &k, p,
                6);
            p += 6;
            continue;
        }
        ev_utf8(dst, n, &k, hi);
        p += 6;
    }
    if (p[0] != '"') {
        if (k < n) {
            dst[k] = 0;
        } else {
            dst[n - 1] = 0;
        }
        return (false);
    }
    dst[k] = 0;
    *pp = p + 1;
    return (true);
}

// Parses one suite: ev line into e;
// malformed lines read as skip,
// never a crash. False means skip.
static bool ev_parse(
    const char *line, Ev *e,
    unsigned long long seq) {
    const char *q = nullptr;
    char *end = nullptr;
    memset(e, 0, sizeof *e);
    e->seq = seq;
    e->ph = -1;
    if (strncmp(line, "suite: ev ",
        10) != 0) {
        return (false);
    }
    q = strstr(line, "\"t\":");
    if (q == nullptr) {
        return (false);
    }
    e->t = strtoull(q + 4, &end,
        10);
    if (end == q + 4) {
        return (false);
    }
    q = strstr(line, "\"k\":");
    if (q == nullptr) {
        return (false);
    }
    q += 4;
    while (*q == ' ' ||
        *q == '\t') {
        q++;
    }
    if (!ev_quoted(&q, e->k,
        sizeof e->k)) {
        return (false);
    }
    if (e->k[0] == 0) {
        return (false);
    }
    q = strstr(line, "\"ph\":");
    if (q != nullptr) {
        long v = strtol(q + 5,
            &end, 10);
        if (end != q + 5) {
            e->ph = (int)v;
            e->has_ph = 1;
        }
    }
    q = strstr(line, "\"msg\":");
    if (q != nullptr) {
        const char *m = q + 6;
        while (*m == ' ' ||
            *m == '\t') {
            m++;
        }
        ev_quoted(&m, e->msg,
            sizeof e->msg);
    }
    return (true);
}

// True for timeline kinds: phase,
// step, marker, and cond.
static bool is_timeline(
    const char *k) {
    if (streq(k, "phase")) {
        return (true);
    }
    if (streq(k, "step")) {
        return (true);
    }
    if (streq(k, "marker")) {
        return (true);
    }
    if (streq(k, "cond")) {
        return (true);
    }
    return (false);
}

// Orders events by time, stable on
// input order.
static int ev_cmp(const void *a,
    const void *b) {
    const Ev *x = a;
    const Ev *y = b;
    if (x->t < y->t) {
        return (-1);
    }
    if (x->t > y->t) {
        return (1);
    }
    if (x->seq < y->seq) {
        return (-1);
    }
    if (x->seq > y->seq) {
        return (1);
    }
    return (0);
}

// True when s ends in suf.
static bool ends_with(const char *s,
    const char *suf) {
    size_t n = strlen(s);
    size_t m = strlen(suf);
    if (m > n) {
        return (false);
    }
    return (strcmp(s + n - m,
        suf) == 0);
}

// Fallback title: dots to spaces,
// first letter capped.
static void pretty_title(
    const char *k, char *out,
    size_t n) {
    size_t i = 0;
    size_t o = 0;
    if (n == 0) {
        return;
    }
    for (i = 0; k[i] != 0 &&
        o + 1 < n; i++) {
        char c = k[i];
        if (c == '.' || c == '_') {
            c = ' ';
        }
        out[o++] = c;
    }
    out[o] = 0;
    for (i = 0; out[i] != 0;
        i++) {
        if (out[i] >= 'a' &&
            out[i] <= 'z') {
            out[i] = (char)
                (out[i] - 32);
            break;
        }
        if ((out[i] >= 'A' &&
            out[i] <= 'Z') ||
            (out[i] >= '0' &&
            out[i] <= '9')) {
            break;
        }
    }
}

// Display title for a metric key:
// known shapes get human titles
// with full-word units; all else
// prettifies dots to spaces.
static void metric_title(
    const char *k, char *out,
    size_t n) {
    if (strncmp(k, "flow.", 5)
        == 0) {
        const char *p = k + 5;
        char *end = nullptr;
        long a = 0;
        long b = 0;
        const char *suf = nullptr;
        const char *wp = nullptr;
        a = strtol(p, &end, 10);
        if (end != p && a >= 0 &&
            strncmp(end,
            ".to.", 4) == 0) {
            p = end + 4;
            b = strtol(p, &end,
                10);
            if (end != p &&
                b >= 0 &&
                end[0] == '.') {
                suf = end + 1;
                wp = nullptr;
                if (streq(suf,
                    "pkts")) {
                    wp = "packets";
                } else if (streq(
                    suf, "bytes")) {
                    wp = "bytes";
                } else if (streq(
                    suf,
                    "radio_drops")) {
                    wp =
                    "radio drops";
                } else if (streq(
                    suf,
                    "policy_drops")) {
                    wp =
                    "policy drops";
                }
                if (wp != nullptr) {
                    snprintf(out, n,
                        "Flow %ld to "
                        "%ld, TUN %s",
                        a, b, wp);
                    return;
                }
            }
        }
        if (streq(k,
            "flow.truncated")) {
            snprintf(out, n, "%s",
                "Flow truncated");
            return;
        }
    }
    if (strncmp(k, "res.node", 8)
        == 0) {
        const char *p = k + 8;
        char *end = nullptr;
        long id = 0;
        const char *rest = nullptr;
        id = strtol(p, &end, 10);
        if (end != p && id >= 0 &&
            end[0] == '.') {
            rest = end + 1;
            if (streq(rest,
                "peakrss.mb")) {
                snprintf(out, n,
                    "Node %ld peak "
                    "RAM, megabytes",
                    id);
                return;
            }
            if (streq(rest,
                "samples")) {
                snprintf(out, n,
                    "Node %ld "
                    "samples, "
                    "count", id);
                return;
            }
            if (streq(rest,
                "cpu.utime")) {
                snprintf(out, n,
                    "Node %ld CPU "
                    "user, ticks",
                    id);
                return;
            }
            if (streq(rest,
                "cpu.stime")) {
                snprintf(out, n,
                    "Node %ld CPU "
                    "system, ticks",
                    id);
                return;
            }
            if (streq(rest,
                "rss.bytes")) {
                snprintf(out, n,
                    "Node %ld RAM "
                    "RSS, bytes",
                    id);
                return;
            }
            if (streq(rest,
                "rchar")) {
                snprintf(out, n,
                    "Node %ld "
                    "process read, "
                    "bytes (disk "
                    "plus net)",
                    id);
                return;
            }
            if (streq(rest,
                "wchar")) {
                snprintf(out, n,
                    "Node %ld "
                    "process "
                    "written, bytes "
                    "(disk plus "
                    "net)", id);
                return;
            }
        }
    }
    if (strncmp(k,
        "profile.node", 12) == 0) {
        const char *p = k + 12;
        char *end = nullptr;
        long id = 0;
        id = strtol(p, &end, 10);
        if (end != p && id >= 0 &&
            streq(end, ".ok")) {
            snprintf(out, n,
                "Node %ld profile "
                "snapshot", id);
            return;
        }
    }
    if (streq(k, "maxrss.mb")) {
        snprintf(out, n, "%s",
            "Harness peak RAM, "
            "megabytes");
        return;
    }
    if (strncmp(k, "phase.", 6)
        == 0) {
        const char *p = k + 6;
        char *end = nullptr;
        long id = 0;
        id = strtol(p, &end, 10);
        if (end != p && id >= 0 &&
            streq(end, ".ms")) {
            snprintf(out, n,
                "Phase %ld "
                "duration, "
                "milliseconds", id);
            return;
        }
    }
    if (strncmp(k, "present.", 8)
        == 0 && ends_with(k,
        ".ms")) {
        size_t kl = strlen(k);
        size_t ml = 0;
        char mid[64];
        if (kl > 11) {
            ml = kl - 8 - 3;
            if (ml >=
                sizeof mid) {
                ml = sizeof mid -
                    1;
            }
            memcpy(mid, k + 8,
                ml);
            mid[ml] = 0;
            snprintf(out, n,
                "Present %s, "
                "milliseconds",
                mid);
            return;
        }
    }
    // Delta series title the base
    // plus a delta mark; suffix
    // match only, never a list.
    if (ends_with(k, ".delta")) {
        size_t kl = strlen(k);
        char base[128];
        char tb2[128];
        size_t nb = 0;
        size_t o = 0;
        if (kl > 6 &&
            kl - 6 < sizeof base) {
            memcpy(base, k, kl - 6);
            base[kl - 6] = 0;
            metric_title(base, tb2,
                sizeof tb2);
            nb = strlen(tb2);
            for (o = 0; o < nb &&
                o + 1 < n; o++) {
                out[o] = tb2[o];
            }
            out[o] = 0;
            if (o + 8 < n) {
                snprintf(out + o,
                    n - o, ", delta");
            }
            return;
        }
    }
    if (strncmp(k, "bench.", 6)
        == 0) {
        const char *p = k + 6;
        char *e1 = nullptr;
        char *e2 = nullptr;
        long a = 0;
        long b = 0;
        a = strtol(p, &e1, 10);
        if (e1 != p && a >= 0 &&
            e1[0] == '.') {
            b = strtol(e1 + 1,
                &e2, 10);
            if (e2 != e1 + 1 &&
                b >= 0 &&
                e2[0] == '.') {
                const char *lf =
                    e2 + 1;
                if (streq(lf,
                    "lat") ||
                    ends_with(k,
                    ".lat")) {
                    snprintf(out, n,
                        "Bench %ld "
                        "%ld latency",
                        a, b);
                    return;
                }
                if (strncmp(lf,
                    "hist.", 5)
                    == 0 ||
                    strstr(k,
                    ".hist.") !=
                    nullptr) {
                    snprintf(out, n,
                        "Bench %ld "
                        "%ld latency "
                        "histogram",
                        a, b);
                    return;
                }
                if (streq(lf,
                    "rate_ach") ||
                    ends_with(k,
                    ".rate_ach")) {
                    snprintf(out, n,
                        "Bench %ld "
                        "%ld achieved "
                        "rate", a, b);
                    return;
                }
            }
        }
    }
    pretty_title(k, out, n);
}

// Unit word for a key; defined past
// mclass below.
static const char *metric_unit(
    const char *k);

// Collects file refs; defined past
// key_stats below.
static int collect_files(Ev *evs,
    size_t n,
    char (*outs)[FileLen],
    int *drop);

// Counts points; defined past tok
// walk below.
static void key_stats(
    const char *key, Ev *evs,
    size_t n, size_t *m, double *v,
    unsigned long long *t);

// Prints graphs and checks; defined
// past tok walk below.
static void render_text_graphs(
    Ev *evs, size_t n);

// True when new bench keys exist;
// defined past hist edges below.
static bool bench_has_new(Ev *evs,
    size_t n);

// Renders bench text views; defined
// past html plot below.
static void render_text_bench(
    Ev *evs, size_t n);

// True when base has summary;
// defined past hist pts below.
static bool bench_tab_hit(
    const char *base, int ph,
    Ev *evs, size_t n);

// Renders bench HTML views; defined
// past html plot below.
static void html_bench(FILE *f,
    Ev *evs, size_t n,
    unsigned long long tmax);

// Renders sorted events: verdict
// first, then t-ordered timeline,
// graphs, files, and notes. Plain
// text with no escapes, so
// NO_COLOR and dumb need no work.
static void render_evs(Ev *evs,
    size_t n, const char *label,
    int trunc, size_t bad) {
    size_t i = 0;
    int have_v = 0;
    const char *vmsg = "";
    (void)bad;
    for (i = 0; i < n; i++) {
        if (streq(evs[i].k,
            "verdict")) {
            have_v = 1;
            vmsg = evs[i].msg;
        }
    }
    if (label != nullptr) {
        printf("== %s\n", label);
    }
    if (have_v != 0 &&
        vmsg[0] != 0) {
        printf("verdict: %s\n",
            vmsg);
    } else {
        fputs("verdict: unknown\n",
            stdout);
    }
    fputs("timeline:\n", stdout);
    for (i = 0; i < n; i++) {
        const Ev *e = &evs[i];
        if (!is_timeline(e->k)) {
            continue;
        }
        printf("+%llums",
            (unsigned long long)
            e->t);
        if (e->has_ph != 0) {
            printf(" [ph %d]",
                e->ph);
        }
        printf(" %s", e->k);
        if (e->msg[0] != 0) {
            printf(" %s", e->msg);
        }
        fputc('\n', stdout);
    }
    render_text_graphs(evs, n);
    {
        char fl[FileCap][FileLen];
        int drop = 0;
        int nfl = collect_files(
            evs, n, fl, &drop);
        int k = 0;
        if (nfl > 0) {
            fputs("files:\n",
                stdout);
            for (k = 0; k < nfl;
                k++) {
                printf("  %s\n",
                    fl[k]);
            }
        }
    }
    if (trunc != 0) {
        printf("note: truncated to"
            " %d events\n", EvCap);
    }
}

// Parses ev lines from one log into
// evs; *n counts kept, *seq numbers
// them, *trunc flags the cap, *bad
// counts skipped lines when not
// null. False on unreadable input;
// malformed lines just skip.
static bool report_file(
    const char *path, Ev *evs,
    size_t *n,
    unsigned long long *seq,
    int *trunc, size_t *bad) {
    FILE *f = fopen(path, "r");
    if (f == nullptr) {
        return (false);
    }
    for (;;) {
        char *ln = nullptr;
        size_t sz = 0;
        ssize_t r = getline(&ln,
            &sz, f);
        Ev e;
        if (r < 0) {
            free(ln);
            break;
        }
        if (ev_parse(ln, &e,
            *seq)) {
            if (*n <
                (size_t)EvCap) {
                evs[*n] = e;
                (*n)++;
            } else {
                *trunc = 1;
            }
        } else if (bad !=
            nullptr) {
            (*bad)++;
        }
        (*seq)++;
        free(ln);
    }
    fclose(f);
    return (true);
}

// Orders log paths for stable output.
static int path_cmp(const void *a,
    const void *b) {
    const char *const *x = a;
    const char *const *y = b;
    return (strcmp(*x, *y));
}

// True for log file names; defined
// with the parser below, declared
// here for the shared dir walk.
static bool is_log(const char *nm);

// Per-log callback for each_log:
// full path plus the log name.
// False marks the log bad; the
// walk still continues.
typedef bool (*log_fn)(
    const char *full,
    const char *name, void *ud);

// each_log runs fn over every
// .log in dir path, sorted. Counts
// fn failures; -1 when the dir will
// not open or holds no logs (both
// reported). One shared walk so
// text, html, and prom sections
// stay identical.
static int each_log(const char *path,
    log_fn fn, void *ud) {
    DIR *d = opendir(path);
    char *names[DirCap];
    size_t nn = 0;
    size_t k = 0;
    int bad = 0;
    if (d == nullptr) {
        fprintf(stderr,
            "rsuite: cannot "
            "read dir %s: %s\n",
            path,
            strerror(errno));
        return (-1);
    }
    for (;;) {
        struct dirent *e =
            readdir(d);
        if (e == nullptr) {
            break;
        }
        if (!is_log(e->d_name)) {
            continue;
        }
        if (nn >=
            (size_t)DirCap) {
            break;
        }
        names[nn] =
            strdup(e->d_name);
        if (names[nn] ==
            nullptr) {
            break;
        }
        nn++;
    }
    closedir(d);
    if (nn == 0) {
        fprintf(stderr,
            "rsuite: no logs in"
            " %s\n", path);
        return (-1);
    }
    qsort(names, nn,
        sizeof *names,
        path_cmp);
    for (k = 0; k < nn; k++) {
        char full[PathCap];
        int r = snprintf(full,
            sizeof full, "%s/%s",
            path, names[k]);
        if (r < 0 ||
            (size_t)r >=
                sizeof full) {
            fprintf(stderr,
                "rsuite: path "
                "too long %s\n",
                names[k]);
            bad++;
        } else if (!fn(full,
            names[k], ud)) {
            bad++;
        }
        free(names[k]);
    }
    return (bad);
}

// True when name ends in .log.
static bool is_log(const char *nm) {
    size_t n = strlen(nm);
    if (n < 5) {
        return (false);
    }
    return (strcmp(nm + n - 4,
        ".log") == 0);
}

// Shows report help only.
static void show_report_use(
    FILE *out) {
    fputs("usage: rsuite report",
        out);
    fputs(" [FILE.log|DIR]\n",
        out);
    fputs("\n", out);
    show_exits(out);
    fputs("parses suite: ev lines",
        out);
    fputs(" from one run log or",
        out);
    fputs(" every .log in a run dir",
        out);
    fputs(" and renders a verdict,",
        out);
    fputs(" timeline, graphs, and",
        out);
    fputs(" checks. Default", out);
    fputs(" path is the current",
        out);
    fputs(" dir. Dir logs sort by",
        out);
    fputs(" name and render one",
        out);
    fputs(" section each. Malformed",
        out);
    fputs(" lines skip; output is",
        out);
    fputs(" plain text with no",
        out);
    fputs(" escapes.\n", out);
    fputs(" --html OUT writes one",
        out);
    fputs(" self-contained offline",
        out);
    fputs(" HTML file instead: one",
        out);
    fputs(" section per log, lanes",
        out);
    fputs(" per phase plus a harness",
        out);
    fputs(" lane, inline SVG plots,",
        out);
    fputs(" no network. Shares the",
        out);
    fputs(" same tolerant parser;",
        out);
    fputs(" malformed lines skip",
        out);
    fputs(" with footer notes.\n",
        out);
    fputs(" --format prom prints",
        out);
    fputs(" Prometheus exposition",
        out);
    fputs(" to stdout instead of",
        out);
    fputs(" text: one gauge sample",
        out);
    fputs(" per numeric metric",
        out);
    fputs(" token, key plus phase",
        out);
    fputs(" plus log labels.",
        out);
    fputs(" Pipe it to a textfile",
        out);
    fputs(" dir or curl it at",
        out);
    fputs(" scrape time.\n",
        out);
}

// Renders one log with label; false
// when the log will not open.
static bool report_one(
    const char *path,
    const char *label) {
    Ev *evs = calloc(
        (size_t)EvCap,
        sizeof *evs);
    size_t n = 0;
    size_t bad = 0;
    unsigned long long seq = 0;
    int trunc = 0;
    if (evs == nullptr) {
        fprintf(stderr, "rsuite: "
            "no mem\n");
        return (false);
    }
    if (!report_file(path, evs,
        &n, &seq, &trunc, &bad)) {
        fprintf(stderr, "rsuite: "
            "cannot read %s: %s\n",
            path,
            strerror(errno));
        free(evs);
        return (false);
    }
    qsort(evs, n, sizeof *evs,
        ev_cmp);
    render_evs(evs, n, label,
        trunc, bad);
    free(evs);
    return (true);
}

// Width of exported SVG plots.
static constexpr int HtmlW = 960;

// Left pad for lane and value
// labels in plots.
static constexpr int HtmlPadL = 110;

// Right pad past the last tick.
static constexpr int HtmlPadR = 20;

// Top pad above the first row.
static constexpr int HtmlTop = 24;

// Height of one timeline lane.
static constexpr int HtmlLaneH = 26;

// Bottom pad below axis labels;
// holds ticks plus the unit line.
static constexpr int HtmlBot = 50;

// Gap kept between a lane label
// end and the plot edge.
static constexpr int HtmlLabGap = 8;

// Unit label offset below the
// axis line; ticks sit at +20.
static constexpr int HtmlUnitDy = 36;

// Height of a metric value plot.
static constexpr int HtmlPlotH = 160;

// Max phase lanes per section;
// extra phases fold into the
// harness lane with a note.
static constexpr int HtmlLanes = 32;

// Max graphed keys per section;
// extra keys note and skip.
static constexpr int HtmlKeys = 16;

// Cap on a metric key kept.
static constexpr int KeyCap = 128;

// Width px of one axis tick digit
// at the plot font size; labels
// size from it for overlap checks.
static constexpr int TickPx = 7;

// Gap px kept between axis labels;
// a middle label nearer than this
// to a neighbor drops instead.
static constexpr int TickGap = 4;

// Writes s HTML-escaped: & < > ".
// Every interpolated string passes
// here, so hostile log bytes can
// never break out of markup.
static void html_esc(FILE *f,
    const char *s) {
    for (; *s != 0; s++) {
        if (*s == '&') {
            fputs("&amp;", f);
        } else if (*s == '<') {
            fputs("&lt;", f);
        } else if (*s == '>') {
            fputs("&gt;", f);
        } else if (*s == '"') {
            fputs("&quot;", f);
        } else {
            fputc(*s, f);
        }
    }
}

// Writes the doc head with inline
// CSS only: no external styles, no
// scripts, no external refs of any
// kind. Body is monospace pre text
// matching the text report; file
// links stay plain <a href> (same
// color, no underline), so refs read
// as text yet stay clickable.
static void html_head(FILE *f,
    const char *title) {
    fputs("<!DOCTYPE html>\n", f);
    fputs("<html lang=\"en\">\n",
        f);
    fputs("<head>\n", f);
    fputs("<meta charset=\"utf-8"
        "\">\n", f);
    fputs("<title>", f);
    html_esc(f, title);
    fputs("</title>\n", f);
    fputs("<style>\n", f);
    fputs("body{font-family:"
        "monospace;margin:2em}\n",
        f);
    fputs("pre{margin:1em 0}\n",
        f);
    fputs("a{color:inherit;"
        "text-decoration:none}\n",
        f);
    fputs("svg text{font-size:12px"
        "}\n", f);
    fputs("</style>\n", f);
    fputs("</head>\n", f);
    fputs("<body>\n", f);
}

// Closes the doc; the footer notes
// the offline export has no refs.
static void html_foot(FILE *f,
    int nsec) {
    fprintf(f, "<pre>sections: %d. "
        "offline export; no "
        "external refs.</pre>\n",
        nsec);
    fputs("</body>\n</html>\n",
        f);
}

// X pixel of t in [0,m] on a plot
// of width w with side pads. Flat
// at the left pad when m is zero.
static double html_x(
    unsigned long long t,
    unsigned long long m, int w) {
    double span = 0.0;
    if (m == 0) {
        return ((double)HtmlPadL);
    }
    span = (double)(w - HtmlPadL -
        HtmlPadR);
    return ((double)HtmlPadL + span *
        (double)t / (double)m);
}

// series_x maps t within the series
// window [lo,hi] to plot width, so
// a short late phase fills the plot
// instead of collapsing to one tick
// of the whole-run range. Equal
// ends center: one instant reads as
// one tick, never NaN.
static double series_x(
    unsigned long long t,
    unsigned long long lo,
    unsigned long long hi) {
    double span = 0.0;
    span = (double)(HtmlW -
        HtmlPadL - HtmlPadR);
    if (hi <= lo) {
        return ((double)HtmlPadL +
            span / 2.0);
    }
    return ((double)HtmlPadL + span *
        (double)(t - lo) /
        (double)(hi - lo));
}

// Lane index of ph in ls[0..nl):
// the old index, else -1 (caller
// adds it or folds to harness).
static int lane_of(int *ls, int nl,
    int ph) {
    int i = 0;
    for (i = 0; i < nl; i++) {
        if (ls[i] == ph) {
            return (i);
        }
    }
    return (-1);
}

// Y center of lane i.
static int lane_y(int i) {
    return (HtmlTop + i *
        HtmlLaneH + HtmlLaneH / 2);
}

// Writes one timeline marker for e
// at (x,y): rects for phases,
// circles for steps, triangles for
// markers, ticks for conds, and a
// colored diamond for verdicts.
// The title carries escaped kind,
// time, and message.
static void html_mark(FILE *f,
    const Ev *e, int x, int y) {
    int tg = 1;
    const char *vc = "#b91c2b";
    if (streq(e->k, "phase")) {
        tg = 0;
        fprintf(f, "<rect x=\"%d\" "
            "y=\"%d\" width=\"9\" "
            "height=\"9\" "
            "fill=\"#1d4ed8\">",
            x - 4, y - 4);
    } else if (streq(e->k,
        "step")) {
        tg = 1;
        fprintf(f, "<circle cx=\""
            "%d\" cy=\"%d\" r=\"4\" "
            "fill=\"#0e7490\">", x,
            y);
    } else if (streq(e->k,
        "marker")) {
        tg = 2;
        fprintf(f, "<polygon "
            "points=\"%d,%d %d,%d "
            "%d,%d\" "
            "fill=\"#b45309\">", x,
            y - 6, x - 5, y + 4,
            x + 5, y + 4);
    } else if (streq(e->k,
        "cond")) {
        tg = 1;
        fprintf(f, "<circle cx=\""
            "%d\" cy=\"%d\" "
            "r=\"2.5\" "
            "fill=\"#4d7c0f\">", x,
            y);
    } else if (streq(e->k,
        "verdict")) {
        tg = 2;
        if (strncmp(e->msg,
            "pass ", 5) == 0) {
            vc = "#15803d";
        } else if (strncmp(
            e->msg, "skip ", 5)
            == 0) {
            vc = "#6b7280";
        }
        fprintf(f, "<polygon "
            "points=\"%d,%d %d,%d "
            "%d,%d %d,%d\" "
            "fill=\"%s\">", x,
            y - 8, x + 7, y, x,
            y + 8, x - 7, y, vc);
    } else {
        tg = 1;
        fprintf(f, "<circle cx=\""
            "%d\" cy=\"%d\" r=\"4\" "
            "fill=\"#0e7490\">", x,
            y);
    }
    fputs("<title>", f);
    html_esc(f, e->k);
    fprintf(f, " +%llu ms",
        e->t);
    if (e->msg[0] != 0) {
        fputc(' ', f);
        html_esc(f, e->msg);
    }
    fputs("</title>", f);
    if (tg == 0) {
        fputs("</rect>", f);
    } else if (tg == 1) {
        fputs("</circle>", f);
    } else {
        fputs("</polygon>", f);
    }
}

// Digits in t; times TickPx gives
// the axis label width.
static int tick_len(
    unsigned long long t) {
    int n = 1;
    while (t >= 10u) {
        t /= 10u;
        n++;
    }
    return (n);
}

// Writes one axis tick at xi for t:
// mark plus label. Anchor 0 starts
// the label at the tick, 1 centers
// it, 2 ends it there; inward anchor
// keeps edge labels inside the view
// so the max reads in full, never
// clipped by the SVG edge.
static void tick_write(FILE *f,
    unsigned long long t, int xi,
    int y0, int anchor) {
    fprintf(f, "<line x1=\"%d\" "
        "y1=\"%d\" x2=\"%d\" "
        "y2=\"%d\" "
        "stroke=\"#111\"/>\n",
        xi, y0, xi, y0 + 5);
    if (anchor == 1) {
        fprintf(f, "<text x=\"%d\" "
            "y=\"%d\" "
            "text-anchor=\"middle\">"
            "%llu</text>\n",
            xi, y0 + 20, t);
        return;
    }
    if (anchor == 2) {
        fprintf(f, "<text x=\"%d\" "
            "y=\"%d\" "
            "text-anchor=\"end\">"
            "%llu</text>\n",
            xi, y0 + 20, t);
        return;
    }
    fprintf(f, "<text x=\"%d\" "
        "y=\"%d\">%llu</text>\n",
        xi, y0 + 20, t);
}

// Writes ticks for [0,tmax]: 0, half,
// max. Equal values merge so tiny
// ranges show one or two sane ticks,
// never stacked dupes; the middle
// drops when its box would hit a
// neighbor box at this width, so no
// two labels ever overlap. html_x
// pins m == 0 left: no div by zero.
static void axis_write(FILE *f,
    unsigned long long tmax, int y0) {
    unsigned long long mid = 0;
    int x0 = 0;
    int xm = 0;
    int x1 = 0;
    int w0 = 0;
    int wm = 0;
    int w1 = 0;
    mid = tmax / 2u;
    x0 = (int)html_x(0u, tmax,
        HtmlW);
    xm = (int)html_x(mid, tmax,
        HtmlW);
    x1 = (int)html_x(tmax, tmax,
        HtmlW);
    w0 = tick_len(0u) * TickPx;
    wm = tick_len(mid) * TickPx;
    w1 = tick_len(tmax) * TickPx;
    tick_write(f, 0u, x0, y0, 0);
    if (mid != 0u &&
        mid != tmax &&
        xm - wm / 2 >=
            x0 + w0 + TickGap &&
        xm + wm / 2 <=
            x1 - w1 - TickGap) {
        tick_write(f, mid, xm, y0,
            1);
    }
    if (tmax != 0u) {
        tick_write(f, tmax, x1, y0,
            2);
    }
}

// Writes one timeline SVG: a lane
// per phase index seen plus one
// harness lane for verdicts,
// markers, and stray events. All
// markers sit at their sorted
// timestamps. Returns the count of
// phases folded into harness.
static int html_timeline(FILE *f,
    Ev *evs, size_t n,
    unsigned long long tmax) {
    int ls[HtmlLanes];
    int nl = 0;
    int fold = 0;
    size_t i = 0;
    int k = 0;
    int h = 0;
    int harn = 0;
    int y0 = 0;
    int x1 = 0;
    for (i = 0; i < n; i++) {
        Ev *e = &evs[i];
        if (!is_timeline(e->k) &&
            !streq(e->k,
                "verdict")) {
            continue;
        }
        if (streq(e->k,
            "verdict") ||
            streq(e->k,
                "marker") ||
            e->has_ph == 0) {
            continue;
        }
        if (lane_of(ls, nl,
            e->ph) >= 0) {
            continue;
        }
        if (nl < HtmlLanes) {
            ls[nl] = e->ph;
            nl++;
            continue;
        }
        fold++;
    }
    harn = nl;
    h = HtmlTop +
        (nl + 1) * HtmlLaneH +
        HtmlBot;
    x1 = HtmlW - HtmlPadR;
    y0 = HtmlTop +
        (nl + 1) * HtmlLaneH;
    fprintf(f, "<svg width=\"%d\" "
        "height=\"%d\" "
        "role=\"img\">\n", HtmlW,
        h);
    for (k = 0; k <= nl; k++) {
        int y = lane_y(k);
        fprintf(f, "<line x1=\"%d\" "
            "y1=\"%d\" x2=\"%d\" "
            "y2=\"%d\" "
            "stroke=\"#e5e5e5\"/>\n",
            HtmlPadL, y, x1, y);
        fprintf(f, "<text x=\"%d\" "
            "y=\"%d\" "
            "text-anchor=\"end\">",
            HtmlPadL - HtmlLabGap,
            y + 4);
        if (k < nl) {
            fprintf(f, "ph %d",
                ls[k]);
        } else {
            fputs("harness", f);
        }
        fputs("</text>\n", f);
    }
    fprintf(f, "<line x1=\"%d\" "
        "y1=\"%d\" x2=\"%d\" "
        "y2=\"%d\" "
        "stroke=\"#111\"/>\n",
        HtmlPadL, y0, x1, y0);
    axis_write(f, tmax, y0);
    fprintf(f, "<text x=\"%d\" "
        "y=\"%d\">time ",
        HtmlPadL, y0 + HtmlUnitDy);
    fputs("(milliseconds)</text>\n",
        f);
    for (i = 0; i < n; i++) {
        Ev *e = &evs[i];
        int li = 0;
        int xi = 0;
        if (!is_timeline(e->k) &&
            !streq(e->k,
                "verdict")) {
            continue;
        }
        if (streq(e->k,
            "verdict") ||
            streq(e->k,
                "marker") ||
            e->has_ph == 0) {
            li = harn;
        } else {
            li = lane_of(ls, nl,
                e->ph);
            if (li < 0) {
                li = harn;
            }
        }
        xi = (int)html_x(e->t,
            tmax, HtmlW);
        html_mark(f, e, xi,
            lane_y(li));
    }
    fputs("</svg>\n", f);
    return (fold);
}

// Class of a metric key: 1 rss, 2
// cpu, 3 counter, 0 duration or
// other (table only, never a
// graph). Durations end in .ms;
// rss and cpu match by name,
// except rchar/wchar which match
// by suffix so IO reads under
// resources, never flow. Bench
// count, loss, target, overrun read
// 3 and graph; bench .ms reads 0.
static int mclass(const char *k) {
    size_t n = strlen(k);
    if (n > 3 && strcmp(k + n - 3,
        ".ms") == 0) {
        return (0);
    }
    if (ends_with(k, ".rchar") ||
        ends_with(k, ".wchar")) {
        return (1);
    }
    if (strcmp(k, "maxrss.mb")
        == 0 ||
        strstr(k, "rss") !=
            nullptr ||
        strstr(k, "mem") !=
            nullptr) {
        return (1);
    }
    if (strstr(k, "cpu") !=
        nullptr) {
        return (2);
    }
    return (3);
}

// Parses s as a plain number for
// graphs; false on bad text,
// trailing junk, or range error,
// so hostile values just skip.
static bool num_of(const char *s,
    double *out) {
    char *end = nullptr;
    double v = 0.0;
    if (s[0] == 0) {
        return (false);
    }
    errno = 0;
    v = strtod(s, &end);
    if (end == s) {
        return (false);
    }
    if (*end != 0) {
        return (false);
    }
    if (errno != 0) {
        return (false);
    }
    *out = v;
    return (true);
}

// Called per metric token: tok[tl)
// is the raw token, eq points at
// the = when one is present, else
// null.
typedef void (*tok_cb)(
    const char *tok, size_t tl,
    const char *eq, void *ud);

// Walks metric msg tokens: blanks
// split, the key is text before
// = when any. Presentation only;
// the ev line parser stays the
// one true parser above.
static void tok_walk(
    const char *msg, tok_cb cb,
    void *ud) {
    const char *p = msg;
    while (*p != 0) {
        const char *e = nullptr;
        const char *eq = nullptr;
        const char *q = nullptr;
        while (*p == ' ' ||
            *p == '\t') {
            p++;
        }
        if (*p == 0) {
            return;
        }
        e = p;
        while (*e != 0 &&
            *e != ' ' &&
            *e != '\t') {
            e++;
        }
        for (q = p; q < e; q++) {
            if (*q == '=') {
                eq = q;
                break;
            }
        }
        cb(p, (size_t)(e - p), eq,
            ud);
        p = e;
    }
}

// One graphed metric key.
typedef struct {
    char k[KeyCap];
} Mkey;

// Key table build state.
typedef struct {
    Mkey *tk;
    int *nt;
    int cls;
    int *drop;
} KeyUd;

// Adds one token key of class cls
// to the table; past HtmlKeys it
// counts drops instead.
static void key_cb(const char *tok,
    size_t tl, const char *eq,
    void *ud) {
    KeyUd *u = ud;
    char k[KeyCap];
    size_t kl = 0;
    int i = 0;
    (void)tl;
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (kl == 0 ||
        kl >= (size_t)KeyCap) {
        return;
    }
    memcpy(k, tok, kl);
    k[kl] = 0;
    if (mclass(k) != u->cls) {
        return;
    }
    for (i = 0; i < *u->nt; i++) {
        if (strcmp(u->tk[i].k, k)
            == 0) {
            return;
        }
    }
    if (*u->nt >= HtmlKeys) {
        (*u->drop)++;
        return;
    }
    memcpy(u->tk[*u->nt].k, k,
        kl + 1);
    (*u->nt)++;
}

// Point series build state: ts/ys
// hold m of cap points for key; t
// is the stamp of the event now
// walking.
typedef struct {
    const char *key;
    unsigned long long *ts;
    double *ys;
    size_t *m;
    size_t cap;
    unsigned long long t;
} PtUd;

// Adds one numeric token value to
// the series; bad values skip.
static void pt_cb(const char *tok,
    size_t tl, const char *eq,
    void *ud) {
    PtUd *u = ud;
    size_t kl = 0;
    size_t vl = 0;
    char vb[32];
    double v = 0.0;
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (strlen(u->key) != kl) {
        return;
    }
    if (memcmp(u->key, tok, kl)
        != 0) {
        return;
    }
    vl = tl - kl - 1;
    if (vl == 0 ||
        vl >= sizeof vb) {
        return;
    }
    memcpy(vb, eq + 1, vl);
    vb[vl] = 0;
    if (!num_of(vb, &v)) {
        return;
    }
    if (*u->m >= u->cap) {
        return;
    }
    u->ts[*u->m] = u->t;
    u->ys[*u->m] = v;
    (*u->m)++;
}

// Unit word for a key from its
// suffix; table in order: .ms
// milliseconds, .mb megabytes,
// .bytes/.rchar/.wchar bytes,
// .pkts packets, any drops suffix
// drops, .count/_count/count
// count, .ok/ok/.status/status
// status, .seconds seconds,
// .ticks ticks; .delta strips and
// recurses on the base, so deltas
// share the base unit. Unknown
// suffixes fall back to mclass:
// counters packets, class 1
// megabytes, class 2 ticks.
static const char *metric_unit(
    const char *k) {
    int c = 0;
    if (ends_with(k, ".delta")) {
        size_t kl = strlen(k);
        char base[128];
        if (kl > 6 &&
            kl - 6 < sizeof base) {
            memcpy(base, k, kl - 6);
            base[kl - 6] = 0;
            return (metric_unit(
                base));
        }
        return ("delta");
    }
    if (ends_with(k, ".ms")) {
        return ("milliseconds");
    }
    if (ends_with(k, ".mb")) {
        return ("megabytes");
    }
    if (ends_with(k, ".bytes") ||
        ends_with(k, ".rchar") ||
        ends_with(k, ".wchar")) {
        return ("bytes");
    }
    if (ends_with(k, ".pkts")) {
        return ("packets");
    }
    if (ends_with(k, "drops")) {
        return ("drops");
    }
    if (ends_with(k, ".count") ||
        ends_with(k, "_count") ||
        streq(k, "count")) {
        return ("count");
    }
    if (ends_with(k, ".ok") ||
        streq(k, "ok") ||
        ends_with(k, ".status") ||
        streq(k, "status")) {
        return ("status");
    }
    if (ends_with(k, ".seconds") ||
        ends_with(k, ".secs")) {
        return ("seconds");
    }
    if (ends_with(k, ".ticks")) {
        return ("ticks");
    }
    if (ends_with(k, ".lat")) {
        return ("milliseconds");
    }
    if (ends_with(k, ".lat_us") ||
        ends_with(k, ".us")) {
        return ("microseconds");
    }
    if (ends_with(k, ".samples") ||
        ends_with(k,
        "samples_dropped")) {
        return ("count");
    }
    if (strstr(k, ".hist_us.") !=
        nullptr) {
        return ("count");
    }
    if (ends_with(k,
        "samples_dropped")) {
        return ("count");
    }
    if (strstr(k, ".hist.") !=
        nullptr) {
        return ("count");
    }
    if (ends_with(k, ".rate_ach")) {
        return ("messages per "
            "second");
    }
    if (ends_with(k,
        ".ceiling.rate")) {
        return ("messages per "
            "second");
    }
    c = mclass(k);
    if (c == 3) {
        return ("packets");
    }
    if (c == 1) {
        return ("megabytes");
    }
    if (c == 2) {
        return ("ticks");
    }
    return ("value");
}

// Stats for one key: count plus the
// lone value and stamp.
typedef struct {
    const char *key;
    size_t n;
    double v;
    unsigned long long t;
    unsigned long long cur;
} StatUd;

// Counts numeric points for StatUd;
// keeps the first value.
static void stat_cb(const char *tok,
    size_t tl, const char *eq,
    void *ud) {
    StatUd *u = ud;
    size_t kl = 0;
    size_t vl = 0;
    char vb[32];
    double v = 0.0;
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (strlen(u->key) != kl) {
        return;
    }
    if (memcmp(u->key, tok, kl)
        != 0) {
        return;
    }
    vl = tl - kl - 1;
    if (vl == 0 ||
        vl >= sizeof vb) {
        return;
    }
    memcpy(vb, eq + 1, vl);
    vb[vl] = 0;
    if (!num_of(vb, &v)) {
        return;
    }
    if (u->n == 0) {
        u->v = v;
        u->t = u->cur;
    }
    u->n++;
}

// Counts numeric points for key;
// *m counts, *v/*t hold the lone
// point when *m is 1.
static void key_stats(
    const char *key, Ev *evs,
    size_t n, size_t *m, double *v,
    unsigned long long *t) {
    StatUd u;
    size_t i = 0;
    u.key = key;
    u.n = 0;
    u.v = 0.0;
    u.t = 0;
    u.cur = 0;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        u.cur = evs[i].t;
        tok_walk(evs[i].msg,
            stat_cb, &u);
    }
    *m = u.n;
    *v = u.v;
    *t = u.t;
}

// Display name for a phase index:
// begin/done NAME text wins, -1
// reads as harness, unknown reads
// as ph N. Names come only from
// phase events, never guesses.
static void phase_name(Ev *evs,
    size_t n, int ph, char *out,
    size_t nout) {
    size_t i = 0;
    if (nout == 0) {
        return;
    }
    out[0] = 0;
    if (ph < 0) {
        snprintf(out, nout, "%s",
            "harness");
        return;
    }
    for (i = 0; i < n; i++) {
        const char *m = nullptr;
        if (!streq(evs[i].k,
            "phase")) {
            continue;
        }
        if (evs[i].ph != ph) {
            continue;
        }
        m = evs[i].msg;
        if (strncmp(m, "begin ",
            6) == 0) {
            m += 6;
        } else if (strncmp(m,
            "done ", 5) == 0) {
            m += 5;
        } else if (m[0] == 0) {
            continue;
        }
        snprintf(out, nout, "%s",
            m);
        return;
    }
    snprintf(out, nout, "ph %d",
        ph);
}

// Per-phase point stats: numeric
// points for key in phase ph only
// (missing ph reads as -1). Whole
// runs never plot: a series that
// spans a heal or reunion would
// join unrelated regimes.
typedef struct {
    const char *key;
    int ph;
    size_t n;
    double v;
    unsigned long long t;
    unsigned long long cur;
    int curph;
} PhStatUd;

// Counts one per-phase point;
// bad values skip like stat_cb.
static void ph_stat_cb(
    const char *tok, size_t tl,
    const char *eq, void *ud) {
    PhStatUd *u = ud;
    size_t kl = 0;
    size_t vl = 0;
    char vb[32];
    double v = 0.0;
    if (eq == nullptr) {
        return;
    }
    if (u->curph != u->ph) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (strlen(u->key) != kl) {
        return;
    }
    if (memcmp(u->key, tok, kl)
        != 0) {
        return;
    }
    vl = tl - kl - 1;
    if (vl == 0 ||
        vl >= sizeof vb) {
        return;
    }
    memcpy(vb, eq + 1, vl);
    vb[vl] = 0;
    if (!num_of(vb, &v)) {
        return;
    }
    if (u->n == 0) {
        u->v = v;
        u->t = u->cur;
    }
    u->n++;
}

// Counts numeric points for key
// in phase ph; *m counts, *v/*t
// hold the first point.
static void key_phase_stats(
    const char *key, int ph,
    Ev *evs, size_t n, size_t *m,
    double *v,
    unsigned long long *t) {
    PhStatUd u;
    size_t i = 0;
    u.key = key;
    u.ph = ph;
    u.n = 0;
    u.v = 0.0;
    u.t = 0;
    u.cur = 0;
    u.curph = 0;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        u.cur = evs[i].t;
        u.curph = evs[i].ph;
        tok_walk(evs[i].msg,
            ph_stat_cb, &u);
    }
    *m = u.n;
    *v = u.v;
    *t = u.t;
}

// One-shot hit state for a key.
typedef struct {
    const char *key;
    int hit;
} HitUd;

// Marks hit when tok names the key
// with a numeric value; hostile
// values never hit.
static void hit_cb(const char *tok,
    size_t tl, const char *eq,
    void *ud) {
    HitUd *u = ud;
    size_t kl = 0;
    size_t vl = 0;
    char vb[32];
    double v = 0.0;
    if (u->hit != 0) {
        return;
    }
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (strlen(u->key) != kl) {
        return;
    }
    if (memcmp(u->key, tok, kl)
        != 0) {
        return;
    }
    vl = tl - kl - 1;
    if (vl == 0 ||
        vl >= sizeof vb) {
        return;
    }
    memcpy(vb, eq + 1, vl);
    vb[vl] = 0;
    if (!num_of(vb, &v)) {
        return;
    }
    u->hit = 1;
}

// True when e holds a numeric
// point for key.
static bool ev_has_key_num(
    const Ev *e, const char *key) {
    HitUd u;
    u.key = key;
    u.hit = 0;
    tok_walk(e->msg, hit_cb, &u);
    return (u.hit != 0);
}

// Distinct phases holding numeric
// points for key into phs (cap);
// returns the count kept.
static int key_phases(
    const char *key, Ev *evs,
    size_t n, int *phs, int cap) {
    int np = 0;
    size_t i = 0;
    int k = 0;
    for (i = 0; i < n; i++) {
        int ph = 0;
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        if (!ev_has_key_num(
            &evs[i], key)) {
            continue;
        }
        ph = evs[i].ph;
        for (k = 0; k < np; k++) {
            if (phs[k] == ph) {
                break;
            }
        }
        if (k < np) {
            continue;
        }
        if (np >= cap) {
            return (np);
        }
        phs[np++] = ph;
    }
    return (np);
}

// True when any flow key ends in
// .delta: suffix match only, so
// new emitter names still work.
// Deltas graph alone; cumulative
// keys hide while deltas exist.
static bool has_delta_keys(
    Mkey *fk, int nf) {
    int j = 0;
    for (j = 0; j < nf; j++) {
        if (ends_with(fk[j].k,
            ".delta")) {
            return (true);
        }
    }
    return (false);
}

// True for tick keys: scheduler
// ticks that seconds supersede.
static bool is_ticks_key(
    const char *k) {
    if (ends_with(k, ".utime")) {
        return (true);
    }
    if (ends_with(k, ".stime")) {
        return (true);
    }
    return (false);
}

// True for sampler counts: our
// own bookkeeping, not signal.
static bool is_samples_key(
    const char *k) {
    if (ends_with(k,
        ".samples")) {
        return (true);
    }
    return (false);
}

// State for a seconds scan.
typedef struct {
    int hit;
} SecUd;

// Marks hit for one numeric
// .secs token; hostile skips.
static void sec_cb(const char *tok,
    size_t tl, const char *eq,
    void *ud) {
    SecUd *u = ud;
    size_t kl = 0;
    size_t vl = 0;
    char kb[KeyCap];
    char vb[32];
    double v = 0.0;
    if (u->hit != 0) {
        return;
    }
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (kl == 0 ||
        kl >= sizeof kb) {
        return;
    }
    memcpy(kb, tok, kl);
    kb[kl] = 0;
    if (!ends_with(kb, ".secs")) {
        return;
    }
    vl = tl - kl - 1;
    if (vl == 0 ||
        vl >= sizeof vb) {
        return;
    }
    memcpy(vb, eq + 1, vl);
    vb[vl] = 0;
    if (!num_of(vb, &v)) {
        return;
    }
    u->hit = 1;
}

// True when any numeric .secs
// point exists; ticks then hide,
// else ticks render as fallback.
static bool has_secs(Ev *evs,
    size_t n) {
    SecUd u;
    size_t i = 0;
    u.hit = 0;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg, sec_cb,
            &u);
        if (u.hit != 0) {
            return (true);
        }
    }
    return (false);
}

// Drops culled keys in place:
// samples always, ticks when
// secs supersede; keeps order.
static void cull_keys(Mkey *ks,
    int *n, int no_ticks) {
    int w = 0;
    int j = 0;
    if (ks == nullptr) {
        return;
    }
    if (n == nullptr) {
        return;
    }
    for (j = 0; j < *n; j++) {
        if (is_samples_key(
            ks[j].k)) {
            continue;
        }
        if (no_ticks != 0 &&
            is_ticks_key(
                ks[j].k)) {
            continue;
        }
        if (w != j) {
            memcpy(ks[w].k,
                ks[j].k,
                sizeof ks[w].k);
        }
        w++;
    }
    *n = w;
}

// True for href-safe path bytes:
// alnum plus slash dot minus.
static bool path_char(char c) {
    if (c >= 'a' && c <= 'z') {
        return (true);
    }
    if (c >= 'A' && c <= 'Z') {
        return (true);
    }
    if (c >= '0' && c <= '9') {
        return (true);
    }
    if (c == '/' || c == '.' ||
        c == '-' || c == '_') {
        return (true);
    }
    return (false);
}

// True when a metric key names a
// file-ish value: snapshots and
// journals match here by infix,
// values still pass safe_rel.
static bool file_hint(
    const char *k) {
    if (strstr(k, "snapshot") !=
        nullptr) {
        return (true);
    }
    if (strstr(k, "profile") !=
        nullptr) {
        return (true);
    }
    if (strstr(k, "journal") !=
        nullptr) {
        return (true);
    }
    if (strstr(k, "forensic") !=
        nullptr) {
        return (true);
    }
    if (strstr(k, "manifest") !=
        nullptr) {
        return (true);
    }
    if (strstr(k, "aside") !=
        nullptr) {
        return (true);
    }
    if (strstr(k, "path") !=
        nullptr) {
        return (true);
    }
    if (strstr(k, "file") !=
        nullptr) {
        return (true);
    }
    return (false);
}

// True when v is a safe relative
// link: has a slash, tight
// charset, not absolute, no dotdot
// and no scheme, so log content
// can never emit absolute file://
// links or escapes the run dir.
static bool safe_rel(
    const char *v) {
    size_t n = 0;
    size_t i = 0;
    int slash = 0;
    if (v == nullptr) {
        return (false);
    }
    n = strlen(v);
    if (n == 0 ||
        n >= (size_t)FileLen) {
        return (false);
    }
    if (v[0] == '/' ||
        v[0] == '~') {
        return (false);
    }
    if (strstr(v, "..") !=
        nullptr) {
        return (false);
    }
    if (strstr(v, "://") !=
        nullptr) {
        return (false);
    }
    for (i = 0; i < n; i++) {
        if (v[i] == '/') {
            slash = 1;
        }
        if (!path_char(v[i])) {
            return (false);
        }
    }
    if (slash == 0) {
        return (false);
    }
    return (true);
}

// File table build state.
typedef struct {
    char (*outs)[FileLen];
    int *nref;
    int *drop;
} FileUd;

// Adds one file-ish token value;
// unsafe paths skip silently.
static void file_cb(const char *tok,
    size_t tl, const char *eq,
    void *ud) {
    FileUd *u = ud;
    size_t kl = 0;
    size_t vl = 0;
    char kb[KeyCap];
    char vb[FileLen];
    int i = 0;
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (kl == 0 ||
        kl >= sizeof kb) {
        return;
    }
    memcpy(kb, tok, kl);
    kb[kl] = 0;
    if (!file_hint(kb)) {
        return;
    }
    vl = tl - kl - 1;
    if (vl == 0 ||
        vl >= sizeof vb) {
        return;
    }
    memcpy(vb, eq + 1, vl);
    vb[vl] = 0;
    if (!safe_rel(vb)) {
        return;
    }
    for (i = 0; i < *u->nref;
        i++) {
        if (strcmp(u->outs[i],
            vb) == 0) {
            return;
        }
    }
    if (*u->nref >= FileCap) {
        (*u->drop)++;
        return;
    }
    memcpy(u->outs[*u->nref],
        vb, vl + 1);
    (*u->nref)++;
}

// Scans one free-text msg for
// slash tokens naming snapshots
// or journals; metric keys cover
// the manifest shape above.
static void msg_file_scan(
    const char *msg,
    char (*outs)[FileLen],
    int *nref, int *drop) {
    const char *p = msg;
    while (*p != 0) {
        const char *e = nullptr;
        size_t l = 0;
        char tok[FileLen];
        int i = 0;
        int hint = 0;
        while (*p == ' ' ||
            *p == '\t') {
            p++;
        }
        if (*p == 0) {
            return;
        }
        e = p;
        while (*e != 0 &&
            *e != ' ' &&
            *e != '\t') {
            e++;
        }
        l = (size_t)(e - p);
        if (l > 0 &&
            l < sizeof tok &&
            memchr(p, '/', l) !=
                nullptr) {
            memcpy(tok, p, l);
            tok[l] = 0;
            hint = strstr(tok,
                "profile") !=
                nullptr ||
                strstr(tok,
                "snapshot") !=
                nullptr ||
                strstr(tok,
                "journal") !=
                nullptr ||
                strstr(tok,
                "forensic") !=
                nullptr ||
                strstr(tok,
                "manifest") !=
                nullptr ||
                strstr(tok,
                "aside") !=
                nullptr;
            if (hint != 0 &&
                safe_rel(tok)) {
                for (i = 0;
                    i < *nref; i++) {
                    if (strcmp(
                        outs[i],
                        tok) == 0) {
                        break;
                    }
                }
                if (i >= *nref) {
                    if (*nref >=
                        FileCap) {
                        (*drop)++;
                    } else {
                        memcpy(
                            outs[
                            *nref],
                            tok,
                            l + 1);
                        (*nref)++;
                    }
                }
            }
        }
        p = e;
    }
}

// Collects safe relative file refs
// from metric values plus msg
// tokens; returns the ref count.
static int collect_files(Ev *evs,
    size_t n,
    char (*outs)[FileLen],
    int *drop) {
    FileUd u;
    size_t i = 0;
    int nref = 0;
    u.outs = outs;
    u.nref = &nref;
    u.drop = drop;
    *drop = 0;
    for (i = 0; i < n; i++) {
        if (streq(evs[i].k,
            "metric")) {
            tok_walk(evs[i].msg,
                file_cb, &u);
        } else {
            msg_file_scan(
                evs[i].msg, outs,
                &nref, drop);
        }
    }
    return (nref);
}

// Prints the static report header
// for text output: one sentence
// naming what the file holds.
static void print_explainer(void) {
    fputs("This report renders one",
        stdout);
    fputs(" run log file per "
        "section.\n", stdout);
    fputs("Series split by phase: "
        "one plot per key and "
        "phase.\n", stdout);
}

// Writes the static intro for HTML:
// the same two lines the text
// report prints, in a pre block.
static void html_explainer(FILE *f) {
    fputs("<pre>This report renders "
        "one run log file per "
        "section.\n", f);
    fputs("Series split by phase: "
        "one plot per key and "
        "phase.</pre>\n", f);
}

// State for text point printing.
typedef struct {
    const char *key;
    unsigned long long t;
} TxtUd;

// Prints one numeric point for
// TxtUd as a text graph row.
static void txt_cb(const char *tok,
    size_t tl, const char *eq,
    void *ud) {
    TxtUd *u = ud;
    size_t kl = 0;
    size_t vl = 0;
    char vb[32];
    double v = 0.0;
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (strlen(u->key) != kl) {
        return;
    }
    if (memcmp(u->key, tok, kl)
        != 0) {
        return;
    }
    vl = tl - kl - 1;
    if (vl == 0 ||
        vl >= sizeof vb) {
        return;
    }
    memcpy(vb, eq + 1, vl);
    vb[vl] = 0;
    if (!num_of(vb, &v)) {
        return;
    }
    printf("  +%llums %s\n",
        (unsigned long long)u->t,
        vb);
}

// bench_val is defined with the
// summary tables below; the graphs
// above borrow it for golden refs.
static void bench_val(
    const char *base,
    const char *leaf, int ph,
    Ev *evs, size_t n, double *v,
    int *found);

// is_bench plus bench_base live
// with the histogram helpers
// below; declared here for the
// same golden refs.
static bool is_bench(
    const char *k);
static bool bench_base(
    const char *k, char *out,
    size_t n);

// Golden overlay palette, shared
// by text refs and SVG lines:
// p50 cyan, p99 amber, max red.
static const char *golden_color(
    int i) {
    static const char *c[3] = {
        "#0e7490", "#b45309",
        "#b91c2b",
    };
    if (i < 0 || i > 2) {
        return ("#6b7280");
    }
    return (c[i]);
}

// Golden tag for slot i.
static const char *golden_tag(
    int i) {
    static const char *t[3] = {
        "p50", "p99", "max",
    };
    if (i < 0 || i > 2) {
        return ("?");
    }
    return (t[i]);
}

// Unit ext matching a bench
// latency key: .lat takes .ms,
// .lat_us takes .us. Nullptr
// unless the key qualifies.
static const char *golden_ext(
    const char *key) {
    if (key == nullptr) {
        return (nullptr);
    }
    if (!is_bench(key)) {
        return (nullptr);
    }
    if (ends_with(key,
        ".lat_us")) {
        return (".us");
    }
    if (ends_with(key,
        ".lat")) {
        return (".ms");
    }
    return (nullptr);
}

// Fetches the golden values for
// key plus unit ext into v/f.
// True when the base parses;
// slots stay absent when the
// leaves never emitted.
static bool golden_fetch(
    const char *key,
    const char *ext, int ph,
    Ev *evs, size_t n, double *v,
    int *f) {
    char base[KeyCap];
    char leaf[16];
    const char *heads[3] = {
        "p50", "p99", "max",
    };
    int i = 0;
    if (key == nullptr ||
        ext == nullptr || v == nullptr ||
        f == nullptr) {
        return (false);
    }
    if (!bench_base(key, base,
        sizeof base)) {
        return (false);
    }
    for (i = 0; i < 3; i++) {
        (void)snprintf(leaf,
            sizeof leaf, "%s%s",
            heads[i], ext);
        bench_val(base, leaf, ph,
            evs, n, &v[i],
            &f[i]);
    }
    return (true);
}

// Prints one multi-point text graph
// for key in one phase: title,
// key, phase, axes, then points.
// Bench latency keys add one refs
// line with the golden p50, p99,
// and max overlay values.
// Whole-run series never render:
// a split replaces them so heals
// and reunions cannot join.
static void text_graph(
    const char *key, int ph,
    const char *pname, Ev *evs,
    size_t n) {
    char tb[128];
    const char *un = nullptr;
    size_t i = 0;
    if (pname == nullptr) {
        pname = "ph";
    }
    metric_title(key, tb,
        sizeof tb);
    un = metric_unit(key);
    printf("%s\n", tb);
    printf("  key: %s\n", key);
    printf("  phase: %s [%d]\n",
        pname, ph);
    printf("  axes: x is time "
        "(milliseconds), y is %s\n",
        un);
    for (i = 0; i < n; i++) {
        TxtUd u;
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        if (evs[i].ph != ph) {
            continue;
        }
        u.key = key;
        u.t = evs[i].t;
        tok_walk(evs[i].msg,
            txt_cb, &u);
    }
    {
        const char *ext =
            golden_ext(key);
        double v[3] = {0.0, 0.0,
                       0.0};
        int f[3] = {0, 0, 0};
        int i = 0;

        if (ext != nullptr &&
            golden_fetch(key, ext,
                ph, evs, n, v, f)) {
            if (f[0] != 0 &&
                f[1] != 0 &&
                f[2] != 0) {
                printf("  refs:"
                    " p50=%g p99=%g"
                    " max=%g %s\n",
                    v[0], v[1],
                    v[2], un);
            } else {
                for (i = 0; i < 3;
                     i++) {
                    if (f[i] == 0) {
                        continue;
                    }
                    printf("  refs:"
                        " %s=%g"
                        " %s\n",
                        golden_tag(
                            i),
                        v[i], un);
                }
            }
        }
    }
}

// Prints one single-point status
// check for key in one phase:
// title plus value and stamp,
// never a graph.
static void text_check(
    const char *key, int ph,
    const char *pname, Ev *evs,
    size_t n) {
    char tb[128];
    size_t m = 0;
    double v = 0.0;
    unsigned long long t = 0;
    if (pname == nullptr) {
        pname = "ph";
    }
    metric_title(key, tb,
        sizeof tb);
    key_phase_stats(key, ph,
        evs, n, &m, &v, &t);
    if (m != 1) {
        return;
    }
    printf("%s\n", tb);
    printf("  key: %s\n", key);
    printf("  phase: %s [%d]\n",
        pname, ph);
    printf("  value: %g at "
        "+%llums\n", v,
        (unsigned long long)t);
}

// Writes one status checklist item
// as the same four plain lines the
// text check prints: title, key,
// phase, then value and stamp. One
// item per key and phase.
static void html_check(FILE *f,
    const char *key, int ph,
    const char *pname, double v,
    unsigned long long t) {
    char tb[128];
    if (pname == nullptr) {
        pname = "ph";
    }
    metric_title(key, tb,
        sizeof tb);
    fputs("<pre><b>", f);
    html_esc(f, tb);
    fputs("</b>\n", f);
    fputs("  key: ", f);
    html_esc(f, key);
    fputc('\n', f);
    fputs("  phase: ", f);
    html_esc(f, pname);
    fprintf(f, " [%d]\n", ph);
    fprintf(f, "  value: %g at "
        "+%llums</pre>\n",
        v, t);
}

// Renders text flow, resource, and
// check sections: one plot per key
// and phase with 2+ points, one
// checklist item per singleton
// phase. Whole-run series never
// render; delta flow keys graph
// alone when any exist while
// cumulative keys hide.
static void render_text_graphs(
    Ev *evs, size_t n) {
    Mkey fk[HtmlKeys];
    Mkey rk[HtmlKeys];
    int nf = 0;
    int nr = 0;
    int fdrop = 0;
    int rdrop = 0;
    KeyUd ku;
    size_t i = 0;
    int j = 0;
    int nm = 0;
    int nc = 0;
    int delta = 0;
    int phs[PhScan];
    int no_ticks = 0;
    (void)fdrop;
    (void)rdrop;
    ku.tk = fk;
    ku.nt = &nf;
    ku.cls = 3;
    ku.drop = &fdrop;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg, key_cb,
            &ku);
    }
    ku.tk = rk;
    ku.nt = &nr;
    ku.drop = &rdrop;
    ku.cls = 1;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg, key_cb,
            &ku);
    }
    ku.cls = 2;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg, key_cb,
            &ku);
    }
    no_ticks = has_secs(evs, n) ?
        1 : 0;
    cull_keys(fk, &nf, no_ticks);
    cull_keys(rk, &nr, no_ticks);
    delta = has_delta_keys(fk, nf);
    fputs("flow rate graphs show "
        "packet counters over\n",
        stdout);
    fputs("time.\n", stdout);
    fputs("Split by phase: one plot "
        "per key and phase; delta "
        "keys graph when present.\n",
        stdout);
    fputs("flow rate:\n", stdout);
    nm = 0;
    for (j = 0; j < nf; j++) {
        size_t w = 0;
        double wv = 0.0;
        unsigned long long wt = 0;
        int np = 0;
        int q = 0;
        if (delta != 0 &&
            !ends_with(fk[j].k,
                ".delta")) {
            continue;
        }
        key_stats(fk[j].k, evs, n,
            &w, &wv, &wt);
        if (w == 0) {
            continue;
        }
        np = key_phases(fk[j].k,
            evs, n, phs, PhScan);
        for (q = 0; q < np; q++) {
            size_t m = 0;
            double v = 0.0;
            unsigned long long t = 0;
            key_phase_stats(
                fk[j].k, phs[q],
                evs, n, &m, &v, &t);
            if (m >= 2) {
                nm++;
            }
        }
    }
    if (nm == 0) {
        fputs("no counter metrics "
            "present.\n", stdout);
    } else {
        for (j = 0; j < nf; j++) {
            int np = 0;
            int q = 0;
            if (delta != 0 &&
                !ends_with(fk[j].k,
                    ".delta")) {
                continue;
            }
            np = key_phases(
                fk[j].k, evs, n,
                phs, PhScan);
            for (q = 0; q < np;
                q++) {
                size_t m = 0;
                double v = 0.0;
                unsigned long long t =
                    0;
                char pn[PhName];
                key_phase_stats(
                    fk[j].k, phs[q],
                    evs, n, &m, &v,
                    &t);
                if (m < 2) {
                    continue;
                }
                phase_name(evs, n,
                    phs[q], pn,
                    sizeof pn);
                text_graph(fk[j].k,
                    phs[q], pn, evs,
                    n);
            }
        }
    }
    fputs("resources graphs show "
        "memory and cpu use over\n",
        stdout);
    fputs("time.\n", stdout);
    fputs("Split by phase like flow "
        "rate.\n", stdout);
    fputs("maxrss.mb is harness "
        "peak RAM; peakrss.mb, "
        "rss.bytes are per-node "
        "RAM; rchar, wchar are "
        "per-node process IO "
        "(disk plus net); flow "
        "keys are TUN network.\n",
        stdout);
    fputs("resources:\n", stdout);
    nm = 0;
    for (j = 0; j < nr; j++) {
        size_t w = 0;
        double wv = 0.0;
        unsigned long long wt = 0;
        int np = 0;
        int q = 0;
        key_stats(rk[j].k, evs, n,
            &w, &wv, &wt);
        if (w == 0) {
            continue;
        }
        np = key_phases(rk[j].k,
            evs, n, phs, PhScan);
        for (q = 0; q < np; q++) {
            size_t m = 0;
            double v = 0.0;
            unsigned long long t = 0;
            key_phase_stats(
                rk[j].k, phs[q],
                evs, n, &m, &v, &t);
            if (m >= 2) {
                nm++;
            }
        }
    }
    if (nm == 0) {
        fputs("no resource metrics "
            "present.\n", stdout);
    } else {
        for (j = 0; j < nr; j++) {
            int np = 0;
            int q = 0;
            np = key_phases(
                rk[j].k, evs, n,
                phs, PhScan);
            for (q = 0; q < np;
                q++) {
                size_t m = 0;
                double v = 0.0;
                unsigned long long t =
                    0;
                char pn[PhName];
                key_phase_stats(
                    rk[j].k, phs[q],
                    evs, n, &m, &v,
                    &t);
                if (m < 2) {
                    continue;
                }
                phase_name(evs, n,
                    phs[q], pn,
                    sizeof pn);
                text_graph(rk[j].k,
                    phs[q], pn, evs,
                    n);
            }
        }
    }
    fputs("status checks:\n",
        stdout);
    nc = 0;
    for (j = 0; j < nf; j++) {
        int np = 0;
        int q = 0;
        if (delta != 0 &&
            !ends_with(fk[j].k,
                ".delta")) {
            continue;
        }
        np = key_phases(fk[j].k,
            evs, n, phs, PhScan);
        for (q = 0; q < np; q++) {
            size_t m = 0;
            double v = 0.0;
            unsigned long long t = 0;
            key_phase_stats(
                fk[j].k, phs[q],
                evs, n, &m, &v, &t);
            if (m == 1) {
                nc++;
            }
        }
    }
    for (j = 0; j < nr; j++) {
        int np = 0;
        int q = 0;
        np = key_phases(rk[j].k,
            evs, n, phs, PhScan);
        for (q = 0; q < np; q++) {
            size_t m = 0;
            double v = 0.0;
            unsigned long long t = 0;
            key_phase_stats(
                rk[j].k, phs[q],
                evs, n, &m, &v, &t);
            if (m == 1) {
                nc++;
            }
        }
    }
    if (nc == 0) {
        fputs("no status checks "
            "present.\n", stdout);
    } else {
        for (j = 0; j < nf; j++) {
            int np = 0;
            int q = 0;
            if (delta != 0 &&
                !ends_with(fk[j].k,
                    ".delta")) {
                continue;
            }
            np = key_phases(
                fk[j].k, evs, n,
                phs, PhScan);
            for (q = 0; q < np;
                q++) {
                char pn[PhName];
                phase_name(evs, n,
                    phs[q], pn,
                    sizeof pn);
                text_check(fk[j].k,
                    phs[q], pn, evs,
                    n);
            }
        }
        for (j = 0; j < nr; j++) {
            int np = 0;
            int q = 0;
            np = key_phases(
                rk[j].k, evs, n,
                phs, PhScan);
            for (q = 0; q < np;
                q++) {
                char pn[PhName];
                phase_name(evs, n,
                    phs[q], pn,
                    sizeof pn);
                text_check(rk[j].k,
                    phs[q], pn, evs,
                    n);
            }
        }
    }
    render_text_bench(evs, n);
}

// Y pixel of v in [mn,mx] on a
// plot with top pad 12: flat sets
// render mid-plot.
static double plot_y(double v,
    double mn, double mx, int flat,
    double inner) {
    double lo = 0.0;
    double hi = 0.0;
    if (flat != 0) {
        return (12.0 + inner /
            2.0);
    }
    lo = mn - (mx - mn) * 0.05;
    hi = mx + (mx - mn) * 0.05;
    return (12.0 + inner *
        (1.0 - (v - lo) /
        (hi - lo)));
}

// State for HTML point printing:
// key plus the stamp of the event
// now walking and the out file.
typedef struct {
    const char *key;
    unsigned long long t;
    FILE *f;
} HtmlPtUd;

// Prints one numeric point for
// HtmlPtUd as an escaped text graph
// row; mirrors txt_cb row for row,
// so HTML pre blocks carry the same
// words as the text report.
static void html_pt_cb(
    const char *tok, size_t tl,
    const char *eq, void *ud) {
    HtmlPtUd *u = ud;
    size_t kl = 0;
    size_t vl = 0;
    char vb[32];
    double v = 0.0;
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (strlen(u->key) != kl) {
        return;
    }
    if (memcmp(u->key, tok, kl)
        != 0) {
        return;
    }
    vl = tl - kl - 1;
    if (vl == 0 ||
        vl >= sizeof vb) {
        return;
    }
    memcpy(vb, eq + 1, vl);
    vb[vl] = 0;
    if (!num_of(vb, &v)) {
        return;
    }
    fprintf(u->f, "  +%llums ",
        u->t);
    html_esc(u->f, vb);
    fputc('\n', u->f);
}

// Writes one value-vs-time plot
// for key in one phase: filled
// area when area is set, else a
// line. A pre block first carries
// the same title, key, phase, axes,
// and point rows the text graph
// prints; the SVG follows inline.
// Empty sets note and return. One
// point renders as a status check,
// never a graph. Whole-run plots
// never render; callers fan out by
// phase instead.
static void html_plot(FILE *f,
    const char *key, int ph,
    const char *pname, Ev *evs,
    size_t n,
    unsigned long long tmax,
    int area) {
    unsigned long long *ts =
        nullptr;
    double *ys = nullptr;
    PtUd u;
    size_t m = 0;
    size_t i = 0;
    double mn = 0.0;
    double mx = 0.0;
    double inner = 0.0;
    double base = 0.0;
    const char *cc = "#15803d";
    const char *un = nullptr;
    char tb[KeyCap];
    int flat = 0;
    int x1 = 0;
    if (n == 0) {
        return;
    }
    if (pname == nullptr) {
        pname = "ph";
    }
    un = metric_unit(key);
    metric_title(key, tb,
        sizeof tb);
    ts = malloc(n * sizeof *ts);
    ys = malloc(n * sizeof *ys);
    if (ts == nullptr ||
        ys == nullptr) {
        free(ts);
        free(ys);
        fputs("<pre>no memory for "
            "plot.</pre>\n", f);
        return;
    }
    u.key = key;
    u.ts = ts;
    u.ys = ys;
    u.m = &m;
    u.cap = n;
    u.t = 0;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        if (evs[i].ph != ph) {
            continue;
        }
        u.t = evs[i].t;
        tok_walk(evs[i].msg, pt_cb,
            &u);
    }
    if (m == 0) {
        free(ts);
        free(ys);
        fputs("<pre><b>", f);
        html_esc(f, tb);
        fputs("</b>\n", f);
        fputs("  key: ", f);
        html_esc(f, key);
        fputc('\n', f);
        fputs("  phase: ", f);
        html_esc(f, pname);
        fprintf(f, " [%d]\n", ph);
        fputs("no numeric "
            "values.</pre>\n", f);
        return;
    }
    if (m == 1) {
        double v = ys[0];
        unsigned long long t = ts[0];
        free(ts);
        free(ys);
        html_check(f, key, ph,
            pname, v, t);
        return;
    }
    mn = ys[0];
    mx = ys[0];
    for (i = 1; i < m; i++) {
        if (ys[i] < mn) {
            mn = ys[i];
        }
        if (ys[i] > mx) {
            mx = ys[i];
        }
    }
    fputs("<pre><b>", f);
    html_esc(f, tb);
    fputs("</b>\n", f);
    fputs("  key: ", f);
    html_esc(f, key);
    fputc('\n', f);
    fputs("  phase: ", f);
    html_esc(f, pname);
    fprintf(f, " [%d]\n", ph);
    fputs("  axes: x is time "
        "(milliseconds), y is ", f);
    html_esc(f, un);
    fputc('\n', f);
    fputs("</pre>\n", f);
    fprintf(f, "<details>"
        "<summary>%zu samples"
        "</summary>\n<pre>\n",
        m);
    for (i = 0; i < n; i++) {
        HtmlPtUd pu;
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        if (evs[i].ph != ph) {
            continue;
        }
        pu.key = key;
        pu.t = evs[i].t;
        pu.f = f;
        tok_walk(evs[i].msg,
            html_pt_cb, &pu);
    }
    fputs("</pre></details>\n",
        f);
    x1 = HtmlW - HtmlPadR;
    inner = (double)(HtmlPlotH -
        12 - 24);
    base = 12.0 + inner;
    flat = (mx == mn);
    (void)tmax;
    if (area != 0) {
        cc = "#1d4ed8";
    }
    fprintf(f, "<svg width=\"%d\" "
        "height=\"%d\" "
        "role=\"img\">\n", HtmlW,
        HtmlPlotH);
    if (area != 0) {
        fputs("<polygon points=\"",
            f);
    } else {
        fputs("<polyline points=\"",
            f);
    }
    for (i = 0; i < m; i++) {
        double xx = series_x(
            ts[i], ts[0],
            ts[m - 1]);
        double yy = plot_y(ys[i],
            mn, mx, flat, inner);
        fprintf(f, "%.1f,%.1f ",
            xx, yy);
    }
    if (area != 0) {
        double xa = series_x(
            ts[m - 1], ts[0],
            ts[m - 1]);
        double xb = series_x(
            ts[0], ts[0],
            ts[m - 1]);
        fprintf(f, "%.1f,%.1f "
            "%.1f,%.1f\" ", xa,
            base, xb, base);
        fputs("fill=\"#bfdbfe\" "
            "stroke=\"#1d4ed8\"/>\n",
            f);
    } else {
        fputs("\" fill=\"none\" "
            "stroke=\"#15803d\" "
            "stroke-width=\"2\"/>\n",
            f);
    }
    for (i = 0; i < m; i++) {
        double xx = series_x(
            ts[i], ts[0],
            ts[m - 1]);
        double yy = plot_y(ys[i],
            mn, mx, flat, inner);
        // Small dots past ~160
        // points: 830 px holds 5 px
        // dots without merging.
        double rr = m > 160 ? 1.0
                            : 2.5;
        fprintf(f, "<circle cx=\""
            "%.1f\" cy=\"%.1f\" "
            "r=\"%.1f\" "
            "fill=\"%s\"/>\n", xx,
            yy, rr, cc);
    }
    {
        const char *ext =
            golden_ext(key);
        double rv[3] = {0.0, 0.0,
                        0.0};
        int rf[3] = {0, 0, 0};
        int g = 0;

        if (ext != nullptr &&
            golden_fetch(key, ext,
                ph, evs, n, rv,
                rf)) {
            for (g = 0; g < 3;
                 g++) {
                double yy = 0.0;
                double top = 12.0;
                if (rf[g] == 0) {
                    continue;
                }
                yy = plot_y(rv[g],
                    mn, mx, flat,
                    inner);
                if (yy < top) {
                    yy = top;
                }
                if (yy > base) {
                    yy = base;
                }
                fprintf(f,
                    "<line x1=\"%d\" "
                    "y1=\"%.1f\" "
                    "x2=\"%d\" "
                    "y2=\"%.1f\" "
                    "stroke=\"%s\" "
                    "stroke-dasharray"
                    "=\"6,3\" "
                    "stroke-width"
                    "=\"2\">\n",
                    HtmlPadL, yy, x1,
                    yy,
                    golden_color(
                        g));
                fprintf(f,
                    "<title>%s %g ",
                    golden_tag(g),
                    rv[g]);
                html_esc(f, un);
                fputs("</title>\n",
                    f);
                fputs("</line>\n",
                    f);
            }
            for (g = 0; g < 3;
                 g++) {
                double ly =
                    30.0 +
                    (double)g *
                        14.0;
                if (rf[g] == 0) {
                    continue;
                }
                fprintf(f,
                    "<line x1=\"%d\" "
                    "y1=\"%.1f\" "
                    "x2=\"%d\" "
                    "y2=\"%.1f\" "
                    "stroke=\"%s\" "
                    "stroke-width"
                    "=\"3\"/>\n",
                    HtmlPadL, ly,
                    HtmlPadL + 24,
                    ly,
                    golden_color(
                        g));
                fprintf(f,
                    "<text x=\"%d\" "
                    "y=\"%.1f\">%s "
                    "%g ", HtmlPadL +
                        28,
                    ly + 4.0,
                    golden_tag(g),
                    rv[g]);
                html_esc(f, un);
                fputs("</text>\n",
                    f);
            }
        }
    }
    fprintf(f, "<text x=\"%d\" "
        "y=\"20\" "
        "text-anchor=\"end\">%g ",
        HtmlPadL - HtmlLabGap, mx);
    html_esc(f, un);
    fputs("</text>\n", f);
    fprintf(f, "<text x=\"%d\" "
        "y=\"%d\" "
        "text-anchor=\"end\">%g ",
        HtmlPadL - HtmlLabGap,
        (int)base, mn);
    html_esc(f, un);
    fputs("</text>\n", f);
    fprintf(f, "<text x=\"%d\" "
        "y=\"%d\" ", x1,
        HtmlPlotH - 6);
    fputs("text-anchor=\"end\">",
        f);
    fprintf(f, "%llu ", ts[m - 1] -
        ts[0]);
    html_esc(f, "time "
        "(milliseconds), span");
    fputs("</text>\n", f);
    fputs("</svg>\n", f);
    free(ts);
    free(ys);
}

// Buckets in a histogram view.
static constexpr int BenchHistN =
    10;

// Cap on bench bases kept; extra
// notes and skips like keys.
static constexpr int BenchCap =
    16;

// True when k names bench data.
static bool is_bench(const char *k) {
    if (k == nullptr) {
        return (false);
    }
    if (strncmp(k, "bench.", 6)
        == 0) {
        return (true);
    }
    return (false);
}

// True when leaf is an old bench
// leaf, kept for gating only.
static bool bench_old(
    const char *lf) {
    if (lf == nullptr) {
        return (false);
    }
    if (streq(lf, "count")) {
        return (true);
    }
    if (streq(lf, "loss")) {
        return (true);
    }
    if (streq(lf, "target")) {
        return (true);
    }
    if (streq(lf, "overrun")) {
        return (true);
    }
    if (streq(lf, "p50.ms")) {
        return (true);
    }
    if (streq(lf, "p99.ms")) {
        return (true);
    }
    if (streq(lf, "max.ms")) {
        return (true);
    }
    return (false);
}

// True when leaf is a ramp leaf;
// the 11 summary leaves only.
static bool bench_ramp_leaf(
    const char *lf) {
    if (lf == nullptr) {
        return (false);
    }
    if (bench_old(lf)) {
        return (true);
    }
    if (streq(lf, "rate_ach")) {
        return (true);
    }
    if (streq(lf, "p50.us")) {
        return (true);
    }
    if (streq(lf, "p99.us")) {
        return (true);
    }
    if (streq(lf, "max.us")) {
        return (true);
    }
    return (false);
}

// Copies ramp base bench.A.B.ramp.I
// of k into out; false when k is
// not a ramp summary leaf.
static bool bench_ramp_base(
    const char *k, char *out,
    size_t n) {
    const char *rp = nullptr;
    const char *ip = nullptr;
    const char *e = nullptr;
    const char *lf = nullptr;
    size_t bl = 0;
    if (k == nullptr) {
        return (false);
    }
    if (out == nullptr) {
        return (false);
    }
    if (n == 0) {
        return (false);
    }
    if (!is_bench(k)) {
        return (false);
    }
    rp = strstr(k, ".ramp.");
    if (rp == nullptr) {
        return (false);
    }
    ip = rp + 6;
    if (*ip == 0) {
        return (false);
    }
    e = ip;
    while (*e >= '0' &&
        *e <= '9') {
        e++;
    }
    if (e == ip) {
        return (false);
    }
    if (*e != '.') {
        return (false);
    }
    lf = e + 1;
    if (!bench_ramp_leaf(lf)) {
        return (false);
    }
    bl = (size_t)(e - k);
    if (bl == 0 || bl >= n) {
        return (false);
    }
    if (bl >= (size_t)KeyCap) {
        return (false);
    }
    memcpy(out, k, bl);
    out[bl] = 0;
    return (true);
}

// Copies ceiling base bench.A.B
// .ceiling of k into out; false
// when k is not ceiling.rate.
static bool bench_ceiling_base(
    const char *k, char *out,
    size_t n) {
    size_t kl = 0;
    size_t bl = 0;
    if (k == nullptr) {
        return (false);
    }
    if (out == nullptr) {
        return (false);
    }
    if (n == 0) {
        return (false);
    }
    if (!is_bench(k)) {
        return (false);
    }
    if (!ends_with(k,
        ".ceiling.rate")) {
        return (false);
    }
    kl = strlen(k);
    if (kl <= 5) {
        return (false);
    }
    bl = kl - 5;
    if (bl == 0 || bl >= n) {
        return (false);
    }
    if (bl >= (size_t)KeyCap) {
        return (false);
    }
    memcpy(out, k, bl);
    out[bl] = 0;
    return (true);
}

// True when base is a ceiling base.
static bool is_ceiling_base(
    const char *b) {
    if (b == nullptr) {
        return (false);
    }
    return (ends_with(b,
        ".ceiling"));
}

// Copies bench base bench.A.B of k
// into out; false when no base.
static bool bench_base(
    const char *k, char *out,
    size_t n) {
    const char *p = nullptr;
    const char *d1 = nullptr;
    const char *d2 = nullptr;
    size_t bl = 0;
    if (k == nullptr) {
        return (false);
    }
    if (out == nullptr) {
        return (false);
    }
    if (n == 0) {
        return (false);
    }
    if (!is_bench(k)) {
        return (false);
    }
    p = k + 6;
    d1 = strchr(p, '.');
    if (d1 == nullptr) {
        return (false);
    }
    d2 = strchr(d1 + 1, '.');
    if (d2 == nullptr) {
        return (false);
    }
    bl = (size_t)(d2 - k);
    if (bl == 0 || bl >= n) {
        return (false);
    }
    if (bl >= (size_t)KeyCap) {
        return (false);
    }
    memcpy(out, k, bl);
    out[bl] = 0;
    return (true);
}

// Bucket index after .hist. in k;
// -1 when none or bad. Skips
// .hist_us. keys so us counts
// never pollute ms buckets.
static int bench_bucket(
    const char *k) {
    const char *h = nullptr;
    char *end = nullptr;
    long v = 0;
    if (k == nullptr) {
        return (-1);
    }
    h = strstr(k, ".hist.");
    if (h == nullptr) {
        return (-1);
    }
    if (strncmp(h, ".hist_us.",
            9) == 0) {
        return (-1);
    }
    h += 6;
    if (*h == 0) {
        return (-1);
    }
    v = strtol(h, &end, 10);
    if (end == h) {
        return (-1);
    }
    if (*end != 0) {
        return (-1);
    }
    if (v < 0 || v > 1000000) {
        return (-1);
    }
    return ((int)v);
}

// Bucket index after .hist_us.;
// -1 when none or bad.
static int bench_bucket_us(
    const char *k) {
    const char *h = nullptr;
    char *end = nullptr;
    long v = 0;
    if (k == nullptr) {
        return (-1);
    }
    h = strstr(k, ".hist_us.");
    if (h == nullptr) {
        return (-1);
    }
    h += 9;
    if (*h == 0) {
        return (-1);
    }
    v = strtol(h, &end, 10);
    if (end == h) {
        return (-1);
    }
    if (*end != 0) {
        return (-1);
    }
    if (v < 0 || v > 1000000) {
        return (-1);
    }
    return ((int)v);
}

// Copies stem before .hist. of k
// into out; false when no stem.
// Skips .hist_us. keys.
static bool hist_stem(const char *k,
    char *out, size_t n) {
    const char *h = nullptr;
    size_t bl = 0;
    if (k == nullptr) {
        return (false);
    }
    if (out == nullptr) {
        return (false);
    }
    if (n == 0) {
        return (false);
    }
    h = strstr(k, ".hist.");
    if (h == nullptr) {
        return (false);
    }
    if (strncmp(h, ".hist_us.",
            9) == 0) {
        return (false);
    }
    bl = (size_t)(h - k);
    if (bl == 0 || bl >= n) {
        return (false);
    }
    if (bl >= (size_t)KeyCap) {
        return (false);
    }
    memcpy(out, k, bl);
    out[bl] = 0;
    return (true);
}

// Copies stem before .hist_us.
// into out; false when no stem.
static bool hist_us_stem(
    const char *k, char *out,
    size_t n) {
    const char *h = nullptr;
    size_t bl = 0;
    if (k == nullptr) {
        return (false);
    }
    if (out == nullptr) {
        return (false);
    }
    if (n == 0) {
        return (false);
    }
    h = strstr(k, ".hist_us.");
    if (h == nullptr) {
        return (false);
    }
    bl = (size_t)(h - k);
    if (bl == 0 || bl >= n) {
        return (false);
    }
    if (bl >= (size_t)KeyCap) {
        return (false);
    }
    memcpy(out, k, bl);
    out[bl] = 0;
    return (true);
}

// Label for bucket b into out;
// ms edges, not just an index.
static void hist_label(int b,
    char *out, size_t n) {
    static const char *ms[10] = {
        "0-1ms",   "1-2ms",
        "2-5ms",   "5-10ms",
        "10-20ms", "20-50ms",
        "50-100ms", "100-200ms",
        "200-500ms", "500ms+",
    };

    if (out == nullptr) {
        return;
    }
    if (n == 0) {
        return;
    }
    if (b < 0 || b >= 10) {
        snprintf(out, n, "%s",
            "unknown");
        return;
    }
    snprintf(out, n, "%s", ms[b]);
}

// Label for us bucket b into out;
// us edges for sub-ms spread.
static void hist_us_label(int b,
    char *out, size_t n) {
    static const char *us[10] = {
        "0-100us", "100-500us",
        "0.5-1ms", "1-5ms",
        "5-10ms", "10-20ms",
        "20-50ms", "50-100ms",
        "100-500ms", "500ms+",
    };

    if (out == nullptr) {
        return;
    }
    if (n == 0) {
        return;
    }
    if (b < 0 || b >= 10) {
        snprintf(out, n, "%s",
            "unknown");
        return;
    }
    snprintf(out, n, "%s", us[b]);
}

// Scan state for new bench keys.
typedef struct {
    int hit;
} BenchNewUd;

// Marks hit for one new bench key;
// suffix shapes hit past any
// prefix; old bench leaves miss.
static void bench_new_cb(
    const char *tok, size_t tl,
    const char *eq, void *ud) {
    BenchNewUd *u = ud;
    (void)tl;
    char kb[KeyCap];
    size_t kl = 0;
    char base[KeyCap];
    const char *lf = nullptr;
    if (u->hit != 0) {
        return;
    }
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (kl == 0 ||
        kl >= sizeof kb) {
        return;
    }
    memcpy(kb, tok, kl);
    kb[kl] = 0;
    if (ends_with(kb, ".lat")) {
        u->hit = 1;
        return;
    }
    if (strstr(kb, ".hist.") !=
        nullptr) {
        u->hit = 1;
        return;
    }
    if (ends_with(kb,
        ".rate_ach")) {
        u->hit = 1;
        return;
    }
    if (!is_bench(kb)) {
        return;
    }
    if (!bench_base(kb, base,
        sizeof base)) {
        return;
    }
    lf = kb + strlen(base);
    if (*lf == '.') {
        lf++;
    }
    if (bench_old(lf)) {
        return;
    }
    u->hit = 1;
}

// True when any new bench key
// exists; old only reads false.
static bool bench_has_new(Ev *evs,
    size_t n) {
    BenchNewUd u;
    size_t i = 0;
    u.hit = 0;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg,
            bench_new_cb, &u);
        if (u.hit != 0) {
            return (true);
        }
    }
    return (false);
}

// Base table build state.
typedef struct {
    char (*bs)[KeyCap];
    int *nb;
    int *drop;
} BenchUd;

// Adds one bench base; ramp plus
// ceiling bases fan out first,
// past cap it counts drops.
static void bench_base_cb(
    const char *tok, size_t tl,
    const char *eq, void *ud) {
    BenchUd *u = ud;
    (void)tl;
    char kb[KeyCap];
    size_t kl = 0;
    char base[KeyCap];
    int i = 0;
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (kl == 0 ||
        kl >= sizeof kb) {
        return;
    }
    memcpy(kb, tok, kl);
    kb[kl] = 0;
    if (!is_bench(kb)) {
        return;
    }
    if (bench_ramp_base(kb, base,
        sizeof base)) {
        for (i = 0; i < *u->nb;
            i++) {
            if (strcmp(u->bs[i],
                base) == 0) {
                return;
            }
        }
        if (*u->nb >= BenchCap) {
            (*u->drop)++;
            return;
        }
        memcpy(u->bs[*u->nb],
            base,
            strlen(base) + 1);
        (*u->nb)++;
        return;
    }
    if (bench_ceiling_base(kb,
        base, sizeof base)) {
        for (i = 0; i < *u->nb;
            i++) {
            if (strcmp(u->bs[i],
                base) == 0) {
                return;
            }
        }
        if (*u->nb >= BenchCap) {
            (*u->drop)++;
            return;
        }
        memcpy(u->bs[*u->nb],
            base,
            strlen(base) + 1);
        (*u->nb)++;
        return;
    }
    if (!bench_base(kb, base,
        sizeof base)) {
        return;
    }
    for (i = 0; i < *u->nb; i++) {
        if (strcmp(u->bs[i],
            base) == 0) {
            return;
        }
    }
    if (*u->nb >= BenchCap) {
        (*u->drop)++;
        return;
    }
    memcpy(u->bs[*u->nb], base,
        strlen(base) + 1);
    (*u->nb)++;
}

// Collects distinct bench bases;
// returns the kept count.
static int bench_bases(Ev *evs,
    size_t n,
    char (*bs)[KeyCap]) {
    BenchUd u;
    int nb = 0;
    int drop = 0;
    size_t i = 0;
    u.bs = bs;
    u.nb = &nb;
    u.drop = &drop;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg,
            bench_base_cb, &u);
    }
    (void)drop;
    return (nb);
}

// Adds one hist stem before .hist.;
// any prefix hits, suffix shape
// only, never a full key list.
static void hist_stem_cb(
    const char *tok, size_t tl,
    const char *eq, void *ud) {
    BenchUd *u = ud;
    (void)tl;
    char kb[KeyCap];
    size_t kl = 0;
    char stem[KeyCap];
    int i = 0;
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (kl == 0 ||
        kl >= sizeof kb) {
        return;
    }
    memcpy(kb, tok, kl);
    kb[kl] = 0;
    if (!hist_stem(kb, stem,
        sizeof stem)) {
        return;
    }
    for (i = 0; i < *u->nb; i++) {
        if (strcmp(u->bs[i],
            stem) == 0) {
            return;
        }
    }
    if (*u->nb >= BenchCap) {
        (*u->drop)++;
        return;
    }
    memcpy(u->bs[*u->nb], stem,
        strlen(stem) + 1);
    (*u->nb)++;
}

// Collects distinct hist stems;
// returns the kept count.
static int hist_stems(Ev *evs,
    size_t n,
    char (*bs)[KeyCap]) {
    BenchUd u;
    int nb = 0;
    int drop = 0;
    size_t i = 0;
    u.bs = bs;
    u.nb = &nb;
    u.drop = &drop;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg,
            hist_stem_cb, &u);
    }
    (void)drop;
    return (nb);
}

// Adds one us hist stem before
// .hist_us.; same dedup shape.
static void hist_us_stem_cb(
    const char *tok, size_t tl,
    const char *eq, void *ud) {
    BenchUd *u = ud;
    (void)tl;
    char kb[KeyCap];
    size_t kl = 0;
    char stem[KeyCap];
    int i = 0;
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (kl == 0 ||
        kl >= sizeof kb) {
        return;
    }
    memcpy(kb, tok, kl);
    kb[kl] = 0;
    if (!hist_us_stem(kb, stem,
        sizeof stem)) {
        return;
    }
    for (i = 0; i < *u->nb; i++) {
        if (strcmp(u->bs[i],
            stem) == 0) {
            return;
        }
    }
    if (*u->nb >= BenchCap) {
        (*u->drop)++;
        return;
    }
    memcpy(u->bs[*u->nb], stem,
        strlen(stem) + 1);
    (*u->nb)++;
}

// Collects distinct us hist stems.
static int hist_us_stems(Ev *evs,
    size_t n,
    char (*bs)[KeyCap]) {
    BenchUd u;
    int nb = 0;
    int drop = 0;
    size_t i = 0;
    u.bs = bs;
    u.nb = &nb;
    u.drop = &drop;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg,
            hist_us_stem_cb, &u);
    }
    (void)drop;
    return (nb);
}

// Lat table build state.
typedef struct {
    Mkey *tk;
    int *nt;
    int *drop;
} LatUd;

// Adds one lat key by suffix; any
// prefix hits so flow fallback
// stays while bench plots it.
static void lat_cb(const char *tok,
    size_t tl, const char *eq,
    void *ud) {
    LatUd *u = ud;
    char kb[KeyCap];
    size_t kl = 0;
    int i = 0;
    (void)tl;
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (kl == 0 ||
        kl >= sizeof kb) {
        return;
    }
    memcpy(kb, tok, kl);
    kb[kl] = 0;
    if (!ends_with(kb, ".lat") &&
        !ends_with(kb, ".lat_us")) {
        return;
    }
    for (i = 0; i < *u->nt; i++) {
        if (strcmp(u->tk[i].k,
            kb) == 0) {
            return;
        }
    }
    if (*u->nt >= BenchCap) {
        (*u->drop)++;
        return;
    }
    memcpy(u->tk[*u->nt].k, kb,
        kl + 1);
    (*u->nt)++;
}

// Collects distinct lat keys by
// suffix; returns the kept count.
static int bench_lats(Ev *evs,
    size_t n, Mkey *tk) {
    LatUd u;
    int nt = 0;
    int drop = 0;
    size_t i = 0;
    u.tk = tk;
    u.nt = &nt;
    u.drop = &drop;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg,
            lat_cb, &u);
    }
    (void)drop;
    return (nt);
}

// Value of base.leaf in phase ph;
// found flags presence.
static void bench_val(
    const char *base,
    const char *leaf, int ph,
    Ev *evs, size_t n, double *v,
    int *found) {
    char full[KeyCap];
    size_t m = 0;
    double vv = 0.0;
    unsigned long long t = 0;
    int r = 0;
    *found = 0;
    *v = 0.0;
    r = snprintf(full,
        sizeof full, "%s.%s", base,
        leaf);
    if (r < 0 ||
        (size_t)r >= sizeof full) {
        return;
    }
    key_phase_stats(full, ph,
        evs, n, &m, &vv, &t);
    if (m == 0) {
        return;
    }
    *v = vv;
    *found = 1;
}

// Prints one bench summary table
// for phase ph; missing leaves
// read as dash, never a crash.
// Ramp rows reuse this table, one
// row per ramp index; ceiling rows
// show rate in the rate column.
static void text_bench_tab(
    Ev *evs, size_t n, int ph,
    const char *pname,
    char (*bs)[KeyCap], int nb) {
    int i = 0;
    printf("phase %s [%d]:\n",
        pname, ph);
    printf("  step count loss "
        "p50 p99 max target "
        "rate_ach overrun p50.us "
        "p99.us max.us scrape\n");
    for (i = 0; i < nb; i++) {
        if (!bench_tab_hit(bs[i],
            ph, evs, n)) {
            continue;
        }
        double c = 0.0;
        double l = 0.0;
        double p50 = 0.0;
        double p99 = 0.0;
        double mx = 0.0;
        double tg = 0.0;
        double ra = 0.0;
        double ov = 0.0;
        double u50 = 0.0;
        double u99 = 0.0;
        double umx = 0.0;
        double sok = 0.0;
        int fc = 0;
        int fl = 0;
        int f50 = 0;
        int f99 = 0;
        int fmx = 0;
        int ftg = 0;
        int fra = 0;
        int fov = 0;
        int fu50 = 0;
        int fu99 = 0;
        int fumx = 0;
        int fsok = 0;
        char cb[32];
        char lb[32];
        char b50[32];
        char b99[32];
        char bmx[32];
        char btg[32];
        char bra[32];
        char bov[32];
        char bu50[32];
        char bu99[32];
        char bumx[32];
        char bsok[32];
        if (is_ceiling_base(bs[i])) {
            bench_val(bs[i], "rate",
                ph, evs, n, &ra,
                &fra);
        } else {
            bench_val(bs[i], "count",
                ph, evs, n, &c, &fc);
            bench_val(bs[i], "loss",
                ph, evs, n, &l, &fl);
            bench_val(bs[i], "p50.ms",
                ph, evs, n, &p50,
                &f50);
            bench_val(bs[i], "p99.ms",
                ph, evs, n, &p99,
                &f99);
            bench_val(bs[i], "max.ms",
                ph, evs, n, &mx, &fmx);
            bench_val(bs[i], "p50.us",
                ph, evs, n, &u50,
                &fu50);
            bench_val(bs[i], "p99.us",
                ph, evs, n, &u99,
                &fu99);
            bench_val(bs[i], "max.us",
                ph, evs, n, &umx,
                &fumx);
            bench_val(bs[i],
                "scrape_ok", ph, evs, n,
                &sok, &fsok);
            bench_val(bs[i], "target",
                ph, evs, n, &tg, &ftg);
            bench_val(bs[i],
                "rate_ach", ph, evs, n,
                &ra, &fra);
            bench_val(bs[i], "overrun",
                ph, evs, n, &ov, &fov);
        }
        if (fc != 0) {
            snprintf(cb,
                sizeof cb, "%g", c);
        } else {
            snprintf(cb,
                sizeof cb, "%s",
                "-");
        }
        if (fl != 0) {
            snprintf(lb,
                sizeof lb, "%g", l);
        } else {
            snprintf(lb,
                sizeof lb, "%s",
                "-");
        }
        if (f50 != 0) {
            snprintf(b50,
                sizeof b50, "%g",
                p50);
        } else {
            snprintf(b50,
                sizeof b50, "%s",
                "-");
        }
        if (f99 != 0) {
            snprintf(b99,
                sizeof b99, "%g",
                p99);
        } else {
            snprintf(b99,
                sizeof b99, "%s",
                "-");
        }
        if (fmx != 0) {
            snprintf(bmx,
                sizeof bmx, "%g",
                mx);
        } else {
            snprintf(bmx,
                sizeof bmx, "%s",
                "-");
        }
        if (ftg != 0) {
            snprintf(btg,
                sizeof btg, "%g",
                tg);
        } else {
            snprintf(btg,
                sizeof btg, "%s",
                "-");
        }
        if (fra != 0) {
            snprintf(bra,
                sizeof bra, "%g",
                ra);
        } else {
            snprintf(bra,
                sizeof bra, "%s",
                "-");
        }
        if (fov != 0) {
            snprintf(bov,
                sizeof bov, "%g",
                ov);
        } else {
            snprintf(bov,
                sizeof bov, "%s",
                "-");
        }
        if (fu50 != 0) {
            snprintf(bu50,
                sizeof bu50, "%g",
                u50);
        } else {
            snprintf(bu50,
                sizeof bu50, "%s",
                "-");
        }
        if (fu99 != 0) {
            snprintf(bu99,
                sizeof bu99, "%g",
                u99);
        } else {
            snprintf(bu99,
                sizeof bu99, "%s",
                "-");
        }
        if (fumx != 0) {
            snprintf(bumx,
                sizeof bumx, "%g",
                umx);
        } else {
            snprintf(bumx,
                sizeof bumx, "%s",
                "-");
        }
        if (fsok != 0) {
            snprintf(bsok,
                sizeof bsok, "%g",
                sok);
        } else {
            snprintf(bsok,
                sizeof bsok, "%s",
                "-");
        }
        printf("  %s %s %s %s "
            "%s %s %s %s %s %s "
            "%s %s %s\n",
            bs[i], cb, lb, b50,
            b99, bmx, btg, bra,
            bov, bu50, bu99,
            bumx, bsok);
    }
}

// True when base has any summary
// value in phase ph; ceiling bases
// hit on rate only, ramp and old
// bases share the 8 leaves.
static bool bench_tab_hit(
    const char *base, int ph,
    Ev *evs, size_t n) {
    double v = 0.0;
    int f = 0;
    if (is_ceiling_base(base)) {
        bench_val(base, "rate", ph,
            evs, n, &v, &f);
        if (f != 0) {
            return (true);
        }
        return (false);
    }
    bench_val(base, "count", ph,
        evs, n, &v, &f);
    if (f != 0) {
        return (true);
    }
    bench_val(base, "loss", ph,
        evs, n, &v, &f);
    if (f != 0) {
        return (true);
    }
    bench_val(base, "p50.ms", ph,
        evs, n, &v, &f);
    if (f != 0) {
        return (true);
    }
    bench_val(base, "p99.ms", ph,
        evs, n, &v, &f);
    if (f != 0) {
        return (true);
    }
    bench_val(base, "max.ms", ph,
        evs, n, &v, &f);
    if (f != 0) {
        return (true);
    }
    bench_val(base, "target", ph,
        evs, n, &v, &f);
    if (f != 0) {
        return (true);
    }
    bench_val(base, "rate_ach",
        ph, evs, n, &v, &f);
    if (f != 0) {
        return (true);
    }
    bench_val(base, "overrun", ph,
        evs, n, &v, &f);
    if (f != 0) {
        return (true);
    }
    return (false);
}

// Collects phases holding bench
// data into phs; suffix shapes
// count past any prefix, so new
// emitter names still split.
// Returns count.
static int bench_phases(Ev *evs,
    size_t n, int *phs, int cap) {
    int np = 0;
    size_t i = 0;
    int k = 0;
    for (i = 0; i < n; i++) {
        int ph = 0;
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        ph = evs[i].ph;
        for (k = 0; k < np; k++) {
            if (phs[k] == ph) {
                break;
            }
        }
        if (k < np) {
            continue;
        }
        {
            const char *msg =
                evs[i].msg;
            const char *p = msg;
            int has = 0;
            while (*p != 0) {
                const char *e =
                    nullptr;
                const char *eq =
                    nullptr;
                const char *q =
                    nullptr;
                char kb[KeyCap];
                size_t kl = 0;
                while (*p == ' ' ||
                    *p == '\t') {
                    p++;
                }
                if (*p == 0) {
                    break;
                }
                e = p;
                while (*e != 0 &&
                    *e != ' ' &&
                    *e != '\t') {
                    e++;
                }
                for (q = p; q < e;
                    q++) {
                    if (*q == '=') {
                        eq = q;
                        break;
                    }
                }
                if (eq != nullptr) {
                    kl = (size_t)
                        (eq - p);
                    if (kl > 0 &&
                        kl <
                        sizeof kb) {
                        memcpy(kb,
                            p, kl);
                        kb[kl] = 0;
                        if (is_bench(
                            kb) ||
                            ends_with(kb,
                            ".lat") ||
                            strstr(kb,
                            ".hist.") !=
                            nullptr ||
                            ends_with(kb,
                            ".rate_ach")) {
                            has = 1;
                            break;
                        }
                    }
                }
                p = e;
            }
            if (has == 0) {
                continue;
            }
        }
        if (np >= cap) {
            return (np);
        }
        phs[np++] = ph;
    }
    return (np);
}

// Renders bench latency series with
// the shared line plot path.
static void text_bench_lat(
    Ev *evs, size_t n) {
    Mkey lk[BenchCap];
    int nl = 0;
    int j = 0;
    int phs[PhScan];
    fputs("benchmark latency shows "
        "per-message latency over "
        "time.\n", stdout);
    fputs("Split by phase like flow "
        "rate.\n", stdout);
    fputs("benchmark latency:\n",
        stdout);
    nl = bench_lats(evs, n, lk);
    if (nl == 0) {
        fputs("no bench latency "
            "present.\n", stdout);
        return;
    }
    {
        int nm = 0;
        int q = 0;
        for (j = 0; j < nl; j++) {
            int np = 0;
            np = key_phases(
                lk[j].k, evs, n,
                phs, PhScan);
            for (q = 0; q < np;
                q++) {
                size_t m = 0;
                double v = 0.0;
                unsigned long long t =
                    0;
                key_phase_stats(
                    lk[j].k,
                    phs[q], evs, n,
                    &m, &v, &t);
                if (m >= 2) {
                    nm++;
                }
            }
        }
        if (nm == 0) {
            fputs("no bench latency "
                "present.\n", stdout);
            return;
        }
        for (j = 0; j < nl; j++) {
            int np = 0;
            np = key_phases(
                lk[j].k, evs, n,
                phs, PhScan);
            for (q = 0; q < np;
                q++) {
                size_t m = 0;
                double v = 0.0;
                unsigned long long t =
                    0;
                char pn[PhName];
                key_phase_stats(
                    lk[j].k,
                    phs[q], evs, n,
                    &m, &v, &t);
                if (m < 2) {
                    continue;
                }
                phase_name(evs, n,
                    phs[q], pn,
                    sizeof pn);
                text_graph(lk[j].k,
                    phs[q], pn, evs,
                    n);
            }
        }
    }
}

// One histogram point: bucket plus
// count.
typedef struct {
    int b;
    double v;
} HistPt;

// Collects histogram points for
// stem in phase ph; stem is the
// prefix before .hist., suffix
// shape only. Returns count.
static int bench_hist_pts(
    const char *base, int ph,
    Ev *evs, size_t n, HistPt *pts,
    int cap) {
    int np = 0;
    size_t i = 0;
    for (i = 0; i < n; i++) {
        const char *msg = nullptr;
        const char *p = nullptr;
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        if (evs[i].ph != ph) {
            continue;
        }
        msg = evs[i].msg;
        p = msg;
        while (*p != 0) {
            const char *e = nullptr;
            const char *eq = nullptr;
            const char *q = nullptr;
            char kb[KeyCap];
            char vb[32];
            size_t kl = 0;
            size_t vl = 0;
            double v = 0.0;
            char bb[KeyCap];
            int b = -1;
            int k = 0;
            while (*p == ' ' ||
                *p == '\t') {
                p++;
            }
            if (*p == 0) {
                break;
            }
            e = p;
            while (*e != 0 &&
                *e != ' ' &&
                *e != '\t') {
                e++;
            }
            for (q = p; q < e; q++) {
                if (*q == '=') {
                    eq = q;
                    break;
                }
            }
            if (eq == nullptr) {
                p = e;
                continue;
            }
            kl = (size_t)(eq - p);
            if (kl == 0 ||
                kl >= sizeof kb) {
                p = e;
                continue;
            }
            memcpy(kb, p, kl);
            kb[kl] = 0;
            if (!hist_stem(kb, bb,
                sizeof bb)) {
                p = e;
                continue;
            }
            if (strcmp(bb, base)
                != 0) {
                p = e;
                continue;
            }
            b = bench_bucket(kb);
            if (b < 0) {
                p = e;
                continue;
            }
            vl = (size_t)(e - eq -
                1);
            if (vl == 0 ||
                vl >= sizeof vb) {
                p = e;
                continue;
            }
            memcpy(vb, eq + 1, vl);
            vb[vl] = 0;
            if (!num_of(vb, &v)) {
                p = e;
                continue;
            }
            for (k = 0; k < np;
                k++) {
                if (pts[k].b == b) {
                    break;
                }
            }
            if (k < np) {
                p = e;
                continue;
            }
            if (np >= cap) {
                p = e;
                continue;
            }
            pts[np].b = b;
            pts[np].v = v;
            np++;
            p = e;
        }
    }
    {
        int a = 0;
        int c = 0;
        for (a = 0; a < np; a++) {
            for (c = a + 1; c < np;
                c++) {
                if (pts[c].b <
                    pts[a].b) {
                    HistPt t =
                        pts[a];
                    pts[a] =
                        pts[c];
                    pts[c] = t;
                }
            }
        }
    }
    return (np);
}

// Collects us histogram points;
// same shape over .hist_us..
static int bench_hist_us_pts(
    const char *base, int ph,
    Ev *evs, size_t n, HistPt *pts,
    int cap) {
    int np = 0;
    size_t i = 0;
    for (i = 0; i < n; i++) {
        const char *msg = nullptr;
        const char *q0 = nullptr;
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        if (evs[i].ph != ph) {
            continue;
        }
        msg = evs[i].msg;
        q0 = msg;
        while (*q0 != 0) {
            const char *e = nullptr;
            const char *eq = nullptr;
            const char *q = nullptr;
            char kb[KeyCap];
            char vb[32];
            size_t kl = 0;
            size_t vl = 0;
            double v = 0.0;
            char bb[KeyCap];
            int b = -1;
            int k = 0;
            while (*q0 == ' ' ||
                *q0 == '	') {
                q0++;
            }
            if (*q0 == 0) {
                break;
            }
            e = q0;
            while (*e != 0 &&
                *e != ' ' &&
                *e != '	') {
                e++;
            }
            for (q = q0; q < e; q++) {
                if (*q == '=') {
                    eq = q;
                    break;
                }
            }
            if (eq == nullptr) {
                q0 = e;
                continue;
            }
            kl = (size_t)(eq - q0);
            if (kl == 0 ||
                kl >= sizeof kb) {
                q0 = e;
                continue;
            }
            memcpy(kb, q0, kl);
            kb[kl] = 0;
            if (!hist_us_stem(kb, bb,
                sizeof bb)) {
                q0 = e;
                continue;
            }
            if (strcmp(bb, base)
                != 0) {
                q0 = e;
                continue;
            }
            b = bench_bucket_us(kb);
            if (b < 0) {
                q0 = e;
                continue;
            }
            vl = (size_t)(e - eq -
                1);
            if (vl == 0 ||
                vl >= sizeof vb) {
                q0 = e;
                continue;
            }
            memcpy(vb, eq + 1, vl);
            vb[vl] = 0;
            if (!num_of(vb, &v)) {
                q0 = e;
                continue;
            }
            for (k = 0; k < np;
                k++) {
                if (pts[k].b == b) {
                    break;
                }
            }
            if (k < np) {
                q0 = e;
                continue;
            }
            if (np >= cap) {
                q0 = e;
                continue;
            }
            pts[np].b = b;
            pts[np].v = v;
            np++;
            q0 = e;
        }
    }
    {
        int a = 0;
        int c = 0;
        for (a = 0; a < np; a++) {
            for (c = a + 1; c < np;
                c++) {
                if (pts[c].b <
                    pts[a].b) {
                    HistPt t =
                        pts[a];
                    pts[a] =
                        pts[c];
                    pts[c] = t;
                }
            }
        }
    }
    return (np);
}

// Prints one histogram in text with
// the shared graph header shape.
static void text_bench_hist_one(
    const char *base, int ph,
    const char *pname, Ev *evs,
    size_t n) {
    HistPt pts[BenchHistN + 8];
    int np = 0;
    char tb[128];
    char full[KeyCap];
    int i = 0;
    int r = 0;
    if (pname == nullptr) {
        pname = "ph";
    }
    np = bench_hist_pts(base, ph,
        evs, n, pts,
        (int)(sizeof pts /
        sizeof pts[0]));
    if (np == 0) {
        return;
    }
    r = snprintf(full,
        sizeof full, "%s.hist.*",
        base);
    if (r < 0 ||
        (size_t)r >= sizeof full) {
        return;
    }
    metric_title(full, tb,
        sizeof tb);
    if (tb[0] == 0) {
        snprintf(tb, sizeof tb,
            "%s", full);
    }
    printf("%s\n", tb);
    printf("  key: %s\n", full);
    printf("  phase: %s [%d]\n",
        pname, ph);
    printf("  axes: x is latency, "
        "y is count\n");
    for (i = 0; i < np; i++) {
        char lab[32];
        hist_label(pts[i].b, lab,
            sizeof lab);
        printf("  %s: %g\n", lab,
            pts[i].v);
    }
}

// Prints one us histogram in text;
// same shape with us edges.
static void text_bench_hist_us_one(
    const char *base, int ph,
    const char *pname, Ev *evs,
    size_t n) {
    HistPt pts[BenchHistN + 8];
    int np = 0;
    char tb[128];
    char full[KeyCap];
    int i = 0;
    int r = 0;
    if (pname == nullptr) {
        pname = "ph";
    }
    np = bench_hist_us_pts(base, ph,
        evs, n, pts,
        (int)(sizeof pts /
        sizeof pts[0]));
    if (np == 0) {
        return;
    }
    r = snprintf(full,
        sizeof full, "%s.hist_us.*",
        base);
    if (r < 0 ||
        (size_t)r >= sizeof full) {
        return;
    }
    metric_title(full, tb,
        sizeof tb);
    if (tb[0] == 0) {
        snprintf(tb, sizeof tb,
            "%s", full);
    }
    printf("%s\n", tb);
    printf("  key: %s\n", full);
    printf("  phase: %s [%d]\n",
        pname, ph);
    printf("  axes: x is latency, "
        "y is count\n");
    for (i = 0; i < np; i++) {
        char lab[32];
        hist_us_label(pts[i].b, lab,
            sizeof lab);
        printf("  %s: %g\n", lab,
            pts[i].v);
    }
}

// Renders bench histograms in text.
static void text_bench_hist(
    Ev *evs, size_t n) {
    char bs[BenchCap][KeyCap];
    char us[BenchCap][KeyCap];
    int nb = 0;
    int nu = 0;
    int phs[PhScan];
    int np = 0;
    int j = 0;
    int q = 0;
    fputs("benchmark histograms "
        "show latency bucket "
        "counts per phase.\n",
        stdout);
    fputs("Bars labeled by latency; "
        "ms plus us; split by phase.\n",
        stdout);
    fputs("benchmark histograms:\n",
        stdout);
    nb = hist_stems(evs, n, bs);
    nu = hist_us_stems(evs, n, us);
    np = bench_phases(evs, n, phs,
        PhScan);
    {
        int any = 0;
        for (j = 0; j < nb; j++) {
            for (q = 0; q < np;
                q++) {
                HistPt pts[
                    BenchHistN + 8];
                int m = 0;
                m = bench_hist_pts(
                    bs[j], phs[q],
                    evs, n, pts,
                    (int)(sizeof pts /
                    sizeof pts[0]));
                if (m > 0) {
                    any = 1;
                    break;
                }
            }
            if (any != 0) {
                break;
            }
        }
        if (any == 0) {
            for (j = 0; j < nu; j++) {
                for (q = 0; q < np;
                    q++) {
                    HistPt pts[
                        BenchHistN + 8];
                    int m = 0;
                    m =
                    bench_hist_us_pts(
                        us[j], phs[q],
                        evs, n, pts,
                        (int)(sizeof pts /
                        sizeof pts[0]));
                    if (m > 0) {
                        any = 1;
                        break;
                    }
                }
                if (any != 0) {
                    break;
                }
            }
        }
        if (any == 0) {
            fputs("no bench "
                "histograms "
                "present.\n", stdout);
            return;
        }
    }
    for (j = 0; j < nb; j++) {
        for (q = 0; q < np; q++) {
            char pn[PhName];
            HistPt pts[
                BenchHistN + 8];
            int m = 0;
            m = bench_hist_pts(
                bs[j], phs[q], evs,
                n, pts,
                (int)(sizeof pts /
                sizeof pts[0]));
            if (m == 0) {
                continue;
            }
            phase_name(evs, n,
                phs[q], pn,
                sizeof pn);
            text_bench_hist_one(
                bs[j], phs[q], pn,
                evs, n);
        }
    }
    for (j = 0; j < nu; j++) {
        for (q = 0; q < np; q++) {
            char pn[PhName];
            HistPt pts[
                BenchHistN + 8];
            int m = 0;
            m = bench_hist_us_pts(
                us[j], phs[q], evs,
                n, pts,
                (int)(sizeof pts /
                sizeof pts[0]));
            if (m == 0) {
                continue;
            }
            phase_name(evs, n,
                phs[q], pn,
                sizeof pn);
            text_bench_hist_us_one(
                us[j], phs[q], pn,
                evs, n);
        }
    }
}

// Renders bench summary tables.
static void text_bench_sum(
    Ev *evs, size_t n) {
    char bs[BenchCap][KeyCap];
    int nb = 0;
    int phs[PhScan];
    int np = 0;
    int q = 0;
    fputs("benchmark summary shows "
        "count loss p50 p99 max "
        "target rate and overrun.\n",
        stdout);
    fputs("One row per step; split "
        "by phase.\n", stdout);
    fputs("benchmark summary:\n",
        stdout);
    nb = bench_bases(evs, n, bs);
    np = bench_phases(evs, n, phs,
        PhScan);
    {
        int any = 0;
        for (q = 0; q < np; q++) {
            int j = 0;
            for (j = 0; j < nb;
                j++) {
                if (
                    bench_tab_hit(
                    bs[j], phs[q],
                    evs, n)) {
                    any = 1;
                    break;
                }
            }
            if (any != 0) {
                break;
            }
        }
        if (any == 0) {
            fputs("no bench summary "
                "present.\n", stdout);
            return;
        }
        for (q = 0; q < np; q++) {
            int j = 0;
            int hit = 0;
            char pn[PhName];
            for (j = 0; j < nb;
                j++) {
                if (
                    bench_tab_hit(
                    bs[j], phs[q],
                    evs, n)) {
                    hit = 1;
                    break;
                }
            }
            if (hit == 0) {
                continue;
            }
            phase_name(evs, n,
                phs[q], pn,
                sizeof pn);
            text_bench_tab(evs, n,
                phs[q], pn, bs,
                nb);
        }
    }
}

// Max ramp steps per ladder plot;
// lint caps steps at 10, 16 keeps
// headroom without growing caps.
static constexpr int LadderCap = 16;

// Collects parent bench.A.B bases
// holding ramp steps into ps.
// Derives from the shared base
// table, stripping .ramp.I.
// Returns the kept count.
static int ladder_parents(Ev *evs,
    size_t n,
    char (*ps)[KeyCap]) {
    char bs[BenchCap][KeyCap];
    int nb = 0;
    int np = 0;
    int j = 0;
    nb = bench_bases(evs, n, bs);
    for (j = 0; j < nb; j++) {
        char *rp = nullptr;
        int k = 0;
        rp = strstr(bs[j],
            ".ramp.");
        if (rp == nullptr) {
            continue;
        }
        *rp = 0;
        for (k = 0; k < np; k++) {
            if (strcmp(ps[k],
                bs[j]) == 0) {
                break;
            }
        }
        if (k >= np &&
            np < BenchCap) {
            memcpy(ps[np], bs[j],
                strlen(bs[j]) + 1);
            np++;
        }
    }
    return (np);
}

// Collects step points for parent
// in phase: rate plus p50, p99,
// max in ms. Stops at the first
// step missing rate. Returns the
// step count.
static int ladder_pts(
    const char *parent, int ph,
    Ev *evs, size_t n,
    double *rate, double *p50,
    double *p99, double *mx) {
    char key[KeyCap];
    int cnt = 0;
    int i = 0;
    if (parent == nullptr) {
        return (0);
    }
    for (i = 0; i < LadderCap;
         i++) {
        double r = 0.0;
        double a = 0.0;
        double b = 0.0;
        double c = 0.0;
        int fr = 0;
        int fa = 0;
        int fb = 0;
        int fc = 0;
        int m = 0;
        m = snprintf(key,
            sizeof key, "%s.ramp."
            "%d", parent, i);
        if (m < 0 ||
            (size_t)m >=
                sizeof key) {
            break;
        }
        bench_val(key,
            "rate_ach", ph, evs, n,
            &r, &fr);
        if (fr == 0) {
            break;
        }
        bench_val(key, "p50.ms",
            ph, evs, n, &a, &fa);
        bench_val(key, "p99.ms",
            ph, evs, n, &b, &fb);
        bench_val(key, "max.ms",
            ph, evs, n, &c, &fc);
        if (fa == 0 || fb == 0 ||
            fc == 0) {
            break;
        }
        rate[cnt] = r;
        p50[cnt] = a;
        p99[cnt] = b;
        mx[cnt] = c;
        cnt++;
    }
    return (cnt);
}

// Prints one ladder in text: one
// row per step, rate plus the
// golden trio, shared units.
static void text_bench_ladder_one(
    const char *parent, int ph,
    const char *pname, Ev *evs,
    size_t n) {
    double rate[LadderCap];
    double p50[LadderCap];
    double p99[LadderCap];
    double mx[LadderCap];
    int cnt = 0;
    int i = 0;
    if (pname == nullptr) {
        pname = "ph";
    }
    cnt = ladder_pts(parent, ph,
        evs, n, rate, p50, p99,
        mx);
    if (cnt == 0) {
        return;
    }
    printf("%s\n", parent);
    printf("phase %s [%d]:\n",
        pname, ph);
    printf("  axes: x is messages "
        "per second, y is "
        "milliseconds\n");
    printf("  step rate p50 p99 "
        "max\n");
    for (i = 0; i < cnt; i++) {
        printf("  %d %g %g %g "
            "%g\n", i, rate[i],
            p50[i], p99[i],
            mx[i]);
    }
}

// Renders ramp ladders in text;
// silent unless ramp steps exist.
static void text_bench_ladder(
    Ev *evs, size_t n) {
    char ps[BenchCap][KeyCap];
    int np = 0;
    int phs[PhScan];
    int nn = 0;
    int j = 0;
    int q = 0;
    np = ladder_parents(evs, n,
        ps);
    if (np == 0) {
        return;
    }
    nn = bench_phases(evs, n,
        phs, PhScan);
    fputs("ramp ladder plots p50 "
        "p99 max over achieved rate "
        "per step.\n", stdout);
    fputs("Split by phase like flow "
        "rate.\n", stdout);
    fputs("ramp ladder:\n",
        stdout);
    for (j = 0; j < np; j++) {
        for (q = 0; q < nn;
             q++) {
            double rate[
                LadderCap];
            double p50[
                LadderCap];
            double p99[
                LadderCap];
            double mx[LadderCap];
            char pn[PhName];
            int m = 0;
            m = ladder_pts(ps[j],
                phs[q], evs, n,
                rate, p50, p99,
                mx);
            if (m == 0) {
                continue;
            }
            phase_name(evs, n,
                phs[q], pn,
                sizeof pn);
            text_bench_ladder_one(
                ps[j], phs[q], pn,
                evs, n);
        }
    }
}

// Overview row: one phase of step
// k with the golden trio.
typedef struct {
    int ph;
    char name[PhName];
    double p50;
    double p99;
    double mx;
} OverRow;

// Max step index scanned per
// phase; steps are dense from 0.
static constexpr int OverCap = 16;

// Collects overview rows for step
// k: one per phase holding p50.ms.
// Sweep steps share k across
// phases, so k groups the sweep
// the file runs. Returns the row
// count.
static int over_rows(int k,
    Ev *evs, size_t n,
    OverRow *rows, int cap) {
    char key[KeyCap];
    int phs[PhScan];
    int nn = 0;
    int q = 0;
    int cnt = 0;
    int m = 0;
    if (rows == nullptr ||
        k < 0) {
        return (0);
    }
    nn = bench_phases(evs, n,
        phs, PhScan);
    for (q = 0; q < nn; q++) {
        double a = 0.0;
        double b = 0.0;
        double c = 0.0;
        int fa = 0;
        int fb = 0;
        int fc = 0;
        char pn[PhName];
        m = snprintf(key,
            sizeof key,
            "bench.%d.%d", phs[q],
            k);
        if (m < 0 ||
            (size_t)m >=
                sizeof key) {
            continue;
        }
        bench_val(key, "p50.ms",
            phs[q], evs, n, &a,
            &fa);
        if (fa == 0) {
            continue;
        }
        bench_val(key, "p99.ms",
            phs[q], evs, n, &b,
            &fb);
        bench_val(key, "max.ms",
            phs[q], evs, n, &c,
            &fc);
        if (fb == 0 || fc == 0) {
            continue;
        }
        if (cnt >= cap) {
            break;
        }
        phase_name(evs, n,
            phs[q], pn,
            sizeof pn);
        rows[cnt].ph = phs[q];
        memcpy(rows[cnt].name,
            pn, sizeof pn);
        rows[cnt].p50 = a;
        rows[cnt].p99 = b;
        rows[cnt].mx = c;
        cnt++;
    }
    return (cnt);
}

// Prints one overview in text:
// one row per phase.
static void text_bench_over_one(
    int k, OverRow *rows,
    int cnt) {
    int i = 0;
    printf("step %d\n", k);
    printf("  phase p50 p99 max\n");
    for (i = 0; i < cnt; i++) {
        printf("  %s [%d]: %g %g "
            "%g\n",
            rows[i].name,
            rows[i].ph,
            rows[i].p50,
            rows[i].p99,
            rows[i].mx);
    }
}

// Renders the cross-phase golden
// overview in text; silent unless
// a step spans two phases.
static void text_bench_overview(
    Ev *evs, size_t n) {
    int k = 0;
    int any = 0;
    for (k = 0; k < OverCap;
         k++) {
        OverRow rows[PhScan];
        int cnt = 0;
        cnt = over_rows(k, evs,
            n, rows, PhScan);
        if (cnt >= 2) {
            any = 1;
            break;
        }
    }
    if (any == 0) {
        return;
    }
    fputs("run overview plots p50 "
        "p99 max across phases per "
        "step; single-phase steps "
        "stay in the summary.\n",
        stdout);
    fputs("run overview:\n",
        stdout);
    for (k = 0; k < OverCap;
         k++) {
        OverRow rows[PhScan];
        int cnt = 0;
        cnt = over_rows(k, evs,
            n, rows, PhScan);
        if (cnt < 2) {
            continue;
        }
        text_bench_over_one(k,
            rows, cnt);
    }
}

// Renders bench text views; no new
// keys means no lines added.
static void render_text_bench(
    Ev *evs, size_t n) {
    if (!bench_has_new(evs, n)) {
        return;
    }
    text_bench_lat(evs, n);
    text_bench_hist(evs, n);
    text_bench_sum(evs, n);
    text_bench_ladder(evs, n);
    text_bench_overview(evs, n);
}

// Writes one histogram bar chart
// for base in phase ph; shares the
// plot pads, ticks, and escapes.
static void html_bench_hist_one(
    FILE *f, const char *base,
    int ph, const char *pname,
    Ev *evs, size_t n,
    unsigned long long tmax) {
    HistPt pts[BenchHistN + 8];
    int np = 0;
    char tb[128];
    char full[KeyCap];
    const char *un = nullptr;
    double mx = 0.0;
    double inner = 0.0;
    double base_y = 0.0;
    int flat = 0;
    int i = 0;
    int r = 0;
    int x1 = 0;
    (void)tmax;
    if (f == nullptr) {
        return;
    }
    if (base == nullptr) {
        return;
    }
    if (pname == nullptr) {
        pname = "ph";
    }
    np = bench_hist_pts(base, ph,
        evs, n, pts,
        (int)(sizeof pts /
        sizeof pts[0]));
    if (np == 0) {
        return;
    }
    r = snprintf(full,
        sizeof full, "%s.hist.*",
        base);
    if (r < 0 ||
        (size_t)r >= sizeof full) {
        return;
    }
    metric_title(full, tb,
        sizeof tb);
    if (tb[0] == 0) {
        snprintf(tb, sizeof tb,
            "%s", full);
    }
    un = "count";
    fputs("<pre><b>", f);
    html_esc(f, tb);
    fputs("</b>\n", f);
    fputs("  key: ", f);
    html_esc(f, full);
    fputc('\n', f);
    fputs("  phase: ", f);
    html_esc(f, pname);
    fprintf(f, " [%d]\n", ph);
    fputs("  axes: x is latency, "
        "y is ", f);
    html_esc(f, un);
    fputc('\n', f);
    fputs("</pre>\n", f);
    fprintf(f, "<details>"
        "<summary>%d buckets"
        "</summary>\n<pre>\n",
        np);
    for (i = 0; i < np; i++) {
        char lab[32];
        hist_label(pts[i].b, lab,
            sizeof lab);
        fputs("  ", f);
        html_esc(f, lab);
        fprintf(f, ": %g\n",
            pts[i].v);
    }
    fputs("</pre></details>\n",
        f);
    mx = pts[0].v;
    for (i = 1; i < np; i++) {
        if (pts[i].v > mx) {
            mx = pts[i].v;
        }
    }
    x1 = HtmlW - HtmlPadR;
    inner = (double)(HtmlPlotH -
        12 - 24);
    base_y = 12.0 + inner;
    flat = (mx == 0.0);
    fprintf(f, "<svg width=\"%d\" "
        "height=\"%d\" "
        "role=\"img\">\n", HtmlW,
        HtmlPlotH);
    for (i = 0; i < np; i++) {
        double xx = 0.0;
        double yy = 0.0;
        double slot = 0.0;
        double bw = 0.0;
        double x0 = 0.0;
        char lab[32];
        slot = (double)(HtmlW -
            HtmlPadL - HtmlPadR) /
            (double)np;
        bw = slot - 4.0;
        if (bw < 1.0) {
            bw = 1.0;
        }
        x0 = (double)HtmlPadL +
            (double)i * slot + 2.0;
        xx = x0;
        yy = plot_y(pts[i].v, 0.0,
            mx, flat, inner);
        hist_label(pts[i].b, lab,
            sizeof lab);
        fprintf(f, "<rect x=\""
            "%.1f\" y=\"%.1f\" "
            "width=\"%.1f\" "
            "height=\"%.1f\" "
            "fill=\"#1d4ed8\">",
            xx, yy, bw,
            base_y - yy);
        fputs("<title>", f);
        html_esc(f, lab);
        fprintf(f, ": %g</title>",
            pts[i].v);
        fputs("</rect>\n", f);
    }
    fprintf(f, "<text x=\"%d\" "
        "y=\"20\" "
        "text-anchor=\"end\">%g ",
        HtmlPadL - HtmlLabGap, mx);
    html_esc(f, un);
    fputs("</text>\n", f);
    fprintf(f, "<text x=\"%d\" "
        "y=\"%d\" "
        "text-anchor=\"end\">0 ",
        HtmlPadL - HtmlLabGap,
        (int)base_y);
    html_esc(f, un);
    fputs("</text>\n", f);
    fprintf(f, "<text x=\"%d\" "
        "y=\"%d\" ", x1,
        HtmlPlotH - 6);
    fputs("text-anchor=\"end\">",
        f);
    html_esc(f, "latency");
    fputs("</text>\n", f);
    fputs("</svg>\n", f);
}

// Writes one us histogram bar chart;
// same shape with us edges.
static void html_bench_hist_us_one(
    FILE *f, const char *base,
    int ph, const char *pname,
    Ev *evs, size_t n,
    unsigned long long tmax) {
    HistPt pts[BenchHistN + 8];
    int np = 0;
    char tb[128];
    char full[KeyCap];
    const char *un = nullptr;
    double mx = 0.0;
    double inner = 0.0;
    double base_y = 0.0;
    int flat = 0;
    int i = 0;
    int r = 0;
    int x1 = 0;
    (void)tmax;
    if (f == nullptr) {
        return;
    }
    if (base == nullptr) {
        return;
    }
    if (pname == nullptr) {
        pname = "ph";
    }
    np = bench_hist_us_pts(base, ph,
        evs, n, pts,
        (int)(sizeof pts /
        sizeof pts[0]));
    if (np == 0) {
        return;
    }
    r = snprintf(full,
        sizeof full, "%s.hist_us.*",
        base);
    if (r < 0 ||
        (size_t)r >= sizeof full) {
        return;
    }
    metric_title(full, tb,
        sizeof tb);
    if (tb[0] == 0) {
        snprintf(tb, sizeof tb,
            "%s", full);
    }
    un = "count";
    fputs("<pre><b>", f);
    html_esc(f, tb);
    fputs("</b>\n", f);
    fputs("  key: ", f);
    html_esc(f, full);
    fputc('\n', f);
    fputs("  phase: ", f);
    html_esc(f, pname);
    fprintf(f, " [%d]\n", ph);
    fputs("  axes: x is latency, "
        "y is ", f);
    html_esc(f, un);
    fputc('\n', f);
    fputs("</pre>\n", f);
    fprintf(f, "<details>"
        "<summary>%d buckets"
        "</summary>\n<pre>\n",
        np);
    for (i = 0; i < np; i++) {
        char lab[32];
        hist_us_label(pts[i].b, lab,
            sizeof lab);
        fputs("  ", f);
        html_esc(f, lab);
        fprintf(f, ": %g\n",
            pts[i].v);
    }
    fputs("</pre></details>\n",
        f);
    mx = pts[0].v;
    for (i = 1; i < np; i++) {
        if (pts[i].v > mx) {
            mx = pts[i].v;
        }
    }
    x1 = HtmlW - HtmlPadR;
    inner = (double)(HtmlPlotH -
        12 - 24);
    base_y = 12.0 + inner;
    flat = (mx == 0.0);
    fprintf(f, "<svg width=\"%d\" "
        "height=\"%d\" "
        "role=\"img\">\n", HtmlW,
        HtmlPlotH);
    for (i = 0; i < np; i++) {
        double xx = 0.0;
        double yy = 0.0;
        double slot = 0.0;
        double bw = 0.0;
        double x0 = 0.0;
        char lab[32];
        slot = (double)(HtmlW -
            HtmlPadL - HtmlPadR) /
            (double)np;
        bw = slot - 4.0;
        if (bw < 1.0) {
            bw = 1.0;
        }
        x0 = (double)HtmlPadL +
            (double)i * slot + 2.0;
        xx = x0;
        yy = plot_y(pts[i].v, 0.0,
            mx, flat, inner);
        hist_us_label(pts[i].b, lab,
            sizeof lab);
        fprintf(f, "<rect x=\""
            "%.1f\" y=\"%.1f\" "
            "width=\"%.1f\" "
            "height=\"%.1f\" "
            "fill=\"#1d4ed8\">",
            xx, yy, bw,
            base_y - yy);
        fputs("<title>", f);
        html_esc(f, lab);
        fprintf(f, ": %g</title>",
            pts[i].v);
        fputs("</rect>\n", f);
    }
    fprintf(f, "<text x=\"%d\" "
        "y=\"20\" "
        "text-anchor=\"end\">%g ",
        HtmlPadL - HtmlLabGap, mx);
    html_esc(f, un);
    fputs("</text>\n", f);
    fprintf(f, "<text x=\"%d\" "
        "y=\"%d\" "
        "text-anchor=\"end\">0 ",
        HtmlPadL - HtmlLabGap,
        (int)base_y);
    html_esc(f, un);
    fputs("</text>\n", f);
    fprintf(f, "<text x=\"%d\" "
        "y=\"%d\" ", x1,
        HtmlPlotH - 6);
    fputs("text-anchor=\"end\">",
        f);
    html_esc(f, "latency");
    fputs("</text>\n", f);
    fputs("</svg>\n", f);
}

// Writes bench latency plots with
// the shared line plot path.
static void html_bench_lat(FILE *f,
    Ev *evs, size_t n,
    unsigned long long tmax) {
    Mkey lk[BenchCap];
    int nl = 0;
    int j = 0;
    int phs[PhScan];
    fputs("<pre>benchmark latency "
        "shows per-message "
        "latency over time.\n",
        f);
    fputs("Split by phase like flow "
        "rate.</pre>\n", f);
    fputs("<pre><b>benchmark "
        "latency:</b></pre>\n", f);
    nl = bench_lats(evs, n, lk);
    if (nl == 0) {
        fputs("<pre>no bench "
            "latency present."
            "</pre>\n", f);
        return;
    }
    {
        int nm = 0;
        int q = 0;
        for (j = 0; j < nl; j++) {
            int np = 0;
            np = key_phases(
                lk[j].k, evs, n,
                phs, PhScan);
            for (q = 0; q < np;
                q++) {
                size_t m = 0;
                double v = 0.0;
                unsigned long long t =
                    0;
                key_phase_stats(
                    lk[j].k,
                    phs[q], evs, n,
                    &m, &v, &t);
                if (m >= 2) {
                    nm++;
                }
            }
        }
        if (nm == 0) {
            fputs("<pre>no bench "
                "latency present."
                "</pre>\n", f);
            return;
        }
        for (j = 0; j < nl; j++) {
            int np = 0;
            np = key_phases(
                lk[j].k, evs, n,
                phs, PhScan);
            for (q = 0; q < np;
                q++) {
                size_t m = 0;
                double v = 0.0;
                unsigned long long t =
                    0;
                char pn[PhName];
                key_phase_stats(
                    lk[j].k,
                    phs[q], evs, n,
                    &m, &v, &t);
                if (m < 2) {
                    continue;
                }
                phase_name(evs, n,
                    phs[q], pn,
                    sizeof pn);
                html_plot(f,
                    lk[j].k,
                    phs[q], pn,
                    evs, n, tmax,
                    0);
            }
        }
    }
}

// Writes bench histograms; empty
// sets note and pass like plots.
static void html_bench_hist(FILE *f,
    Ev *evs, size_t n,
    unsigned long long tmax) {
    char bs[BenchCap][KeyCap];
    char us[BenchCap][KeyCap];
    int nb = 0;
    int nu = 0;
    int phs[PhScan];
    int np = 0;
    int j = 0;
    int q = 0;
    fputs("<pre>benchmark "
        "histograms show latency "
        "bucket counts per "
        "phase.\n", f);
    fputs("Bars labeled by latency; "
        "ms plus us; split by phase."
        "</pre>\n", f);
    fputs("<pre><b>benchmark "
        "histograms:</b></pre>\n",
        f);
    nb = hist_stems(evs, n, bs);
    nu = hist_us_stems(evs, n, us);
    np = bench_phases(evs, n, phs,
        PhScan);
    {
        int any = 0;
        for (j = 0; j < nb; j++) {
            for (q = 0; q < np;
                q++) {
                HistPt pts[
                    BenchHistN + 8];
                int m = 0;
                m = bench_hist_pts(
                    bs[j], phs[q],
                    evs, n, pts,
                    (int)(sizeof pts /
                    sizeof pts[0]));
                if (m > 0) {
                    any = 1;
                    break;
                }
            }
            if (any != 0) {
                break;
            }
        }
        if (any == 0) {
            for (j = 0; j < nu; j++) {
                for (q = 0; q < np;
                    q++) {
                    HistPt pts[
                        BenchHistN + 8];
                    int m = 0;
                    m =
                    bench_hist_us_pts(
                        us[j], phs[q],
                        evs, n, pts,
                        (int)(sizeof pts /
                        sizeof pts[0]));
                    if (m > 0) {
                        any = 1;
                        break;
                    }
                }
                if (any != 0) {
                    break;
                }
            }
        }
        if (any == 0) {
            fputs("<pre>no bench "
                "histograms present."
                "</pre>\n", f);
            return;
        }
    }
    for (j = 0; j < nb; j++) {
        for (q = 0; q < np; q++) {
            char pn[PhName];
            HistPt pts[
                BenchHistN + 8];
            int m = 0;
            m = bench_hist_pts(
                bs[j], phs[q], evs,
                n, pts,
                (int)(sizeof pts /
                sizeof pts[0]));
            if (m == 0) {
                continue;
            }
            phase_name(evs, n,
                phs[q], pn,
                sizeof pn);
            html_bench_hist_one(f,
                bs[j], phs[q], pn,
                evs, n, tmax);
        }
    }
    for (j = 0; j < nu; j++) {
        for (q = 0; q < np; q++) {
            char pn[PhName];
            HistPt pts[
                BenchHistN + 8];
            int m = 0;
            m = bench_hist_us_pts(
                us[j], phs[q], evs,
                n, pts,
                (int)(sizeof pts /
                sizeof pts[0]));
            if (m == 0) {
                continue;
            }
            phase_name(evs, n,
                phs[q], pn,
                sizeof pn);
            html_bench_hist_us_one(
                f, us[j], phs[q],
                pn, evs, n, tmax);
        }
    }
}

// Writes one bench summary table
// for phase ph in HTML pre shape.
// Ramp rows reuse this table, one
// row per ramp index; ceiling rows
// show rate in the rate column.
static void html_bench_tab(FILE *f,
    Ev *evs, size_t n, int ph,
    const char *pname,
    char (*bs)[KeyCap], int nb) {
    int i = 0;
    if (pname == nullptr) {
        pname = "ph";
    }
    fputs("<pre><b>phase ", f);
    html_esc(f, pname);
    fprintf(f, " [%d]</b>\n", ph);
    fputs("  step count loss "
        "p50 p99 max target "
        "rate_ach overrun p50.us "
        "p99.us max.us scrape\n", f);
    for (i = 0; i < nb; i++) {
        if (!bench_tab_hit(bs[i],
            ph, evs, n)) {
            continue;
        }
        double c = 0.0;
        double l = 0.0;
        double p50 = 0.0;
        double p99 = 0.0;
        double mx = 0.0;
        double tg = 0.0;
        double ra = 0.0;
        double ov = 0.0;
        double u50 = 0.0;
        double u99 = 0.0;
        double umx = 0.0;
        double sok = 0.0;
        int fc = 0;
        int fl = 0;
        int f50 = 0;
        int f99 = 0;
        int fmx = 0;
        int ftg = 0;
        int fra = 0;
        int fov = 0;
        int fu50 = 0;
        int fu99 = 0;
        int fumx = 0;
        int fsok = 0;
        char cb[32];
        char lb[32];
        char b50[32];
        char b99[32];
        char bmx[32];
        char btg[32];
        char bra[32];
        char bov[32];
        char bu50[32];
        char bu99[32];
        char bumx[32];
        char bsok[32];
        if (is_ceiling_base(bs[i])) {
            bench_val(bs[i], "rate",
                ph, evs, n, &ra,
                &fra);
        } else {
            bench_val(bs[i], "count",
                ph, evs, n, &c, &fc);
            bench_val(bs[i], "loss",
                ph, evs, n, &l, &fl);
            bench_val(bs[i], "p50.ms",
                ph, evs, n, &p50,
                &f50);
            bench_val(bs[i], "p99.ms",
                ph, evs, n, &p99,
                &f99);
            bench_val(bs[i], "max.ms",
                ph, evs, n, &mx, &fmx);
            bench_val(bs[i], "p50.us",
                ph, evs, n, &u50,
                &fu50);
            bench_val(bs[i], "p99.us",
                ph, evs, n, &u99,
                &fu99);
            bench_val(bs[i], "max.us",
                ph, evs, n, &umx,
                &fumx);
            bench_val(bs[i],
                "scrape_ok", ph, evs, n,
                &sok, &fsok);
            bench_val(bs[i], "target",
                ph, evs, n, &tg, &ftg);
            bench_val(bs[i],
                "rate_ach", ph, evs, n,
                &ra, &fra);
            bench_val(bs[i], "overrun",
                ph, evs, n, &ov, &fov);
        }
        if (fc != 0) {
            snprintf(cb,
                sizeof cb, "%g", c);
        } else {
            snprintf(cb,
                sizeof cb, "%s",
                "-");
        }
        if (fl != 0) {
            snprintf(lb,
                sizeof lb, "%g", l);
        } else {
            snprintf(lb,
                sizeof lb, "%s",
                "-");
        }
        if (f50 != 0) {
            snprintf(b50,
                sizeof b50, "%g",
                p50);
        } else {
            snprintf(b50,
                sizeof b50, "%s",
                "-");
        }
        if (f99 != 0) {
            snprintf(b99,
                sizeof b99, "%g",
                p99);
        } else {
            snprintf(b99,
                sizeof b99, "%s",
                "-");
        }
        if (fmx != 0) {
            snprintf(bmx,
                sizeof bmx, "%g",
                mx);
        } else {
            snprintf(bmx,
                sizeof bmx, "%s",
                "-");
        }
        if (ftg != 0) {
            snprintf(btg,
                sizeof btg, "%g",
                tg);
        } else {
            snprintf(btg,
                sizeof btg, "%s",
                "-");
        }
        if (fra != 0) {
            snprintf(bra,
                sizeof bra, "%g",
                ra);
        } else {
            snprintf(bra,
                sizeof bra, "%s",
                "-");
        }
        if (fov != 0) {
            snprintf(bov,
                sizeof bov, "%g",
                ov);
        } else {
            snprintf(bov,
                sizeof bov, "%s",
                "-");
        }
        if (fu50 != 0) {
            snprintf(bu50,
                sizeof bu50, "%g",
                u50);
        } else {
            snprintf(bu50,
                sizeof bu50, "%s",
                "-");
        }
        if (fu99 != 0) {
            snprintf(bu99,
                sizeof bu99, "%g",
                u99);
        } else {
            snprintf(bu99,
                sizeof bu99, "%s",
                "-");
        }
        if (fumx != 0) {
            snprintf(bumx,
                sizeof bumx, "%g",
                umx);
        } else {
            snprintf(bumx,
                sizeof bumx, "%s",
                "-");
        }
        if (fsok != 0) {
            snprintf(bsok,
                sizeof bsok, "%g",
                sok);
        } else {
            snprintf(bsok,
                sizeof bsok, "%s",
                "-");
        }
        fputs("  ", f);
        html_esc(f, bs[i]);
        fprintf(f, " %s %s %s %s "
            "%s %s %s %s %s %s "
            "%s %s\n", cb, lb,
            b50, b99, bmx, btg,
            bra, bov, bu50,
            bu99, bumx, bsok);
    }
    fputs("</pre>\n", f);
}

// Writes bench summary tables.
static void html_bench_sum(FILE *f,
    Ev *evs, size_t n) {
    char bs[BenchCap][KeyCap];
    int nb = 0;
    int phs[PhScan];
    int np = 0;
    int q = 0;
    fputs("<pre>benchmark summary "
        "shows count loss p50 p99 "
        "max target rate and "
        "overrun.\n", f);
    fputs("One row per step; split "
        "by phase.</pre>\n", f);
    fputs("<pre><b>benchmark "
        "summary:</b></pre>\n", f);
    nb = bench_bases(evs, n, bs);
    np = bench_phases(evs, n, phs,
        PhScan);
    {
        int any = 0;
        for (q = 0; q < np; q++) {
            int j = 0;
            for (j = 0; j < nb;
                j++) {
                if (
                    bench_tab_hit(
                    bs[j], phs[q],
                    evs, n)) {
                    any = 1;
                    break;
                }
            }
            if (any != 0) {
                break;
            }
        }
        if (any == 0) {
            fputs("<pre>no bench "
                "summary present."
                "</pre>\n", f);
            return;
        }
        for (q = 0; q < np; q++) {
            int j = 0;
            int hit = 0;
            char pn[PhName];
            for (j = 0; j < nb;
                j++) {
                if (
                    bench_tab_hit(
                    bs[j], phs[q],
                    evs, n)) {
                    hit = 1;
                    break;
                }
            }
            if (hit == 0) {
                continue;
            }
            phase_name(evs, n,
                phs[q], pn,
                sizeof pn);
            html_bench_tab(f, evs,
                n, phs[q], pn, bs,
                nb);
        }
    }
}

// Renders bench HTML views; no new
// keys means no lines added.
// Writes one ladder chart: p50,
// p99, and max over achieved rate
// with the golden palette, shared
// axes, one legend.
static void html_bench_ladder_one(
    FILE *f, const char *parent,
    int ph, const char *pname,
    Ev *evs, size_t n) {
    double rate[LadderCap];
    double ys[3][LadderCap];
    int cnt = 0;
    double p50[LadderCap];
    double p99[LadderCap];
    double mx[LadderCap];
    double mn = 0.0;
    double mxx = 0.0;
    double rmn = 0.0;
    double rmx = 0.0;
    double inner = 0.0;
    double base_y = 0.0;
    int flat = 0;
    int rflat = 0;
    int i = 0;
    int g = 0;
    int x1 = 0;
    char full[KeyCap];
    int r = 0;
    if (f == nullptr) {
        return;
    }
    if (parent == nullptr) {
        return;
    }
    if (pname == nullptr) {
        pname = "ph";
    }
    cnt = ladder_pts(parent, ph,
        evs, n, rate, p50, p99,
        mx);
    if (cnt == 0) {
        return;
    }
    for (i = 0; i < cnt; i++) {
        ys[0][i] = p50[i];
        ys[1][i] = p99[i];
        ys[2][i] = mx[i];
    }
    mn = p50[0];
    mxx = p50[0];
    for (g = 0; g < 3; g++) {
        for (i = 0; i < cnt;
             i++) {
            if (ys[g][i] < mn) {
                mn = ys[g][i];
            }
            if (ys[g][i] > mxx) {
                mxx = ys[g][i];
            }
        }
    }
    rmn = rate[0];
    rmx = rate[0];
    for (i = 1; i < cnt; i++) {
        if (rate[i] < rmn) {
            rmn = rate[i];
        }
        if (rate[i] > rmx) {
            rmx = rate[i];
        }
    }
    r = snprintf(full,
        sizeof full, "%s.ladder",
        parent);
    if (r < 0 ||
        (size_t)r >= sizeof full) {
        return;
    }
    fputs("<pre><b>", f);
    html_esc(f, full);
    fputs("</b>\n", f);
    fputs("  key: ", f);
    html_esc(f, parent);
    fputs(".ramp.*\n", f);
    fputs("  phase: ", f);
    html_esc(f, pname);
    fprintf(f, " [%d]\n", ph);
    fputs("  axes: x is messages "
        "per second, y is "
        "milliseconds\n", f);
    fputs("</pre>\n", f);
    fprintf(f, "<details>"
        "<summary>%d steps"
        "</summary>\n<pre>\n",
        cnt);
    for (i = 0; i < cnt; i++) {
        fprintf(f, "  step %d: "
            "rate %g p50 %g p99 "
            "%g max %g\n", i,
            rate[i], p50[i],
            p99[i], mx[i]);
    }
    fputs("</pre></details>\n",
        f);
    x1 = HtmlW - HtmlPadR;
    inner = (double)(HtmlPlotH -
        12 - 24);
    base_y = 12.0 + inner;
    flat = (mxx == mn);
    rflat = (rmx == rmn);
    fprintf(f, "<svg width=\"%d\" "
        "height=\"%d\" "
        "role=\"img\">\n", HtmlW,
        HtmlPlotH);
    for (g = 0; g < 3; g++) {
        int k = 0;
        fputs("<polyline "
            "points=\"", f);
        for (k = 0; k < cnt;
             k++) {
            double xx = 0.0;
            double yy = 0.0;
            if (rflat != 0) {
                xx = (double)
                         HtmlPadL +
                    ((double)(x1 -
                        HtmlPadL)) /
                        2.0;
            } else {
                xx = (double)
                         HtmlPadL +
                    (rate[k] - rmn) /
                        (rmx - rmn) *
                        (double)(x1 -
                            HtmlPadL);
            }
            yy = plot_y(ys[g][k],
                mn, mxx, flat,
                inner);
            fprintf(f, "%.1f,%.1f ",
                xx, yy);
        }
        fprintf(f,
            "\" fill=\"none\" "
            "stroke=\"%s\" "
            "stroke-width"
            "=\"2\"/>\n",
            golden_color(g));
    }
    for (i = 0; i < cnt; i++) {
        double xx = 0.0;
        if (rflat != 0) {
            xx = (double)HtmlPadL +
                ((double)(x1 -
                    HtmlPadL)) /
                    2.0;
        } else {
            xx = (double)HtmlPadL +
                (rate[i] - rmn) /
                    (rmx - rmn) *
                    (double)(x1 -
                        HtmlPadL);
        }
        for (g = 0; g < 3; g++) {
            double yy =
                plot_y(ys[g][i],
                    mn, mxx, flat,
                    inner);
            fprintf(f, "<circle "
                "cx=\"%.1f\" "
                "cy=\"%.1f\" r"
                "=\"2.5\" fill"
                "=\"%s\"/>\n",
                xx, yy,
                golden_color(g));
        }
    }
    for (g = 0; g < 3; g++) {
        double ly = 30.0 +
            (double)g * 14.0;
        fprintf(f,
            "<line x1=\"%d\" "
            "y1=\"%.1f\" "
            "x2=\"%d\" "
            "y2=\"%.1f\" "
            "stroke=\"%s\" "
            "stroke-width"
            "=\"3\"/>\n",
            HtmlPadL, ly,
            HtmlPadL + 24, ly,
            golden_color(g));
        fprintf(f,
            "<text x=\"%d\" "
            "y=\"%.1f\">%s</"
            "text>\n",
            HtmlPadL + 28,
            ly + 4.0,
            golden_tag(g));
    }
    fprintf(f, "<text x=\"%d\" "
        "y=\"20\" "
        "text-anchor=\"end\">%g "
        "milliseconds</text>\n",
        HtmlPadL - HtmlLabGap,
        mxx);
    fprintf(f, "<text x=\"%d\" "
        "y=\"%d\" "
        "text-anchor=\"end\">0 "
        "milliseconds</text>\n",
        HtmlPadL - HtmlLabGap,
        (int)base_y);
    fprintf(f, "<text x=\"%d\" "
        "y=\"%d\" ", x1,
        HtmlPlotH - 6);
    fputs("text-anchor=\"end\">",
        f);
    fputs("messages per "
        "second</text>\n", f);
    fputs("</svg>\n", f);
}

// Renders ramp ladders in HTML;
// silent unless ramp steps exist.
static void html_bench_ladder(
    FILE *f, Ev *evs, size_t n) {
    char ps[BenchCap][KeyCap];
    int np = 0;
    int phs[PhScan];
    int nn = 0;
    int j = 0;
    int q = 0;
    if (f == nullptr) {
        return;
    }
    np = ladder_parents(evs, n,
        ps);
    if (np == 0) {
        return;
    }
    nn = bench_phases(evs, n,
        phs, PhScan);
    fputs("<pre>ramp ladder plots "
        "p50 p99 max over achieved "
        "rate per step.</pre>\n",
        f);
    fputs("<pre><b>ramp ladder:"
        "</b></pre>\n", f);
    for (j = 0; j < np; j++) {
        for (q = 0; q < nn;
             q++) {
            double rate[
                LadderCap];
            double p50[
                LadderCap];
            double p99[
                LadderCap];
            double mx[LadderCap];
            char pn[PhName];
            int m = 0;
            m = ladder_pts(ps[j],
                phs[q], evs, n,
                rate, p50, p99,
                mx);
            if (m == 0) {
                continue;
            }
            phase_name(evs, n,
                phs[q], pn,
                sizeof pn);
            html_bench_ladder_one(
                f, ps[j], phs[q],
                pn, evs, n);
        }
    }
}

// Writes one overview chart: the
// golden trio across phases with
// the shared palette and legend.
static void html_bench_over_one(
    FILE *f, int k,
    OverRow *rows, int cnt) {
    double mn = 0.0;
    double mxx = 0.0;
    double inner = 0.0;
    int flat = 0;
    int x1 = 0;
    int i = 0;
    int g = 0;
    double ys[3][PhScan];
    if (f == nullptr ||
        rows == nullptr) {
        return;
    }
    if (cnt < 2 ||
        cnt > PhScan) {
        return;
    }
    for (i = 0; i < cnt; i++) {
        ys[0][i] = rows[i].p50;
        ys[1][i] = rows[i].p99;
        ys[2][i] = rows[i].mx;
    }
    mn = ys[0][0];
    mxx = ys[0][0];
    for (g = 0; g < 3; g++) {
        int j = 0;
        for (j = 0; j < cnt;
             j++) {
            if (ys[g][j] < mn) {
                mn = ys[g][j];
            }
            if (ys[g][j] > mxx) {
                mxx = ys[g][j];
            }
        }
    }
    x1 = HtmlW - HtmlPadR;
    inner = (double)(HtmlPlotH -
        12 - 24);
    flat = (mxx == mn);
    fprintf(f, "<pre><b>step "
        "%d overview</b>\n", k);
    fprintf(f, "  key: "
        "bench.*.%d\n", k);
    fputs("  axes: x is phase, y "
        "is milliseconds\n", f);
    for (i = 0; i < cnt; i++) {
        fprintf(f, "  %s [%d]: "
            "p50 %g p99 %g max "
            "%g\n",
            rows[i].name,
            rows[i].ph,
            rows[i].p50,
            rows[i].p99,
            rows[i].mx);
    }
    fputs("</pre>\n", f);
    fprintf(f, "<svg width=\"%d\" "
        "height=\"%d\" "
        "role=\"img\">\n", HtmlW,
        HtmlPlotH);
    for (g = 0; g < 3; g++) {
        int k = 0;
        fputs("<polyline "
            "points=\"", f);
        for (k = 0; k < cnt;
             k++) {
            double xx =
                (double)HtmlPadL +
                (double)k /
                    (double)(cnt -
                        1) *
                    (double)(x1 -
                        HtmlPadL);
            double yy = plot_y(
                ys[g][k], mn,
                mxx, flat,
                inner);
            fprintf(f, "%.1f,%.1f ",
                xx, yy);
        }
        fprintf(f,
            "\" fill=\"none\" "
            "stroke=\"%s\" "
            "stroke-width"
            "=\"2\"/>\n",
            golden_color(g));
    }
    for (i = 0; i < cnt; i++) {
        double xx =
            (double)HtmlPadL +
            (double)i /
                (double)(cnt -
                    1) *
                (double)(x1 -
                    HtmlPadL);
        for (g = 0; g < 3; g++) {
            double yy = plot_y(
                ys[g][i], mn,
                mxx, flat,
                inner);
            fprintf(f, "<circle "
                "cx=\"%.1f\" "
                "cy=\"%.1f\" r"
                "=\"2.5\" fill"
                "=\"%s\"/>\n",
                xx, yy,
                golden_color(g));
        }
        fprintf(f, "<text x=\"%.1f\" "
            "y=\"%d\" "
            "text-anchor"
            "=\"middle\">",
            xx, HtmlPlotH - 24);
        html_esc(f,
            rows[i].name);
        fputs("</text>\n", f);
    }
    for (g = 0; g < 3; g++) {
        double ly = 30.0 +
            (double)g * 14.0;
        fprintf(f,
            "<line x1=\"%d\" "
            "y1=\"%.1f\" "
            "x2=\"%d\" "
            "y2=\"%.1f\" "
            "stroke=\"%s\" "
            "stroke-width"
            "=\"3\"/>\n",
            HtmlPadL, ly,
            HtmlPadL + 24, ly,
            golden_color(g));
        fprintf(f,
            "<text x=\"%d\" "
            "y=\"%.1f\">%s "
            "milliseconds</"
            "text>\n",
            HtmlPadL + 28,
            ly + 4.0,
            golden_tag(g));
    }
    fprintf(f, "<text x=\"%d\" "
        "y=\"20\" "
        "text-anchor=\"end\">%g "
        "milliseconds</text>\n",
        HtmlPadL - HtmlLabGap,
        mxx);
    fputs("</svg>\n", f);
}

// Renders the cross-phase golden
// overview in HTML; silent unless
// a base spans two phases.
static void html_bench_overview(
    FILE *f, Ev *evs, size_t n) {
    int k = 0;
    int any = 0;
    if (f == nullptr) {
        return;
    }
    for (k = 0; k < OverCap;
         k++) {
        OverRow rows[PhScan];
        int cnt = 0;
        cnt = over_rows(k, evs,
            n, rows, PhScan);
        if (cnt >= 2) {
            any = 1;
            break;
        }
    }
    if (any == 0) {
        return;
    }
    fputs("<pre>run overview plots "
        "p50 p99 max across phases "
        "per step.</pre>\n", f);
    fputs("<pre><b>run overview:"
        "</b></pre>\n", f);
    for (k = 0; k < OverCap;
         k++) {
        OverRow rows[PhScan];
        int cnt = 0;
        cnt = over_rows(k, evs,
            n, rows, PhScan);
        if (cnt < 2) {
            continue;
        }
        html_bench_over_one(f, k,
            rows, cnt);
    }
}

static void html_bench(FILE *f,
    Ev *evs, size_t n,
    unsigned long long tmax) {
    if (f == nullptr) {
        return;
    }
    if (!bench_has_new(evs, n)) {
        return;
    }
    html_bench_lat(f, evs, n,
        tmax);
    html_bench_hist(f, evs, n,
        tmax);
    html_bench_sum(f, evs, n);
    html_bench_ladder(f, evs, n);
    html_bench_overview(f, evs,
        n);
}


// Renders one log section as pre
// blocks carrying the same lines
// the text report prints, in the
// same order, with the timeline and
// value SVGs inline. A null label
// prints no header, matching text.
// Missing classes note and pass;
// nothing here can crash on input.
static void html_section(FILE *f,
    const char *label, Ev *evs,
    size_t n, size_t bad, int trunc) {
    unsigned long long tmax = 0;
    size_t i = 0;
    int j = 0;
    (void)bad;
    int fold = 0;
    int have_v = 0;
    const char *vmsg = "";
    Mkey fk[HtmlKeys];
    Mkey rk[HtmlKeys];
    int nf = 0;
    int nr = 0;
    int fdrop = 0;
    int rdrop = 0;
    KeyUd ku;
    int no_ticks = 0;
    for (i = 0; i < n; i++) {
        if (streq(evs[i].k,
            "verdict")) {
            have_v = 1;
            vmsg = evs[i].msg;
        }
    }
    if (n > 0) {
        tmax = evs[n - 1].t;
    }
    if (label != nullptr) {
        fputs("<pre><b>== ", f);
        html_esc(f, label);
        fputs("</b></pre>\n", f);
    }
    if (have_v != 0 &&
        vmsg[0] != 0) {
        fputs("<pre><b>verdict: ",
            f);
        html_esc(f, vmsg);
        fputs("</b></pre>\n", f);
    } else {
        fputs("<pre><b>verdict: "
            "unknown</b></pre>\n",
            f);
    }
    fputs("<pre><b>timeline:</b>\n",
        f);
    for (i = 0; i < n; i++) {
        const Ev *e = &evs[i];
        if (!is_timeline(e->k)) {
            continue;
        }
        fprintf(f, "+%llums",
            (unsigned long long)
            e->t);
        if (e->has_ph != 0) {
            fprintf(f, " [ph %d]",
                e->ph);
        }
        fputc(' ', f);
        html_esc(f, e->k);
        if (e->msg[0] != 0) {
            fputc(' ', f);
            html_esc(f, e->msg);
        }
        fputc('\n', f);
    }
    fputs("</pre>\n", f);
    fold = html_timeline(f, evs, n,
        tmax);
    if (fold > 0) {
        fprintf(f, "<pre>note: %d "
            "phases folded into "
            "harness.</pre>\n",
            fold);
    }
    no_ticks = has_secs(evs, n) ?
        1 : 0;
    ku.tk = fk;
    ku.nt = &nf;
    ku.cls = 3;
    ku.drop = &fdrop;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg, key_cb,
            &ku);
    }
    cull_keys(fk, &nf, no_ticks);
    fputs("<pre>flow rate graphs "
        "show packet counters "
        "over\n", f);
    fputs("time.\n", f);
    fputs("Split by phase: one plot "
        "per key and phase; delta "
        "keys graph when "
        "present.</pre>\n", f);
    fputs("<pre><b>flow "
        "rate:</b></pre>\n", f);
    {
        int nm = 0;
        int delta = 0;
        int phs[PhScan];
        int q = 0;
        char pn[PhName];
        delta = has_delta_keys(fk,
            nf);
        for (j = 0; j < nf; j++) {
            size_t w = 0;
            double wv = 0.0;
            unsigned long long wt =
                0;
            int np = 0;
            if (delta != 0 &&
                !ends_with(fk[j].k,
                    ".delta")) {
                continue;
            }
            key_stats(fk[j].k, evs,
                n, &w, &wv, &wt);
            if (w == 0) {
                nm++;
                continue;
            }
            np = key_phases(fk[j].k,
                evs, n, phs,
                PhScan);
            for (q = 0; q < np;
                q++) {
                size_t m = 0;
                double v = 0.0;
                unsigned long long t =
                    0;
                key_phase_stats(
                    fk[j].k, phs[q],
                    evs, n, &m, &v,
                    &t);
                if (m >= 2) {
                    nm++;
                }
            }
        }
        if (nm == 0) {
            fputs("<pre>no counter "
                "metrics present."
                "</pre>\n", f);
        } else {
            for (j = 0; j < nf;
                j++) {
                size_t w = 0;
                double wv = 0.0;
                unsigned long long t =
                    0;
                int np = 0;
                if (delta != 0 &&
                    !ends_with(
                        fk[j].k,
                        ".delta")) {
                    continue;
                }
                key_stats(fk[j].k,
                    evs, n, &w, &wv,
                    &t);
                if (w == 0) {
                    phase_name(evs,
                        n, -1, pn,
                        sizeof pn);
                    html_plot(f,
                        fk[j].k, -1,
                        pn, evs, n,
                        tmax, 1);
                    continue;
                }
                np = key_phases(
                    fk[j].k, evs, n,
                    phs, PhScan);
                for (q = 0; q < np;
                    q++) {
                    size_t m = 0;
                    double v = 0.0;
                    unsigned long long
                        tt = 0;
                    key_phase_stats(
                        fk[j].k,
                        phs[q], evs,
                        n, &m, &v,
                        &tt);
                    if (m < 2) {
                        continue;
                    }
                    phase_name(evs,
                        n, phs[q], pn,
                        sizeof pn);
                    html_plot(f,
                        fk[j].k,
                        phs[q], pn,
                        evs, n, tmax,
                        1);
                }
            }
        }
    }
    ku.tk = rk;
    ku.nt = &nr;
    ku.drop = &rdrop;
    ku.cls = 1;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg, key_cb,
            &ku);
    }
    ku.cls = 2;
    for (i = 0; i < n; i++) {
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        tok_walk(evs[i].msg, key_cb,
            &ku);
    }
    cull_keys(rk, &nr, no_ticks);
    fputs("<pre>resources graphs "
        "show memory and cpu use "
        "over\n", f);
    fputs("time.\n", f);
    fputs("Split by phase like flow "
        "rate.\n", f);
    fputs("maxrss.mb is harness "
        "peak RAM; peakrss.mb, "
        "rss.bytes are per-node "
        "RAM; rchar, wchar are "
        "per-node process IO "
        "(disk plus net); flow "
        "keys are TUN "
        "network.</pre>\n", f);
    fputs("<pre><b>resources:</b>"
        "</pre>\n", f);
    {
        int nm = 0;
        int phs[PhScan];
        int q = 0;
        char pn[PhName];
        for (j = 0; j < nr; j++) {
            size_t w = 0;
            double wv = 0.0;
            unsigned long long wt =
                0;
            int np = 0;
            key_stats(rk[j].k, evs,
                n, &w, &wv, &wt);
            if (w == 0) {
                nm++;
                continue;
            }
            np = key_phases(rk[j].k,
                evs, n, phs,
                PhScan);
            for (q = 0; q < np;
                q++) {
                size_t m = 0;
                double v = 0.0;
                unsigned long long t =
                    0;
                key_phase_stats(
                    rk[j].k, phs[q],
                    evs, n, &m, &v,
                    &t);
                if (m >= 2) {
                    nm++;
                }
            }
        }
        if (nm == 0) {
            fputs("<pre>no resource "
                "metrics present."
                "</pre>\n", f);
        } else {
            for (j = 0; j < nr;
                j++) {
                size_t w = 0;
                double wv = 0.0;
                unsigned long long t =
                    0;
                int np = 0;
                key_stats(rk[j].k,
                    evs, n, &w, &wv,
                    &t);
                if (w == 0) {
                    phase_name(evs,
                        n, -1, pn,
                        sizeof pn);
                    html_plot(f,
                        rk[j].k, -1,
                        pn, evs, n,
                        tmax, 1);
                    continue;
                }
                np = key_phases(
                    rk[j].k, evs, n,
                    phs, PhScan);
                for (q = 0; q < np;
                    q++) {
                    size_t m = 0;
                    double v = 0.0;
                    unsigned long long
                        tt = 0;
                    key_phase_stats(
                        rk[j].k,
                        phs[q], evs,
                        n, &m, &v,
                        &tt);
                    if (m < 2) {
                        continue;
                    }
                    phase_name(evs,
                        n, phs[q], pn,
                        sizeof pn);
                    html_plot(f,
                        rk[j].k,
                        phs[q], pn,
                        evs, n, tmax,
                        1);
                }
            }
        }
    }
    fputs("<pre><b>status "
        "checks:</b></pre>\n", f);
    {
        int nc = 0;
        int delta = 0;
        int phs[PhScan];
        int q = 0;
        char pn[PhName];
        delta = has_delta_keys(fk,
            nf);
        for (j = 0; j < nf; j++) {
            int np = 0;
            if (delta != 0 &&
                !ends_with(fk[j].k,
                    ".delta")) {
                continue;
            }
            np = key_phases(fk[j].k,
                evs, n, phs,
                PhScan);
            for (q = 0; q < np;
                q++) {
                size_t m = 0;
                double v = 0.0;
                unsigned long long t =
                    0;
                key_phase_stats(
                    fk[j].k, phs[q],
                    evs, n, &m, &v,
                    &t);
                if (m == 1) {
                    nc++;
                }
            }
        }
        for (j = 0; j < nr; j++) {
            int np = key_phases(
                rk[j].k, evs, n,
                phs, PhScan);
            for (q = 0; q < np;
                q++) {
                size_t m = 0;
                double v = 0.0;
                unsigned long long t =
                    0;
                key_phase_stats(
                    rk[j].k, phs[q],
                    evs, n, &m, &v,
                    &t);
                if (m == 1) {
                    nc++;
                }
            }
        }
        if (nc == 0) {
            fputs("<pre>no status "
                "checks present."
                "</pre>\n", f);
        } else {
            for (j = 0; j < nf;
                j++) {
                int np = 0;
                if (delta != 0 &&
                    !ends_with(
                        fk[j].k,
                        ".delta")) {
                    continue;
                }
                np = key_phases(
                    fk[j].k, evs, n,
                    phs, PhScan);
                for (q = 0; q < np;
                    q++) {
                    size_t m = 0;
                    double v = 0.0;
                    unsigned long long
                        t = 0;
                    key_phase_stats(
                        fk[j].k,
                        phs[q], evs,
                        n, &m, &v,
                        &t);
                    if (m != 1) {
                        continue;
                    }
                    phase_name(evs,
                        n, phs[q], pn,
                        sizeof pn);
                    html_check(f,
                        fk[j].k,
                        phs[q], pn, v,
                        t);
                }
            }
            for (j = 0; j < nr;
                j++) {
                int np = key_phases(
                    rk[j].k, evs, n,
                    phs, PhScan);
                for (q = 0; q < np;
                    q++) {
                    size_t m = 0;
                    double v = 0.0;
                    unsigned long long
                        t = 0;
                    key_phase_stats(
                        rk[j].k,
                        phs[q], evs,
                        n, &m, &v,
                        &t);
                    if (m != 1) {
                        continue;
                    }
                    phase_name(evs,
                        n, phs[q], pn,
                        sizeof pn);
                    html_check(f,
                        rk[j].k,
                        phs[q], pn, v,
                        t);
                }
            }
        }
    }
    html_bench(f, evs, n, tmax);
    {
        char fl[FileCap][FileLen];
        int drop = 0;
        int nfl = collect_files(
            evs, n, fl, &drop);
        int k = 0;
        if (nfl > 0) {
            fputs("<pre><b>files:</b>"
                "\n", f);
            for (k = 0; k < nfl;
                k++) {
                fputs("  <a href=\"",
                    f);
                html_esc(f, fl[k]);
                fputs("\">", f);
                html_esc(f, fl[k]);
                fputs("</a>\n", f);
            }
            fputs("</pre>\n", f);
        }
    }
    if (trunc != 0) {
        fprintf(f, "<pre>note: "
            "truncated to %d "
            "events.</pre>\n", EvCap);
    }
}

// Renders path as one HTML section
// with label; false when the log
// will not open.
static bool html_one(FILE *f,
    const char *path,
    const char *label) {
    Ev *evs = calloc(
        (size_t)EvCap,
        sizeof *evs);
    size_t n = 0;
    size_t bad = 0;
    unsigned long long seq = 0;
    int trunc = 0;
    if (evs == nullptr) {
        fputs("<pre>out of memory."
            "</pre>\n", f);
        return (false);
    }
    if (!report_file(path, evs,
        &n, &seq, &trunc, &bad)) {
        fputs("<pre>cannot read ",
            f);
        html_esc(f, path);
        fputs(".</pre>\n", f);
        free(evs);
        return (false);
    }
    qsort(evs, n, sizeof *evs,
        ev_cmp);
    html_section(f, label, evs, n,
        bad, trunc);
    free(evs);
    return (true);
}

// Writes the full HTML doc for one
// log or a run dir of logs into
// out. 0 on success, 1 when logs
// miss or fail.
// Walk state for the html dir
// walk: out file plus the done
// section count html_foot needs.
typedef struct {
    FILE *f;
    int *nsec;
} HtmlWalkUd;

// html dir callback: one section
// per log, counting successes.
static bool html_one_cb(
    const char *full,
    const char *name, void *ud) {
    HtmlWalkUd *u = ud;
    if (u == nullptr ||
        u->f == nullptr ||
        u->nsec == nullptr) {
        return (false);
    }
    if (!html_one(u->f, full,
        name)) {
        return (false);
    }
    (*u->nsec)++;
    return (true);
}

static int html_report(
    const char *out,
    const char *path) {
    struct stat st;
    FILE *f = nullptr;
    int bad = 0;
    int nsec = 0;
    f = fopen(out, "w");
    if (f == nullptr) {
        fprintf(stderr, "rsuite: "
            "cannot write %s: %s\n",
            out,
            strerror(errno));
        return (ExFail);
    }
    // fopen follows umask; force
    // 0644 so readers see it.
    if (chmod(out, FileMode) != 0) {
        chmod_note(out);
    }
    if (stat(path, &st) != 0) {
        fprintf(stderr, "rsuite: "
            "cannot stat %s: %s\n",
            path,
            strerror(errno));
        fclose(f);
        return (ExFail);
    }
    html_head(f, "rsuite report");
    html_explainer(f);
    if (!S_ISDIR(st.st_mode)) {
        if (!html_one(f, path,
            nullptr)) {
            bad = 1;
        } else {
            nsec++;
        }
        html_foot(f, nsec);
        if (fclose(f) != 0) {
            fprintf(stderr,
                "rsuite: cannot "
                "write %s: %s\n",
                out,
                strerror(errno));
            return (ExFail);
        }
        if (bad != 0) {
            return (ExFail);
        }
        return (ExOk);
    }
    {
        HtmlWalkUd u;
        int w = 0;
        u.f = f;
        u.nsec = &nsec;
        w = each_log(path,
            html_one_cb, &u);
        if (w < 0) {
            fputs("<pre>no logs."
                "</pre>\n", f);
            html_foot(f, 0);
            fclose(f);
            return (ExFail);
        }
        if (w != 0) {
            bad = 1;
        }
        html_foot(f, nsec);
        if (fclose(f) != 0) {
            fprintf(stderr,
                "rsuite: cannot "
                "write %s: %s\n",
                out,
                strerror(errno));
            return (ExFail);
        }
        if (bad != 0) {
            return (ExFail);
        }
        return (ExOk);
    }
}

// True for URL bytes kept raw:
// letters, digits, unreserved marks,
// and slash; all else percent-encodes.
static bool url_raw(unsigned char c) {
    if (c >= 'a' && c <= 'z') {
        return (true);
    }
    if (c >= 'A' && c <= 'Z') {
        return (true);
    }
    if (c >= '0' && c <= '9') {
        return (true);
    }
    if (c == '-' || c == '_' ||
        c == '.' || c == '~' ||
        c == '/') {
        return (true);
    }
    return (false);
}

// Prints s as URL text: raw bytes
// pass, spaces plus non-ASCII and
// reserved bytes emit as %XX. The
// visible line keeps the raw path,
// so plain readers see no change.
static void url_print(
    const char *s) {
    static const char hd[] =
        "0123456789ABCDEF";
    for (; *s != 0; s++) {
        unsigned char c =
            (unsigned char)*s;
        if (url_raw(c)) {
            fputc(c, stdout);
            continue;
        }
        fprintf(stdout, "%%%c%c",
            hd[c >> 4],
            hd[c & 15u]);
    }
}

// Prints the report line: an OSC 8
// file hyperlink on a tty with
// escapes on, plain text elsewhere.
// TTY only: TAP and JSON parsers
// read stdout byte-exact, so one
// escape there corrupts the stream;
// piped and flagged runs thus keep
// the exact old bytes, as do dumb
// and NO_COLOR terms. The link wraps
// the same visible text, so display
// and copy read as before.
static void report_line(
    const char *out, int prog) {
    char abs[PathCap];
    const char *url = nullptr;
    if (prog == 0 ||
        prog_color() == 0) {
        printf("report %s\n", out);
        return;
    }
    url = out;
    if (out[0] != '/') {
        size_t dn = 0;
        size_t on = 0;
        if (getcwd(abs, sizeof abs)
            == nullptr) {
            printf("report %s\n",
                out);
            return;
        }
        dn = strlen(abs);
        on = strlen(out);
        if (dn + 1 + on + 1 >
            sizeof abs) {
            printf("report %s\n",
                out);
            return;
        }
        abs[dn] = '/';
        memcpy(abs + dn + 1, out,
            on + 1);
        url = abs;
    }
    printf("\x1b]8;;file://");
    url_print(url);
    printf("\x1b\\report %s"
        "\x1b]8;;\x1b\\\n", out);
}

// Renders per-run report.html from
// the run log dir through the one
// shared HTML path, then prints its
// path as a report line, linked on
// a tty, plain when piped. Best
// effort: a warn lands on stderr
// and the run verdict never moves.
static void auto_html(
    const char *dir, int prog) {
    char out[PathCap];
    int m = 0;

    if (dir == nullptr) {
        return;
    }
    m = snprintf(out, sizeof out,
        "%s/report.html", dir);
    if (m < 0 ||
        (size_t)m >= sizeof out) {
        fprintf(stderr, "rsuite: "
            "report path too long\n");
        return;
    }
    if (html_report(out, dir) !=
        ExOk) {
        return;
    }
    report_line(out, prog);
}

// Runs report: one log renders bare,
// a dir renders each .log sorted in
// its own section. 0 on success, 1
// when logs miss or fail, 2 on bad
// usage.
// Writes one log as Prometheus
// exposition: a gauge sample per
// numeric metric token. Non-numeric
// tokens skip; labels carry key,
// phase, and the log name in dir
// mode. Lossless and generic: every
// harness key exports unchanged.
static void prom_label_esc(FILE *f,
    const char *s) {
    if (f == nullptr ||
        s == nullptr) {
        return;
    }
    for (; *s != 0; s++) {
        if (*s == '\\' ||
            *s == '"') {
            fputc('\\', f);
            fputc(*s, f);
        } else if (*s == '\n') {
            fputs("\\n", f);
        } else {
            fputc(*s, f);
        }
    }
}

// prometheus walk state: out plus
// the phase plus the log label.
typedef struct {
    FILE *f;
    const char *log;
    int ph;
} PromUd;

// Emits one gauge line per numeric
// metric token; bad values skip.
static void prom_cb(const char *tok,
    size_t tl, const char *eq,
    void *ud) {
    PromUd *u = ud;
    char kb[KeyCap];
    char vb[32];
    size_t kl = 0;
    size_t vl = 0;
    double v = 0.0;
    if (u == nullptr ||
        u->f == nullptr) {
        return;
    }
    if (eq == nullptr) {
        return;
    }
    kl = (size_t)(eq - tok);
    if (kl == 0 ||
        kl >= sizeof kb) {
        return;
    }
    memcpy(kb, tok, kl);
    kb[kl] = 0;
    vl = tl - kl - 1;
    if (vl == 0 ||
        vl >= sizeof vb) {
        return;
    }
    memcpy(vb, eq + 1, vl);
    vb[vl] = 0;
    if (!num_of(vb, &v)) {
        return;
    }
    fputs("resonance_metric{key=\"",
        u->f);
    prom_label_esc(u->f, kb);
    fputs("\"", u->f);
    if (u->log != nullptr) {
        fputs(",log=\"", u->f);
        prom_label_esc(u->f,
            u->log);
        fputc('"', u->f);
    }
    fprintf(u->f,
        ",phase=\"%d\"} %g\n",
        u->ph, v);
}

// Exports one log; false when the
// log will not open.
static bool prom_one(FILE *f,
    const char *path,
    const char *label) {
    Ev *evs = calloc(
        (size_t)EvCap,
        sizeof *evs);
    size_t n = 0;
    size_t i = 0;
    size_t bad = 0;
    unsigned long long seq = 0;
    int trunc = 0;
    if (evs == nullptr) {
        fprintf(stderr, "rsuite: "
            "no mem\n");
        return (false);
    }
    if (!report_file(path, evs,
        &n, &seq, &trunc, &bad)) {
        fprintf(stderr, "rsuite: "
            "cannot read %s: %s\n",
            path,
            strerror(errno));
        free(evs);
        return (false);
    }
    qsort(evs, n, sizeof *evs,
        ev_cmp);
    for (i = 0; i < n; i++) {
        PromUd u;
        if (!streq(evs[i].k,
            "metric")) {
            continue;
        }
        u.f = f;
        u.log = label;
        u.ph = evs[i].ph;
        tok_walk(evs[i].msg,
            prom_cb, &u);
    }
    (void)trunc;
    (void)bad;
    free(evs);
    return (true);
}

// prom dir callback: stdout
// exposition per log.
static bool prom_one_cb(
    const char *full,
    const char *name, void *ud) {
    (void)ud;
    return (prom_one(stdout,
        full, name));
}

// Writes Prometheus exposition for
// one log or a run dir of logs to
// stdout. 0 on success, 1 when
// logs miss or fail.
static int prom_report(
    const char *path) {
    struct stat st;
    fputs("# HELP resonance_metric "
        "harness metric sample; "
        "key plus phase plus log "
        "select the series.\n",
        stdout);
    fputs("# TYPE resonance_metric "
        "gauge\n", stdout);
    if (stat(path, &st) != 0) {
        fprintf(stderr, "rsuite: "
            "cannot stat %s: %s\n",
            path,
            strerror(errno));
        return (ExFail);
    }
    if (!S_ISDIR(st.st_mode)) {
        if (!prom_one(stdout,
            path, nullptr)) {
            return (ExFail);
        }
        return (ExOk);
    }
    {
        int w = 0;
        w = each_log(path,
            prom_one_cb, nullptr);
        if (w != 0) {
            return (ExFail);
        }
    }
    return (ExOk);
}

// text dir callback: one text
// section per log.
static bool text_one_cb(
    const char *full,
    const char *name, void *ud) {
    (void)ud;
    return (report_one(full,
        name));
}

static int do_report(int argc,
    char **argv) {
    const char *path = ".";
    struct stat st;
    int i = 2;
    int bad = 0;
    const char *html = nullptr;
    const char *fmt = nullptr;
    while (i < argc) {
        const char *a = argv[i];
        if (streq(a, "--html")) {
            if (i + 1 >= argc) {
                return (use_err(
                    "report needs a "
                    "value after "
                    "--html"));
            }
            html = argv[i + 1];
            i += 2;
            continue;
        }
        if (is_opt(a, "--html")) {
            html = opt_val(a,
                "--html", argv, argc,
                &i);
            i++;
            continue;
        }
        if (streq(a, "--format")) {
            if (i + 1 >= argc) {
                return (use_err(
                    "report needs a "
                    "value after "
                    "--format"));
            }
            fmt = argv[i + 1];
            i += 2;
            continue;
        }
        if (is_opt(a, "--format")) {
            fmt = opt_val(a,
                "--format", argv,
                argc, &i);
            i++;
            continue;
        }
        if (streq(a, "--help") ||
            streq(a, "-h")) {
            show_report_use(
                stdout);
            return (ExOk);
        }
        if (a[0] == '-') {
            fprintf(stderr,
                "rsuite: unknown "
                "flag %s\n", a);
            show_use(stderr);
            return (ExUse);
        }
        break;
    }
    if (argc - i > 1) {
        return (use_err("report "
            "takes one path"));
    }
    if (argc - i == 1) {
        path = argv[i];
    }
    if (html != nullptr &&
        html[0] == 0) {
        return (use_err("report "
            "needs a non-empty "
            "--html"));
    }
    if (fmt != nullptr &&
        strcmp(fmt, "text") != 0 &&
        strcmp(fmt, "prom") != 0) {
        return (use_err("report "
            "format wants text "
            "or prom"));
    }
    if (html != nullptr &&
        fmt != nullptr &&
        strcmp(fmt, "prom") == 0) {
        return (use_err("report "
            "takes one of --html "
            "or --format prom"));
    }
    if (html != nullptr) {
        return (html_report(html,
            path));
    }
    if (fmt != nullptr &&
        strcmp(fmt, "prom") == 0) {
        return (prom_report(path));
    }
    if (stat(path, &st) != 0) {
        fprintf(stderr, "rsuite: "
            "cannot stat %s: %s\n",
            path,
            strerror(errno));
        return (ExFail);
    }
    print_explainer();
    if (!S_ISDIR(st.st_mode)) {
        if (!report_one(path,
            nullptr)) {
            return (ExFail);
        }
        return (ExOk);
    }
    {
        int w = 0;
        w = each_log(path,
            text_one_cb, nullptr);
        if (w < 0) {
            return (ExFail);
        }
        bad = w;
        if (bad != 0) {
            return (ExFail);
        }
        return (ExOk);
    }
}

// SHA-256 round constants.
static const uint32_t ShaK[64] = {
    0x428a2f98, 0x71374491,
    0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d,
    0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138,
    0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb,
    0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624,
    0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f,
    0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb,
    0xbef9a3f7, 0xc67178f2
};

// Rotates x right by n.
static uint32_t sha_ro(
    uint32_t x, int n) {
    return ((x >> (unsigned)n) |
        (x << (unsigned)(32 - n)));
}

// Hashes n bytes into out[ShaLen].
static void sha256(
    const unsigned char *in,
    size_t n, unsigned char out[]) {
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85,
        0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c,
        0x1f83d9ab, 0x5be0cd19
    };
    unsigned char *msg = nullptr;
    uint64_t bits = (uint64_t)n *
        8u;
    size_t plen = 0;
    size_t k = 0;
    size_t b = 0;
    plen = n + 1;
    while (plen % 64u != 56u) {
        plen++;
    }
    msg = calloc(plen + 8u, 1);
    if (msg == nullptr) {
        memset(out, 0,
            (size_t)ShaLen);
        return;
    }
    memcpy(msg, in, n);
    msg[n] = 0x80u;
    for (k = 0; k < 8u; k++) {
        msg[plen + k] =
            (unsigned char)(bits >>
            (unsigned)(56u -
            8u * k));
    }
    for (b = 0; b < plen + 8u;
        b += 64u) {
        uint32_t w[64];
        uint32_t a = 0;
        uint32_t bb = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        uint32_t e2 = 0;
        uint32_t f = 0;
        uint32_t g = 0;
        uint32_t hh = 0;
        for (k = 0; k < 16u;
            k++) {
            size_t o = b +
                k * 4u;
            w[k] =
                ((uint32_t)
                msg[o] << 24u) |
                ((uint32_t)
                msg[o + 1]
                << 16u) |
                ((uint32_t)
                msg[o + 2]
                << 8u) |
                (uint32_t)
                msg[o + 3];
        }
        for (k = 16u; k < 64u;
            k++) {
            uint32_t s0 =
                sha_ro(w[k - 15u],
                7) ^
                sha_ro(w[k - 15u],
                18) ^
                (w[k - 15u] >>
                3u);
            uint32_t s1 =
                sha_ro(w[k - 2u],
                17) ^
                sha_ro(w[k - 2u],
                19) ^
                (w[k - 2u] >>
                10u);
            w[k] = w[k - 16u] +
                s0 + w[k - 7u] +
                s1;
        }
        a = h[0];
        bb = h[1];
        c = h[2];
        d = h[3];
        e2 = h[4];
        f = h[5];
        g = h[6];
        hh = h[7];
        for (k = 0; k < 64u;
            k++) {
            uint32_t s1 =
                sha_ro(e2, 6) ^
                sha_ro(e2, 11) ^
                sha_ro(e2, 25);
            uint32_t ch =
                (e2 & f) ^
                (~e2 & g);
            uint32_t t1 = hh +
                s1 + ch + ShaK[k] +
                w[k];
            uint32_t s0 =
                sha_ro(a, 2) ^
                sha_ro(a, 13) ^
                sha_ro(a, 22);
            uint32_t mj =
                (a & bb) ^
                (a & c) ^
                (bb & c);
            uint32_t t2 = s0 +
                mj;
            hh = g;
            g = f;
            f = e2;
            e2 = d + t1;
            d = c;
            c = bb;
            bb = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += bb;
        h[2] += c;
        h[3] += d;
        h[4] += e2;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    free(msg);
    for (k = 0; k < 8u; k++) {
        out[k * 4u] =
            (unsigned char)
            (h[k] >> 24u);
        out[k * 4u + 1u] =
            (unsigned char)
            (h[k] >> 16u);
        out[k * 4u + 2u] =
            (unsigned char)
            (h[k] >> 8u);
        out[k * 4u + 3u] =
            (unsigned char)h[k];
    }
}

// Writes the hex SHA-256 of path
// into hex[65]; false on read fail.
static bool file_sha256(
    const char *path,
    char hex[65]) {
    static const char dig[] =
        "0123456789abcdef";
    char *tx = nullptr;
    size_t tn = 0;
    unsigned char sum[32];
    int k = 0;
    if (read_all(path, &tx, &tn)
        == 0) {
        return (false);
    }
    sha256(
        (const unsigned char *)tx,
        tn, sum);
    free(tx);
    for (k = 0; k < ShaLen;
        k++) {
        hex[2 * k] =
            dig[sum[k] >> 4u];
        hex[2 * k + 1] =
            dig[sum[k] & 15u];
    }
    hex[64] = 0;
    return (true);
}

// Probes git for the short commit;
// "unknown" when unavailable.
static void git_commit(char *dst,
    size_t n) {
    FILE *p = nullptr;
    snprintf(dst, n, "unknown");
    p = popen("git rev-parse "
        "--short HEAD 2>/dev/null",
        "r");
    if (p == nullptr) {
        return;
    }
    if (fgets(dst, (int)n, p) !=
        nullptr) {
        size_t k = strlen(dst);
        while (k > 0 &&
            (dst[k - 1] == '\n' ||
            dst[k - 1] == '\r')) {
            k--;
            dst[k] = 0;
        }
        if (dst[0] == 0) {
            snprintf(dst, n,
                "unknown");
        }
    }
    pclose(p);
}

// Writes a UTC date stamp.
static void utc_now(char *dst,
    size_t n) {
    time_t t = time(nullptr);
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (gmtime_r(&t, &tm) ==
        nullptr) {
        snprintf(dst, n,
            "unknown");
        return;
    }
    if (strftime(dst, n,
        "%Y-%m-%dT%H:%M:%SZ",
        &tm) == 0) {
        snprintf(dst, n,
            "unknown");
    }
}

// Prints s shell quoted for
// rerun.sh: raw when plain, else
// single quoted with ' escaped.
static void sh_word(FILE *f,
    const char *s) {
    int plain = 1;
    const char *p = s;
    if (s[0] == 0) {
        plain = 0;
    }
    for (; *p != 0; p++) {
        if (*p == ' ' ||
            *p == '\t' ||
            *p == '"' ||
            *p == '\'' ||
            *p == '\\' ||
            *p == '$' ||
            *p == '`') {
            plain = 0;
        }
    }
    if (plain != 0) {
        fputs(s, f);
        return;
    }
    fputc('\'', f);
    for (; *s != 0; s++) {
        if (*s == '\'') {
            fputs("'\\''", f);
        } else {
            fputc(*s, f);
        }
    }
    fputc('\'', f);
}

// Reports docker presence as an
// observed string; never executes
// anything.
static const char *docker_seen(
    void) {
    if (acc_ok("/usr/bin/docker",
        X_OK) != 0) {
        return ("present");
    }
    if (acc_ok(
        "/usr/local/bin/docker",
        X_OK) != 0) {
        return ("present");
    }
    return ("absent");
}

// Shows bundle help only.
static void show_bundle_use(
    FILE *out) {
    fputs("usage: rsuite bundle",
        out);
    fputs(" FILE.yaml", out);
    fputs(" [--out DIR]\n", out);
    fputs("\n", out);
    show_exits(out);
    fputs("seals a reproduction",
        out);
    fputs(" bundle: copies the YAML,",
        out);
    fputs(" writes manifest.json",
        out);
    fputs(" with commit, date, argv,",
        out);
    fputs(" and file sha, notes",
        out);
    fputs(" concord, weld, and",
        out);
    fputs(" docker presence as",
        out);
    fputs(" observed strings, and",
        out);
    fputs(" emits rerun.sh.",
        out);
    fputs(" Default DIR is",
        out);
    fputs(" base-bundle in the",
        out);
    fputs(" current dir. Bundling",
        out);
    fputs(" never executes the",
        out);
    fputs(" suite and needs no",
        out);
    fputs(" privs.\n", out);
}

// Runs bundle: copies the YAML,
// writes the manifest and rerun.sh,
// prints the bundle dir. 0 on
// success, 1 on IO failure, 2 on
// bad usage.
static int do_bundle(int argc,
    char **argv) {
    const char *in = nullptr;
    const char *dir = nullptr;
    char ddflt[PathCap];
    char *tx = nullptr;
    size_t tn = 0;
    const char *base = nullptr;
    char ypath[PathCap];
    char mpath[PathCap];
    char rpath[PathCap];
    char hex[65];
    char commit[64];
    char date[32];
    char conc[PathCap];
    char weld[PathCap];
    FILE *f = nullptr;
    int i = 2;
    int r = 0;
    while (i < argc) {
        const char *a = argv[i];
        if (streq(a, "--help") ||
            streq(a, "-h")) {
            show_bundle_use(
                stdout);
            return (ExOk);
        }
        if (is_opt(a, "--out")) {
            if (streq(a,
                "--out") &&
                i + 1 >= argc) {
                return (use_err(
                    "bundle needs a"
                    " value after "
                    "--out"));
            }
            dir = opt_val(a,
                "--out", argv,
                argc, &i);
            i++;
            continue;
        }
        if (a[0] == '-') {
            fprintf(stderr,
                "rsuite: unknown "
                "flag %s\n", a);
            show_use(stderr);
            return (ExUse);
        }
        if (in != nullptr) {
            return (use_err(
                "bundle takes one"
                " file"));
        }
        in = a;
        i++;
    }
    if (in == nullptr) {
        return (use_err("bundle "
            "needs a file"));
    }
    if (dir != nullptr &&
        dir[0] == 0) {
        return (use_err("bundle "
            "needs a non-empty "
            "--out"));
    }
    if (read_all(in, &tx, &tn)
        == 0) {
        fprintf(stderr, "rsuite: "
            "cannot read %s: %s\n",
            in, strerror(errno));
        return (ExFail);
    }
    base = base_of(in);
    if (dir == nullptr) {
        size_t bl = strlen(base);
        if (bl > 5 && strcmp(
            base + bl - 5, ".yaml")
            == 0) {
            bl -= 5;
        }
        r = snprintf(ddflt,
            sizeof ddflt,
            "%.*s-bundle", (int)bl,
            base);
        if (r < 0 ||
            (size_t)r >=
                sizeof ddflt) {
            free(tx);
            return (use_err(
                "bundle path too"
                " long"));
        }
        dir = ddflt;
    }
    if (ensure_logdir(dir) == 0) {
        fprintf(stderr, "rsuite: "
            "cannot use dir %s: "
            "%s\n", dir,
            strerror(errno));
        free(tx);
        return (ExFail);
    }
    r = snprintf(ypath,
        sizeof ypath, "%s/%s",
        dir, base);
    if (r < 0 || (size_t)r >=
        sizeof ypath) {
        free(tx);
        return (use_err("bundle "
            "path too long"));
    }
    f = fopen(ypath, "w");
    if (f == nullptr) {
        fprintf(stderr, "rsuite: "
            "cannot write %s: "
            "%s\n", ypath,
            strerror(errno));
        free(tx);
        return (ExFail);
    }
    if (tn > 0 && fwrite(tx, 1,
        tn, f) != tn) {
        fprintf(stderr, "rsuite: "
            "cannot write %s: "
            "%s\n", ypath,
            strerror(errno));
        fclose(f);
        free(tx);
        return (ExFail);
    }
    fclose(f);
    if (chmod(ypath, FileMode)
        != 0) {
        chmod_note(ypath);
    }
    if (!file_sha256(in, hex)) {
        fprintf(stderr, "rsuite: "
            "cannot hash %s\n",
            in);
        free(tx);
        return (ExFail);
    }
    free(tx);
    git_commit(commit,
        sizeof commit);
    utc_now(date, sizeof date);
    exe_sib(conc, sizeof conc,
        "concord");
    exe_sib(weld, sizeof weld,
        "weld");
    r = snprintf(mpath,
        sizeof mpath,
        "%s/manifest.json", dir);
    if (r < 0 || (size_t)r >=
        sizeof mpath) {
        return (use_err("bundle "
            "path too long"));
    }
    f = fopen(mpath, "w");
    if (f == nullptr) {
        fprintf(stderr, "rsuite: "
            "cannot write %s: "
            "%s\n", mpath,
            strerror(errno));
        return (ExFail);
    }
    fputs("{\n", f);
    fputs("  \"rsuite\": ", f);
    json_str(f, commit);
    fputs(",\n  \"date\": ", f);
    json_str(f, date);
    fputs(",\n  \"argv\": [", f);
    for (i = 0; i < argc; i++) {
        if (i > 0) {
            fputs(", ", f);
        }
        json_str(f, argv[i]);
    }
    fputs("],\n  \"file\": ", f);
    json_str(f, base);
    fputs(",\n  \"sha256\": ", f);
    json_str(f, hex);
    fputs(",\n  \"concord\": ",
        f);
    json_str(f, conc);
    fprintf(f, ",\n  \"concord_"
        "present\": %s",
        acc_ok(conc, X_OK) != 0 ?
        "true" : "false");
    fputs(",\n  \"weld\": ", f);
    json_str(f, weld);
    fprintf(f, ",\n  \"weld_"
        "present\": %s",
        acc_ok(weld, X_OK) != 0 ?
        "true" : "false");
    fputs(",\n  \"docker\": ", f);
    json_str(f, docker_seen());
    fputs("\n}\n", f);
    fclose(f);
    if (chmod(mpath, FileMode)
        != 0) {
        chmod_note(mpath);
    }
    r = snprintf(rpath,
        sizeof rpath, "%s/"
        "rerun.sh", dir);
    if (r < 0 || (size_t)r >=
        sizeof rpath) {
        return (use_err("bundle "
            "path too long"));
    }
    f = fopen(rpath, "w");
    if (f == nullptr) {
        fprintf(stderr, "rsuite: "
            "cannot write %s: "
            "%s\n", rpath,
            strerror(errno));
        return (ExFail);
    }
    fputs("#!/bin/sh\n", f);
    fputs("# Replays the bundled",
        f);
    fputs(" suite file.\n", f);
    fputs("# Limits: run ids",
        f);
    fputs(" re-acquire on each\n",
        f);
    fputs("# run; no SUT pinning",
        f);
    fputs(" is recorded.\n", f);
    fputs("exec rsuite run ", f);
    sh_word(f, base);
    fputc('\n', f);
    fclose(f);
    if (chmod(rpath, 0755)
        != 0) {
        fprintf(stderr, "rsuite: "
            "cannot chmod %s: "
            "%s\n", rpath,
            strerror(errno));
        return (ExFail);
    }
    printf("%s\n", dir);
    return (ExOk);
}

// Entry; dispatches subcommands.
int main(int argc, char **argv) {
    if (argc < 2) {
        return (use_err("needs a"
            " subcommand"));
    }
    if (streq(argv[1], "--help") ||
        streq(argv[1], "-h")) {
        show_use(stdout);
        return (ExOk);
    }
    if (streq(argv[1], "lint")) {
        if (argc < 3) {
            return (use_err(
                "lint needs at "
                "least one file"));
        }
        if (argc == 3 &&
            (streq(argv[2],
                "--help") ||
            streq(argv[2], "-h"))) {
            show_lint_use(stdout);
            return (ExOk);
        }
        return (do_lint(argc - 2,
            argv + 2));
    }
    if (streq(argv[1], "run")) {
        return (do_run_args(argc,
            argv));
    }
    if (streq(argv[1], "list")) {
        if (argc < 3) {
            return (use_err(
                "list needs at "
                "least one file"));
        }
        if (argc == 3 &&
            (streq(argv[2],
                "--help") ||
            streq(argv[2], "-h"))) {
            show_list_use(stdout);
            return (ExOk);
        }
        return (do_list(argc - 2,
            argv + 2));
    }
    if (streq(argv[1], "nemesis")) {
        return (do_nemesis(argc,
            argv));
    }
    if (streq(argv[1],
        "minimize")) {
        return (do_minimize(argc,
            argv));
    }
    if (streq(argv[1], "report")) {
        return (do_report(argc,
            argv));
    }
    if (streq(argv[1], "bundle")) {
        return (do_bundle(argc,
            argv));
    }
    if (streq(argv[1], "doctor")) {
        if (argc == 2) {
            return (do_doctor());
        }
        if (argc == 3 &&
            (streq(argv[2],
                "--help") ||
            streq(argv[2], "-h"))) {
            show_doctor_use(stdout);
            return (ExOk);
        }
        return (use_err("doctor "
            "takes no arguments"));
    }
    fprintf(stderr,
        "rsuite: unknown "
        "subcommand %s\n", argv[1]);
    show_use(stderr);
    return (ExUse);
}
