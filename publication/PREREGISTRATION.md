# Preregistration: GPU compaction offload campaigns

Written before the full campaigns run. Changes after data collection are
listed at the end with their reasons.

## Systems compared

| Name | Mode | Description |
|---|---|---|
| Native | `native_cpu` | RocksDB 11.8, hardware CRC32C, no offload |
| Compression only | `gpu_output` | GPU LZ4 + CRC32C of output blocks (LZ4 configurations only) |
| Merge | `cuda_merge` + `LSM_DISABLE_GPU_BUILD=1` | GPU merge, CPU block building |
| Full offload | `cuda_merge` | GPU merge and block building |

All runs use `LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking
LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024 LSM_MERGE_READ_THREADS=4
LSM_GPU_MERGE_STREAMS=2`. Every design is run at 2 and 4 background jobs
(more for larger core budgets). Each is reported at its best setting, and
all settings are kept in the data.

## Hypotheses

- **H1 (isolated compaction):** full offload reduces CPU time per GiB
  compacted by at least 2× against native, for every value size and codec.
- **H2 (compaction-bound database):** with batched writes on 4 cores, full
  offload raises ingest-and-compact throughput by at least 1.3× and lowers
  p99 write latency, against native at its best background-job setting.
- **H3 (equal cost):** native RocksDB needs at least twice as many cores as
  full offload to match full offload's ingest-and-compact throughput on
  batched writes.
- **H4 (no harm):** on read-heavy workloads (YCSB B), full offload's
  throughput and p99 read latency are within 5% of native's.
- **H5 (boundary):** on the SATA SSD, gains shrink once compaction becomes
  I/O-bound. We report where.
- **H6 (read trade-off):** at equal CPU, aggressive compaction settings with
  full offload give lower p99 read latency than native's default settings.
- **H7 (sharing):** one GPU serving 4 database instances keeps at least 80% of
  the single-instance per-instance gain.

## Primary metrics

- **Ingest-and-compact throughput:** operations divided by the time from the
  first operation to the end of catch-up compaction.
- **CPU time per operation:** whole process, over the same interval.
- **Tail latency:** p99 write latency, and p99 read latency where reads
  exist, during the measurement window.
- **Energy per operation:** RAPL package + DRAM plus GPU board power over the
  same interval, absolute and minus idle power.
- **Isolated compaction:** throughput in MiB of input per second and CPU
  seconds per GiB of input.

Secondary metrics: write amplification, stall fraction, compaction CPU
share, GPU fallbacks.

## Design and analysis

- **Repetitions:** in randomized order within blocks; the grids are in
  `configs/system/`.

  | Campaign | Grid | Repetitions | Seed |
  |---|---|---|---|
  | Main | `main.json` | 5 | 101 |
  | Equal cost | `equal-cost.json` (4-core points come from Main) | 5 | 102 |
  | YCSB | `ycsb.json` | 3 | 103 |
  | Contention | `contention.json` (4 cores without background load come from Main) | 3 | 104 |
  | Read trade-off | `read-tradeoff.json` | 3 | 105 |
  | Shared GPU | `shared-gpu.json` | 3 | 106 |
  | SSD scale | `ssd-scale.json` (200 M keys) | 3 | 107 |
  | Isolated compaction | `compaction_sweep.sh` | 3 warm runs | n/a |
- **Pilot:** a short run of every driver feature (merge-only design,
  background load, shared GPU, multi-node core budgets, YCSB mixes) checked
  for failures before the campaigns; pilot data is discarded.
- **Statistics:** medians with 95% percentile-bootstrap confidence
  intervals (10,000 resamples). Ratios against native are computed per
  repetition block (paired) and bootstrapped. A hypothesis holds if the
  95% CI of the paired ratio lies entirely beyond the stated threshold.
- **Exclusions:** a trial is excluded only if:
  - it fails verification, exits non-zero, or the GPU path falls back for
    more than 1% of jobs (all reported); or
  - other users' processes used more than 5% of host CPU during it (recorded
    per trial); such trials are re-run at the end of the campaign.
- **Fixed settings:** 50 M keys and 128-byte values on /dev/shm; 200 M keys
  on the SSD. Each run has 30 s of warm-up, then 120 s of measurement
  (/dev/shm) or 1,800 s (SSD scale runs).

## Comparison with earlier designs (added before these runs)

Earlier GPU and FPGA compaction systems run on other stores (LevelDB, X-Engine,
key-value-separated stores), so their published speedups are not comparable
with ours. We rebuild each design inside our system. Each rebuild uses our GPU
code for the steps that design offloads, and RocksDB's own CPU code for every
step it does not. Every rebuild passes the same exact differential check as
native RocksDB.

