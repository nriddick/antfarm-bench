# Five-pass series: Opus 5.5 / Sonnet 5.5 × high / medium × Antfarm / oneTBB / Rayon (2026-10-21 – 10-22)

Five independent passes of 12 trajectories (60 in total). Each trajectory runs initial → opt1 → library-use gate →
paired selection → late-revealed split-smt high-count stage → gate → paired evaluation → disposition → Sonnet 5.5
medium review. Tables: [COMPARISON.md](COMPARISON.md). Library-limitation statements: [LIMITATIONS.md](LIMITATIONS.md).
Per pass: `../passN/SUMMARY.md`, plus [pass 1's assessment](../pass1/ASSESSMENT.md).

## Execution

- **Completion:** 60/60 trajectories exited 0, and protected inputs were unchanged in all 180 producer stages. Wall time: pass 1
  6 h 06 min (all six high-count counts evaluated), passes 2–5 4 h 44 min – 4 h 52 min (N=8/15/30/40), about 25.3 h in total.
- **Correctness:** every final (adapted) version passed every parent audit and plain process at every evaluated N. Failures
  are confined to earlier versions:
  - pre-adapt Antfarm hosts rejected at startup for N above the distinct-CPU/SMT limits (every Antfarm trajectory);
  - one audit-protocol bug, pass 3 Sonnet medium × Rayon (wrong generation indices), present at initial and opt1 and fixed at adapt.
- **Gates:** 119 of 120 gate runs passed on both paths. The failure, pass 3 Sonnet medium × Rayon adapt native, rejected the broadcast +
  barrier pattern the user had ruled acceptable. It is arbiter drift; the prompts state the ruling explicitly from pass 4.
- **Reviews:** all 60 were acceptable with qualifications.
- **Infrastructure:** one server-side `529` (pass 1, resumed and recorded). Pass-1 harness fixes: `stage.py`
  runner-failure reporting and API-error resume. From pass 2: reviewer by-design note, joint no-regression rule and the
  reduced high-count evaluation set.

## Composite frames (the frame adapter's measure)

Median across passes, at N≥30:

| | small frame | heavy frame |
|---|---|---|
| Antfarm (Opus high / Opus medium / Sonnet high) | 0.109–0.131 ms | 0.83–0.86 ms |
| oneTBB (all four) | 0.131–0.144 ms | 0.85–0.87 ms |
| Rayon (all four) | 0.194–0.258 ms | 0.95–1.16 ms |

- **Spread:** results are tight across passes and models, except Sonnet medium × Antfarm, which has a wide range (heavy 0.83–1.25 ms,
  small up to 0.47 ms at N=40).
- **N≥30:** Antfarm leads the small frame and ties oneTBB on the heavy frame. Rayon trails both and loses ground from N=30 to N=40.
- **N=8/15:** all three libraries are within a few percent.
- **What drives it:** all libraries execute the same compiled payload, so these differences come from dispatch, overlap and
  hosting, not kernels.

## Native GoL (the agents' own kernels)

- **Model dominates.** Opus high, Opus medium and Sonnet high reach 42–98 ms at 1024² (N=15–40, medians) on every
  library. Sonnet medium is roughly 5–10× slower, with very wide ranges, on Antfarm, oneTBB and Rayon alike.
- **Sonnet high × Rayon** is the most consistent native result: 42 ms at 1024² and about 26 ms at 256², flat from N=15 to 40.
  It uses persistent row bands hosted by a Rayon broadcast with a producer barrier.
- **Rayon's high-N weakness:** Opus × Rayon degrades sharply at small boards and high N (64²: 114 ms at N=8 → 724 ms at N=40 for Opus
  high). Rayon's broadcast reaches every thread, and idle threads spin.
- **Antfarm and oneTBB** stay flatter from N=15 to 40. The new per-group guidance shows in Antfarm's Opus-high 1024² staying
  at about 52 ms through N=40.

## Selection

opt1 native changes were retained in most trajectories: Antfarm 20/20, Rayon 18/20, oneTBB 17/20. Frame changes were
retained far less: Antfarm 15/20, Rayon 10/20, oneTBB 2/20. Composite frames rarely improved enough to clear 1.5%,
especially for oneTBB, where the initial `parallel_for`/`task_group` adapters were already near the payload floor.

## Library limitations reported by agents

Verified against the pinned sources in [VERIFIED-LIMITATIONS.md](VERIFIED-LIMITATIONS.md); use that file, not the raw extract, for publication. Summary of the agents' statements:

- **Antfarm:**
  - no oversubscription (one pool worker per CPU; duplicate CPUs collapse);
  - FarmHost filters SMT siblings by default;
  - the per-LLC-group host is not packaged (roadmap), so agents assembled it from documented parts;
  - `parallelFor` range constraints;
  - idle-spin samples dominate profiles.
- **oneTBB:**
  - workers re-enter arenas in different slots each generation, so `static_partitioner` rows move across L3 domains;
  - `global_control` caps workers at N-1 for a computing caller;
  - no debug symbols for attributing library time;
  - the `leave_policy` control is preview-only.
- **Rayon:**
  - no L3/SMT awareness;
  - no broadcast to a subset of the pool;
  - idle spinning cannot be configured off;
  - automatic splitting coarsens frame chunks.
  - Eight stage reports claim the caller cannot become a pool worker, but rayon-core 1.12.1's `use_current_thread` (in the
    supplied docs) allows it, and 14 of 20 final Rayon sources reference it. These claims are unreliable.

## Decisions after the series (2026-10-22)

1. `stage.py check` now includes audit builds, for future runs.
2. The gate/review ruling example added from pass 4 stays.
3. Limitation claims are verified in [VERIFIED-LIMITATIONS.md](VERIFIED-LIMITATIONS.md):
   - most are confirmed or partly confirmed;
   - the Rayon "caller cannot be a worker" claim is false;
   - three rows concern benchmark packaging or the supplied helper rather than the libraries.
