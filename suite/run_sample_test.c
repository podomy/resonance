#define _GNU_SOURCE
#include "run/run.h"

#include <assert.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Run sampler over real sleep kids:
// samples arrive with sane rss,
// peaks never drop, dead pids add
// nothing. No sleeps, no privs.

// Forced passes past the store depth.
static constexpr int RS_MORE =
    RUN_SAMPLE_CAP + 2;

// Forks sleep 30. Returns the pid.
static pid_t rs_mk_child(void) {
    pid_t pid;

    pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        execlp("sleep", "sleep",
               "30", (char *)nullptr);
        _exit(127);
    }
    return (pid);
}

// Empties a run: two nodes, every
// pid, log, and TUN fd at -1, plus
// a fresh sample store.
static void rs_mk_run(Run *r,
                      int n) {
    int i;

    free(r->shist);
    memset(r, 0, sizeof(*r));
    r->n = n;
    r->dead = -1;
    for (i = 0; i < TUN_MAP_MAX;
         i++) {
        r->pids[i] = -1;
        r->logfds[i] = -1;
        r->fds[i] = -1;
    }
    r->shist = calloc(
        (size_t)TUN_MAP_MAX *
            (size_t)RUN_SAMPLE_CAP,
        sizeof(RunSample));
    assert(r->shist != nullptr);
}

// Frees a run store made by
// rs_mk_run. Call at test end.
static void rs_free_run(Run *r) {
    free(r->shist);
    r->shist = nullptr;
}

// Empty history answers false and
// zero for every bad shape.
static void rs_empty(void) {
    static Run r;
    RunSample out;
    unsigned long long a;
    unsigned long long b;

    rs_mk_run(&r, 0);
    run_sample_tick(&r);
    run_sample_tick(nullptr);
    assert(!run_sample_get(
        nullptr, 0, &out));
    assert(!run_sample_get(&r, 0,
                           &out));
    assert(!run_sample_get(
        &r, -1, &out));
    assert(!run_sample_get(
        &r, 99, &out));
    assert(!run_sample_get(
        &r, 0, nullptr));
    assert(!run_sample_hist(
        &r, 0, 0, &out));
    assert(!run_sample_hist(
        &r, 0, -1, &out));
    assert(!run_sample_hist(
        &r, 0, RUN_SAMPLE_CAP,
        &out));
    assert(run_sample_count(
               nullptr, 0) == 0);
    assert(run_sample_count(&r,
                            0) == 0);
    assert(run_sample_count(
               &r, -1) == 0);
    assert(!run_sample_peak(
        nullptr, 0, &a));
    assert(!run_sample_peak(
        &r, 0, nullptr));
    assert(!run_sample_peak(
        &r, 0, &a));
    assert(!run_sample_cpu(
        nullptr, 0, &a, &b));
    assert(!run_sample_cpu(
        &r, 0, nullptr, &b));
    assert(!run_sample_cpu(
        &r, 0, &a, nullptr));
    assert(!run_sample_cpu(
        &r, 0, &a, &b));
    rs_free_run(&r);
}