| Name | Stands in for | Settings | GPU | CPU (RocksDB code) |
|---|---|---|---|---|
| Sort/merge offload (`sort_merge`) | gLSM, gParaKV | `cuda_merge` + `LSM_DISABLE_GPU_BUILD=1 LSM_GPU_CPU_DECOMPRESS=1 LSM_DISABLE_GPU_OUTPUT=1` | unpack, sort, merge, drop hidden versions | checksum check and decompression of input; `CompactionIterator`; block building, compression and checksums |
| Pipeline offload without compression (`pipeline_no_compression`) | LUDA, MSST'24, FPGA (FAST'20) | `cuda_merge` + `LSM_GPU_CPU_DECOMPRESS=1 LSM_GPU_CPU_COMPRESS=1` | unpack, sort, merge, deletions, block building, filter hashes | checksum check and decompression of input; compression and checksum of output |

Fairness rules:

- **Checksums follow compression.** RocksDB checksums a block after
  compressing it, so wherever compression stays on the CPU, its checksum does
  too. Compactions whose input has no compressed block keep input checksums on
  the GPU. With no compression, the pipeline rebuild is therefore identical to
  full offload.
- **Generous to the earlier designs.**
  - Input decompression in the rebuilds runs on the 4 parallel read threads.
  - LUDA's CPU-assisted sort is replaced by our GPU sort.
- **Not reproduced:**
  - MSST'24's direct SSD-to-GPU transfers, which cannot help on /dev/shm.
  - gLSM/gParaKV's separate value storage, which our GPU path does not
    support. This is covered by running gParaKV's own code (H12).

Hypotheses:

- **H8 (no compression):** with no compression, the pipeline rebuild's
  isolated compaction time is within 15% of full offload's.
- **H9 (isolated, compressed):** with LZ4 and with ZSTD, full offload uses less
  CPU per GiB than the pipeline rebuild, which uses less than the sort/merge
  rebuild. The gap between full offload and the pipeline rebuild is larger
  for ZSTD than for LZ4.
- **H10 (database, compressed):** with batched writes on 4 cores and LZ4, full
  offload's paired throughput ratio against native exceeds every rebuild's.
  The pipeline rebuild's gain over native (ratio − 1) is less than half of
  full offload's.
- **H11 (equal cost):** on 8 cores with batched LZ4 writes, the pipeline
  rebuild's throughput is below native's on 32 cores, while full offload's is
  not.
- **H12 (gParaKV, descriptive):** all systems run under gParaKV's own YCSB
  driver, on the same 4 cores and GPU with data on tmpfs (gParaKV's
  persistent-memory pool is emulated in DRAM). The systems are gParaKV,
  native RocksDB, RocksDB with BlobDB (its own key/value separation) and full
  offload. Block compression is off, as in gParaKV's driver; native and full
  offload are also run with LZ4. Each run loads the records with one client,
  then runs YCSB A (Zipfian), with 128-byte and 1-KiB values. We report load
  and YCSB A throughput and CPU time, with no directional hypothesis.

Runs:

| Campaign | Grid / script | Repetitions | Seed |
|---|---|---|---|
| Isolated, earlier designs | `prior_compaction_sweep.sh` (same workloads as the isolated sweep) | 3 warm runs | n/a |
| Database, LZ4 | `prior-lz4.json` (golden `golden-50m`) | 5 | 111 |
| Database, ZSTD | `prior-zstd.json` (golden built with `--compression zstd`) | 3 | 112 |
| Database, no compression | `prior-none.json` (golden built with `--compression none`) | 3 | 113 |
| Equal cost, earlier designs | `prior-equal-cost.json` | 5 | 114 |
| gParaKV head-to-head | `gparakv/setup.sh`, `gparakv/run.sh` | 3 | fixed per repetition |

Analysis, statistics and exclusions are as above. Native and full offload are
re-run inside these campaigns so that every ratio is paired within a block.

## Changes after data collection

- **Shared GPU, first attempt (discarded, kept as `shared-gpu-attempt1`):** all
  4- and 8-instance trials failed. Native instances were killed by the kernel
  because every instance's memory was bound to NUMA node 0 (about 48 GB). GPU
  instances failed because a failed GPU allocation left a buffer reporting its
  old size, which led to an illegal memory access, and because the output path
  failed the compaction on GPU out-of-memory instead of falling back to the
  CPU. Fixes: multi-instance trials use `numactl --preferred` (prefer the
  GPU's node, allow spilling); allocation failures reset the buffer; GPU
  out-of-memory falls back to CPU compression and is counted. The fixes touch
  only failure paths. A full re-run is pending; single-instance results were
  unaffected.
