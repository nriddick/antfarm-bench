#ifndef AFCORE_H
#define AFCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AF_ABI_VERSION 1u
enum { AF_OK = 0, AF_UNSUPPORTED = 1, AF_ERROR = 2 };
enum { AF_WAIT_SPIN = 0, AF_WAIT_PARK = 1, AF_WAIT_FOREVER = 2, AF_WAIT_NATIVE = 3 };
#define AF_WORKER_AUTO 0xFFFFFFFFu

typedef struct AfRunCfg {
    uint32_t workers;
    const int* worker_cpus;
    int control_cpu;
    uint32_t wait_mode;
    const char* knobs_json;
} AfRunCfg;

typedef struct AfAudit {
    uint64_t cell_callbacks, aux_callbacks, foreign_callbacks;
    uint64_t nested, duplicate_or_order, missing, value_mismatch;
} AfAudit;

/* All callbacks receive self as their first argument and return an AF_* status.
 * Canonical grids are row-major uint8_t[h][w] arrays containing only 0/1.
 */
typedef struct AfGolImpl {
    uint32_t abi_version;
    void* self;
    /* Untimed. Return only after af_workers_registered() == cfg->workers (W).
     * af_gol_main re-checks this condition and fails the process otherwise.
     */
    int (*start)(void*, const AfRunCfg*);
    /* Untimed. Load canonical bytes; reset state and generation counter to 0. */
    int (*load)(void*, const uint8_t*, uint32_t, uint32_t);
    /* Timed and resumable: advance generations from the current state.
     * Return only once all requested generations are complete and visible
     * to store(). Apply the cell rule defined in SPEC.md (synchronous update,
     * toroidal grid).
     */
    int (*run)(void*, uint32_t generations);
    /* Untimed. Write the current state as canonical 0/1 bytes. */
    int (*store)(void*, uint8_t* out);
    int (*telemetry)(void*, char* json, size_t cap); /* Optional. */
    int (*stop)(void*);
} AfGolImpl;

int af_gol_main(int argc, char** argv, const AfGolImpl*);
int af_pin_self(int cpu);

/* Registration helpers (af_worker_*) and the rendezvous must be called only
 * on worker threads, before start returns. The control thread may query
 * af_workers_registered().
 */
/* Pin to worker_cpus[index] and register TID + pthread CPU clock; idempotent
 * per TID. AF_WORKER_AUTO assigns the lowest free slot, keyed by TID.
 */
int af_worker_enter(uint32_t index);
/* Register at the caller's current singleton CPU, which must be a worker CPU;
 * capture TID + pthread CPU clock without re-pinning.
 */
int af_worker_adopt(void);
/* Bounded wait until W workers are registered, using a shared absolute
 * deadline in nanoseconds. Return AF_ERROR on timeout.
 */
int af_workers_rendezvous(uint64_t deadline_ns);
uint32_t af_workers_registered(void);
/* Open-hosting runs only (otherwise AF_ERROR). Register the calling thread
 * without changing its affinity, which must be nonempty and contained in the
 * allocation: the set of distinct worker_cpus values. worker_cpus is then a
 * suggested slot map, and af_worker_enter/af_worker_adopt remain available.
 * The control (calling) thread may register: it is then one of the W workers,
 * never an extra one. Registration closes when start returns.
 */
int af_worker_register(uint32_t index);

/* Audit hooks are no-ops in libafcore and live in libafcore-audit.
 * Report each (gen, cell) exactly once on the registered worker that computed
 * it, supplying its actual next 0/1 state for comparison with the reference.
 * gen is the 0-based absolute generation index since the last load, computing
 * gen -> gen+1; cell is the canonical row-major index. A body may report many
 * cells. Do not call the combined hook inside an open legacy pair.
 * A legacy begin/end pair is one report; pairs must not overlap.
 * aux counts explicitly reported auxiliary events.
 */
void af_audit_cell(uint32_t gen, uint32_t cell, uint8_t next_state);
void af_audit_cell_begin(uint32_t gen, uint32_t cell);
void af_audit_cell_end(uint8_t next_state);
void af_audit_aux(void);

/* SplitMix64, one draw per cell in row-major order; alive iff
 * ((draw >> 32) % 100) < density_pct.
 */
void af_gol_init(uint64_t seed, uint32_t density_pct, uint32_t w, uint32_t h, uint8_t* out);
/* Write the state after generations to out; leave in untouched. */
void af_gol_ref_run(const uint8_t* in, uint8_t* out, uint32_t w, uint32_t h, uint32_t generations);
/* FNV-1a 64 over n canonical bytes (offset 0xcbf29ce484222325,
 * prime 0x100000001b3).
 */
uint64_t af_gol_hash(const uint8_t* grid, size_t n);

#ifdef __cplusplus
} /* extern "C" */
#define AF_ABI_ASSERT(condition, message) static_assert(condition, message)
#else
#define AF_ABI_ASSERT(condition, message) _Static_assert(condition, message)
#endif

/* ABI v1 freezes this 64-bit layout for all bindings. Any change requires an
 * AF_ABI_VERSION bump and a matching update to abi-layout.json and bindings.
 * Incompatible target layouts fail at compile time.
 */
#define AF_ABI_FIELD(type, field, offset, size) \
    AF_ABI_ASSERT(offsetof(type, field) == (offset), #type "." #field " offset"); \
    AF_ABI_ASSERT(sizeof(((type*)0)->field) == (size), #type "." #field " size")

AF_ABI_ASSERT(sizeof(AfRunCfg) == 32, "AfRunCfg size");
AF_ABI_FIELD(AfRunCfg, workers, 0, 4);
AF_ABI_FIELD(AfRunCfg, worker_cpus, 8, 8);
AF_ABI_FIELD(AfRunCfg, control_cpu, 16, 4);
AF_ABI_FIELD(AfRunCfg, wait_mode, 20, 4);
AF_ABI_FIELD(AfRunCfg, knobs_json, 24, 8);

AF_ABI_ASSERT(sizeof(AfAudit) == 56, "AfAudit size");
AF_ABI_FIELD(AfAudit, cell_callbacks, 0, 8);
AF_ABI_FIELD(AfAudit, aux_callbacks, 8, 8);
AF_ABI_FIELD(AfAudit, foreign_callbacks, 16, 8);
AF_ABI_FIELD(AfAudit, nested, 24, 8);
AF_ABI_FIELD(AfAudit, duplicate_or_order, 32, 8);
AF_ABI_FIELD(AfAudit, missing, 40, 8);
AF_ABI_FIELD(AfAudit, value_mismatch, 48, 8);

AF_ABI_ASSERT(sizeof(AfGolImpl) == 64, "AfGolImpl size");
AF_ABI_FIELD(AfGolImpl, abi_version, 0, 4);
AF_ABI_FIELD(AfGolImpl, self, 8, 8);
AF_ABI_FIELD(AfGolImpl, start, 16, 8);
AF_ABI_FIELD(AfGolImpl, load, 24, 8);
AF_ABI_FIELD(AfGolImpl, run, 32, 8);
AF_ABI_FIELD(AfGolImpl, store, 40, 8);
AF_ABI_FIELD(AfGolImpl, telemetry, 48, 8);
AF_ABI_FIELD(AfGolImpl, stop, 56, 8);

#undef AF_ABI_FIELD
#undef AF_ABI_ASSERT

#endif /* AFCORE_H */
