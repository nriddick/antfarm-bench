# Pass 1: Opus 5.5 / Sonnet 5.5 × high / medium × Antfarm / oneTBB / Rayon, all stages (2026-10-21)

**Configuration.** Twelve trajectories (4 model configurations × 3 libraries), each running initial → opt1 → gate →
paired selection → split-smt adapt (N=8…40) → gate → paired evaluation → disposition → Sonnet-medium review. The rest:
- **Antfarm:** `5f257e5`, with the multi-LLC guidance.
- **Prompts and budget:** the compact prompt and RUN.md, the per-stage testing budget, the library-use gate.
- **Selection:** 1.5% on native or composite frames, with 4 pairs (opaque-C descriptive, one pair).
- **Scheduling:** six pipelines in flight, with evaluations serialized under the writer-preferring quiet lock.
- **Harness:** `dd59519`, plus `8182fb7` (stage.py runner failures) and `42187b4` (API-error resume), both applied mid-pass.

Wall time: 14:01–20:07 UTC (6 h 06 min). All 12 completed with exit 0. Generated tables: [SUMMARY.md](SUMMARY.md).

## Did it run smoothly?

- **Correctness:** every parent evaluation process passed except the pre-adapt Antfarm versions at N≥24 (four
  trajectories, 124 startup rejections each: SMT filtering and one worker per CPU). Every adapted version runs at every N.
- **Gates:** all 24 gate verdicts passed (opt1 and adapt, native and frame). **Reviews:** all 12 acceptable with qualifications.
- **Protected inputs:** unchanged in all 36 producer stages.
- **Infrastructure:**
  - Sonnet medium × Antfarm adapt ended on a server-side `529 Overloaded`. It was resumed (recorded), and its gate,
    evaluation and review were rerun; the first versions are kept under `superseded-api-error/`. Automatic API-error resume now covers
    producers, gates and reviews.
  - `stage.py` had reported a frame build failure as a passing check (only native rows were summarized). This was found in
    initial stages and fixed mid-pass; affected agents had noticed the build error and rerun the check.
- **Testing budget:** followed in 35 of 36 stages. Extra checks always followed a failed check. Opus high × Rayon's adapt
  stage reran one failing baseline runner directly. The counter over-counts `--help` and textual mentions, so the
  evidence directories are authoritative.

## Implementer behaviour

All 36 stages used their library's own constructs, with no producer schedulers (gate and review). Recurring review
qualifications:
1. **Participation below N** at high counts: deliberate team sizing, single-group small boards, parked or idle extras.
   This is permitted and recorded, but it means the high-N ratios largely measure avoiding oversubscription, not scaling to N.
2. **Untested final edits after the instrumented run.** This is the budget as designed; parent audits cover them and all passed.
3. **Thin opt1 profiler evidence** behind some hypotheses.
4. **Overstated or non-reproducing report claims**, compared with the parent's paired data.

Selection: several frame changes were retained jointly on native gains (hosting changed), with composite ratios
0.996–1.001. Opus high × oneTBB's opt1 was rejected on both workloads.

## Headline measurements (parent split-smt evaluation, adapted source)

| Trajectory | heavy frame ms N=15 / 30 / 40 | native 1024² ms N=15 / 30 / 40 |
|---|---|---|
| opus-high-antfarm | 1.541 / 0.858 / 0.855 | 53.7 / 53.2 / 53.9 |
| opus-medium-antfarm | 1.492 / 0.826 / 0.842 | 67.3 / 88.2 / 83.8 |
| sonnet-high-antfarm | 1.481 / 0.818 / 0.820 | 57.1 / 63.0 / 63.3 |
| sonnet-medium-antfarm | 1.488 / 0.832 / 0.846 | 255.3 / 327.6 / 340.3 |
| opus-high-tbb | 1.500 / 0.854 / 0.854 | 60.4 / 60.4 / 59.9 |
| opus-medium-tbb | 1.489 / 0.848 / 0.873 | 60.4 / 70.4 / 98.4 |
| sonnet-high-tbb | 1.493 / 0.843 / 0.842 | 54.7 / 54.3 / 54.1 |
| sonnet-medium-tbb | 1.492 / 0.854 / 0.871 | 59.5 / 87.2 / 163.9 |
| opus-high-rayon | 1.526 / 0.955 / 1.126 | 48.1 / 77.6 / 100.1 |
| opus-medium-rayon | 1.533 / 0.957 / 1.115 | 48.8 / 61.6 / 83.2 |
| sonnet-high-rayon | 1.530 / 0.951 / 1.119 | 48.6 / 48.5 / 48.9 |
| sonnet-medium-rayon | 1.556 / 0.981 / 0.967 | 768.8 / 656.5 / 653.8 |

These are single trajectories per cell, so no ranking. Composite frames cluster tightly by library: Antfarm and oneTBB
0.82–0.87 ms at N≥30, Rayon 0.95–1.13 ms. Model choice shows mostly in native code. Sonnet medium's Antfarm and Rayon
kernels are about 4–6× and 13–16× slower at 1024².
