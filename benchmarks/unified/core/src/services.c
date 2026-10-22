/* af_gol_main: CLI, the check | verify | audit | cold | trial passes, warmup
 * convergence, timing around run() only, rusage / worker CPU / VmHWM, pin
 * verification and the afbench/1 JSONL record (the fields this process owns
 * plus the opaque --meta context).
 *
 * argv follows the C convention: argv[0] is the program name and is skipped.
 * Exit status: 0 ok or nonconverged, 1 failed or unsupported, 2 usage or
 * internal error (including a failed record write).
 */
#define _GNU_SOURCE
#include "afcore_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define EXIT_USAGE 2
#define TELEMETRY_CAP 65536
#define MAX_CELLS ((uint64_t)1 << 30)
#define DEFAULT_SEED UINT64_C(0x5EED)
#define DEFAULT_DENSITY 35u
#define DEFAULT_WARMUP_CAP_MS 30000.0
#define WARMUP_TOLERANCE 0.02

enum { PASS_CHECK, PASS_VERIFY, PASS_AUDIT, PASS_COLD, PASS_TRIAL, PASS_COUNT };
static const char* const pass_names[PASS_COUNT] = {"check", "verify", "audit", "cold", "trial"};
static const char* const wait_names[] = {"spin", "park", "forever", "native"};

/* Record keys written by this layer; --meta may not override them. */
static const char* const owned_keys[] = {
    "schema", "tier", "workload", "pass", "status", "error", "params", "seed", "workers",
    "worker_cpus", "control_cpu", "wait_mode", "policy", "block", "order_index", "rep", "cold",
    "warmup", "timing", "cost", "pin_ok", "checksum", "checksum_expected", "checksum_ok", "gol",
    "audit", "telemetry", "knobs", "placement", "participation",
};

typedef struct OptInt {
    int set;
    long long v;
} OptInt;

typedef struct Options {
    const char *workload, *cpus, *policy, *params, *knobs, *out, *meta, *targets, *ref_cache;
    int pass, wait_mode, quiet_timing, audit_required, cpu_slots, open_hosting;
    uint32_t workers;
    int control_cpu, have_workers, have_control;
    uint64_t seed;
    OptInt block, order_index, rep;
    double min_ms, min_warmup_ms, warmup_cap_ms;
} Options;

typedef struct Ctx {
    const AfGolImpl* impl;
    Options o;
    int cpus[AFC_MAX_WORKERS];
    uint32_t workers;
    AfRunCfg cfg;
    char* knobs_text;
    AfcJson *knobs, *meta, *telemetry;
    AfcTargets targets;
    const AfcTarget* target;
    const char* ref_cache;
    const char* size_label;
    uint32_t w, h, generations, density;
    uint64_t seed;

    const char* status;
    char error[1024];
    int pin_ok; /* -1 until pin verification first runs */
    int have_checksum, checksum_ok;
    uint64_t checksum, checksum_expected, live_count;

    int timed;
    uint64_t wall_ns;
    double cpu_user_s, cpu_sys_s, cpu_worker_s;
    int have_worker_cpu;
    long long peak_rss_kib;
    char rss_error[256];

    int warmup;
    double* windows_ms;
    size_t nwindows;
    double warmup_ms_total;
    int converged;

    int audited;
    AfAudit audit;
    uint64_t slot_cells[AFC_MAX_WORKERS]; /* audited reports per registered slot */
    char* placement; /* registry description captured before stop, or NULL */
} Ctx;

static void fail(Ctx* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(Ctx* c, const char* fmt, ...)
{
    if (strcmp(c->status, "failed") && strcmp(c->status, "unsupported")) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->error, sizeof c->error, fmt, ap);
        va_end(ap);
        c->status = "failed";
    }
}

static int ok(const Ctx* c) { return !strcmp(c->status, "ok"); }

static void say(const Ctx* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(const Ctx* c, const char* fmt, ...)
{
    (void)c;
    va_list ap;
    va_start(ap, fmt);
    fputs("afcore: ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    fflush(stdout);
}

/* ---- option parsing ---- */

static void usage(void)
{
    fputs("usage: <impl> --pass check|verify|audit|cold|trial --workers W --cpus LIST --control-cpu C\n"
          "  [--workload gol] [--params JSON] [--seed S] [--wait spin|park|forever|native]\n"
          "  [--policy NAME] [--block N] [--order-index N] [--rep N] [--min-ms MS]\n"
          "  [--min-warmup-ms MS] [--warmup-cap-ms MS] [--knobs PATH] [--out JSONL] [--meta PATH]\n"
          "  [--gol-targets PATH] [--ref-cache DIR] [--quiet-timing] [--audit-required] [--cpu-slots]\n"
          "  [--open-hosting]\n"
          "  | --emit-targets PATH\n",
          stderr);
}

static int parse_u64(const char* s, uint64_t* out)
{
    int hex = s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    if (!*s || *s == '-' || *s == '+')
        return 0;
    char* end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, hex ? 16 : 10);
    if (errno || *end || (hex && !s[2]))
        return 0;
    *out = v;
    return 1;
}

static int parse_ll(const char* s, long long* out)
{
    char* end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno || !*s || *end)
        return 0;
    *out = v;
    return 1;
}

static int parse_ms(const char* s, double* out)
{
    char* end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || !*s || *end || !(v >= 0))
        return 0;
    *out = v;
    return 1;
}

static int lookup(const char* s, const char* const* names, int n)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(s, names[i]))
            return i;
    return -1;
}

