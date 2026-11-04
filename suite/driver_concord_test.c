#include "../driver/concord.c"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Driver concord gates plus arg
// rejects. No daemons, no shell
// success paths: every positive
// shell call is avoided, rejects
// return before fork or popen.

// Image refs: alnum plus _ . : /
// @ + , - only.
static void dc_image(void) {
    assert(image_ok("probe"));
    assert(image_ok("probe:v2"));
    assert(image_ok(
        "reg.io/x@y:1+z,a-b_c.d/e"));
    assert(!image_ok(""));
    assert(!image_ok(nullptr));
    assert(!image_ok("bad!!"));
    assert(!image_ok("a b"));
    assert(!image_ok("a;b"));
    assert(!image_ok("a`b"));
    assert(!image_ok("a$b"));
    assert(!image_ok("a\"b"));
}

// Flags: [A-Za-z0-9_=., /-] only.
// Raw percent must resolve first.
static void dc_flags(void) {
    assert(flags_ok(""));
    assert(flags_ok("--memory 100"));
    assert(flags_ok(
        "a=b,c.d/e-f_g h"));
    assert(!flags_ok(nullptr));
    assert(!flags_ok("a;b"));
    assert(!flags_ok("x|y"));
    assert(!flags_ok("x&y"));
    assert(!flags_ok("x$y"));
    assert(!flags_ok("60%host"));
    assert(!flags_ok("x`y"));
}

// Statedir: [A-Za-z0-9/_.-] only.
static void dc_statedir(void) {
    assert(statedir_ok(
        "/var/tmp/resonance"));
    assert(statedir_ok("a"));
    assert(statedir_ok("a.b-c_d/e"));
    assert(!statedir_ok(nullptr));
    assert(!statedir_ok(""));
    assert(!statedir_ok("a b"));
    assert(!statedir_ok("a;b"));
    assert(!statedir_ok("a*"));
    assert(!statedir_ok("a$"));
}

// Wid: hex plus dash only.
static void dc_wid(void) {
    assert(wid_ok("abc123"));
    assert(wid_ok("ABCDEF-012345"));
    assert(wid_ok("00000000-0000-"
                 "0000-0000-"
                 "000000000000"));
    assert(!wid_ok(nullptr));
    assert(!wid_ok(""));
    assert(!wid_ok("xyz"));
    assert(!wid_ok("workload-1"));
    assert(!wid_ok("a b"));
    assert(!wid_ok("a/b"));
}

