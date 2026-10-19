# Methodology audit

Every problem we found in our own measurements, what it did to the numbers,
and how it is handled now. The first group affected the earlier,
compression-only evaluation (git commit `a97ae43`); the second was found
while building and measuring the full GPU offload.

## Problems in the earlier evaluation

| # | Problem | Effect | Fix |
|---|---|---|---|
| 1 | **Hidden setting.** The published GPU results required `LSM_DISABLE_GPU_INPUT=1`, which no config or result recorded. | Without it, the same binary made GPU jobs 35–45% *slower* than the CPU. | The runner records all `LSM_*` variables in `build.json`; the setting is documented and used in every run. |
| 2 | **Hidden CPU.** RocksDB's per-job `cpu_micros` misses the GPU path's helper threads. | Reported a 32–37% CPU saving; whole-process CPU actually rose 3%. | All CPU figures are whole-process (`getrusage`) or whole-trial. |
| 3 | **Software CRC32C in the baseline.** RocksDB was built `PORTABLE=ON`, which selects software CRC32C. | Native compaction was about 16% slower than necessary (82 vs 95 MiB/s), which flattered the GPU. | `LSM_ROCKSDB_PORTABLE=0` builds with `-march=native`; all current results use it, with identical output. |
| 4 | **Asymmetric verification.** The harness set `verify_compression=true`, so the CPU path decompressed and compared every compressed block; the GPU path did not. | Extra CPU work on the native side only. | `engine.verify_output` (default true for compatibility); comparisons use RocksDB's defaults on both sides, and the benchmarks never set it. |
| 5 | **Undisclosed binary change.** Two `lsm_db` binaries were used across campaigns; only instrumentation differed. | None on compaction; disclosed. | Builds are recorded per run. |

## Problems found during the full-offload work

| # | Problem | Effect | Fix |
|---|---|---|---|
| 6 | **Harness overhead.** With the database on 4 cores, request handling in the gRPC harness used the CPU. | Every backend reached about 3,800 ops/s, so compaction never mattered. | In-process benchmarks (`lsm_compaction_bench`, `lsm_system_bench`), as db_bench-style studies use. |
| 7 | **Oversubscribed writers.** 8 writer threads on 4 cores spent about 26% of CPU spinning in RocksDB's write queue. | Hid compaction savings and depressed all throughput. | One client thread per core; adaptive yield documented. Native rose from 154k to 181k ops/s. |
| 8 | **Configuration confound.** An early +14% GPU gain came from using 2 background jobs instead of 4, which helped native RocksDB just as much. | A false attribution to the GPU. | Each design is swept over background jobs and reported at its best setting. |
| 9 | **Deferred compaction.** With short windows, native RocksDB postpones compaction (68–114 s of backlog after 2.5 minutes) without ever stalling. | Window throughput overstates native by 34–74% and made GPU offload look useless under CPU contention. | The ingest-and-compact metric counts the time to finish all resulting compaction. |
| 10 | **Filters unchecked.** The version digest reads through iterators, which never consult Bloom filters. | A wrong GPU filter hash would have gone unnoticed. | Point-lookup digest over up to 200,000 keys; all runs match. |
| 11 | **Scratch data deleted.** Zyra's `logind` removed `/dev/shm` files when the last session ended. | Runs failed part-way. | `loginctl enable-linger`; documented in the runbook. |
| 12 | **Stale error flag.** A GPU eligibility flag shared the parser's error word, so jobs with deletes reported corruption on fallback. | Caught by the differential matrix before any measurement. | Separate flag; the delete cases match native. |

## Data and provenance

- The raw results behind every current figure and table are in
  [data/](data/), and [make_figures.py](make_figures.py) regenerates the
  figures and [data/summary.md](data/summary.md) from them, refusing any run
  that failed its correctness check.
- Each result records its full configuration, CPU placement, `LSM_*`
  environment and RocksDB engine fingerprint.
- Raw bundles of the earlier evaluation (about 3.3 GB) are not in the
  repository. Their summary tables, figures and analysis code are in git
  history at commit `a97ae43`.

## Open items

- Whole-database results are single runs; repeat 5 times in randomized order.
- Energy must be integrated through catch-up compaction (see
  [RESULTS.md](RESULTS.md) §3.4).
- Native single writes with 4 background jobs, and GPU compression only with 4
  background jobs, are not yet measured.
- Very large jobs can fall back to the CPU when nvCOMP rejects the batch.
- Archive raw data in a DOI-backed store before submission.