static int parse_options(int argc, char** argv, Options* o, const char** emit)
{
    *o = (Options){.pass = -1, .wait_mode = AF_WAIT_NATIVE, .seed = DEFAULT_SEED,
                   .warmup_cap_ms = DEFAULT_WARMUP_CAP_MS, .workload = "gol"};
    *emit = NULL;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--cpu-slots")) {
            o->cpu_slots = 1;
            continue;
        }
        if (!strcmp(a, "--open-hosting")) {
            o->open_hosting = 1;
            continue;
        }
        if (!strcmp(a, "--quiet-timing")) {
            o->quiet_timing = 1;
            continue;
        }
        if (!strcmp(a, "--audit-required")) {
            o->audit_required = 1;
            continue;
        }
        if (!strcmp(a, "--help") || !strcmp(a, "-h"))
            return 0;
        char name[64];
        const char* v;
        const char* eq = strchr(a, '=');
        if (strncmp(a, "--", 2)) {
            fprintf(stderr, "afcore: unexpected argument '%s'\n", a);
            return 0;
        }
        if (eq) {
            snprintf(name, sizeof name, "%.*s", (int)(eq - a), a);
            v = eq + 1;
        } else {
            snprintf(name, sizeof name, "%s", a);
            if (i + 1 >= argc) {
                fprintf(stderr, "afcore: %s needs a value\n", a);
                return 0;
            }
            v = argv[++i];
        }
        uint64_t u = 0;
        int good = 1;
        if (!strcmp(name, "--workload"))
            o->workload = v;
        else if (!strcmp(name, "--pass"))
            good = (o->pass = lookup(v, pass_names, PASS_COUNT)) >= 0;
        else if (!strcmp(name, "--workers")) {
            good = parse_u64(v, &u) && u >= 1 && u <= AFC_MAX_WORKERS;
            o->workers = (uint32_t)u;
            o->have_workers = 1;
        } else if (!strcmp(name, "--cpus"))
            o->cpus = v;
        else if (!strcmp(name, "--control-cpu")) {
            good = parse_u64(v, &u) && u < 1u << 20;
            o->control_cpu = (int)u;
            o->have_control = 1;
        } else if (!strcmp(name, "--policy"))
            o->policy = v;
        else if (!strcmp(name, "--wait"))
            good = (o->wait_mode = lookup(v, wait_names, 4)) >= 0;
        else if (!strcmp(name, "--params"))
            o->params = v;
        else if (!strcmp(name, "--seed"))
            good = parse_u64(v, &o->seed);
        else if (!strcmp(name, "--block"))
            good = o->block.set = parse_ll(v, &o->block.v);
        else if (!strcmp(name, "--order-index"))
            good = o->order_index.set = parse_ll(v, &o->order_index.v);
        else if (!strcmp(name, "--rep"))
            good = o->rep.set = parse_ll(v, &o->rep.v);
        else if (!strcmp(name, "--min-ms"))
            /* Reserved for Tier K batch repetition; accepted but unused in Phase 1. */
            good = parse_ms(v, &o->min_ms);
        else if (!strcmp(name, "--min-warmup-ms"))
            good = parse_ms(v, &o->min_warmup_ms);
        else if (!strcmp(name, "--warmup-cap-ms"))
            good = parse_ms(v, &o->warmup_cap_ms);
        else if (!strcmp(name, "--knobs"))
            o->knobs = v;
        else if (!strcmp(name, "--out"))
            o->out = v;
        else if (!strcmp(name, "--meta"))
            o->meta = v;
        else if (!strcmp(name, "--gol-targets"))
            o->targets = v;
        else if (!strcmp(name, "--ref-cache"))
            o->ref_cache = v;
        else if (!strcmp(name, "--emit-targets"))
            *emit = v;
        else {
            fprintf(stderr, "afcore: unknown option %s\n", name);
            return 0;
        }
        if (!good) {
            fprintf(stderr, "afcore: bad value for %s: '%s'\n", name, v);
            return 0;
        }
    }
    if (*emit)
        return 1;
    if (strcmp(o->workload, "gol")) {
        fprintf(stderr, "afcore: unsupported workload '%s'\n", o->workload);
        return 0;
    }
    if (o->pass < 0 || !o->have_workers || !o->cpus || !o->have_control) {
        fputs("afcore: --pass, --workers, --cpus and --control-cpu are required\n", stderr);
        return 0;
    }
    if (o->pass != PASS_CHECK && !o->params) {
        fputs("afcore: --params is required for this pass\n", stderr);
        return 0;
    }
    return 1;
}

static int load_json_file(const char* path, AfcJson** out, int need_object)
{
    char err[512];
    size_t n;
    char* text = afc_read_file(path, &n, err, sizeof err);
    if (text) {
        *out = afc_json_parse(text, n, err, sizeof err);
        free(text);
    }
    if (!text || !*out) {
        fprintf(stderr, "afcore: %s: %s\n", path, err);
        return 0;
    }
    if (need_object && (*out)->type != AFC_JOBJ) {
        fprintf(stderr, "afcore: %s: expected a JSON object\n", path);
        return 0;
    }
    return 1;
}

static int parse_params(Ctx* c)
{
    char err[256];
    AfcJson* p = afc_json_parse(c->o.params, strlen(c->o.params), err, sizeof err);
    int good = p && p->type == AFC_JOBJ;
    uint64_t w = 0, h = 0, g = 0, d = DEFAULT_DENSITY;
    for (size_t i = 0; good && i < p->len; i++) {
        const char* k = p->keys[i];
        const AfcJson* v = &p->items[i];
        if (!strcmp(k, "w"))
            good = afc_json_u64(v, &w);
        else if (!strcmp(k, "h"))
            good = afc_json_u64(v, &h);
        else if (!strcmp(k, "generations"))
            good = afc_json_u64(v, &g);
        else if (!strcmp(k, "density_pct"))
            good = afc_json_u64(v, &d) && d <= 100;
        else if (!strcmp(k, "size") && v->type == AFC_JSTR) {
            free((void*)c->size_label);
            good = (c->size_label = strdup(v->str)) != NULL;
        }
        else
            good = 0;
    }
    good = good && w >= 1 && h >= 1 && g >= 1 && w <= UINT32_MAX && h <= UINT32_MAX &&
           g <= INT32_MAX && w * h <= MAX_CELLS;
    afc_json_free(p);
    if (!good) {
        fprintf(stderr, "afcore: --params must be {\"w\",\"h\",\"generations\"[,\"density_pct\",\"size\"]} "
                        "with w*h <= 2^30: '%s'\n", c->o.params);
        return 0;
    }
    c->w = (uint32_t)w;
    c->h = (uint32_t)h;
    c->generations = (uint32_t)g;
    c->density = (uint32_t)d;
    return 1;
}