// Null bin means default; only
// length gates an explicit path.
static void dc_bin(void) {
    char big[300];
    char edge[256];
    size_t k;

    assert(bin_ok(nullptr));
    assert(!bin_ok(""));
    assert(bin_ok("/bin/true"));
    memset(big, 'a', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    assert(!bin_ok(big));
    for (k = 0; k < sizeof(edge) - 1;
         k++) {
        edge[k] = 'a';
    }
    edge[sizeof(edge) - 1] = '\0';
    assert(bin_ok(edge));
}

// Delim after %host: end, space,
// or comma only.
static void dc_pct_delim(void) {
    assert(pct_delim('\0'));
    assert(pct_delim(' '));
    assert(pct_delim(','));
    assert(!pct_delim('x'));
    assert(!pct_delim(';'));
    assert(!pct_delim('%'));
}

// Percent expansion with explicit
// free MB: pure, no /proc read.
static void dc_resolve(void) {
    char out[64];

    assert(resolve_host_percent(
        "--memory 60%host", out,
        sizeof(out), 1000ULL));
    assert(strcmp(out,
                 "--memory 600") == 0);
    assert(resolve_host_percent(
        "100%host", out,
        sizeof(out), 3000ULL));
    assert(strcmp(out, "3000") == 0);
    assert(resolve_host_percent(
        "0%host", out, sizeof(out),
        500ULL));
    assert(strcmp(out, "0") == 0);
    assert(resolve_host_percent(
        "plain 42", out,
        sizeof(out), 100ULL));
    assert(strcmp(out, "plain 42") ==
           0);
    assert(resolve_host_percent(
        "3%host,4", out,
        sizeof(out), 100ULL));
    assert(strcmp(out, "3,4") == 0);
    assert(!resolve_host_percent(
        "%", out, sizeof(out),
        100ULL));
    assert(!resolve_host_percent(
        "5%x", out, sizeof(out),
        100ULL));
    assert(!resolve_host_percent(
        "10%hostx", out,
        sizeof(out), 100ULL));
    assert(!resolve_host_percent(
        "10001%host", out,
        sizeof(out), 100ULL));
    assert(!resolve_host_percent(
        nullptr, out, sizeof(out),
        100ULL));
    assert(!resolve_host_percent(
        "1%host", nullptr,
        sizeof(out), 100ULL));
    assert(!resolve_host_percent(
        "1%host", out, 0, 100ULL));
    assert(!resolve_host_percent(
        "60%host", out, 2, 100ULL));
    assert(!resolve_host_percent(
        "1%host", out, sizeof(out),
        1844674407370956ULL));
}

// Exact-fit out still succeeds.
static void dc_resolve_edge(void) {
    char out[4];

    assert(resolve_host_percent(
        "60%host", out, sizeof(out),
        100ULL));
    assert(strcmp(out, "60") == 0);
}

// Submit rejects before any shell:
// bad gates, bare percent, or a
// statedir that cannot fit CMD.
static void dc_submit_reject(void) {
    char out[64];
    char sd[700];
    size_t k;

    assert(!concord_submit(
        nullptr, 0, "probe",
        nullptr, out, sizeof(out)));
    assert(!concord_submit(
        "bad;dir", 0, "probe",
        nullptr, out, sizeof(out)));
    assert(!concord_submit(
        "/var/tmp/resonance", 0,
        "bad!!", nullptr, out,
        sizeof(out)));
    assert(!concord_submit(
        "/var/tmp/resonance", 0,
        nullptr, nullptr, out,
        sizeof(out)));
    assert(!concord_submit(
        "/var/tmp/resonance", 0,
        "probe", "x;y", out,
        sizeof(out)));
    assert(!concord_submit(
        "/var/tmp/resonance", 0,
        "probe", "60%", out,
        sizeof(out)));
    assert(!concord_submit(
        "/var/tmp/resonance", 0,
        "probe", nullptr, nullptr,
        64));
    memset(sd, 'a', sizeof(sd));
    sd[sizeof(sd) - 1] = '\0';
    for (k = 0; k < 600; k++) {
        sd[k] = 'a';
    }
    sd[600] = '\0';
    assert(!concord_submit(
        sd, 0, "probe", nullptr,
        out, sizeof(out)));
}

// Stop, update, list, assigned,
// mesh rejects never reach shells.
static void dc_cli_reject(void) {
    assert(!concord_stop("bad;dir",
                        0, "abc123"));
    assert(!concord_stop(
        "/var/tmp/resonance", -1,
        "abc123"));
    assert(!concord_stop(
        "/var/tmp/resonance", 0,
        "zz!"));
    assert(!concord_stop(
        nullptr, 0, "abc123"));
    assert(!concord_stop(
        "/var/tmp/resonance", 0,
        nullptr));
    assert(!concord_update(
        "bad;dir", 0, "abc123",
        "probe"));
    assert(!concord_update(
        "/var/tmp/resonance", 0,
        "zz!", "probe"));
    assert(!concord_update(
        "/var/tmp/resonance", 0,
        "abc123", "bad!!"));
    assert(!concord_update(
        "/var/tmp/resonance", -1,
        "abc123", "probe"));
    assert(!concord_list_has(
        nullptr, 0, "abc"));
    assert(!concord_list_has(
        "bad;dir", 0, "abc"));
    assert(!concord_list_has(
        "/var/tmp/resonance", 0,
        nullptr));
    assert(!concord_assigned_anywhere(
        nullptr, "abc123"));
    assert(!concord_assigned_anywhere(
        "bad;dir", "abc123"));
    assert(!concord_assigned_anywhere(
        "/var/tmp/resonance",
        nullptr));
    assert(!concord_assigned_anywhere(
        "/var/tmp/resonance",
        "zz!"));
    assert(concord_mesh_count(
               nullptr, 0) == -1);
    assert(concord_mesh_count(
               "bad;dir", 0) == -1);
}

// Long but charset-clean statedir
// fails the CMD snprintf, pre-shell.
static void dc_mesh_trunc(void) {
    char sd[700];

    memset(sd, 'a', sizeof(sd));
    sd[600] = '\0';
    assert(concord_mesh_count(
               sd, 0) == -1);
}

// Makes an empty run with a valid
// net. Pids, logs, fds at -1.
static void dc_mk_run(Run *r,
                      int n) {
    int i;

    memset(r, 0, sizeof(*r));
    r->net = sim_default_net();
    r->n = n;
    r->dead = -1;
    for (i = 0; i < TUN_MAP_MAX;
         i++) {
        r->pids[i] = -1;
        r->logfds[i] = -1;
        r->fds[i] = -1;
    }
}

// Spawn rejects before any fork:
// null run, bad net, bad variant,
// null skew arg, bad skew node,
// or zero nodes.
static void dc_spawn_reject(void) {
    Run r;

    assert(!concord_spawn(
        nullptr,
        CONCORD_SPAWN_PLAIN, 0,
        nullptr));
    memset(&r, 0, sizeof(r));
    r.n = 2;
    assert(!concord_spawn(
        &r, CONCORD_SPAWN_PLAIN, 0,
        nullptr));
    dc_mk_run(&r, 2);
    assert(!concord_spawn(&r, 99, 0,
                         nullptr));
    assert(!concord_spawn(
        &r, CONCORD_SPAWN_SKEW, 0,
        nullptr));
    assert(!concord_spawn(
        &r, CONCORD_SPAWN_SKEW, 7,
        "300"));
    dc_mk_run(&r, 0);
    assert(!concord_spawn(
        &r, CONCORD_SPAWN_PLAIN, 0,
        nullptr));
}

// Restart rejects: null, bad slot,
// bad net, unknown durability.
static void dc_restart_reject(void) {
    Run r;

    assert(!concord_restart(
        nullptr, 0, CONCORD_FRESH));
    dc_mk_run(&r, 2);
    assert(!concord_restart(
        &r, 5, CONCORD_FRESH));
    memset(&r, 0, sizeof(r));
    r.n = 2;
    assert(!concord_restart(
        &r, 0, CONCORD_FRESH));
    dc_mk_run(&r, 2);
    assert(!concord_restart(&r, 0,
                            99));
}

// Kill on a dead slot is idempotent
// and marks stay-dead, no signals.
static void dc_kill_dead(void) {
    Run r;

    dc_mk_run(&r, 2);
    assert(concord_kill(&r, 0,
                        SIGTERM));
    assert(r.dead == 0);
    assert(concord_kill(&r, 0,
                        SIGTERM));
    assert(concord_kill(&r, 1,
                        SIGTERM));
    assert(r.dead == 0);
}

// Binary probe is repeatable and
// matches access on the default.
static void dc_have_binary(void) {
    bool a;
    bool b;

    a = concord_have_binary();
    b = concord_have_binary();
    assert(a == b);
    assert(a == (access(
                    "./bin/concord",
                    X_OK) == 0));
}

// Journal corrupt writes exact bytes
// CORRUPT plus newline: 8 total.
static void dc_journal(void) {
    static const char dir[] =
        "/tmp/opencode/dc-jr";
    static const char sub[] =
        "/tmp/opencode/dc-jr/node0";
    static const char con[] =
        "/tmp/opencode/dc-jr/node0"
        "/concord";
    static const char path[] =
        "/tmp/opencode/dc-jr/node0"
        "/concord/journal.jsonl";
    Run r;
    FILE *fp;
    char buf[16];
    size_t n;

    dc_mk_run(&r, 2);
    assert(!concord_journal_corrupt(
        nullptr, 0));
    assert(!concord_journal_corrupt(
        &r, 5));
    memset(&r, 0, sizeof(r));
    r.n = 2;
    assert(!concord_journal_corrupt(
        &r, 0));
    dc_mk_run(&r, 2);
    (void)snprintf(
        r.net.statedir,
        sizeof(r.net.statedir),
        "%s", dir);
    assert(sim_net_valid(&r.net));
    assert(!concord_journal_corrupt(
        &r, 0));
    (void)mkdir(dir, 0700);
    (void)mkdir(sub, 0700);
    (void)mkdir(con, 0700);
    assert(concord_journal_corrupt(
        &r, 0));
    fp = fopen(path, "rb");
    assert(fp != nullptr);
    n = fread(buf, 1, sizeof(buf),
              fp);
    assert(fclose(fp) == 0);
    assert(n == 8);
    assert(memcmp(buf, "CORRUPT\n",
                 8) == 0);
    assert(remove(path) == 0);
    assert(rmdir(con) == 0);
    assert(rmdir(sub) == 0);
    assert(rmdir(dir) == 0);
}

// Disk fill: arg gates, one real MB,
// then zero removes the filler.
static void dc_fill(void) {
    static const char dir[] =
        "/tmp/opencode/dc-fill";
    static const char path[] =
        "/tmp/opencode/dc-fill"
        "/node0/filler.bin";
    static const char sub[] =
        "/tmp/opencode/dc-fill"
        "/node0";
    Run r;
    struct stat st;

    dc_mk_run(&r, 2);
    assert(!concord_disk_fill(
        nullptr, 0, 1));
    assert(!concord_disk_fill(&r,
                              5, 1));
    memset(&r, 0, sizeof(r));
    r.n = 2;
    assert(!concord_disk_fill(&r, 0,
                              1));
    dc_mk_run(&r, 2);
    assert(!concord_disk_fill(
        &r, 0, 8796093022208ULL));
    (void)snprintf(
        r.net.statedir,
        sizeof(r.net.statedir),
        "%s", dir);
    assert(sim_net_valid(&r.net));
    (void)mkdir(dir, 0700);
    assert(concord_disk_fill(&r, 0,
                             1));
    assert(stat(path, &st) == 0);
    assert(st.st_size ==
           1024 * 1024);
    assert(concord_disk_fill(&r, 0,
                             0));
    assert(access(path, F_OK) != 0);
    assert(rmdir(sub) == 0);
    assert(rmdir(dir) == 0);
}

// Readonly flips on owned dirs and
// back, via bounded chmod shells.
static void dc_readonly(void) {
    static const char dir[] =
        "/tmp/opencode/dc-ro";
    static const char sub[] =
        "/tmp/opencode/dc-ro/node0";
    Run r;

    dc_mk_run(&r, 2);
    assert(!concord_disk_readonly(
        nullptr, 0, true));
    assert(!concord_disk_readonly(
        &r, 5, true));
    memset(&r, 0, sizeof(r));
    r.n = 2;
    assert(!concord_disk_readonly(
        &r, 0, true));
    dc_mk_run(&r, 2);
    (void)snprintf(
        r.net.statedir,
        sizeof(r.net.statedir),
        "%s", dir);
    assert(sim_net_valid(&r.net));
    (void)mkdir(dir, 0700);
    (void)mkdir(sub, 0700);
    assert(concord_disk_readonly(
        &r, 0, true));
    assert(concord_disk_readonly(
        &r, 0, false));
    assert(rmdir(sub) == 0);
    assert(rmdir(dir) == 0);
}

// Runs all checks. Returns 0.
int main(void) {
    dc_image();
    dc_flags();
    dc_statedir();
    dc_wid();
    dc_bin();
    dc_pct_delim();
    dc_resolve();
    dc_resolve_edge();
    dc_submit_reject();
    dc_cli_reject();
    dc_mesh_trunc();
    dc_spawn_reject();
    dc_restart_reject();
    dc_kill_dead();
    dc_have_binary();
    dc_journal();
    dc_fill();
    dc_readonly();
    printf("driver_concord_test: ok\n");
    return (0);
}
