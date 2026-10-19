# Audit of the CUDA compaction results

**Scope.** I checked every quantitative claim in
[`results/cuda-results-summary.md`](../results/cuda-results-summary.md) against
the raw result bundles from the Zyra host. That covers all 122 trials (4,468
compaction jobs) from the five campaigns the summary cites. I recomputed every
statistic independently, without reusing the original analysis code, using
[`analyze.py`](analyze.py).

**Bottom line.** The data is complete and every trial verified correctly. The
central compaction claims reproduce. However, five claims are wrong or
misleading, and three methodological issues must be disclosed before
submission. The most consequential correction: **the GPU path cuts
compaction-thread CPU, but whole-process CPU rises.** The summary's "31–37%
lower CPU time" therefore cannot be presented as a host-CPU or efficiency
saving.

---

## 1. Data preservation and provenance

| Check | Finding |
|---|---|
| Raw data location | The summary's sources lived only in `/tmp/sa29ali/*` on Zyra (volatile). They are now copied, minus database files, to `artifacts/zyra-final/` (3.3 GB, gitignored). **Archive this in a durable DOI-backed store (e.g., Zenodo) before submission.** |
| Trial count | 18 stress + 6 stress-confirm + 66 mixed + 8 capacity + 24 screening = 122. All present. |
| Source ↔ binary | The Zyra source tree `~/lsm-tree` is byte-identical (SHA-256 over all 51 tracked files under `apps/`, `src/`, `include/`, `cmake/`, `tools/`, `tests/`, and `CMakeLists.txt`) to local commit `792cf59`. No source file is newer than the mixed-campaign `lsm_db` binary (`39ceaa7e…`). |
| Two `lsm_db` binaries | The stress campaigns used `lsm_db` `81081e0d…`; the mixed, capacity, and screening campaigns used `39ceaa7e…`. Build timestamps show that only `apps/lsm_db.cpp` was recompiled in between (18:50:54, after the stress runs). RocksDB, the CUDA kernel library (`liblsm_blocks.a`), and the worker were never rebuilt. The only schema difference is a new `dropped_job_samples` counter, which is instrumentation only. **Disclose this; it does not change the compaction path.** The exact pre-edit `lsm_db.cpp` is not archived. |
| Untracked configs | The six `configs/cuda-realistic-*.json` files used by the mixed campaign are untracked in git. **Commit them.** |
| Hidden configuration | **The published GPU results require `LSM_DISABLE_GPU_INPUT=1`**: every published GPU trial shows 0 input batches. The variable appears in the runbook's stress-sweep command but is recorded nowhere in the configs, result bundles, or summary. Re-running on 2026-10-06 without it produced GPU jobs **35–45% slower than CPU** (88–98 ms vs 66 ms; 66/66 pairs) with the *identical archived binary*. With it set, the archived and current binaries both reproduce the published 51 ms. The runner now records all `LSM_*` variables in `build.json`. |
| Build | Release `-O3`, GCC 12.1.0, CUDA 12.3.107 (`sm_86`), nvCOMP 5.3.0.16, RocksDB 11.8.1 @ `abeebd96`. |

## 2. Claim-by-claim verdicts

✅ = reproduces · ⚠️ = numerically right but mislabelled or misleading · ❌ = not supported by the data