static int setup(Ctx* c)
{
    char err[512];
    int all[CPU_SETSIZE];
    uint32_t n;
    /* Explicit worker-slot mode preserves order/repeated singleton assignments.
     * Ordinary --cpus retains the historical set parser and behavior. */
    if (c->o.cpu_slots) {
        const char *p=c->o.cpus;n=0;
        while (*p) {
            if (*p<'0'||*p>'9'||n>=CPU_SETSIZE) return 0;
            char *end;errno=0;long cpu=strtol(p,&end,10);
            if(errno||cpu<0||cpu>=CPU_SETSIZE||(*end&&*end!=',')) return 0;
            all[n++]=(int)cpu;
            if(*end==','&&!end[1]) return 0;
            p=end+(*end==',');
        }
        if(n!=c->o.workers) {fprintf(stderr,"afcore: --cpu-slots needs exactly W entries\n");return 0;}
    } else if (afc_parse_cpu_list(c->o.cpus, all, CPU_SETSIZE, &n, err, sizeof err) != AF_OK) {
        fprintf(stderr, "afcore: --cpus: %s\n", err);
        return 0;
    }
    if (n < c->o.workers) {
        fprintf(stderr, "afcore: --cpus lists %u CPUs, fewer than --workers %u\n", n, c->o.workers);
        return 0;
    }
    for (uint32_t i = 0; i < n; i++)
        if (all[i] == c->o.control_cpu) {
            fprintf(stderr, "afcore: control CPU %d is also listed in --cpus\n", c->o.control_cpu);
            return 0;
        }
    c->workers = c->o.workers;
    memcpy(c->cpus, all, c->workers * sizeof *all);
    if (c->o.pass == PASS_CHECK) {
        c->w = c->h = 256;
        c->generations = 200;
        c->density = DEFAULT_DENSITY;
        c->seed = DEFAULT_SEED;
    } else {
        if (!parse_params(c))
            return 0;
        c->seed = c->o.seed;
    }
    if (c->o.knobs) {
        size_t len;
        if (!(c->knobs_text = afc_read_file(c->o.knobs, &len, err, sizeof err)) ||
            !(c->knobs = afc_json_parse(c->knobs_text, len, err, sizeof err))) {
            fprintf(stderr, "afcore: --knobs: %s\n", err);
            return 0;
        }
    }
    if (c->o.meta) {
        if (!load_json_file(c->o.meta, &c->meta, 1))
            return 0;
        for (size_t i = 0; i < c->meta->len; i++)
            for (size_t k = 0; k < sizeof owned_keys / sizeof owned_keys[0]; k++)
                if (!strcmp(c->meta->keys[i], owned_keys[k])) {
                    fprintf(stderr, "afcore: --meta may not set the owned record key '%s'\n", owned_keys[k]);
                    return 0;
                }
    }
    if (c->o.targets) {
        if (afc_targets_load(c->o.targets, &c->targets, err, sizeof err) != AF_OK) {
            fprintf(stderr, "afcore: --gol-targets: %s\n", err);
            return 0;
        }
        c->ref_cache = c->o.ref_cache ? c->o.ref_cache : ".build/unified/gol-ref";
        c->target = afc_targets_find(&c->targets, c->w, c->h, c->generations, c->seed, c->density);
    }
    c->cfg = (AfRunCfg){.workers = c->workers, .worker_cpus = c->cpus, .control_cpu = c->o.control_cpu,
                        .wait_mode = (uint32_t)c->o.wait_mode, .knobs_json = c->knobs_text ? c->knobs_text : "{}"};
    return 1;
}

/* ---- impl calls and verification ---- */

static int call_load(Ctx* c, const uint8_t* grid, uint32_t w, uint32_t h)
{
    int rc = c->impl->load(c->impl->self, grid, w, h);
    if (rc != AF_OK)
        fail(c, "load(%ux%u) returned %d", w, h, rc);
    return rc == AF_OK;
}

static int check_pins(Ctx* c)
{
    char why[256];
    if (afc_registry_verify_pins(why, sizeof why)) {
        if (c->pin_ok < 0)
            c->pin_ok = 1;
        return 1;
    }
    c->pin_ok = 0;
    fail(c, "pin verification failed: %s", why);
    return 0;
}

static int call_run(Ctx* c, uint32_t g, uint64_t* ns)
{
    uint64_t t0 = afc_now_ns();
    int rc = c->impl->run(c->impl->self, g);
    uint64_t t1 = afc_now_ns();
    if (ns)
        *ns = t1 - t0;
    if (rc != AF_OK) {
        fail(c, "run(%u) returned %d", g, rc);
        return 0;
    }
    return check_pins(c);
}

