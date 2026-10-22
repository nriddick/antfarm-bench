/* TID-keyed worker registry: registration (explicit slot, auto slot, adopt,
 * open register), the bounded rendezvous, sealing, per-thread CPU clocks and
 * /proc placement checks. Pinned mode requires each worker on its slot CPU and
 * excludes the control thread. Open mode (open hosting) accepts any affinity
 * within the allocation, the distinct worker_cpus values, and lets the control
 * thread register as one of the W workers (caller participation).
 * Registration is serialized by one mutex; after the seal the slot table is
 * read-only, so audit lookups need no locking beyond the first resolve.
 */
#define _GNU_SOURCE
#include "afcore_internal.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct Slot {
    pid_t tid;
    int cpu;
    clockid_t clock;
} Slot;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Slot slots[AFC_MAX_WORKERS];
static uint32_t nworkers;
static int open_mode;
static cpu_set_t allocation;
static int control_cpu = -1;
static pid_t control_tid;
static _Atomic int configured, sealed;
static _Atomic uint32_t registered, late_registrations;
static _Atomic uint64_t shared_deadline;
static _Thread_local int tls_slot = AFC_SLOT_UNKNOWN;

uint64_t afc_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

static int tid_alive(pid_t tid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d", (int)tid);
    return access(path, F_OK) == 0;
}

/* The thread's only allowed CPU, or -1 when its mask is not a singleton. */
static int singleton_cpu(pid_t tid)
{
    cpu_set_t set;
    if (sched_getaffinity(tid, sizeof set, &set) || CPU_COUNT(&set) != 1)
        return -1;
    for (int c = 0; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &set))
            return c;
    return -1;
}

AFC_API int af_pin_self(int cpu)
{
    if (cpu < 0 || cpu >= CPU_SETSIZE)
        return AF_ERROR;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set))
        return AF_ERROR;
    return sched_getcpu() == cpu ? AF_OK : AF_ERROR;
}

int afc_registry_configure(uint32_t workers, const int* cpus, int control, int open)
{
    if (workers == 0 || workers > AFC_MAX_WORKERS)
        return AF_ERROR;
    pthread_mutex_lock(&lock);
    memset(slots, 0, sizeof slots);
    nworkers = workers;
    open_mode = open;
    CPU_ZERO(&allocation);
    for (uint32_t i = 0; i < workers; i++) {
        slots[i].cpu = cpus[i];
        CPU_SET(cpus[i], &allocation);
    }
    control_cpu = control;
    control_tid = gettid();
    atomic_store(&registered, 0);
    atomic_store(&late_registrations, 0);
    atomic_store(&shared_deadline, 0);
    atomic_store(&sealed, 0);
    atomic_store(&configured, 1);
    pthread_mutex_unlock(&lock);
    return AF_OK;
}

void afc_registry_seal(void) { atomic_store(&sealed, 1); }

static int find_tid(pid_t tid)
{
    for (uint32_t i = 0; i < nworkers; i++)
        if (slots[i].tid == tid)
            return (int)i;
    return -1;
}

static int slot_free(uint32_t i) { return !slots[i].tid || !tid_alive(slots[i].tid); }

/* A nonempty affinity mask contained in the allocation. */
static int within_allocation(pid_t tid)
{
    cpu_set_t set, outside;
    if (sched_getaffinity(tid, sizeof set, &set) || !CPU_COUNT(&set))
        return 0;
    CPU_XOR(&outside, &set, &allocation);
    CPU_AND(&outside, &outside, &set);
    return CPU_COUNT(&outside) == 0;
}

enum { ENTER_PIN, ENTER_ADOPT, ENTER_OPEN };

/* ENTER_ADOPT: register at the current singleton CPU without pinning (an open
 * run accepts any allocation-contained mask). ENTER_OPEN: open runs only;
 * register without changing affinity, which must lie within the allocation. */
