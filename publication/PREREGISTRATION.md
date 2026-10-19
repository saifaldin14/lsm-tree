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

## Changes after data collection

None yet.
