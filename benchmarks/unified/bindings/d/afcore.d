/++
 + D mirror of the frozen afcore.h ABI v1.
 +
 + An implementation is a class implementing `GolImpl`; `mixin golMain!T;`
 + at module scope supplies `main`, which hands a vtable to `af_gol_main`.
 + Every trampoline catches `Throwable` and returns `AF_ERROR`. An optional
 + `int telemetry(char* json, size_t cap)` member on `T` is exported when
 + present. Audit hooks exist only under `version (AfAudit)`. The mixin also
 + sets the druntime option `gcopt=parallel:0`, so garbage collection never
 + creates threads besides the control thread and the pool workers.
 +/
module afcore;

enum uint AF_ABI_VERSION = 1;
enum : int { AF_OK = 0, AF_UNSUPPORTED = 1, AF_ERROR = 2 }
enum : uint { AF_WAIT_SPIN = 0, AF_WAIT_PARK = 1, AF_WAIT_FOREVER = 2, AF_WAIT_NATIVE = 3 }
enum uint AF_WORKER_AUTO = 0xFFFF_FFFF;

struct AfRunCfg
{
    uint workers;
    const(int)* worker_cpus;
    int control_cpu;
    uint wait_mode;
    const(char)* knobs_json;
}

struct AfAudit
{
    ulong cell_callbacks, aux_callbacks, foreign_callbacks;
    ulong nested, duplicate_or_order, missing, value_mismatch;
}

extern (C) nothrow
{
    alias AfStart = int function(void*, const(AfRunCfg)*);
    alias AfLoad = int function(void*, const(ubyte)*, uint, uint);
    alias AfRun = int function(void*, uint);
    alias AfStore = int function(void*, ubyte*);
    alias AfTelemetry = int function(void*, char*, size_t);
    alias AfStop = int function(void*);
}

struct AfGolImpl
{
    uint abi_version;
    void* self;
    AfStart start;
    AfLoad load;
    AfRun run;
    AfStore store;
    AfTelemetry telemetry;
    AfStop stop;
}

// Values from benchmarks/unified/core/abi-layout.json.
static assert(AfRunCfg.sizeof == 32 && AfRunCfg.alignof == 8);
static assert(AfRunCfg.workers.offsetof == 0 && AfRunCfg.workers.sizeof == 4);
static assert(AfRunCfg.worker_cpus.offsetof == 8 && AfRunCfg.worker_cpus.sizeof == 8);
static assert(AfRunCfg.control_cpu.offsetof == 16 && AfRunCfg.control_cpu.sizeof == 4);
static assert(AfRunCfg.wait_mode.offsetof == 20 && AfRunCfg.wait_mode.sizeof == 4);
static assert(AfRunCfg.knobs_json.offsetof == 24 && AfRunCfg.knobs_json.sizeof == 8);
static assert(AfAudit.sizeof == 56 && AfAudit.alignof == 8);
static assert(AfAudit.cell_callbacks.offsetof == 0 && AfAudit.cell_callbacks.sizeof == 8);
static assert(AfAudit.aux_callbacks.offsetof == 8 && AfAudit.aux_callbacks.sizeof == 8);
static assert(AfAudit.foreign_callbacks.offsetof == 16 && AfAudit.foreign_callbacks.sizeof == 8);
static assert(AfAudit.nested.offsetof == 24 && AfAudit.nested.sizeof == 8);
static assert(AfAudit.duplicate_or_order.offsetof == 32 && AfAudit.duplicate_or_order.sizeof == 8);
static assert(AfAudit.missing.offsetof == 40 && AfAudit.missing.sizeof == 8);
static assert(AfAudit.value_mismatch.offsetof == 48 && AfAudit.value_mismatch.sizeof == 8);
static assert(AfGolImpl.sizeof == 64 && AfGolImpl.alignof == 8);
static assert(AfGolImpl.abi_version.offsetof == 0 && AfGolImpl.abi_version.sizeof == 4);
static assert(AfGolImpl.self.offsetof == 8 && AfGolImpl.self.sizeof == 8);
static assert(AfGolImpl.start.offsetof == 16 && AfGolImpl.start.sizeof == 8);
static assert(AfGolImpl.load.offsetof == 24 && AfGolImpl.load.sizeof == 8);
static assert(AfGolImpl.run.offsetof == 32 && AfGolImpl.run.sizeof == 8);
static assert(AfGolImpl.store.offsetof == 40 && AfGolImpl.store.sizeof == 8);
static assert(AfGolImpl.telemetry.offsetof == 48 && AfGolImpl.telemetry.sizeof == 8);
static assert(AfGolImpl.stop.offsetof == 56 && AfGolImpl.stop.sizeof == 8);

extern (C) nothrow
{
    int af_gol_main(int argc, char** argv, const(AfGolImpl)* impl);
}

