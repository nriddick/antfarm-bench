/* Seeded input, serial toroidal reference stepper, FNV-1a 64, the committed
 * target table (reader and generator) and the on-disk reference cache.
 * Constants follow DESIGN §4.1 (ported from the spike 542 reference).
 */
#define _GNU_SOURCE
#include "afcore_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t splitmix64(uint64_t* state)
{
    uint64_t z = (*state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

AFC_API void af_gol_init(uint64_t seed, uint32_t density_pct, uint32_t w, uint32_t h, uint8_t* out)
{
    uint64_t state = seed;
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++)
        out[i] = (uint8_t)(((splitmix64(&state) >> 32) % 100) < density_pct);
}

AFC_API uint64_t af_gol_hash(const uint8_t* grid, size_t n)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < n; i++) {
        hash ^= grid[i];
        hash *= UINT64_C(0x100000001b3);
    }
    return hash;
}

/* One synchronous generation: next = rule(cur); cur and next must not alias. */
void afc_gol_step(const uint8_t* cur, uint8_t* next, uint32_t w, uint32_t h)
{
    for (size_t y = 0; y < h; y++) {
        size_t ym = y == 0 ? h - 1 : y - 1, yp = y + 1 == h ? 0 : y + 1;
        const uint8_t *up = cur + ym * w, *mid = cur + y * w, *down = cur + yp * w;
        for (size_t x = 0; x < w; x++) {
            size_t xm = x == 0 ? w - 1 : x - 1, xp = x + 1 == w ? 0 : x + 1;
            unsigned s = up[xm] + up[x] + up[xp] + mid[xm] + mid[xp] + down[xm] + down[x] + down[xp];
            next[y * w + x] = (uint8_t)(s == 3 || (s == 2 && mid[x]));
        }
    }
}

/* Unlike the spike 542 stepper (result left in whichever local buffer was
 * last written), the state after `generations` always lands in out and in is
 * never written. in == out is allowed.
 */
AFC_API void af_gol_ref_run(const uint8_t* in, uint8_t* out, uint32_t w, uint32_t h, uint32_t generations)
{
    size_t n = (size_t)w * h;
    if (generations == 0) {
        memmove(out, in, n);
        return;
    }
    uint8_t* scratch = malloc(in == out ? 2 * n : n);
    if (!scratch) {
        fputs("afcore: af_gol_ref_run: out of memory\n", stderr);
        abort();
    }
    const uint8_t* cur = in;
    if (in == out) {
        memcpy(scratch + n, in, n);
        cur = scratch + n;
    }
    for (uint32_t k = 0; k < generations; k++) {
        /* Alternate so that the final generation is written to out. */
        uint8_t* dst = (generations - 1 - k) % 2 == 0 ? out : scratch;
        afc_gol_step(cur, dst, w, h);
        cur = dst;
    }
    free(scratch);
}

uint64_t afc_live_count(const uint8_t* grid, size_t n)
{
    uint64_t live = 0;
    for (size_t i = 0; i < n; i++)
        live += grid[i];
    return live;
}

uint32_t afc_checkpoint_gen(uint32_t generations, uint32_t k)
{
    return (uint32_t)((uint64_t)generations * k / AFC_CHECKPOINTS);
}

/* ---- target table ---- */

static int hex_u64(const AfcJson* v, uint64_t* out)
{
    if (!v || v->type != AFC_JSTR || v->len != 16)
        return 0;
    char* end;
    errno = 0;
    *out = strtoull(v->str, &end, 16);
    return !errno && *end == 0;
}