| # | Claim in summary | Recomputed | Verdict |
|---|---|---|---|
| 1 | Stress: median compaction time 17.8–21.9% lower | −18.0% to −22.4% per rate; 12/12 pairs; geo-mean −21.1% [−23.1, −19.0] | ✅ |
| 2 | Stress: total compaction time 15.4–18.8% lower | Per GiB: −15.4% to −20.5%; 12/12 pairs | ✅ |
| 3 | Stress: compaction **CPU time** 31.8–36.6% lower | Compaction-*thread* CPU/GiB −35.8% (12/12). **Whole-process CPU +3.0% in-window (11/12 pairs GPU higher, p = 0.006).** Profiling (fixed-period `perf` cpu-clock, 2026-10-06) shows why. The GPU path runs its output pipeline on ~107 short-lived `std::async` helper threads that RocksDB's per-job `cpu_micros` does not count. Counting *all* compaction threads, one write-only 5k trial used 3.90 CPU-s natively vs **3.94 CPU-s** on the GPU path (RocksDB reported 2.70 vs 1.68). | ❌ Accounting artifact. True compaction CPU is unchanged with CUDA's default sync (−15% with blocking sync), and total host CPU *increases*. |
| 4 | "Mean GPU compaction **p99** 87.0–90.0 ms" | In all 122 trials the reported "p99" equals the **maximum** job, because there are only 10–53 jobs per trial | ⚠️ Mislabelled: it is the slowest job. Native has the lower slowest job in 10/12 stress pairs. |
| 5 | 5k confirmation: GPU p99 83.26–83.51 ms, "native-p99 parity" | Values correct, and they are max-job. Native max: 80.8, 79.7, and 325.6 (outlier) ms. GPU worse in 2 of 3 pairs. | ⚠️ "Parity" overstated. |
| 6 | "Native compaction share of measurement window 26.3/35.3/44.2%" | Those numbers are whole-process-lifetime compaction (warm-up + window + final drain) ÷ 8 s. Measured between the 1 Hz engine snapshots bracketing the window (an 8.0 s interval starting ~0.25 s early), the share is **21.2/27.7/35.4%** (GPU: 16.8/23.0/27.7%) | ❌ Overstated by ~25%. |
| 7 | "At 5,000 writes/s, GPU improves both median and p99 write latency" | Across all six 5k pairs: GPU write p50 better 3/6, write p99 better 3/6. In the independent confirmation, the GPU was **worse** in both non-outlier pairs (43.5 vs 22.6 ms; 39.4 vs 24.6 ms). | ❌ Not supported. Based on means of three trials, one skewed by a native outlier. The confirmation data that contradicts it was omitted. |
| 8 | Mixed: median compaction 3.5–20.0% lower; "GPU compaction is cheaper at **every** retained point" | Compaction-thread CPU/GiB is lower at every point (33/33 ✅). Median job duration: update-heavy 4,000/s has GPU better in **1/3** pairs; 5/33 mixed pairs overall favor CPU | ⚠️ True for CPU/GiB, not for duration. |
| 9 | Mixed: total compaction 5.4–21.9% lower; CPU time 21.5–39.9% lower | Per GiB: −5.5% to −22.2%; thread CPU/GiB −22.0% to −40.1% | ✅ (same caveat as #3) |
| 10 | Update-heavy 5.3–6.7k and balanced 8–10k: "foreground p99 changes by only 4–12%" | Direction is **GPU worse**. Across all 33 mixed pairs, write p99 is worse in 25/33 (median ratio 1.045; sign test p = 0.005) | ⚠️ Should say "GPU slightly *increases* foreground p99". |
| 11 | Mixed: "at least 99.89% of offered operations completed in-window" | Minimum is 99.815% (balanced 10k, GPU, rep 0) | ⚠️ Minor; correct to 99.8%. |
| 12 | Mixed: pending compaction bytes zero; all eligible GPU batches on GPU | 0/122 trials with pending bytes; 4,468/4,468 GPU output batches on GPU, 0 CPU fallback | ✅ |
| 13 | 18/18, 6/6, 66/66 trials valid; full payload + native checksum verification passed; 0 failed requests; 0 payload mismatches | Confirmed. Note: one *valid* native confirmation trial completed only 94.8% of requests in-window (transient 496 ms stall). | ✅ |
| 14 | Write-heavy 5,556/s excluded: "GPU queue p99 467 ms in screening" | Correct, but the same screening trial had **native at 13.7 ms**, and the exclusion rested on n = 1. **Re-run 2026-10-06 with 6 randomized pairs: no stall in either backend** (queue p99 14.3–19.8 ms GPU, 13.6–19.0 ms CPU; ≥ 99.91% in-window). | ❌ Exclusion not supported; the point is now reported (RESULTS §3.5). |
| 15 | Read-dominant capacity 20k clean, 25k overloaded (91% in-window), 30k at 70–72% | 25k: 91.2% GPU / 91.4% CPU; 30k: 71.6% / 69.6% | ✅ Capacity is **identical** for both backends. |
| 16 | Python tests 17/17 | Re-run locally: 17/17 pass | ✅ |
| 17 | CTest 9/9, compute-sanitizer 6 passed, "871 files audited" | No logs archived. **Re-run on Zyra (2026-10-06): `blocks.transforms` fails intermittently at commit `792cf59`** (16/25 runs of `lsm_block_tests`). `RejectsCorruptionMalformedLz4AndBudgets` hits a CUDA "illegal memory access" on malformed LZ4 *input* in the `cuda_batched`/`cuda_hybrid` decompression path, which poisons the context for later checks. The experiments never use this path (`local_transform.batches = 0`; only output compression runs). | ❌ "9/9 passed" was not reproducible. A robustness bug on malformed input must be fixed or disclosed before claiming malformed-input safety. |

## 3. Methodological issues the summary does not disclose

1. **Toy-scale database.** 16,000 records × 1 KiB ≈ 16 MiB logical, ~9 MiB of
   live SST, with a 64 MiB block cache and a 256 KiB memtable. The whole database
   fits in cache and page cache, so storage I/O is effectively absent. Every
   compaction job is an ~9 MiB L0→L1 merge. This is the **single largest
   threat to external validity** and will be the first reviewer objection at a
   top venue.
2. **Short runs.** 2 s warm-up and 8 s measurement windows, with 10–53
   compaction jobs per trial. No steady-state LSM shape is reached (4 levels
   configured, but the data occupies L0/L1 only).
3. **Non-randomized order in the stress sweep.** The GPU ran first in 8/9
   stress pairs, and the native backend ran first in all 3 confirmation pairs.
   The randomized mixed campaign (16 GPU-first, 17 CPU-first) shows the effect
   in both orders (median-job ratio 0.884 vs 0.830), so order does **not**
   explain the effect. It should still be stated.
4. **Averaging tail percentiles.** The summary's foreground tables are means
   of three p99 values; a single 300–500 ms stall dominates such a mean. Use
   per-trial values and paired medians, as in the new
   [`tables/per-point.md`](tables/per-point.md).
5. **No unpinned-NUMA control.** Zyra is a 4-socket Xeon Gold 5220 host; the
   GPU is attached to NUMA node 0, and processes were not pinned. *Tested
   2026-10-06:* pinning `lsm_db` to node 0 cut whole-process CPU by 8–16% for
   **both** backends (6/6 blocks), so the unpinned baseline carried a large
   placement penalty unrelated to the GPU.
6. **Energy not reported.** NVML shows GPU board power of **74 W in GPU mode
   vs 16 W in CPU mode** (+58 W). That is about +464 J per 8 s window, or
   ~1.6 kJ per GiB compacted. CPU RAPL counters were access-denied, so system
   energy is unknown. However, since host CPU did not decrease, the offload is
   energy-negative in this configuration.

## 4. New findings from the re-analysis

* **The effect is bimodal, not uniform** ([fig5](figures/fig5-job-duration-raster.png)).
  In 40 of 45 GPU trials, the steady-state median job time (excluding each
  trial's first 5 warm-up jobs) fell by **19.9%** [95% CI 19.1–20.7%]
  (40/40 pairs). In the other **5 trials (11%)**, all in
  the mixed campaign, 81–100% of steady-state jobs ran at CPU speed. In those
  trials, GPU kernel time per block (~77.5 µs) and GPU power were unchanged,
  but compaction-thread CPU per GiB rose from ~3.7 to ~5.0 CPU-s. The slowdown
  is therefore **host-side**. In the 6-block ablation (RESULTS §3.4), the regime appeared in 3/6 balanced
  trials with default sync and no pinning, and 0–1/6 under each other
  condition. The counts are too small to attribute it to one factor.
* **Offloaded CPU reappears elsewhere.** The GPU path runs on ~107 `std::async`
  helper threads that RocksDB's job counter misses. With every compaction thread
  counted, CPU is unchanged (3.90 vs 3.94 CPU-s; RESULTS §3.2). Spin-waiting is
  confirmed: blocking sync lowers GPU-path process CPU by 4.3% (6/6) and has no
  effect on the CPU backend.
* **GPU kernels are ~18% of compaction wall time.** The remaining ~82% is
  CPU-side merge, staging copies, and RocksDB bookkeeping. This bounds any
  further benefit from faster kernels (Amdahl).
* **No foreground or capacity benefit.** Write/read p50 are unchanged
  (median ratios 1.004 and 0.997). Typical p99 is ~4% worse with the GPU, and
  saturation capacity is the same for both backends.

## 5. What to fix in the repository

* Commit `configs/cuda-realistic-*.json`.
* Rename `elapsed_micros_p99` in summaries, or compute it with interpolation
  and report n. The current nearest-rank value on ≤53 samples is the maximum.
* Emit a timestamped engine snapshot exactly at the warm-up/measurement
  boundaries and at window end. Current 1 Hz snapshots are offset ~0.25 s and
  stop before the window ends. Never divide lifetime totals by the window
  length.
* Randomize pair order in every campaign. The tooling already does so for the
  mixed campaign.
* Archive test, sanitizer, and audit logs alongside each result bundle.