static int enter(uint32_t index, int how)
{
    if (!atomic_load(&configured))
        return AF_ERROR;
    pid_t tid = gettid();
    int rc = AF_ERROR;
    pthread_mutex_lock(&lock);
    if (tid == control_tid && !open_mode)
        goto out;
    if (how == ENTER_OPEN && !open_mode)
        goto out;
    int adopt = how != ENTER_PIN;
    if (adopt && open_mode) {
        if (!within_allocation(0))
            goto out;
    } else if (adopt) {
        int cpu = singleton_cpu(0);
        index = AF_WORKER_AUTO;
        for (uint32_t i = 0; i < nworkers; i++)
            if (slots[i].cpu == cpu)
                index = i;
        if (cpu < 0 || index == AF_WORKER_AUTO)
            goto out;
    }
    int own = find_tid(tid);
    if (own >= 0) {
        if (index == AF_WORKER_AUTO || index == (uint32_t)own) {
            tls_slot = own;
            rc = AF_OK;
        }
        goto out;
    }
    if (atomic_load(&sealed)) {
        atomic_fetch_add(&late_registrations, 1);
        goto out;
    }
    uint32_t slot = index;
    if (index == AF_WORKER_AUTO) {
        for (slot = 0; slot < nworkers && !slot_free(slot); slot++)
            ;
    } else if (index < nworkers && !slot_free(index)) {
        slot = nworkers;
    }
    if (slot >= nworkers)
        goto out;
    if (!adopt && af_pin_self(slots[slot].cpu) != AF_OK)
        goto out;
    clockid_t clock;
    if (pthread_getcpuclockid(pthread_self(), &clock))
        goto out;
    int fresh = slots[slot].tid == 0;
    slots[slot].tid = tid;
    slots[slot].clock = clock;
    if (fresh)
        atomic_fetch_add(&registered, 1);
    tls_slot = (int)slot;
    rc = AF_OK;
out:
    pthread_mutex_unlock(&lock);
    return rc;
}

AFC_API int af_worker_enter(uint32_t index) { return enter(index, ENTER_PIN); }

AFC_API int af_worker_adopt(void) { return enter(AF_WORKER_AUTO, ENTER_ADOPT); }

AFC_API int af_worker_register(uint32_t index) { return enter(index, ENTER_OPEN); }

AFC_API int af_workers_rendezvous(uint64_t deadline_ns)
{
    if (!atomic_load(&configured))
        return AF_ERROR;
    uint64_t none = 0;
    atomic_compare_exchange_strong(&shared_deadline, &none, deadline_ns);
    uint64_t deadline = atomic_load(&shared_deadline);
    for (unsigned spin = 1;; spin++) {
        if (atomic_load(&registered) >= nworkers)
            return AF_OK;
        if (afc_now_ns() > deadline)
            return AF_ERROR;
        if (spin % 1024 == 0)
            sched_yield();
        else
            cpu_relax();
    }
}

AFC_API uint32_t af_workers_registered(void) { return atomic_load(&registered); }

int afc_registry_current_slot(void)
{
    if (tls_slot != AFC_SLOT_UNKNOWN)
        return tls_slot;
    pid_t tid = gettid();
    pthread_mutex_lock(&lock);
    int slot = tid == control_tid && !open_mode ? -1 : find_tid(tid);
    pthread_mutex_unlock(&lock);
    tls_slot = slot >= 0 ? slot : AFC_SLOT_FOREIGN;
    return tls_slot;
}

/* Field 39 of /proc/<tid>/stat: the CPU the thread last ran on. */
static int last_cpu(pid_t tid)
{
    char path[64], line[1024];
    snprintf(path, sizeof path, "/proc/self/task/%d/stat", (int)tid);
    FILE* f = fopen(path, "r");
    if (!f)
        return -1;
    size_t got = fread(line, 1, sizeof line - 1, f);
    fclose(f);
    line[got] = 0;
    char* s = strrchr(line, ')');
    if (!s)
        return -1;
    int field = 2, cpu = -1;
    char* save;
    for (char* tok = strtok_r(s + 1, " ", &save); tok; tok = strtok_r(NULL, " ", &save))
        if (++field == 39) {
            sscanf(tok, "%d", &cpu);
            break;
        }
    return cpu;
}

/* End-of-window check: W live workers, each with a singleton mask equal to
 * its slot CPU and last run there; no late registrations; control thread
 * pinned to the control CPU. Returns 1 when everything holds.
 */
