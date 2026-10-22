/* Internal interfaces shared by the libafcore translation units. Not part of
 * the frozen ABI in afcore.h; tools and tests link these through the static
 * archives only.
 */
#ifndef AFCORE_INTERNAL_H
#define AFCORE_INTERNAL_H

#include "afcore.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define AFC_API __attribute__((visibility("default")))
#define AFC_MAX_WORKERS 64u
#define AFC_SLOT_UNKNOWN (-2)
#define AFC_SLOT_FOREIGN (-1)
#define AFC_CHECKPOINTS 8u

/* ---- growable text buffer and JSON (json.c) ---- */
typedef struct AfcBuf {
    char* p;
    size_t len, cap;
    int oom;
} AfcBuf;

void afc_buf_put(AfcBuf* b, const char* s, size_t n);
void afc_buf_puts(AfcBuf* b, const char* s);
void afc_buf_printf(AfcBuf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void afc_buf_json_string(AfcBuf* b, const char* s, size_t n);
void afc_buf_free(AfcBuf* b);

enum { AFC_JNULL, AFC_JBOOL, AFC_JNUM, AFC_JSTR, AFC_JARR, AFC_JOBJ };
typedef struct AfcJson {
    int type;
    int boolean;
    char* str;   /* decoded string, or the number's source text */
    size_t len;  /* string length; element count for arrays and objects */
    struct AfcJson* items;
    char** keys; /* object member names, parallel to items */
    size_t* key_lens;
} AfcJson;

AfcJson* afc_json_parse(const char* text, size_t n, char* err, size_t cap);
void afc_json_free(AfcJson* v);
const AfcJson* afc_json_get(const AfcJson* obj, const char* key);
int afc_json_u64(const AfcJson* v, uint64_t* out);
void afc_json_write(AfcBuf* b, const AfcJson* v);
char* afc_read_file(const char* path, size_t* n, char* err, size_t cap);

/* ---- CPU lists (topology_cpus.c) ---- */
int afc_parse_cpu_list(const char* text, int* out, uint32_t cap, uint32_t* n, char* err, size_t errcap);

/* ---- worker registry (registry.c) ---- */
/* open: open hosting (allocation-contained affinity, caller may register). */
int afc_registry_configure(uint32_t workers, const int* cpus, int control_cpu, int open);
void afc_registry_seal(void);
int afc_registry_current_slot(void);
int afc_registry_verify_pins(char* why, size_t cap);
int afc_registry_worker_cpu_ns(uint64_t* total);
void afc_registry_describe(AfcBuf* b);
uint64_t afc_now_ns(void);

/* ---- reference and targets (gol.c) ---- */
typedef struct AfcTarget {
    uint32_t w, h, generations, density_pct;
    uint64_t seed, hash, live_count;
    uint32_t checkpoint_gens[AFC_CHECKPOINTS];
    uint64_t checkpoint_hashes[AFC_CHECKPOINTS];
} AfcTarget;

typedef struct AfcTargets {
    AfcTarget* items;
    size_t n;
} AfcTargets;

void afc_gol_step(const uint8_t* cur, uint8_t* next, uint32_t w, uint32_t h);
uint64_t afc_live_count(const uint8_t* grid, size_t n);
uint32_t afc_checkpoint_gen(uint32_t generations, uint32_t k);
int afc_targets_load(const char* path, AfcTargets* out, char* err, size_t cap);
void afc_targets_free(AfcTargets* t);
const AfcTarget* afc_targets_find(const AfcTargets* t, uint32_t w, uint32_t h,
                                  uint32_t generations, uint64_t seed, uint32_t density_pct);
int afc_reference(const AfcTarget* target, const char* cache_dir, uint32_t w, uint32_t h,
                  uint32_t generations, uint64_t seed, uint32_t density_pct, uint8_t* out,
                  char* err, size_t cap);
int afc_emit_targets(const char* path);

/* ---- audit (audit.c in libafcore-audit; stubs in services.c otherwise) ---- */
int afc_audit_available(void);
int afc_audit_prepare(const uint8_t* initial, uint32_t w, uint32_t h, uint32_t generations,
                      char* err, size_t cap);
void afc_audit_arm(void);
void afc_audit_disarm(void);
void afc_audit_collect(AfAudit* out);
/* Cell reports made by a registered slot during the last armed run. */
uint64_t afc_audit_slot_cells(uint32_t slot);
void afc_audit_release(void);

#endif /* AFCORE_INTERNAL_H */
