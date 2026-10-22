/++
 + Supplied default FarmHost arrangement for the harness worker CPUs.
 + Updated for Antfarm 1.9.0-progress single-shot C callback API.
 +
 + Open-hosting runs: this is one valid starting arrangement, not a contract.
 + It may be edited or replaced using Antfarm's supported facilities. With
 + wait mode native (open runs) the idle policy defaults to FarmHost's own
 + default and the options hook may change it; other modes keep the
 + historical protected mapping below.
 +
 + `AntfarmPool.create(cfg, optionsHook)` runs inside `start`, on the control
 + thread, before any other Antfarm call:
 + 1. validates W, distinct worker CPUs, a separate control CPU and the wait mode;
 + 2. widens the control thread natively to {control} ∪ worker CPUs and calls
 +    `scopeOsToCallingThread()` before topology discovery;
 + 3. derives groups from the worker CPUs' L3 domains, home (first CPU) first;
 + 4. sets the protected options: `workers=W`, `pool.processors`, `groups`,
 +    `CallerPlacement.unchanged`, `callerConsumes=false`, and the wait mode
 +    (spin; park with `idleTimeout = 1.msecs`; forever = park with
 +    `Duration.max`). The options hook sees these values and may set any
 +    other field; the protected fields are then re-asserted;
 + 5. re-pins the control thread and verifies that the host resolved exactly
 +    the worker CPUs and that the process has exactly W singleton-pinned
 +    workers plus the control thread;
 + 6. registers every worker through an untimed bootstrap: W one-shot raw ST
 +    entries (`avgCost=7`, claim ceiling 1) each call `af_worker_adopt()`
 +    and the shared-deadline `af_workers_rendezvous`. Creation fails unless
 +    all W register; on failure the host is closed or abandoned first.
 +
 + Wake rule: in park and forever modes call `host.wake()` after every
 + accepted raw write through `host.token`, including partial acceptance.
 + `publishRaw` does this. Host actor/park paths (`step`, `flush`, `pump`,
 + `publish`) wake on their own. `close()` closes the host, abandoning it
 + if work is undrained.
 +
 + `createShortBootstrap` is for harness fault-injection tests only and always
 + fails. The `golMain` mixin runs druntime with `gcopt=parallel:0`, so garbage
 + collection adds no threads beside the control thread and the workers.
 +/
module pool_antfarm;

import afcore;
import antfarm : DEFAULT_AVG_COST, PayloadBody, Callback, TableCompletionHook;
import threadpool.host;
import threadpool.topology : ProcessorId, discover, scopeOsToCallingThread;
import core.atomic : MemoryOrder, atomicFetchAdd, atomicLoad;
import core.time : Duration, msecs;
import std.exception : enforce;
import std.format : format;

/// Protected finite park timeout (FarmHost's default).
enum Duration parkTimeout = 1.msecs;
/// Bootstrap claim ceiling 1 (128 >> 7): one entry per worker.
enum uint bootstrapAvgCost = 7;
/// Shared rendezvous deadline, measured from bootstrap publication.
enum Duration bootstrapWindow = 1000.msecs;

alias OptionsHook = void delegate(ref FarmHostOptions options);

final class AntfarmPool
{
    private FarmHost host_;
    private ushort[] groups_;
    // Referenced from Farm tables until they complete, including on failure.
    private Bootstrap* bootstrap_;

    /// Create the pool and register all W workers; throws on any failure.
    /// `new FarmHost` waits for its workers to start without a timeout of
    /// its own (upstream has none); only the harness watchdog bounds it.
    static AntfarmPool create(ref const AfRunCfg cfg, scope OptionsHook optionsHook = null)
    {
        return new AntfarmPool(cfg, optionsHook, cfg.workers);
    }

    /// For fault-injection testing only; always fails. Publishes only
    /// `entries` < W bootstrap entries, so registration times out.
    static AntfarmPool createShortBootstrap(ref const AfRunCfg cfg, uint entries)
    {
        enforce(entries < cfg.workers, "short bootstrap needs fewer entries than workers");
        return new AntfarmPool(cfg, null, entries);
    }

    @property FarmHost host() { return host_; }

    /// Worker L3 groups as passed to FarmHost, home group first.
    @property const(ushort)[] groups() const { return groups_; }