/* store(), then canonical-byte check, exact compare and hash compare. */
static int verify_store(Ctx* c, const uint8_t* expect, uint64_t expect_hash, uint32_t w, uint32_t h,
                        const char* what)
{
    size_t n = (size_t)w * h;
    uint8_t* got = malloc(n);
    if (!got) {
        fail(c, "out of memory for store buffer");
        return 0;
    }
    memset(got, 0xFF, n);
    int rc = c->impl->store(c->impl->self, got);
    int good = rc == AF_OK;
    if (!good)
        fail(c, "store returned %d (%s)", rc, what);
    size_t diff = 0, first = n, bad = n;
    for (size_t i = 0; good && i < n; i++) {
        if (got[i] > 1 && bad == n)
            bad = i;
        if (got[i] != expect[i] && diff++ == 0)
            first = i;
    }
    if (good && bad < n) {
        fail(c, "verification failed (%s): store wrote non-canonical byte %u at cell %zu", what, got[bad], bad);
        good = 0;
    }
    c->have_checksum = good || rc == AF_OK;
    c->checksum = af_gol_hash(got, n);
    c->checksum_expected = expect_hash;
    c->live_count = afc_live_count(got, n);
    c->checksum_ok = good && diff == 0 && c->checksum == expect_hash;
    if (good && diff) {
        fail(c, "verification failed (%s): %zu cells differ, first at cell %zu (got %u, want %u)", what, diff,
             first, got[first], expect[first]);
        good = 0;
    } else if (good && c->checksum != expect_hash) {
        fail(c, "verification failed (%s): hash %016llx != expected %016llx", what,
             (unsigned long long)c->checksum, (unsigned long long)expect_hash);
        good = 0;
    }
    free(got);
    return good;
}

static int reference(Ctx* c, uint8_t* out)
{
    char err[256];
    if (afc_reference(c->target, c->ref_cache, c->w, c->h, c->generations, c->seed, c->density, out, err,
                      sizeof err) == AF_OK)
        return 1;
    fail(c, "reference: %s", err);
    return 0;
}

static uint8_t* initial_grid(Ctx* c)
{
    uint8_t* g = malloc((size_t)c->w * c->h);
    if (g)
        af_gol_init(c->seed, c->density, c->w, c->h, g);
    else
        fail(c, "out of memory for initial grid");
    return g;
}

/* ---- check pass ---- */

typedef struct Pattern {
    const char* name;
    uint32_t w, h;
    const char* cells;   /* rows of '#'/'.', row-major */
    uint32_t steps[2];   /* run() segments; 0 ends */
    const char* expect[2];
} Pattern;

static const Pattern patterns[] = {
    {"glider-8x8-returns-after-32", 8, 8,
     ".#......" "..#....." "###....." "........" "........" "........" "........" "........",
     {32, 0},
     {".#......" "..#....." "###....." "........" "........" "........" "........" "........", NULL}},
    {"blinker-period-2", 5, 5, "....." "....." ".###." "....." ".....", {1, 1},
     {"....." "..#.." "..#.." "..#.." ".....", "....." "....." ".###." "....." "....."}},
    {"block-still", 4, 4, "...." ".##." ".##." "....", {1, 0}, {"...." ".##." ".##." "....", NULL}},
    {"all-alive-3x3-dies", 3, 3, "#########", {1, 0}, {".........", NULL}},
};

static void decode(const char* cells, uint8_t* out, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out[i] = cells[i] == '#';
}

static void check_pattern(Ctx* c, const Pattern* p)
{
    size_t n = (size_t)p->w * p->h;
    uint8_t grid[64], want[64];
    char what[96];
    decode(p->cells, grid, n);
    int good = call_load(c, grid, p->w, p->h);
    for (int s = 0; good && s < 2 && p->steps[s]; s++) {
        decode(p->expect[s], want, n);
        snprintf(what, sizeof what, "check %s segment %d", p->name, s + 1);
        good = call_run(c, p->steps[s], NULL) && verify_store(c, want, af_gol_hash(want, n), p->w, p->h, what);
    }
    say(c, "check %s %s", p->name, good ? "ok" : "FAILED");
}

static void check_anchor(Ctx* c, uint32_t w, uint32_t h, uint32_t g, uint64_t anchor)
{
    size_t n = (size_t)w * h;
    uint8_t *init = malloc(n), *ref = malloc(n);
    char what[96];
    snprintf(what, sizeof what, "check anchor %ux%u x %u", w, h, g);
    int good = init && ref;
    if (!good)
        fail(c, "out of memory for %s", what);
    if (good) {
        af_gol_init(DEFAULT_SEED, DEFAULT_DENSITY, w, h, init);
        af_gol_ref_run(init, ref, w, h, g);
        if (af_gol_hash(ref, n) != anchor) {
            fail(c, "reference self-check failed (%s): %016llx != %016llx", what,
                 (unsigned long long)af_gol_hash(ref, n), (unsigned long long)anchor);
            good = 0;
        }
    }
    good = good && call_load(c, init, w, h) && call_run(c, g, NULL) && verify_store(c, ref, anchor, w, h, what);
    say(c, "%s %s", what, good ? "ok" : "FAILED");
    free(init);
    free(ref);
}

static void pass_check(Ctx* c)
{
    for (size_t i = 0; i < sizeof patterns / sizeof patterns[0]; i++)
        check_pattern(c, &patterns[i]);
    check_anchor(c, 8, 8, 3, UINT64_C(0x7ecfd9117854dcaa));
    check_anchor(c, 256, 256, 200, UINT64_C(0x8da52f791a8397d4));
}

/* ---- verify pass: run(G/8) segments with a compare after each ---- */