extern (C) nothrow @nogc
{
    int af_pin_self(int cpu);
    int af_worker_enter(uint index);
    int af_worker_adopt();
    /// Open-hosting runs only; see afcore.h.
    int af_worker_register(uint index);
    int af_workers_rendezvous(ulong deadline_ns);
    uint af_workers_registered();
    void af_audit_cell(uint gen, uint cell, ubyte next_state);
    void af_audit_cell_begin(uint gen, uint cell);
    void af_audit_cell_end(ubyte next_state);
    void af_audit_aux();
    void af_gol_init(ulong seed, uint density_pct, uint w, uint h, ubyte* output);
    void af_gol_ref_run(const(ubyte)* input, ubyte* output, uint w, uint h, uint generations);
    ulong af_gol_hash(const(ubyte)* grid, size_t n);
}

version (AfAudit)
{
    /// Report the cell's computed next 0/1 state; gen is absolute since load.
    void auditCell(uint gen, uint cell, ubyte next) nothrow @nogc { af_audit_cell(gen, cell, next); }
    /// Open a legacy report pair, which must not overlap another report.
    void auditCellBegin(uint gen, uint cell) nothrow @nogc { af_audit_cell_begin(gen, cell); }
    /// Close the legacy report pair with the cell's computed next 0/1 state.
    void auditCellEnd(ubyte next) nothrow @nogc { af_audit_cell_end(next); }
    /// Explicit auxiliary events: publication, barriers, continuations, conductors.
    void auditAux() nothrow @nogc { af_audit_aux(); }
}

/// Grids are row-major ubyte[h][w] of 0/1; every method returns an AF_* status.
interface GolImpl
{
    /// Untimed. Return only after af_workers_registered() == cfg.workers.
    int start(ref const AfRunCfg cfg);
    /// Untimed. Load canonical bytes; reset state and generation counter to 0.
    int load(const(ubyte)* grid, uint w, uint h);
    /// Timed and resumable: advance generations from the current state.
    int run(uint generations);
    /// Untimed. Write the current state as canonical 0/1 bytes.
    int store(ubyte* output);
    int stop();
}

mixin template golMain(T)
{
    // Parallel GC marking would start one thread per host CPU on the first
    // collection, ignoring affinity: unregistered extra threads on whichever
    // CPU triggered it. Mark on the collecting thread instead.
    extern (C) __gshared string[] rt_options = ["gcopt=parallel:0"];

    int main()
    {
        import afcore : golMainEntry;
        return golMainEntry!T();
    }
}

/// The vtable `golMain` passes to af_gol_main for `impl`.
AfGolImpl golTable(T : GolImpl)(T impl)
{
    AfGolImpl table;
    table.abi_version = AF_ABI_VERSION;
    table.self = cast(void*) impl;
    table.start = &startTrampoline!T;
    table.load = &loadTrampoline!T;
    table.run = &runTrampoline!T;
    table.store = &storeTrampoline!T;
    static if (__traits(hasMember, T, "telemetry"))
        table.telemetry = &telemetryTrampoline!T;
    table.stop = &stopTrampoline!T;
    return table;
}

int golMainEntry(T : GolImpl)()
{
    import core.memory : GC;
    import core.runtime : Runtime;

    auto impl = new T;
    // af_gol_main holds only the raw self pointer.
    GC.addRoot(cast(void*) impl);
    scope (exit) GC.removeRoot(cast(void*) impl);
    const table = golTable(impl);
    auto args = Runtime.cArgs;
    return af_gol_main(args.argc, args.argv, &table);
}

private int guarded(string method, F)(scope F call) nothrow
{
    try
        return call();
    catch (Throwable error)
    {
        import core.stdc.stdio : fprintf, stderr;
        fprintf(stderr, "afcore: %.*s: %.*s\n", cast(int) method.length, method.ptr,
                cast(int) error.msg.length, error.msg.ptr);
        return AF_ERROR;
    }
}

private extern (C) int startTrampoline(T)(void* self, const(AfRunCfg)* cfg) nothrow
{
    return guarded!"start"(() => (cast(T) self).start(*cfg));
}

private extern (C) int loadTrampoline(T)(void* self, const(ubyte)* grid, uint w, uint h) nothrow
{
    return guarded!"load"(() => (cast(T) self).load(grid, w, h));
}

private extern (C) int runTrampoline(T)(void* self, uint generations) nothrow
{
    return guarded!"run"(() => (cast(T) self).run(generations));
}

private extern (C) int storeTrampoline(T)(void* self, ubyte* output) nothrow
{
    return guarded!"store"(() => (cast(T) self).store(output));
}

private extern (C) int telemetryTrampoline(T)(void* self, char* json, size_t cap) nothrow
{
    return guarded!"telemetry"(() => (cast(T) self).telemetry(json, cap));
}

private extern (C) int stopTrampoline(T)(void* self) nothrow
{
    return guarded!"stop"(() => (cast(T) self).stop());
}