static int u32_field(const AfcJson* obj, const char* key, uint32_t* out)
{
    uint64_t v;
    if (!afc_json_u64(afc_json_get(obj, key), &v) || v > UINT32_MAX)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

static int parse_target(const AfcJson* e, AfcTarget* t)
{
    const AfcJson* gens = afc_json_get(e, "checkpoint_gens");
    const AfcJson* hashes = afc_json_get(e, "checkpoint_hashes");
    if (!u32_field(e, "w", &t->w) || !u32_field(e, "h", &t->h) ||
        !u32_field(e, "generations", &t->generations) || !u32_field(e, "density_pct", &t->density_pct) ||
        !afc_json_u64(afc_json_get(e, "seed"), &t->seed) || !hex_u64(afc_json_get(e, "hash"), &t->hash) ||
        !afc_json_u64(afc_json_get(e, "live_count"), &t->live_count) || !gens || gens->type != AFC_JARR ||
        gens->len != AFC_CHECKPOINTS || !hashes || hashes->type != AFC_JARR || hashes->len != AFC_CHECKPOINTS)
        return 0;
    for (uint32_t k = 0; k < AFC_CHECKPOINTS; k++) {
        uint64_t g;
        if (!afc_json_u64(&gens->items[k], &g) || g != afc_checkpoint_gen(t->generations, k + 1) ||
            !hex_u64(&hashes->items[k], &t->checkpoint_hashes[k]))
            return 0;
        t->checkpoint_gens[k] = (uint32_t)g;
    }
    return 1;
}

int afc_targets_load(const char* path, AfcTargets* out, char* err, size_t cap)
{
    size_t n;
    char* text = afc_read_file(path, &n, err, cap);
    if (!text)
        return AF_ERROR;
    AfcJson* doc = afc_json_parse(text, n, err, cap);
    free(text);
    if (!doc)
        return AF_ERROR;
    const AfcJson* entries = afc_json_get(doc, "entries");
    const AfcJson* schema = afc_json_get(doc, "schema");
    int rc = AF_ERROR;
    *out = (AfcTargets){0};
    if (!schema || schema->type != AFC_JSTR || strcmp(schema->str, "afbench-gol-targets/1") ||
        !entries || entries->type != AFC_JARR) {
        snprintf(err, cap, "%s: not an afbench-gol-targets/1 document", path);
        goto done;
    }
    out->items = calloc(entries->len ? entries->len : 1, sizeof *out->items);
    if (!out->items) {
        snprintf(err, cap, "%s: out of memory", path);
        goto done;
    }
    for (size_t i = 0; i < entries->len; i++)
        if (!parse_target(&entries->items[i], &out->items[i])) {
            snprintf(err, cap, "%s: malformed entry %zu", path, i);
            afc_targets_free(out);
            goto done;
        }
    out->n = entries->len;
    rc = AF_OK;
done:
    afc_json_free(doc);
    return rc;
}

void afc_targets_free(AfcTargets* t)
{
    free(t->items);
    *t = (AfcTargets){0};
}

const AfcTarget* afc_targets_find(const AfcTargets* t, uint32_t w, uint32_t h, uint32_t generations,
                                  uint64_t seed, uint32_t density_pct)
{
    for (size_t i = 0; t && i < t->n; i++) {
        const AfcTarget* e = &t->items[i];
        if (e->w == w && e->h == h && e->generations == generations && e->seed == seed &&
            e->density_pct == density_pct)
            return e;
    }
    return NULL;
}

/* ---- reference with cache ---- */

static int mkdirs(const char* dir)
{
    char path[4096];
    if (snprintf(path, sizeof path, "%s", dir) >= (int)sizeof path)
        return -1;
    for (char* p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(path, 0755) && errno != EEXIST)
                return -1;
            *p = '/';
        }
    return mkdir(path, 0755) && errno != EEXIST ? -1 : 0;
}

static int read_cache(const char* path, uint8_t* out, size_t n)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t got = fread(out, 1, n, f);
    int extra = fgetc(f) != EOF;
    fclose(f);
    return got == n && !extra;
}

static void write_cache(const char* dir, const char* path, const uint8_t* grid, size_t n)
{
    char tmp[4200];
    if (mkdirs(dir) || snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid()) >= (int)sizeof tmp)
        goto unwritable;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        goto unwritable;
    size_t done = 0;
    while (done < n) {
        ssize_t k = write(fd, grid + done, n - done);
        if (k <= 0)
            break;
        done += (size_t)k;
    }
    if (close(fd) || done != n || rename(tmp, path)) {
        unlink(tmp);
        goto unwritable;
    }
    return;
unwritable:
    fprintf(stderr, "afcore: reference cache %s not written: %s\n", path, strerror(errno));
}

/* Compute (or load from cache) the reference state after `generations`. The
 * cache is used only with a target entry; a cached file is used only when its
 * hash equals the target, and a computed reference that disagrees with the
 * target is an error.
 */
int afc_reference(const AfcTarget* target, const char* cache_dir, uint32_t w, uint32_t h,
                  uint32_t generations, uint64_t seed, uint32_t density_pct, uint8_t* out, char* err,
                  size_t cap)
{
    size_t n = (size_t)w * h;
    char path[4096] = "";
    if (target && cache_dir) {
        snprintf(path, sizeof path, "%s/%ux%u-g%u-s0x%llx-d%u.bin", cache_dir, w, h, generations,
                 (unsigned long long)seed, density_pct);
        if (read_cache(path, out, n) && af_gol_hash(out, n) == target->hash)
            return AF_OK;
    }
    uint8_t* initial = malloc(n);
    if (!initial) {
        snprintf(err, cap, "out of memory for reference");
        return AF_ERROR;
    }
    af_gol_init(seed, density_pct, w, h, initial);
    af_gol_ref_run(initial, out, w, h, generations);
    free(initial);
    if (target && af_gol_hash(out, n) != target->hash) {
        snprintf(err, cap, "reference hash %016llx does not match target %016llx",
                 (unsigned long long)af_gol_hash(out, n), (unsigned long long)target->hash);
        return AF_ERROR;
    }
    if (path[0])
        write_cache(cache_dir, path, out, n);
    return AF_OK;
}