static void pass_verify(Ctx* c)
{
    size_t n = (size_t)c->w * c->h;
    uint8_t* init = initial_grid(c);
    uint8_t* ref = malloc(n);
    if (!init || !ref) {
        fail(c, "reference buffer allocation failed");
        goto done;
    }
    memcpy(ref, init, n);
    if (!call_load(c, init, c->w, c->h))
        goto done;
    uint32_t prev = 0;
    for (uint32_t k = 1; k <= AFC_CHECKPOINTS && ok(c); k++) {
        uint32_t gk = afc_checkpoint_gen(c->generations, k);
        if (gk == prev)
            continue;
        af_gol_ref_run(ref, ref, c->w, c->h, gk - prev);
        uint64_t hash = af_gol_hash(ref, n);
        if (c->target && hash != c->target->checkpoint_hashes[k - 1]) {
            fail(c, "reference checkpoint %u (gen %u) hash %016llx does not match target %016llx", k, gk,
                 (unsigned long long)hash, (unsigned long long)c->target->checkpoint_hashes[k - 1]);
            break;
        }
        char what[64];
        snprintf(what, sizeof what, "checkpoint %u, generation %u", k, gk);
        int good = call_run(c, gk - prev, NULL) && verify_store(c, ref, hash, c->w, c->h, what);
        say(c, "verify %s %s", what, good ? "ok" : "FAILED");
        prev = gk;
    }
done:
    free(init);
    free(ref);
}

/* ---- audit pass ---- */

static void pass_audit(Ctx* c)
{
    char err[256];
    size_t n = (size_t)c->w * c->h;
    uint8_t* init = initial_grid(c);
    uint8_t* ref = malloc(n);
    if (!afc_audit_available()) {
        fail(c, "the audit pass requires libafcore-audit");
        goto done;
    }
    if (!ref)
        fail(c, "reference buffer allocation failed");
    if (!init || !ref || !reference(c, ref))
        goto done;
    if (afc_audit_prepare(init, c->w, c->h, c->generations, err, sizeof err) != AF_OK) {
        fail(c, "audit: %s", err);
        goto done;
    }
    if (call_load(c, init, c->w, c->h)) {
        afc_audit_arm();
        int ran = call_run(c, c->generations, NULL);
        afc_audit_disarm();
        afc_audit_collect(&c->audit);
        for (uint32_t i = 0; i < c->workers; i++)
            c->slot_cells[i] = afc_audit_slot_cells(i);
        c->audited = 1;
        if (ran)
            verify_store(c, ref, af_gol_hash(ref, n), c->w, c->h, "audit final state");
    }
    afc_audit_release();
    if (!c->audited)
        goto done;
    uint64_t expected;
    const AfAudit* a = &c->audit;
    AfcBuf msg = {0};
    /* The verdict comes from the counters; msg is diagnostics only. */
    int overflow = __builtin_mul_overflow((uint64_t)n, (uint64_t)c->generations, &expected);
    int audit_bad = overflow || a->cell_callbacks != expected || a->foreign_callbacks || a->nested ||
                    a->duplicate_or_order || a->missing || a->value_mismatch;
    if (overflow)
        afc_buf_puts(&msg, "; W*H*G overflows 64 bits");
    else if (a->cell_callbacks != expected)
        afc_buf_printf(&msg, "; cell_callbacks %llu != W*H*G %llu", (unsigned long long)a->cell_callbacks,
                       (unsigned long long)expected);
    const struct {
        const char* name;
        uint64_t value;
    } counters[] = {{"foreign_callbacks", a->foreign_callbacks}, {"nested", a->nested},
                    {"duplicate_or_order", a->duplicate_or_order}, {"missing", a->missing},
                    {"value_mismatch", a->value_mismatch}};
    for (size_t i = 0; i < sizeof counters / sizeof counters[0]; i++)
        if (counters[i].value)
            afc_buf_printf(&msg, "; %s=%llu", counters[i].name, (unsigned long long)counters[i].value);
    say(c, "audit cell_callbacks=%llu aux_callbacks=%llu foreign=%llu nested=%llu duplicate_or_order=%llu "
           "missing=%llu value_mismatch=%llu",
        (unsigned long long)a->cell_callbacks, (unsigned long long)a->aux_callbacks,
        (unsigned long long)a->foreign_callbacks, (unsigned long long)a->nested,
        (unsigned long long)a->duplicate_or_order, (unsigned long long)a->missing,
        (unsigned long long)a->value_mismatch);
    if (audit_bad)
        fail(c, "audit failed%s%s", msg.oom || !msg.len ? " (diagnostic allocation failed)" : ": ",
             msg.oom || !msg.len ? "" : msg.p + 2);
    afc_buf_free(&msg);
done:
    free(init);
    free(ref);
}

/* ---- timed passes ---- */

static long long read_vmhwm(void)
{
    FILE* f = fopen("/proc/self/status", "r");
    char line[256];
    long long kib = -1;
    while (f && fgets(line, sizeof line, f))
        if (sscanf(line, "VmHWM: %lld kB", &kib) == 1)
            break;
    if (f)
        fclose(f);
    return kib;
}

static void reset_hwm(Ctx* c)
{
    int fd = open("/proc/self/clear_refs", O_WRONLY);
    c->rss_error[0] = 0;
    if (fd < 0 || write(fd, "5", 1) != 1)
        snprintf(c->rss_error, sizeof c->rss_error, "clear_refs: %s", strerror(errno));
    if (fd >= 0)
        close(fd);
}

static double tv_s(struct timeval t) { return (double)t.tv_sec + (double)t.tv_usec / 1e6; }

/* load (untimed), then run(G) inside the timing window with cost capture. */
static int measured_run(Ctx* c, const uint8_t* init)
{
    struct rusage r0, r1;
    uint64_t cpu0 = 0, cpu1 = 0, ns = 0;
    if (!call_load(c, init, c->w, c->h))
        return 0;
    reset_hwm(c);
    int have0 = afc_registry_worker_cpu_ns(&cpu0) == AF_OK;
    getrusage(RUSAGE_SELF, &r0);
    uint64_t t0 = afc_now_ns();
    int rc = c->impl->run(c->impl->self, c->generations);
    ns = afc_now_ns() - t0;
    getrusage(RUSAGE_SELF, &r1);
    int have1 = afc_registry_worker_cpu_ns(&cpu1) == AF_OK;
    long long hwm = read_vmhwm();
    c->timed = 1;
    c->wall_ns = ns;
    c->cpu_user_s = tv_s(r1.ru_utime) - tv_s(r0.ru_utime);
    c->cpu_sys_s = tv_s(r1.ru_stime) - tv_s(r0.ru_stime);
    c->have_worker_cpu = have0 && have1;
    c->cpu_worker_s = (double)(cpu1 - cpu0) / 1e9;
    c->peak_rss_kib = c->rss_error[0] ? -1 : hwm;
    if (!c->rss_error[0] && hwm < 0)
        snprintf(c->rss_error, sizeof c->rss_error, "VmHWM not found in /proc/self/status");
    if (rc != AF_OK) {
        fail(c, "run(%u) returned %d", c->generations, rc);
        return 0;
    }
    return check_pins(c);
}

