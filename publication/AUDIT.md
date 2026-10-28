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
| 13 | **Wrong codec in the output-only design.** The output-only GPU path compressed blocks with LZ4 even when the column family was configured for no compression or ZSTD. | Its "128 B, uncompressed" isolated result (136 MiB/s, 7.9 CPU-s/GiB) measured LZ4 compression, not the configured setting. Stored data was still correct. | The output-only path now applies only to LZ4 column families; the isolated sweep was re-run. |
| 14 | **Shared-GPU failures.** With 4 and 8 instances, native instances were killed for lack of memory (every instance bound to NUMA node 0), and GPU instances hit an illegal memory access: a failed GPU allocation left the buffer reporting its old capacity. GPU out-of-memory on the output path also failed the compaction instead of falling back. | The first shared-GPU campaign is unusable (kept as `shared-gpu-attempt1`); single-instance campaigns were unaffected. | Multi-instance trials prefer node 0 but may spill (`numactl --preferred`); buffers forget their capacity before reallocating; GPU errors on the output path fall back to CPU compression and are counted. Re-run pending. |
| 15 | **Engine patch not idempotent.** One patch step inserted a block that later steps edited, so re-running CMake inserted it again; another anchored on text that only an already-patched tree contains. | A fresh checkout failed to configure, and an old local tree failed to compile. Zyra's long-lived tree was correct, so no measurement was affected. | The inserted block now carries its final text and the old step is a migration; a fresh configure followed by a second configure leaves the patched sources byte-identical. |

## Problems found by the methodology v2 audit

New instrumentation (CPU by thread role, process and device I/O, RocksDB PerfContext on
sampled writes, per-second throttle state) and `perf` profiles on Zyra, with 50 M keys and
batched writes. Pilot numbers below are from 40–60 s runs; the v2 campaigns re-measure
everything (PREREGISTRATION.md, "Methodology v2").

| # | Problem | Evidence | Fix |
|---|---|---|---|
| 16 | **Native capped by subcompactions at high core counts.** RocksDB runs one L0→L1 compaction at a time, split into at most `max_subcompactions` threads (4 in v1), and throttles writes while it runs. | Native on 32 cores: 0.84 M writes/s using 7.8 cores, with 17 of 40 seconds below 25% of the mean rate (about 4k writes/s while one compaction used exactly 4 cores). With subcompactions = 32: 1.45 M writes/s, 11.7 cores, 2 throttled seconds. Full offload is unaffected (1.73 M at 16 cores either way). | Subcompactions are part of the v2 tuning search for every design. The v1 equal-cost comparison overstates our advantage at 16–32 native cores and is superseded by v2. |
| 17 | **Low CPU use with 2 background jobs.** 2 jobs give RocksDB one flush and one compaction thread. | Native on 4 cores used 2.0 cores: compaction 1.0, flush 0.3, and 4 clients only 0.7 (throttled). Full offload on 16 cores was flush-bound at 1.0 core with 2 jobs and used 1.8 flush cores with 16 jobs. | v1's main campaign reported both 2 and 4 jobs; v2 tunes background jobs with the core count. |
| 18 | **Clients mostly wait in RocksDB's write queue.** | Sampled batched writes (100 keys) on 16–32 cores spent 68–75% of their wall time in `write_thread_wait` (e.g. 952 of 1,398 µs) and about 160 µs inserting into the memtable; 16–32 clients used only 2.3–4.4 cores. | A RocksDB property (one leader per write group). Client threads 1× and 2× the cores and pipelined writes are in the tuning search. |
| 19 | **L0 file count in the timeline was always 0.** `rocksdb.num-files-at-level0` is a string property and was read as an integer. | Timeline column only; stall statistics were correct. | Read as a string; the timeline also records RocksDB's delayed write rate and write-stopped flag. |
| 20 | **Benchmark overhead (checked, not a problem).** | Value generation and the client loop were 3–6% of CPU samples; flush CPU scaled with throughput (about 1 core per 0.95 M writes/s) rather than capping it. | None needed. |
| 21 | **Replication sent to backups one after another.** | With 2 backups every replicated write paid two round trips. | The RPC client now sends to every backup before awaiting replies: replication time fell 37% (batched) and 15% (single writes) in R6. Single-write compact-once throughput did not change (about 50k writes/s), because physical replication ships the WAL inside RocksDB's serial write path, so only one round trip is ever in flight. |
| 22 | **Leaked monitors.** Interrupted campaigns left `perf stat` and `iostat` running for days. | Four orphaned processes on Zyra (stopped). | The campaign driver stops its monitors in a `finally` block. |
| 23 | **Fixed-size runs and memory-only storage.** `lsm_compaction_bench` and YCSB ran a fixed amount of work; every whole-database run used `/dev/shm`. | On `/dev/shm` a 32-core native run writes about 540 MiB/s through system calls, more than a SATA SSD sustains, so real disks may move the bottleneck. | `lsm_compaction_bench --duration`, YCSB `maxexecutiontime`, and SATA SSD runs with direct I/O (V4). |

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

- Shared-GPU campaign: re-run with the fixes in item 14.
- SATA SSD scale campaign and the `db_bench` cross-check: not yet run.
- Archive raw data in a DOI-backed store before submission.