// True when pid has execed into
// sleep: comm reads (sleep). Ends
// the pre-exec race where the fork
// copy still shows rss from the
// parent image.
static bool rs_execed(pid_t pid) {
    char path[64];
    char buf[256];
    char *a;
    char *b;
    ssize_t k;
    int fd;
    int m;

    m = snprintf(path, sizeof(path),
                 "/proc/%ld/stat",
                 (long)pid);
    if (m < 0 ||
        m >= (int)sizeof(path))
        return (false);
    fd = open(path,
              O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return (false);
    k = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (k <= 0)
        return (false);
    buf[k] = '\0';
    a = strchr(buf, '(');
    b = strrchr(buf, ')');
    if (a == nullptr || b == nullptr ||
        b <= a + 1)
        return (false);
    b[0] = '\0';
    return (strcmp(a + 1, "sleep") ==
            0);
}

// Spins until pid shows rss above
// zero past exec. A live child always
// gets scheduled, so this ends in ms;
// the bound only fires on a dead
// system. Yields, never sleeps.
static void rs_wait_rss(pid_t pid) {
    static Run tmp;
    RunSample (*keep)[RUN_SAMPLE_CAP] =
        nullptr;
    time_t end;

    memset(&tmp, 0, sizeof(tmp));
    tmp.shist = calloc(
        (size_t)TUN_MAP_MAX *
            (size_t)RUN_SAMPLE_CAP,
        sizeof(RunSample));
    assert(tmp.shist != nullptr);
    end = time(nullptr) + 5;
    for (;;) {
        RunSample s;

        keep = tmp.shist;
        memset(&tmp, 0,
               sizeof(tmp));
        tmp.shist = keep;
        tmp.n = 1;
        tmp.dead = -1;
        tmp.pids[0] = pid;
        tmp.slast_ms = 0;
        run_sample_tick(&tmp);
        if (run_sample_get(&tmp, 0,
                           &s) &&
            s.rss_bytes > 0 &&
            rs_execed(pid)) {
            free(tmp.shist);
            tmp.shist = nullptr;
            return;
        }
        assert(time(nullptr) <
               end);
        sched_yield();
    }
}

// Live kids sample: pid matches,
// rss sane, peak equals rss, cpu
// reads, back slots line up.
static void rs_live(void) {
    static Run r;
    RunSample got;
    RunSample back;
    unsigned long long peak;
    unsigned long long ut;
    unsigned long long st;
    pid_t c0;
    pid_t c1;
    int w;

    c0 = rs_mk_child();
    c1 = rs_mk_child();
    rs_wait_rss(c0);
    rs_wait_rss(c1);
    rs_mk_run(&r, 2);
    r.pids[0] = c0;
    r.pids[1] = c1;
    r.slast_ms = 0;
    run_sample_tick(&r);
    assert(run_sample_count(&r,
                            0) == 1);
    assert(run_sample_count(&r,
                            1) == 1);
    assert(run_sample_get(&r, 0,
                          &got));
    assert(got.valid);
    assert(got.pid == c0);
    assert(got.rss_bytes > 0);
    assert(got.at != 0);
    assert(run_sample_peak(&r, 0,
                           &peak));
    assert(peak == got.rss_bytes);
    assert(run_sample_cpu(&r, 0,
                          &ut, &st));
    assert(run_sample_hist(&r, 0,
                           0, &back));
    assert(back.pid == c0);
    assert(back.rss_bytes ==
           got.rss_bytes);
    assert(!run_sample_hist(
        &r, 0, 1, &back));
    assert(!run_sample_hist(
        &r, 0, -1, &back));
    assert(!run_sample_hist(
        &r, 2, 0, &back));
    assert(run_sample_get(&r, 1,
                          &back));
    assert(back.pid == c1);
    kill(c0, SIGKILL);
    kill(c1, SIGKILL);
    w = 0;
    assert(waitpid(c0, &w, 0) ==
           c0);
    assert(waitpid(c1, &w, 0) ==
           c1);
    rs_free_run(&r);
}

// Back-to-back ticks add nothing
// (250 ms gate); clearing the
// stamp forces the next pass.
static void rs_gate(void) {
    static Run r;
    pid_t c0;
    int w;

    c0 = rs_mk_child();
    rs_mk_run(&r, 1);
    r.pids[0] = c0;
    r.slast_ms = 0;
    run_sample_tick(&r);
    assert(run_sample_count(&r,
                            0) == 1);
    run_sample_tick(&r);
    assert(run_sample_count(&r,
                            0) == 1);
    r.slast_ms = 0;
    run_sample_tick(&r);
    assert(run_sample_count(&r,
                            0) == 2);
    kill(c0, SIGKILL);
    w = 0;
    assert(waitpid(c0, &w, 0) ==
           c0);
    rs_free_run(&r);
}

// Forced passes past depth cap the
// store, keep every kept slot valid,
// never drop peaks, and count the
// rest as dropped, never overwrite.
static void rs_ring(void) {
    static Run r;
    unsigned long long last;
    unsigned long long peak;
    RunSample s;
    pid_t c0;
    int i;
    int w;

    c0 = rs_mk_child();
    rs_wait_rss(c0);
    rs_mk_run(&r, 1);
    r.pids[0] = c0;
    last = 0;
    for (i = 0; i < RS_MORE; i++) {
        r.slast_ms = 0;
        run_sample_tick(&r);
        assert(run_sample_peak(
            &r, 0, &peak));
        assert(peak >= last);
        last = peak;
    }
    assert(run_sample_count(
               &r, 0) ==
           RUN_SAMPLE_CAP);
    assert(run_sample_dropped(
               &r, 0) == 2);
    assert(run_sample_hist(
        &r, 0,
        RUN_SAMPLE_CAP - 1, &s));
    assert(s.valid);
    assert(s.pid == c0);
    for (i = 0; i <
         RUN_SAMPLE_CAP; i++) {
        assert(run_sample_hist(
            &r, 0, i, &s));
        assert(s.valid);
        assert(s.pid == c0);
        assert(s.rss_bytes > 0);
        assert(s.rss_bytes <=
               peak);
    }
    assert(!run_sample_hist(
        &r, 0, RUN_SAMPLE_CAP,
        &s));
    kill(c0, SIGKILL);
    w = 0;
    assert(waitpid(c0, &w, 0) ==
           c0);
    rs_free_run(&r);
}

// Dead pid adds no sample; the live
// sibling keeps sampling; alive
// tracks each reap and clears slots.
static void rs_dead(void) {
    static Run r;
    RunSample before;
    RunSample after;
    pid_t c0;
    pid_t c1;
    int w;

    c0 = rs_mk_child();
    c1 = rs_mk_child();
    rs_wait_rss(c0);
    rs_wait_rss(c1);
    rs_mk_run(&r, 2);
    r.pids[0] = c0;
    r.pids[1] = c1;
    r.slast_ms = 0;
    run_sample_tick(&r);
    assert(run_alive(&r, -1));
    assert(run_alive(&r, 0));
    assert(!run_alive(nullptr,
                      -1));
    kill(c0, SIGKILL);
    w = 0;
    assert(waitpid(c0, &w, 0) ==
           c0);
    assert(run_sample_get(&r, 0,
                          &before));
    r.slast_ms = 0;
    run_sample_tick(&r);
    assert(run_sample_get(&r, 0,
                          &after));
    assert(after.pid == c0);
    assert(after.at == before.at);
    assert(after.rss_bytes ==
           before.rss_bytes);
    assert(run_sample_count(
               &r, 1) == 2);
    assert(run_alive(&r, -1));
    assert(!run_alive(&r, 1));
    kill(c1, SIGKILL);
    w = 0;
    assert(waitpid(c1, &w, 0) ==
           c1);
    assert(!run_alive(&r, -1));
    assert(r.pids[0] == -1);
    assert(r.pids[1] == -1);
    rs_free_run(&r);
}

// One stay-dead slot: the second
// note keeps the first; rearm
// clears the mark.
static void rs_note_rearm(void) {
    static Run r;

    rs_mk_run(&r, 2);
    run_note_dead(nullptr, 0);
    run_note_dead(&r, -1);
    run_note_dead(&r, 2);
    run_note_dead(&r, 1);
    assert(r.dead == 1);
    run_note_dead(&r, 0);
    assert(r.dead == 1);
    run_rearm(nullptr, 0);
    run_rearm(&r, -1);
    run_rearm(&r, 2);
    run_rearm(&r, 1);
    assert(r.dead == -1);
    rs_free_run(&r);
}

// Hold arms then fires; throttle
// fires at most once per window.
// Zero windows stay deterministic.
static void rs_hold_throttle(void) {
    time_t t;

    assert(!run_hold(nullptr,
                     10));
    t = 0;
    assert(!run_hold(&t, 100));
    assert(t != 0);
    assert(!run_hold(&t, 100));
    assert(run_hold(&t, 0));
    assert(!run_throttle(nullptr,
                         10));
    t = 0;
    assert(run_throttle(&t,
                        100));
    assert(!run_throttle(&t,
                         100));
    assert(run_throttle(&t, 0));
}

// Runs all checks. Returns 0.
int main(void) {
    rs_empty();
    rs_live();
    rs_gate();
    rs_ring();
    rs_dead();
    rs_note_rearm();
    rs_hold_throttle();
    printf("run_sample_test: ok\n");
    return (0);
}