static int push_window(Ctx* c, double ms)
{
    double* w = realloc(c->windows_ms, (c->nwindows + 1) * sizeof *w);
    if (!w) {
        fail(c, "out of memory for warmup windows");
        return 0;
    }
    c->windows_ms = w;
    w[c->nwindows++] = ms;
    c->warmup_ms_total += ms;
    return 1;
}

static void pass_timed(Ctx* c, int warmup)
{
    size_t n = (size_t)c->w * c->h;
    uint8_t* init = initial_grid(c);
    uint8_t* ref = malloc(n);
    if (!ref)
        fail(c, "reference buffer allocation failed");
    if (!init || !ref || !reference(c, ref))
        goto done;
    uint64_t ref_hash = c->target ? c->target->hash : af_gol_hash(ref, n);
    c->warmup = warmup;
    /* Q13: converged once one adjacent pair agrees within 2 % and the
     * cumulative warmup time reaches --min-warmup-ms; stop at the cap.
     * Cumulative warmup time and the cap count only the summed run()
     * windows, not load/store/verify.
     */
    while (warmup && ok(c) && c->warmup_ms_total < c->o.warmup_cap_ms) {
        uint64_t ns;
        if (!call_load(c, init, c->w, c->h) || !call_run(c, c->generations, &ns) ||
            !verify_store(c, ref, ref_hash, c->w, c->h, "warmup") || !push_window(c, (double)ns / 1e6))
            break;
        if (c->nwindows >= 2) {
            double r = c->windows_ms[c->nwindows - 1] / c->windows_ms[c->nwindows - 2] - 1;
            if ((r < 0 ? -r : r) <= WARMUP_TOLERANCE && c->warmup_ms_total >= c->o.min_warmup_ms) {
                c->converged = 1;
                break;
            }
        }
    }
    if (!ok(c))
        goto done;
    if (measured_run(c, init))
        verify_store(c, ref, ref_hash, c->w, c->h, warmup ? "trial" : "cold");
    if (warmup && ok(c) && !c->converged)
        c->status = "nonconverged";
    if (c->timed && !c->o.quiet_timing)
        say(c, "%s wall_ms=%.3f ns_per_cell=%.3f warmup_windows=%zu converged=%s", pass_names[c->o.pass],
            (double)c->wall_ns / 1e6, (double)c->wall_ns / ((double)n * c->generations), c->nwindows,
            warmup ? (c->converged ? "true" : "false") : "n/a");
done:
    free(init);
    free(ref);
}

/* ---- record ---- */

static void put_hex(AfcBuf* b, uint64_t v) { afc_buf_printf(b, "\"%016llx\"", (unsigned long long)v); }

static void put_double(AfcBuf* b, double v)
{
    if (v != v || v > 1e300 || v < -1e300)
        afc_buf_puts(b, "null");
    else
        afc_buf_printf(b, "%.9g", v);
}

static void put_optint(AfcBuf* b, OptInt v)
{
    if (v.set)
        afc_buf_printf(b, "%lld", v.v);
    else
        afc_buf_puts(b, "null");
}

static void put_string_or_null(AfcBuf* b, const char* s)
{
    if (s)
        afc_buf_json_string(b, s, strlen(s));
    else
        afc_buf_puts(b, "null");
}

static void put_json_or_null(AfcBuf* b, const AfcJson* v)
{
    if (v)
        afc_json_write(b, v);
    else
        afc_buf_puts(b, "null");
}