    /// Publish every body as single-shot raw entries on `host.token`, waking
    /// parked workers after each accepted write and stepping the host on
    /// backpressure. With `completion`, every accepted write is one tracked
    /// table whose hook runs exactly once. Returns the number of accepted
    /// writes; count them before waiting for completions. A refused write is
    /// retried without a deadline, as in upstream `FarmHost.publish`.
    size_t publishRaw(BR)(Callback callback, BR bodies, ulong bodyWords,
                          TableCompletionHook* completion = null, uint avgCost = DEFAULT_AVG_COST)
    {
        import std.range : popFrontExactly;

        size_t writes;
        while (!bodies.empty)
        {
            const written = host_.farm.write(callback, bodies, bodyWords,
                                            host_.token, avgCost, completion);
            if (written == 0)
            {
                host_.step();
                continue;
            }
            ++writes;
            host_.wake();
            bodies.popFrontExactly(cast(size_t) written);
        }
        return writes;
    }

    /// Close the host (abandon if undrained). Idempotent.
    void close()
    {
        if (host_ !is null)
            host_.closeOrAbandon();
    }

    private this(ref const AfRunCfg cfg, scope OptionsHook optionsHook, uint bootstrapEntries)
    {
        const cpus = validate(cfg);
        try
        {
            setAffinity(0, cpus ~ cfg.control_cpu);
            scopeOsToCallingThread();
            const snap = discover();
            ProcessorId[] processors;
            foreach (cpu; cpus)
            {
                const id = ProcessorId(0, cast(ushort) cpu);
                const p = snap.byId(id);
                enforce(p !is null, format("worker CPU %s is not in the discovered topology", cpu));
                processors ~= id;
                bool seen;
                foreach (g; groups_)
                    seen |= g == p.llcIndex;
                if (!seen)
                    groups_ ~= p.llcIndex;
            }
            FarmHostOptions options;
            protect(options, cfg, processors);
            if (optionsHook !is null)
                optionsHook(options);
            protect(options, cfg, processors);
            host_ = new FarmHost(options);
            enforce(af_pin_self(cfg.control_cpu) == AF_OK,
                    format("cannot re-pin the control thread to CPU %s", cfg.control_cpu));
            verifyPlacement(cfg, cpus);
            registerWorkers(cfg, bootstrapEntries);
        }
        catch (Throwable error)
        {
            if (host_ !is null)
            {
                try
                    host_.closeOrAbandon();
                catch (Throwable)
                {
                }
            }
            af_pin_self(cfg.control_cpu);
            throw error;
        }
    }

    private void protect(ref FarmHostOptions options, ref const AfRunCfg cfg,
                         const(ProcessorId)[] processors)
    {
        options.workers = cfg.workers;
        options.pool.processors = processors;
        options.groups = groups_.length == 1 ? HostGroups.only(groups_[0]) : HostGroups.span(groups_);
        options.caller = CallerPlacement.unchanged;
        options.callerConsumes = false;
        if (cfg.wait_mode == AF_WAIT_NATIVE)
            return; // open runs: idle stays as defaulted or chosen by the hook
        options.idle = cfg.wait_mode == AF_WAIT_SPIN ? WorkerIdle.spin : WorkerIdle.park;
        options.idleTimeout = cfg.wait_mode == AF_WAIT_FOREVER ? Duration.max : parkTimeout;
    }

    private void verifyPlacement(ref const AfRunCfg cfg, const(int)[] cpus)
    {
        import core.sys.posix.unistd : getpid;
        import std.algorithm : sort;
        import std.conv : to;
        import std.file : SpanMode, dirEntries;
        import std.path : baseName;

        int[] resolved;
        foreach (p; host_.resolution.workers)
            resolved ~= p.group == 0 ? p.lp : -1;
        enforce(resolved.sort.release == cpus.dup.sort.release,
                format("FarmHost resolved workers %s, expected %s", resolved, cpus));
        int[] pinned;
        bool control;
        foreach (entry; dirEntries("/proc/self/task", SpanMode.shallow))
        {
            const tid = entry.name.baseName.to!int;
            const mask = affinity(tid);
            if (tid == getpid())
            {
                enforce(mask == [cfg.control_cpu],
                        format("control thread affinity %s, expected [%s]", mask, cfg.control_cpu));
                control = true;
                continue;
            }
            enforce(mask.length == 1 && contains(cpus, mask[0]) && !contains(pinned, mask[0]),
                    format("unexpected thread %s with affinity %s", tid, mask));
            pinned ~= mask[0];
        }
        enforce(control && pinned.length == cpus.length,
                format("found %s pinned workers for %s worker CPUs", pinned.length, cpus.length));
    }