/* ---- target generator ---- */

typedef struct TargetSpec {
    const char* id;
    uint32_t w, h, generations;
} TargetSpec;

/* DESIGN §4.1 anchors, §4.5 sizes and §4.7 audit sizes. */
static const TargetSpec target_specs[] = {
    {"anchor-8x8-g3", 8, 8, 3},
    {"anchor-256x256-g200", 256, 256, 200},
    {"anchor-1024x1024-g20", 1024, 1024, 20},
    {"G-S0", 64, 64, 16384},
    {"G-S1", 256, 256, 4096},
    {"G-S2", 1024, 1024, 256},
    {"G-S3", 4096, 4096, 16},
    {"audit-64x64-g64", 64, 64, 64},
    {"audit-256x256-g16", 256, 256, 16},
};
static const uint64_t target_seeds[] = {0x5EED, 1, 2};
#define TARGET_DENSITY 35u

static int emit_entry(FILE* f, const TargetSpec* spec, uint64_t seed, int last)
{
    size_t n = (size_t)spec->w * spec->h;
    uint8_t *cur = malloc(n), *next = malloc(n);
    if (!cur || !next) {
        free(cur);
        free(next);
        return AF_ERROR;
    }
    af_gol_init(seed, TARGET_DENSITY, spec->w, spec->h, cur);
    uint32_t gens[AFC_CHECKPOINTS];
    uint64_t hashes[AFC_CHECKPOINTS];
    uint32_t done = 0;
    for (uint32_t k = 0; k < AFC_CHECKPOINTS; k++) {
        gens[k] = afc_checkpoint_gen(spec->generations, k + 1);
        for (; done < gens[k]; done++) {
            afc_gol_step(cur, next, spec->w, spec->h);
            uint8_t* t = cur;
            cur = next;
            next = t;
        }
        hashes[k] = af_gol_hash(cur, n);
    }
    fprintf(f,
            "    {\"id\": \"%s\", \"w\": %u, \"h\": %u, \"generations\": %u, \"seed\": %llu, "
            "\"seed_hex\": \"0x%llx\", \"density_pct\": %u, \"hash\": \"%016llx\", \"live_count\": %llu, "
            "\"checkpoint_gens\": [",
            spec->id, spec->w, spec->h, spec->generations, (unsigned long long)seed,
            (unsigned long long)seed, TARGET_DENSITY, (unsigned long long)af_gol_hash(cur, n),
            (unsigned long long)afc_live_count(cur, n));
    for (uint32_t k = 0; k < AFC_CHECKPOINTS; k++)
        fprintf(f, "%s%u", k ? ", " : "", gens[k]);
    fputs("], \"checkpoint_hashes\": [", f);
    for (uint32_t k = 0; k < AFC_CHECKPOINTS; k++)
        fprintf(f, "%s\"%016llx\"", k ? ", " : "", (unsigned long long)hashes[k]);
    fprintf(f, "]}%s\n", last ? "" : ",");
    free(cur);
    free(next);
    return AF_OK;
}

int afc_emit_targets(const char* path)
{
    int to_stdout = !strcmp(path, "-");
    FILE* f = to_stdout ? stdout : fopen(path, "w");
    if (!f) {
        fprintf(stderr, "afcore: %s: %s\n", path, strerror(errno));
        return AF_ERROR;
    }
    fputs("{\n  \"schema\": \"afbench-gol-targets/1\",\n"
          "  \"generator\": \"benchmarks/unified/core/tools/gen_targets.c\",\n"
          "  \"hash\": \"FNV-1a 64 over canonical row-major 0/1 bytes\",\n"
          "  \"checkpoint_rule\": \"checkpoint_gens[k-1] = floor(k * generations / 8), k = 1..8\",\n"
          "  \"entries\": [\n",
          f);
    size_t specs = sizeof target_specs / sizeof target_specs[0];
    size_t seeds = sizeof target_seeds / sizeof target_seeds[0];
    int rc = AF_OK;
    for (size_t i = 0; rc == AF_OK && i < specs; i++)
        for (size_t j = 0; rc == AF_OK && j < seeds; j++)
            rc = emit_entry(f, &target_specs[i], target_seeds[j], i + 1 == specs && j + 1 == seeds);
    fputs("  ]\n}\n", f);
    if ((to_stdout ? fflush(f) : fclose(f)) || rc != AF_OK) {
        fprintf(stderr, "afcore: writing %s failed\n", path);
        return AF_ERROR;
    }
    return AF_OK;
}