static void build_record(const Ctx* c, AfcBuf* b)
{
    uint64_t items = (uint64_t)c->w * c->h * c->generations;
    afc_buf_printf(b, "{\"schema\":\"afbench/1\",\"tier\":\"A\",\"workload\":\"gol\",\"pass\":\"%s\",\"status\":\"%s\",",
                   pass_names[c->o.pass], c->status);
    afc_buf_puts(b, "\"error\":");
    put_string_or_null(b, c->error[0] ? c->error : NULL);
    afc_buf_printf(b, ",\"params\":{\"w\":%u,\"h\":%u,\"generations\":%u,\"density_pct\":%u", c->w, c->h,
                   c->generations, c->density);
    if (c->size_label) {
        afc_buf_puts(b, ",\"size\":");
        put_string_or_null(b, c->size_label);
    }
    afc_buf_printf(b, "},\"seed\":%llu,\"workers\":%u,\"worker_cpus\":[", (unsigned long long)c->seed, c->workers);
    for (uint32_t i = 0; i < c->workers; i++)
        afc_buf_printf(b, "%s%d", i ? "," : "", c->cpus[i]);
    afc_buf_printf(b, "],\"control_cpu\":%d,\"wait_mode\":\"%s\",\"policy\":", c->o.control_cpu,
                   wait_names[c->o.wait_mode]);
    put_string_or_null(b, c->o.policy);
    afc_buf_puts(b, ",\"block\":");
    put_optint(b, c->o.block);
    afc_buf_puts(b, ",\"order_index\":");
    put_optint(b, c->o.order_index);
    afc_buf_puts(b, ",\"rep\":");
    put_optint(b, c->o.rep);
    afc_buf_printf(b, ",\"cold\":%s,\"warmup\":", c->o.pass == PASS_COLD ? "true" : "false");
    if (c->warmup) {
        afc_buf_puts(b, "{\"windows_ms\":[");
        for (size_t i = 0; i < c->nwindows; i++) {
            if (i)
                afc_buf_puts(b, ",");
            put_double(b, c->windows_ms[i]);
        }
        afc_buf_printf(b, "],\"converged\":%s,\"ms_total\":", c->converged ? "true" : "false");
        put_double(b, c->warmup_ms_total);
        afc_buf_puts(b, ",\"min_warmup_ms\":");
        put_double(b, c->o.min_warmup_ms);
        afc_buf_puts(b, ",\"cap_ms\":");
        put_double(b, c->o.warmup_cap_ms);
        afc_buf_puts(b, "}");
    } else {
        afc_buf_puts(b, "null");
    }
    afc_buf_puts(b, ",\"timing\":");
    if (c->timed) {
        afc_buf_printf(b, "{\"wall_ns\":%llu,\"items\":%llu,\"ns_per_item\":", (unsigned long long)c->wall_ns,
                       (unsigned long long)items);
        put_double(b, (double)c->wall_ns / (double)items);
        afc_buf_puts(b, ",\"items_per_s\":");
        if (c->wall_ns)
            put_double(b, (double)items * 1e9 / (double)c->wall_ns);
        else
            afc_buf_puts(b, "null");
        afc_buf_puts(b, "},\"cost\":{\"cpu_user_s\":");
        put_double(b, c->cpu_user_s);
        afc_buf_puts(b, ",\"cpu_sys_s\":");
        put_double(b, c->cpu_sys_s);
        afc_buf_puts(b, ",\"cpu_worker_s\":");
        if (c->have_worker_cpu)
            put_double(b, c->cpu_worker_s);
        else
            afc_buf_puts(b, "null");
        afc_buf_puts(b, ",\"peak_rss_kib\":");
        if (c->peak_rss_kib >= 0)
            afc_buf_printf(b, "%lld", c->peak_rss_kib);
        else
            afc_buf_puts(b, "null");
        afc_buf_puts(b, ",\"peak_rss_error\":");
        put_string_or_null(b, c->rss_error[0] ? c->rss_error : NULL);
        afc_buf_puts(b, "}");
    } else {
        afc_buf_puts(b, "null,\"cost\":null");
    }
    afc_buf_printf(b, ",\"pin_ok\":%s,\"checksum\":",
                   c->pin_ok < 0 ? "null" : c->pin_ok ? "true" : "false");
    if (c->have_checksum) {
        put_hex(b, c->checksum);
        afc_buf_puts(b, ",\"checksum_expected\":");
        put_hex(b, c->checksum_expected);
        afc_buf_printf(b, ",\"checksum_ok\":%s", c->checksum_ok ? "true" : "false");
    } else {
        afc_buf_puts(b, "null,\"checksum_expected\":null,\"checksum_ok\":false");
    }
    afc_buf_printf(b, ",\"gol\":{\"w\":%u,\"h\":%u,\"generations\":%u,\"seed\":%llu,\"density_pct\":%u,\"hash\":",
                   c->w, c->h, c->generations, (unsigned long long)c->seed, c->density);
    if (c->have_checksum) {
        put_hex(b, c->checksum);
        afc_buf_puts(b, ",\"hash_expected\":");
        put_hex(b, c->checksum_expected);
        afc_buf_printf(b, ",\"live_count\":%llu}", (unsigned long long)c->live_count);
    } else {
        afc_buf_puts(b, "null,\"hash_expected\":null,\"live_count\":null}");
    }
    afc_buf_puts(b, ",\"audit\":");
    if (c->audited) {
        const AfAudit* a = &c->audit;
        afc_buf_printf(b,
                       "{\"cell_callbacks\":%llu,\"aux_callbacks\":%llu,\"foreign_callbacks\":%llu,"
                       "\"nested\":%llu,\"duplicate_or_order\":%llu,\"missing\":%llu,\"value_mismatch\":%llu}",
                       (unsigned long long)a->cell_callbacks, (unsigned long long)a->aux_callbacks,
                       (unsigned long long)a->foreign_callbacks, (unsigned long long)a->nested,
                       (unsigned long long)a->duplicate_or_order, (unsigned long long)a->missing,
                       (unsigned long long)a->value_mismatch);
    } else {
        afc_buf_puts(b, "null");
    }
    afc_buf_puts(b, ",\"participation\":");
    if (c->audited) {
        afc_buf_puts(b, "{\"slot_cells\":[");
        for (uint32_t i = 0; i < c->workers; i++)
            afc_buf_printf(b, i ? ",%llu" : "%llu", (unsigned long long)c->slot_cells[i]);
        afc_buf_puts(b, "]}");
    } else {
        afc_buf_puts(b, "null");
    }
    afc_buf_puts(b, ",\"placement\":");
    afc_buf_puts(b, c->placement ? c->placement : "null");
    afc_buf_puts(b, ",\"telemetry\":");
    put_json_or_null(b, c->telemetry);
    afc_buf_puts(b, ",\"knobs\":");
    put_json_or_null(b, c->knobs);
    for (size_t i = 0; c->meta && i < c->meta->len; i++) {
        afc_buf_puts(b, ",");
        afc_buf_json_string(b, c->meta->keys[i], c->meta->key_lens[i]);
        afc_buf_puts(b, ":");
        afc_json_write(b, &c->meta->items[i]);
    }
    afc_buf_puts(b, "}\n");
}