    private void registerWorkers(ref const AfRunCfg cfg, uint entries)
    {
        auto state = new Bootstrap;
        bootstrap_ = state;
        state.word[0] = cast(ulong) state;
        state.hook = TableCompletionHook(cast(void*) state, &bootstrapDone);
        state.deadline = nowNs() + bootstrapWindow.total!"nsecs";
        auto pending = Repeated(state.word[], entries);
        size_t tables;
        // Never wait between partial publications: claimed entries block in
        // the rendezvous until the rest are published.
        while (!pending.empty)
        {
            const written = host_.farm.write(&bootstrapEntry, pending, 1,
                                                  host_.token, bootstrapAvgCost, &state.hook);
            if (written != 0)
            {
                pending.count -= cast(size_t) written;
                ++tables;
                host_.wake();
                continue;
            }
            enforce(nowNs() < state.deadline, "bootstrap publication timed out");
            host_.step();
        }
        host_.waitUntil(() => atomicLoad!(MemoryOrder.acq)(state.tables) == tables,
                        2 * bootstrapWindow);
        const registered = af_workers_registered();
        enforce(atomicLoad(state.failures) == 0 && registered == cfg.workers,
                format("bootstrap registered %s of %s workers (%s entry failures)",
                       registered, cfg.workers, atomicLoad(state.failures)));
    }
}

private struct Bootstrap
{
    ulong[1] word;
    ulong deadline;
    shared uint failures;
    shared ulong tables;
    TableCompletionHook hook;
}

// One body per bootstrap entry, each the Bootstrap pointer.
private struct Repeated
{
    const(ulong)[] word;
    size_t count;
    @property bool empty() const nothrow @nogc { return count == 0; }
    @property const(ulong)[] front() const nothrow @nogc { return word; }
    void popFront() nothrow @nogc { --count; }
    @property Repeated save() const nothrow @nogc { return Repeated(word, count); }
    @property size_t length() const nothrow @nogc { return count; }
}

private extern(C) void bootstrapEntry(const(ulong)* body, size_t bodyWords) nothrow @nogc @system
{
    auto state = cast(Bootstrap*) body[0];
    if (af_worker_adopt() != AF_OK || af_workers_rendezvous(state.deadline) != AF_OK)
        atomicFetchAdd(state.failures, 1u);
}

private extern(C) void bootstrapDone(void* context) nothrow @nogc @system
{
    atomicFetchAdd!(MemoryOrder.rel)((cast(Bootstrap*) context).tables, 1UL);
}

private const(int)[] validate(ref const AfRunCfg cfg)
{
    import core.sys.linux.sched : cpu_set_t;

    enforce(cfg.workers >= 1 && cfg.worker_cpus !is null, "at least one worker CPU is required");
    enum limit = cpu_set_t.sizeof * 8;
    enforce(cfg.control_cpu >= 0 && cfg.control_cpu < limit,
            format("control CPU %s is out of range", cfg.control_cpu));
    const cpus = cfg.worker_cpus[0 .. cfg.workers];
    foreach (i, cpu; cpus)
    {
        enforce(cpu >= 0 && cpu < limit, format("worker CPU %s is out of range", cpu));
        enforce(cpu != cfg.control_cpu, format("worker CPU %s is the control CPU", cpu));
        enforce(!contains(cpus[0 .. i], cpu), format("worker CPU %s is listed twice", cpu));
    }
    enforce(cfg.wait_mode == AF_WAIT_SPIN || cfg.wait_mode == AF_WAIT_PARK
            || cfg.wait_mode == AF_WAIT_FOREVER || cfg.wait_mode == AF_WAIT_NATIVE,
            format("Antfarm supports spin, park, forever or native, not wait mode %s", cfg.wait_mode));
    return cpus;
}

private void setAffinity(int tid, const(int)[] cpus)
{
    import core.sys.linux.sched : CPU_SET, cpu_set_t, sched_setaffinity;

    cpu_set_t set;
    foreach (cpu; cpus)
        CPU_SET(cpu, &set);
    enforce(sched_setaffinity(tid, set.sizeof, &set) == 0,
            format("cannot widen the control thread to CPUs %s", cpus));
}

private int[] affinity(int tid)
{
    import core.sys.linux.sched : CPU_ISSET, cpu_set_t, sched_getaffinity;

    cpu_set_t set;
    enforce(sched_getaffinity(tid, set.sizeof, &set) == 0, format("cannot read affinity of thread %s", tid));
    int[] cpus;
    foreach (cpu; 0 .. cast(int) (set.sizeof * 8))
        if (CPU_ISSET(cpu, &set))
            cpus ~= cpu;
    return cpus;
}

private bool contains(const(int)[] values, int value)
{
    foreach (v; values)
        if (v == value)
            return true;
    return false;
}

private ulong nowNs() nothrow @nogc
{
    import core.sys.posix.time : CLOCK_MONOTONIC, clock_gettime, timespec;

    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return cast(ulong) t.tv_sec * 1_000_000_000UL + t.tv_nsec;
}