int afc_registry_verify_pins(char* why, size_t cap)
{
    why[0] = 0;
    pthread_mutex_lock(&lock);
    int ok = 1;
    uint32_t late = atomic_load(&late_registrations);
    if (atomic_load(&registered) != nworkers) {
        snprintf(why, cap, "%u of %u workers registered", atomic_load(&registered), nworkers);
        ok = 0;
    } else if (late) {
        snprintf(why, cap, "%u registration(s) after start returned", late);
        ok = 0;
    } else if (!open_mode && singleton_cpu(control_tid) != control_cpu) {
        snprintf(why, cap, "control thread %d is not pinned to CPU %d", (int)control_tid, control_cpu);
        ok = 0;
    }
    for (uint32_t i = 0; ok && i < nworkers; i++) {
        const Slot* s = &slots[i];
        int mask = singleton_cpu(s->tid), ran = last_cpu(s->tid);
        if (!tid_alive(s->tid)) {
            snprintf(why, cap, "worker %u (tid %d) exited", i, (int)s->tid);
            ok = 0;
        } else if (open_mode) {
            if (!within_allocation(s->tid) || ran < 0 || ran >= CPU_SETSIZE || !CPU_ISSET(ran, &allocation)) {
                snprintf(why, cap, "worker %u (tid %d) affinity or last CPU %d is outside the allocation", i,
                         (int)s->tid, ran);
                ok = 0;
            }
        } else if (mask != s->cpu || ran != s->cpu) {
            snprintf(why, cap, "worker %u (tid %d) expected CPU %d, mask CPU %d, last ran on %d", i,
                     (int)s->tid, s->cpu, mask, ran);
            ok = 0;
        }
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

int afc_registry_worker_cpu_ns(uint64_t* total)
{
    uint64_t sum = 0;
    int rc = AF_OK;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < nworkers; i++) {
        struct timespec t;
        if (!slots[i].tid || clock_gettime(slots[i].clock, &t)) {
            rc = AF_ERROR;
            break;
        }
        sum += (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
    }
    pthread_mutex_unlock(&lock);
    *total = sum;
    return rc;
}

static void put_mask(AfcBuf* b, pid_t tid)
{
    cpu_set_t set;
    afc_buf_puts(b, "[");
    if (!sched_getaffinity(tid, sizeof set, &set))
        for (int c = 0, first = 1; c < CPU_SETSIZE; c++)
            if (CPU_ISSET(c, &set)) {
                afc_buf_printf(b, first ? "%d" : ",%d", c);
                first = 0;
            }
    afc_buf_puts(b, "]");
}

/* Observed placement as a JSON object: mode, allocation, whether the control
 * (caller) thread is a registered worker, and each slot's affinity, last CPU
 * and cumulative thread CPU time. */
void afc_registry_describe(AfcBuf* b)
{
    pthread_mutex_lock(&lock);
    int caller = find_tid(control_tid);
    afc_buf_printf(b, "{\"mode\":\"%s\",\"allocation\":[", open_mode ? "open" : "pinned");
    for (int c = 0, first = 1; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &allocation)) {
            afc_buf_printf(b, first ? "%d" : ",%d", c);
            first = 0;
        }
    afc_buf_puts(b, "],\"control_cpu\":");
    afc_buf_printf(b, "%d,\"control_mask\":", control_cpu);
    put_mask(b, control_tid);
    if (caller >= 0)
        afc_buf_printf(b, ",\"caller_slot\":%d,\"slots\":[", caller);
    else
        afc_buf_puts(b, ",\"caller_slot\":null,\"slots\":[");
    for (uint32_t i = 0; i < nworkers; i++) {
        const Slot* s = &slots[i];
        struct timespec t = {0, 0};
        int alive = s->tid && tid_alive(s->tid);
        if (alive)
            clock_gettime(s->clock, &t);
        afc_buf_printf(b, "%s{\"tid\":%d,\"alive\":%s,\"mask\":", i ? "," : "", (int)s->tid,
                       alive ? "true" : "false");
        if (alive)
            put_mask(b, s->tid);
        else
            afc_buf_puts(b, "[]");
        afc_buf_printf(b, ",\"last_cpu\":%d,\"cpu_ns\":%llu}", alive ? last_cpu(s->tid) : -1,
                       (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec);
    }
    afc_buf_puts(b, "]}");
    pthread_mutex_unlock(&lock);
}