static int write_record(const Ctx* c)
{
    AfcBuf b = {0};
    build_record(c, &b);
    int good = !b.oom;
    if (good && c->o.out) {
        int fd = open(c->o.out, O_WRONLY | O_CREAT | O_APPEND, 0644);
        good = fd >= 0 && write(fd, b.p, b.len) == (ssize_t)b.len;
        if (fd >= 0 && close(fd))
            good = 0;
        if (!good)
            fprintf(stderr, "afcore: writing record to %s failed: %s\n", c->o.out, strerror(errno));
    }
    afc_buf_free(&b);
    return good;
}

static void collect_telemetry(Ctx* c)
{
    if (!c->impl->telemetry)
        return;
    char* buf = calloc(1, TELEMETRY_CAP);
    char err[256];
    if (!buf) {
        fail(c, "out of memory for telemetry");
        return;
    }
    int rc = c->impl->telemetry(c->impl->self, buf, TELEMETRY_CAP);
    buf[TELEMETRY_CAP - 1] = 0;
    if (rc == AF_OK && !(c->telemetry = afc_json_parse(buf, strlen(buf), err, sizeof err)))
        fail(c, "telemetry is not valid JSON: %s", err);
    else if (rc != AF_OK && rc != AF_UNSUPPORTED)
        fail(c, "telemetry returned %d", rc);
    free(buf);
}

AFC_API int af_gol_main(int argc, char** argv, const AfGolImpl* impl)
{
    Ctx ctx = {.impl = impl, .status = "ok", .pin_ok = -1, .peak_rss_kib = -1};
    Ctx* c = &ctx;
    const char* emit;
    if (!parse_options(argc, argv, &c->o, &emit)) {
        usage();
        return EXIT_USAGE;
    }
    if (emit)
        return afc_emit_targets(emit) == AF_OK ? 0 : EXIT_USAGE;
    if (!impl || impl->abi_version != AF_ABI_VERSION || !impl->start || !impl->load || !impl->run ||
        !impl->store || !impl->stop) {
        fprintf(stderr, "afcore: implementation vtable is incomplete or not ABI version %u\n", AF_ABI_VERSION);
        return EXIT_USAGE;
    }
    if (!setup(c))
        return EXIT_USAGE;

    int started = 0;
    if (c->o.audit_required && !afc_audit_available())
        fail(c, "--audit-required but this binary is linked with libafcore, not libafcore-audit");
    else if (af_pin_self(c->o.control_cpu) != AF_OK)
        fail(c, "cannot pin the control thread to CPU %d", c->o.control_cpu);
    else if (afc_registry_configure(c->workers, c->cpus, c->o.control_cpu, c->o.open_hosting) != AF_OK)
        fail(c, "registry configuration failed");
    else {
        started = 1;
        int rc = impl->start(impl->self, &c->cfg);
        if (rc == AF_UNSUPPORTED) {
            c->status = "unsupported";
            snprintf(c->error, sizeof c->error, "start returned AF_UNSUPPORTED");
        } else if (rc != AF_OK) {
            fail(c, "start returned %d", rc);
        } else if (af_workers_registered() != c->workers) {
            fail(c, "start returned with %u of %u workers registered", af_workers_registered(), c->workers);
        }
        afc_registry_seal();
    }
    if (ok(c) && !check_pins(c))
        say(c, "workers not pinned after start");
    if (ok(c)) {
        switch (c->o.pass) {
        case PASS_CHECK: pass_check(c); break;
        case PASS_VERIFY: pass_verify(c); break;
        case PASS_AUDIT: pass_audit(c); break;
        case PASS_COLD: pass_timed(c, 0); break;
        case PASS_TRIAL: pass_timed(c, 1); break;
        }
    }
    if (started) {
        AfcBuf placement = {0};
        afc_registry_describe(&placement);
        if (placement.oom)
            afc_buf_free(&placement);
        else
            c->placement = placement.p;
        if (strcmp(c->status, "unsupported"))
            collect_telemetry(c);
        int rc = impl->stop(impl->self);
        if (rc != AF_OK)
            fail(c, "stop returned %d", rc);
    }
    say(c, "pass=%s workers=%u status=%s%s%s", pass_names[c->o.pass], c->workers, c->status,
        c->error[0] ? " error=" : "", c->error);
    int written = write_record(c);
    afc_json_free(c->knobs);
    afc_json_free(c->meta);
    afc_json_free(c->telemetry);
    free(c->placement);
    afc_targets_free(&c->targets);
    free(c->knobs_text);
    free(c->windows_ms);
    free((void*)c->size_label);
    if (!written)
        return EXIT_USAGE;
    return ok(c) || !strcmp(c->status, "nonconverged") ? 0 : 1;
}

#ifndef AF_AUDIT
/* libafcore: audit hooks are no-ops and the audit pass is unavailable. */
AFC_API void af_audit_cell(uint32_t gen, uint32_t cell, uint8_t next_state)
{
    (void)gen;
    (void)cell;
    (void)next_state;
}
AFC_API void af_audit_cell_begin(uint32_t gen, uint32_t cell)
{
    (void)gen;
    (void)cell;
}
AFC_API void af_audit_cell_end(uint8_t next_state) { (void)next_state; }
AFC_API void af_audit_aux(void) {}
int afc_audit_available(void) { return 0; }
int afc_audit_prepare(const uint8_t* initial, uint32_t w, uint32_t h, uint32_t generations, char* err, size_t cap)
{
    (void)initial;
    (void)w;
    (void)h;
    (void)generations;
    snprintf(err, cap, "audit hooks are not compiled into libafcore");
    return AF_ERROR;
}
void afc_audit_arm(void) {}
void afc_audit_disarm(void) {}
void afc_audit_collect(AfAudit* out) { *out = (AfAudit){0}; }
uint64_t afc_audit_slot_cells(uint32_t slot)
{
    (void)slot;
    return 0;
}
void afc_audit_release(void) {}
#endif
