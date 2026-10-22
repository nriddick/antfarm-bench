# Open-frame experiment kit

Minimal state for running the open-hosting GoL + composite-frame comparison. Independent coding agents implement
native Game of Life and a composite-frame adapter with **Antfarm**, **oneTBB** or **Rayon**, through three stages:
1. a reasoned initial implementation;
2. one profiler-guided optimization;
3. a late-revealed higher-worker-count stage.

The parent evaluates each stage independently, gates library use and reviews each trajectory.

Extracted from `antfarm_savina` at `345a2119` (2026-10-22). The reports of the last run are in
[`reports/2026-10-22-five-pass/`](reports/2026-10-22-five-pass/series/ASSESSMENT.md).

## Layout

The kit mirrors the original workspace's relative layout, so paths inside the code are unchanged.

| Path | Contents |
|---|---|
| `scripts/unified/` | trusted runners: `gol_frame.py` (native), `microframe.py` (frames), `stage.py` (the agents' check/measure budget), `afbuild.py`, `topology.py`, `mkbundle.py` (documentation bundles), `vendor_rust.py`, `build_payload.py` |
| `benchmarks/unified/core/` | trusted C core (registry, audit, reference, driver) source; workspaces receive it prebuilt |
| `benchmarks/unified/microframe/` | frame ABI v2 (`microframe.h`), shared payload source `kernels.c`, recipes, per-library bridges and neutral stubs |
| `benchmarks/unified/bindings/{d,cpp,rust}` | language bindings, supplied pool helpers and native stubs |
| `benchmarks/unified/applied/docs-bundle/manifest.json` | hashes of the library documentation bundles given to agents |
| `docs/unified/prompts/` | initial, opt1, adapt (+ native-only), gate and review prompts |
| `results/unified/open-frame/` | orchestration (`agents.py`, `pipeline.py`, `run_pass.py`, `evaluate.py`, `gate.py`, `selection.py`, `disposition.py`, `review.py`, `summarize.py`, `compare.py`, `status.py`) and `payload/`, the prebuilt shared payload objects and core libraries with their manifest |
| `reports/2026-10-22-five-pass/` | last run: per-pass summaries, series assessment, comparison tables, verified library limitations |
| `scripts/bootstrap.py` | fetches the two untracked inputs below |

## Requirements and bootstrap

Linux x86-64. The last run used a Ryzen 9 5900XT (16 cores / 32 threads, two L3 domains); placement is derived from
the discovered topology. Tools: Python 3, clang/clang++ (23.1), LDC (1.43), Rust/cargo 1.99 (`rust-toolchain.toml`),
oneTBB headers and library (2023.1 headers, `libtbb.so.12.19`), `perf`. Agent CLIs: `claude` (Opus 5.5 /
Sonnet 5.5); `codex` only if the `luna-high` reviewer is selected.

```sh
python3 -B scripts/bootstrap.py --antfarm-source /path/to/local/antfarm   # or set AFBENCH_ANTFARM_MIRROR
#   optional: --cargo-home ~/.cargo  (default; copies the 7 pinned crates)  |  --fetch-crates (network)
python3 -B scripts/unified/build_payload.py --out /tmp/payload-check      # optional: rebuild, compare object hashes
```

- **Antfarm:** checked out at `upstream/antfarm`, pinned to `5f257e5ab894307be6e1e73f4c64fdb23743a085` (1.9.1-progress,
  multi-LLC hosting guidance). That branch is local-only until pushed. Vendored source is identical to `f3459f8`.
- **Rayon:** `rayon 1.10.0` / `rayon-core 1.12.1`, from `.build/cargo-home`.
- **Payload:** objects `bdb9e282…` (plain) and `38ad2657…` (audit), built at `antfarm_savina` `92999b1`; the
  manifest lists commands and compiler.
- **`AFBENCH_MAIN_ROOT`:** defaults to this kit's absolute path, which workspaces need because they receive copies of
  the runners. Set it if the kit moves.

## Contract in force

- **Workers:** exactly N registered workload threads; a computing caller counts inside N. Affinity is free within the allocation
  (the distinct slot CPUs, control CPU excluded). No prescribed waiting.
- **Frame ABI v2:** one timed `frame()` call; batches may overlap; wave order within a batch is the only dependency.
- **Library use:** the selected library must distribute and balance the work. Static partitions, producer barriers
  between dependent phases, and a single library dispatch (e.g. a broadcast) hosting static bands are allowed; producer
  queues, claim counters, work stealing or worker loops pulling work outside the library's scheduler are not. Hosting
  assembled from documented library primitives as the library's own guidance recommends is idiomatic.
- **Testing budget per stage:**
  - many confident changes are allowed;
  - one `stage.py check` (plain **and audit** builds; reruns only after a failure);
  - one `stage.py measure [--perf]`, then only hypothesis tweaks or partial reverts;
  - later stages may first `measure` their starting code.
- **Library-use gate** (Sonnet 5.5 medium, `NATIVE/FRAME PASS|FAIL`) before retention.
- **Selection:** a workload is retained when the paired geomean ratio is below 0.985 over 4 fresh pairs per
  configuration. Native GoL owns `impl/`; the frame adapter is judged on the **composite frames only**; standalone
  opaque-C recipes must pass but are descriptive (one pair). When the shared hosting helper changes, the decision is joint, and
  neither workload may regress by more than 1.5%.
- **High-count stage:** agents see split-smt N=8/15/24/30/32/40; the parent evaluates N=8/15/30/40. The disposition is descriptive.
- **Review:** Sonnet 5.5 medium, in an isolated `/tmp` root. Untested post-measurement tweaks and below-N participation are by design.

**Changed since the last run (not exercised yet):** `stage.py check` now includes audit builds.

## Running

```sh
cd results/unified/open-frame
setsid nohup python3 -B run_pass.py pass1 6 > ../../../.build/pass1.log 2>&1 &   # 4 models x 3 libraries, 6 in flight
python3 -B status.py pass1                                                       # one line per trajectory
python3 -B summarize.py pass1 && python3 -B compare.py series pass1 [pass2 ...] # tables from recorded evidence
AFBENCH_WAVE=w python3 -B pipeline.py opus-high antfarm                         # one trajectory
```

Options: `AFBENCH_REVIEWER=luna-high`, `AFBENCH_SCOPE=native` (native-only adapt, frame adapter protected), and
`agents.py resume|resume-api MODEL SYSTEM STAGE` for interrupted sessions (API errors are resumed automatically).

Operational notes from the last run:
- **Detaching:** launch passes with `setsid nohup`; agent harness background jobs are killed after about 2 h.
- **Pass length:** about 4.8 h per pass at the reduced evaluation set (pass 1 with six counts took 6.1 h). Evaluations serialize
  under the quiet lock (`.build/open-frame/quiet.lock`, writer-preferring); producer sessions overlap.
- **Size:** about 150 MB of committed evidence per pass. Consider Git LFS before pushing.
- **Shell:** this host has no `/bin/bash`; run shell scripts with `bash script.sh`.
