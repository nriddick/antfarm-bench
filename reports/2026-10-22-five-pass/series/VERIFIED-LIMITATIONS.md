# Agents' library-limitation claims, verified (2026-10-22)

Distinct claims consolidated from the 60 trajectories' stage reports ([raw keyword extract](LIMITATIONS.md)), checked
against the pinned sources and installed libraries:
- **Antfarm:** `upstream/antfarm` at `5f257e5`.
- **oneTBB:** installed headers (`/usr/include/oneapi/tbb`, version.h 2023.1.0) and `libtbb.so.12.19`.
- **Rayon:** the vendored `rayon-core-1.12.1` and `rayon-1.10.0`.

Lines that are measurement caveats ("can't separate H1 from H2", profiler resolution) are not library claims and are
omitted. Verdicts: **confirmed**, **partly** (true with a stated qualification), **false**, or **not a library property**.

## Antfarm

| Claim | Verdict | Evidence |
|---|---|---|
| FarmHost cannot run more workers than accepted CPUs; repeated CPUs never add a worker (no oversubscription) | confirmed | `threadpool/source/threadpool/resolution.d:69,483` (duplicate requests never add a worker); `host.d:1291` (rejects workers > accepted CPUs) |
| FarmHost skips SMT siblings by default | confirmed (configurable) | `host.d:120–124` (`skipSmtSiblings = true` default); `resolution.d:469`; settable through `PoolOptions` |
| One pool per process; FarmHost hosts one Farm; no packaged per-LLC-group host | confirmed | `threadpool/source/threadpool/pool.d:399` ("a pool is already running"); `docs/guide/hosting.md:195–197` (per-group-Farm host is the missing primitive, on the roadmap). The same guide documents a hand-assembled per-group pool as the interim route |
| Work cannot be directed to particular workers or an L3 domain | partly | Within one Farm, consumers claim table shards by consumer index (`SPEC.md` §7a–c, lines 519–537); there is no per-entry targeting. Per-group Farms (documented interim shape) localize work by group |
| Per-group Farms cannot balance work across groups | confirmed (by design) | `hosting.md` "Several physical groups": every Farm's consumers stay in one group |
| The consumer set cannot be sized per region | confirmed | `antfarm/parallel.d:30–36`: `Options` has only grain, cost class and a wake hook |
| Blocking `FarmHost.parallelFor` requires a consuming caller | confirmed (alternative exists) | `host.d:791–799` (fatal without caller consumption); the asynchronous `parallelFor` plus completion hook works with an orchestration-only caller |
| The blocking parallel loop's caller idle behaviour cannot be configured | confirmed (caller only) | `parallel.d:294–301`: the caller help-drains/polls until the region finishes. Worker idle *is* configurable (`host.d:142–145`, `idle`, `idleTimeout`) |
| `parallelFor` ranges must be random-access and cannot hold mutable pointers | partly | `parallel.d:38–47`: finite, sliceable, random-access, POD ≤ 8-byte alignment. The no-unshared-mutable-alias rule applies to *asynchronous* ranges only; the synchronous adapter permits borrowed mutable data (`parallel.d:12–13`) |
| `ParallelHandle` is move-only with a private constructor | confirmed | `parallel.d:220–222` (`@disable this(this)`, private constructor) |
| The multi-domain fibre package is not available | not a library property | `ant_fibers/` exists upstream (`hosting.md:303`) but is outside this benchmark's vendored subset (`antfarm/**`, `threadpool/source/**`) |
| The supplied helper forces `callerConsumes=false` | not a library property | The benchmark's editable `pool_antfarm.d` default, not Antfarm |

## oneTBB

| Claim | Verdict | Evidence |
|---|---|---|
| Workers re-entering the arena can take different slots, so `static_partitioner` rows move between L3 domains | partly (consistent with source) | `partitioner.h:364–375`: static/affinity partitioning targets arena slot ids (`spawn(t, ctx, slot_id(my_head))`), not threads; the API gives no stable thread-to-slot binding. The observed reassignment is runtime behaviour, not a documented guarantee either way |
| `global_control(max_allowed_parallelism, N)` leaves N−1 workers | confirmed (not a limitation here) | Empirical: with value 4, exactly 4 distinct threads ran, including the caller. That is the exact-N contract with a computing caller |
| Arenas cannot be restricted to specific CPUs | confirmed (constraints API) | `task_arena.h:160–181`: constraints are NUMA node, core type, max concurrency and threads per core only. Threads can still be pinned on entry through a `task_scheduler_observer`, as the supplied helper does |
| `leave_policy::fast` is preview-only | confirmed | `task_arena.h:99,140–195` (under `__TBB_PREVIEW_PARALLEL_PHASE`) |
| oneTBB time cannot be attributed in profiles (no symbols) | not a library property | `libtbb.so.12.19` is stripped: a distribution packaging fact |

## Rayon

| Claim | Verdict | Evidence |
|---|---|---|
| The calling thread cannot become a pool worker | **false** | `rayon-core-1.12.1/src/lib.rs:544–556`: `ThreadPoolBuilder::use_current_thread` makes it thread index 0. Caveats: it does not run the stealing loop by itself (only when yielding into Rayon, e.g. `scope`, `yield_now`), and start handlers do not run for it, so harness registration must be done explicitly. 14 of 20 final Rayon sources reference it, yet 8 stage reports claimed the opposite |
| Broadcast cannot target a subset of the pool | confirmed | `broadcast/mod.rs:10–36,91–122`: `broadcast`/`spawn_broadcast` run on every thread |
| Idle spinning/sleeping cannot be configured | confirmed | `sleep/mod.rs:57–102`: fixed `ROUNDS_UNTIL_SLEEPY = 32` and `ROUNDS_UNTIL_SLEEPING`; the builder offers no wait setting (`lib.rs` builder: thread count, names, stack size, breadth-first, handlers, `use_current_thread`) |
| Rayon cannot park threads | partly | Idle workers do go to sleep automatically after their spin rounds (`sleep/mod.rs`), but there is no API to park or exclude chosen threads |
| No L3/SMT awareness and no way to keep work on the same thread across passes | confirmed | No work-to-thread affinity API (`thread_pool/mod.rs` exposes `current_thread_index` only); placement exists only through user pinning in handlers |
| Automatic splitting coarsens frame chunks | not a limitation | Adaptive splitting is configurable per iterator (`with_min_len`/`with_max_len` in rayon 1.10); the agent's own remedy |

## Use in the write-up

The confirmed and partly-confirmed rows are library properties supported by source. The Rayon caller claim must not be
repeated. The "not a library property" rows concern this benchmark's packaging or supplied helper.
