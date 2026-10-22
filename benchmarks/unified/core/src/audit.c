/* Live audit hooks (libafcore-audit only; DESIGN §4.7 item 1).
 *
 * Coverage: last_gen[cell] must CAS from gen-1 to gen (initially -1).
 * Protocol: a thread-local open-pair flag. Value: `next` is compared with the
 * precomputed reference state of the cell at gen+1. Identity: reports on
 * unregistered threads or the control thread are foreign; they still take
 * part in coverage so a stray report is not reported as missing.
 * Registered workers own one padded counter block each (single writer);
 * foreign threads share one block updated with atomic adds. `open` mirrors
 * the thread-local inside-cell state so that begins still unmatched when
 * run() returns are counted as nested at collect.
 */
#ifndef AF_AUDIT
#error "audit.c is compiled only into libafcore-audit (-DAF_AUDIT)"
#endif

#define _GNU_SOURCE
#include "afcore_internal.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bound on the in-memory reference sequence (cells x generations bytes). */
#define AUDIT_SEQUENCE_CAP ((size_t)256 << 20)

typedef struct Counters {
    _Atomic uint64_t cells, aux, nested, order, value, open;
} __attribute__((aligned(64))) Counters;

static Counters worker_counters[AFC_MAX_WORKERS];
static Counters foreign_counters;
static _Atomic int32_t* last_gen;
static uint8_t* sequence; /* sequence[g * n + i]: state of cell i at generation g + 1 */
static uint32_t cells_n, generations_n;
static _Atomic int armed;
static _Atomic uint32_t epoch;

static _Thread_local struct {
    uint32_t epoch;
    int inside, checkable;
    uint32_t gen, cell;
} cur;

static inline void bump(_Atomic uint64_t* c, int shared)
{
    if (shared)
        atomic_fetch_add_explicit(c, 1, memory_order_relaxed);
    else
        atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) + 1, memory_order_relaxed);
}

static inline void unbump(_Atomic uint64_t* c, int shared)
{
    if (shared)
        atomic_fetch_sub_explicit(c, 1, memory_order_relaxed);
    else
        atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) - 1, memory_order_relaxed);
}

static inline Counters* counters(int* shared)
{
    int slot = afc_registry_current_slot();
    *shared = slot < 0;
    return slot < 0 ? &foreign_counters : &worker_counters[slot];
}

static inline void sync_epoch(void)
{
    uint32_t e = atomic_load_explicit(&epoch, memory_order_relaxed);
    if (cur.epoch != e) {
        cur.epoch = e;
        cur.inside = 0;
    }
}

AFC_API void af_audit_cell_begin(uint32_t gen, uint32_t cell)
{
    if (!atomic_load_explicit(&armed, memory_order_relaxed))
        return;
    int shared;
    Counters* c = counters(&shared);
    sync_epoch();
    bump(&c->cells, shared);
    if (cur.inside)
        bump(&c->nested, shared);
    else
        bump(&c->open, shared);
    cur.inside = 1;
    cur.gen = gen;
    cur.cell = cell;
    cur.checkable = 0;
    if (cell >= cells_n || gen >= generations_n) {
        bump(&c->order, shared);
        return;
    }
    int32_t expected = (int32_t)gen - 1;
    if (!atomic_compare_exchange_strong(&last_gen[cell], &expected, (int32_t)gen))
        bump(&c->order, shared);
    cur.checkable = 1;
}

AFC_API void af_audit_cell_end(uint8_t next_state)
{
    if (!atomic_load_explicit(&armed, memory_order_relaxed))
        return;
    int shared;
    Counters* c = counters(&shared);
    sync_epoch();
    if (!cur.inside) {
        bump(&c->nested, shared);
        return;
    }
    cur.inside = 0;
    unbump(&c->open, shared);
    if (cur.checkable && next_state != sequence[(size_t)cur.gen * cells_n + cur.cell])
        bump(&c->value, shared);
}

AFC_API void af_audit_cell(uint32_t gen, uint32_t cell, uint8_t next_state)
{
    if (!atomic_load_explicit(&armed, memory_order_relaxed))
        return;
    sync_epoch();
    if (cur.inside) {
        int shared;
        Counters* c = counters(&shared);
        bump(&c->nested, shared);
        return;
    }
    af_audit_cell_begin(gen, cell);
    af_audit_cell_end(next_state);
}

AFC_API void af_audit_aux(void)
{
    if (!atomic_load_explicit(&armed, memory_order_relaxed))
        return;
    int shared;
    Counters* c = counters(&shared);
    bump(&c->aux, shared);
}

int afc_audit_available(void) { return 1; }

int afc_audit_prepare(const uint8_t* initial, uint32_t w, uint32_t h, uint32_t generations, char* err,
                      size_t cap)
{
    afc_audit_release();
    size_t n = (size_t)w * h;
    if (n > INT32_MAX || generations > INT32_MAX || (generations && n > AUDIT_SEQUENCE_CAP / generations)) {
        snprintf(err, cap, "audit size %ux%u x %u exceeds the %zu MiB reference-sequence cap", w, h,
                 generations, AUDIT_SEQUENCE_CAP >> 20);
        return AF_ERROR;
    }
    last_gen = malloc(n * sizeof *last_gen);
    sequence = malloc(n * (generations ? generations : 1));
    if (!last_gen || !sequence) {
        afc_audit_release();
        snprintf(err, cap, "out of memory for audit state");
        return AF_ERROR;
    }
    const uint8_t* prev = initial;
    for (uint32_t g = 0; g < generations; g++) {
        afc_gol_step(prev, sequence + (size_t)g * n, w, h);
        prev = sequence + (size_t)g * n;
    }
    cells_n = (uint32_t)n;
    generations_n = generations;
    return AF_OK;
}

void afc_audit_arm(void)
{
    for (uint32_t i = 0; i < cells_n; i++)
        atomic_init(&last_gen[i], -1);
    memset(worker_counters, 0, sizeof worker_counters);
    memset(&foreign_counters, 0, sizeof foreign_counters);
    atomic_fetch_add(&epoch, 1);
    atomic_store(&armed, 1);
}

void afc_audit_disarm(void) { atomic_store(&armed, 0); }

void afc_audit_collect(AfAudit* out)
{
    *out = (AfAudit){0};
    for (uint32_t s = 0; s <= AFC_MAX_WORKERS; s++) {
        Counters* c = s < AFC_MAX_WORKERS ? &worker_counters[s] : &foreign_counters;
        out->cell_callbacks += atomic_load(&c->cells);
        out->aux_callbacks += atomic_load(&c->aux);
        out->nested += atomic_load(&c->nested) + atomic_load(&c->open);
        out->duplicate_or_order += atomic_load(&c->order);
        out->value_mismatch += atomic_load(&c->value);
    }
    out->foreign_callbacks = atomic_load(&foreign_counters.cells) + atomic_load(&foreign_counters.aux);
    for (uint32_t i = 0; i < cells_n; i++)
        if (atomic_load(&last_gen[i]) != (int32_t)generations_n - 1)
            out->missing++;
}

uint64_t afc_audit_slot_cells(uint32_t slot)
{
    return slot < AFC_MAX_WORKERS ? atomic_load(&worker_counters[slot].cells) : 0;
}

void afc_audit_release(void)
{
    free((void*)last_gen);
    free(sequence);
    last_gen = NULL;
    sequence = NULL;
    cells_n = generations_n = 0;
}
