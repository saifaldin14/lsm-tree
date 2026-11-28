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

### Earlier designs with tuned settings (added 2026-11-10, before these runs)

The whole-database comparison above ran every design with 1 subcompaction and 2 or 4
background jobs. AUDIT item 16 shows that this caps native RocksDB. It is re-run with tuned
settings, for the strongest earlier design only: the pipeline rebuild, which was best in
every compressed workload.

- **Arms, 4 cores, batched writes:**
  - Native and full offload use their registered 4-core settings from `v3-headline.json`.
  - The pipeline rebuild is run twice, with native's settings and with full offload's.
    The better of the two (higher median throughput) is reported, giving the earlier
    design the benefit of the doubt.
- **Run:** 50 M records, 120 s warm-up plus 300 s, verification on, memory preferred on
  node 0, 3 blocks, schedule seed 7.
  - LZ4 uses golden `golden-50m`.
  - ZSTD uses a golden built with `--compression zstd`.
- **Reported:** throughput, ingest-plus-compaction throughput and CPU per write, each with
  95% bootstrap CIs, for full offload against the better pipeline arm and against native.
- **Hypothesis H8b:** with tuned settings, full offload's throughput and its CPU per write
  are both better than the better pipeline arm's, with the 95% CI excluding 1, for LZ4
  and for ZSTD. Whatever the outcome, these runs replace the untuned numbers in the README.
- **Outcome, LZ4 (2026-11-10, `data/prior-tuned/lz4`, 3 blocks, all verified): supported.**
  Full offload 756k writes/s and 4.28 µs CPU per write; better pipeline arm (full
  offload's flags) 592k and 6.69 µs: 1.28× throughput (CI 1.27 to 1.28), 1.56× less CPU
  (CI 1.55 to 1.57). Pipeline with native's flags 523k; native 498k. ZSTD paused for the
  MPS quick pass and resumes after it.

### Shared GPU with MPS, tuned (added 2026-11-10, before these runs)

The cost of a dedicated GPU is not repaid at 4 to 8 cores. One database uses a small share
of the GPU, so the GPU's cost per database falls if several share it. The earlier
shared-GPU campaign (untuned native, no MPS) kept 95% of ours' single-database throughput
with 2 databases and 69% with 4, and its total GPU throughput stopped growing at 4. Each
database process has its own CUDA context, so the GPU time-slices between them. CUDA MPS
runs all processes through one server so their kernels can overlap.

- **Run** (`shared_mps.sh`, zyra, after the tuned earlier-design campaign ends):
  - CUDA MPS daemon started as the user for the GPU arms only, and stopped afterwards.
  - Databases per GPU N in {1, 2, 4}, at 4 and at 8 cores per database. Each database
    has its own cores and its own copy of `golden-50m`.
  - Native and full offload use their registered `v3-headline.json` settings for that
    core count, with the same N.
  - 30 s warm-up plus 120 s, as the registered shared-GPU campaign; verification on;
    2 blocks, schedule seed 108.
- **Reported:** per-database ingest-plus-compaction throughput, the share of ours'
  N=1 throughput each database keeps, GPU utilisation, CPU fallbacks, and cost per
  write with the GPU priced at `prices.json` divided by N.
- **Parity thresholds**, computed before the runs by `cost_model.py` from the tuned runs
  and `prices.json`: ours' per-database cost matches native's when each database keeps
  at least 92% (4 cores, N=4), 96% (8 cores, N=2) or 81% (8 cores, N=4) of its N=1
  throughput.
- **Hypothesis H7b:** at 8 cores with N=4, each database keeps at least 81%. No other
  outcome is predicted; every N and core count is reported.
- **Also run, after MPS is stopped:** native RocksDB on 32 cores with its registered
  tuned settings and the X3 protocol (120 s plus 300 s, 3 blocks, seed 109), replacing
  the 60 s tuning trial in the cost figure.
- **Order changed before any data (10:58):** to report sooner, a quick pass runs first: one
  block (seed 108) of full offload at 8 cores with N in {1, 2, 4}, at 4 cores with N in
  {1, 4}, and native at N=1 for 4 and 8 cores. Then the 32-core run, then the full grid
  above. The quick pass is reported as one block; the full grid replaces it when done.
  The tuned earlier-design campaign is paused (it resumes by trial) and runs last.
- **Outcome of the quick pass (2026-11-10, `data/shared-mps/mps-quick`, 1 block, all
  verified, 24 MPS clients logged): H7b not supported.** At 8 cores with N=4 each database
  kept 62% (threshold 81%), with 13 to 139 CPU fallbacks per database; at 4 cores with N=4,
  71% (threshold 92%). At 8 cores with N=2 each kept 94% (threshold 96%): $0.242 per
  billion against $0.237 for the 5-minute RocksDB run, and $0.248 for RocksDB measured in
  the same campaign.
LUDA's code is public (github.com/xpsair/luda, commit 89792ac). It is LevelDB 1.22 with
GPU flushes and compactions, and no block compression.

- **Build (`luda/setup.sh`).** moderngpu is pinned to 01aabbe (2019-09-12, before LUDA's
  code). Two changes were needed to LUDA's source to make it build and run on this machine,
  and nothing else is changed:
  - LUDA's one `mgpu::merge` call passes moderngpu's sm_35 launch shape (128 x 11). The
    default for newer GPUs needs 61,440 bytes of shared memory for LUDA's 32-byte
    record, over the 48 KiB limit, so it does not link.
  - `thread_write_file` gets the `return nullptr;` it is missing. GCC 13 treats the
    missing return as unreachable and crashes the first compaction.
- **Workload limits found before any measurement.** LUDA keeps each key in a fixed
  32-byte buffer and sizes its buffers for 1 KiB values in 15 MiB tables, as its own test
  does. YCSB's keys (up to 23 bytes plus 8 bytes of LevelDB metadata) overflow it.
  So LUDA does not run under gParaKV's YCSB driver without a further change, and we do
  not make one. The comparison uses `db_bench`, which LUDA builds, with its 16-byte keys
  and 1 KiB values.
- **Systems,** each through its own `db_bench`, on the same 4 cores and GPU with data on
  tmpfs:
  - LevelDB 1.22: the stock release LUDA forked (github.com/google/leveldb, tag 1.22). LUDA's
    own switch for turning the GPU off (`GPU_ACCELERATE` 0) deadlocks in the first
    compaction, so it is not used. This was found in a 500k-key trial before any measurement.
  - LUDA.
  - Native RocksDB (`lsm_db_bench`, `LSM_DB_BENCH_MODE=native_cpu`).
  - Full offload (`lsm_db_bench`, `LSM_DB_BENCH_MODE=cuda_merge`).
- **Settings.** No compression anywhere: LUDA has none, and LevelDB was built without
  snappy. The LevelDB-family systems use LUDA's table options (15 MiB memtable and files,
  10-bit Bloom filter). RocksDB uses the same memtable and file size and Bloom filter,
  with 4 background jobs and 4 subcompactions.
- **Measurement.** `fillrandom` of 10 M keys from one thread, then `readrandom` of 1 M
  keys. We report fill throughput and CPU time from `/usr/bin/time`. Correctness check:
  LevelDB's `db_bench` uses a fixed seed, so LUDA must find exactly as many keys as
  LevelDB in `readrandom`.
- **Hypothesis.** None; this is descriptive, as H12. The two code bases differ (LevelDB
  against RocksDB), so the comparisons that mean something are LUDA against LevelDB and
  ours against RocksDB, each on the same harness.
- **Repetitions.** 3, in shuffled order.
- **Outcome (written after the runs; data in `data/luda`).** All 12 runs passed the
  correctness check: LevelDB and LUDA each found 1,000,000 of 1,000,000 keys, and native
  and full offload each found 632,048. Medians: LevelDB 44.6k writes/s and 278 CPU-s,
  LUDA 116k and 183, native RocksDB 186k and 231, full offload 182k and 239. LUDA against
  LevelDB: 2.6x the writes, 1.5x less CPU. Full offload against native: 0.98x and 1.03x
  more CPU, consistent with H8 (no advantage without compression).

## Remote compaction and replication over RDMA (added before these runs)

Every result above comes from one machine. These experiments move compaction off the
database node and share it across replicas, over 100 Gb/s RoCE (ConnectX-6 Dx).

Systems (all verified against native RocksDB as above):

- **Remote merge (`remote_merge`):**
  - The database node keeps RocksDB's `CompactionJob`, file cuts and installation.
  - It sends each merge request and its input blocks to `lsm_compaction_server`, which
    runs `cuda_merge` (GPU) or `cpu_merge` (CPU) and returns the finished blocks.
  - Transports: RDMA (push by default, pull for comparison) or TCP.
  - No shared filesystem is needed.
- **Physical replication (compact once):** the primary mirrors every file write to the
  backups, which never compact.
- **Logical replication (per-replica compaction):** every write batch goes to the
  backups, which run their own RocksDB and compact it themselves.

Related work these experiments compare against (checked against the papers):

- **Shipping compaction results to replicas:**
  - Tebis (EuroSys'22): the primary compacts and ships its index to backups over RDMA.
  - RubbleDB (ATC'23): compacts once and ships SSTs to replicas over NVMe-oF.
- **Offloading compaction to other nodes:**
  - Nova-LSM (SIGMOD'21): offloads compaction to storage nodes over RDMA.
  - dLSM (ICDE'23): an LSM-tree on RDMA-disaggregated memory.
  - CaaS-LSM (SIGMOD'24): remote CPU compaction as a service for RocksDB.
- None uses a GPU, and we found no shared GPU compaction service.
- Our remote-CPU and compact-once-on-CPU configurations stand in for these designs.

Hypotheses:

- **H13:** remote GPU compaction over RDMA keeps at least 80% of the local GPU
  throughput gain (batched writes, LZ4, 4 cores on the database node).
- **H14:** for the same remote GPU compaction:
  - RDMA gives higher throughput than TCP;
  - RDMA uses less database-node CPU per operation.
- **H15:** one GPU server serves 4 database instances at no less than 80% of the
  single-instance throughput gain each.
- **H16:** with 2 backups, compact-once replication on the GPU server uses at most half
  the total cluster CPU per write of per-replica compaction.
- **H17:** at equal hardware cost, a remote GPU compaction server gives higher database
  throughput than a remote CPU compaction server.

Measurements:

| ID | Experiment | Repetitions |
|---|---|---|
| R1 | Transport: our RDMA (push, pull) and TCP vs `ib_write_bw`/`ib_read_bw`, 64 KiB-512 MiB | 3 |
| R2 | Isolated remote compaction, same workloads as the isolated sweep: local native, local GPU, remote CPU and remote GPU over RDMA and TCP | 3 warm runs |
| R3 | Whole database on the database node, 4 and 8 cores, LZ4 and ZSTD: native, remote GPU over RDMA, remote CPU over RDMA, remote GPU over TCP | 5 |
| R4 | 1, 2, 4, 8 database instances sharing one GPU server | 3 |
| R5 | Network capped at 10/25/50/100 Gb/s | 3 |
| R6 | Replication, 2 backups: per-replica compaction, compact once on CPU, compact once on GPU | 5 |

Metrics include CPU and energy on every node involved (database node, compaction server
and backups), and bytes on the network.

Hardware status when this was written:

- No host had both a usable GPU and RDMA: the A6000 host has no RDMA card, and the
  RDMA-capable GPU host has no GPU driver installed.
- Runs that need a GPU over RDMA wait for that hardware. Everything else (transport,
  remote CPU, and the CPU replication designs) runs now and is labelled as such.

### Outcomes so far (CPU compaction server)

- **H13, H15, H17:** not tested. They need a GPU compaction server reachable over RDMA.
- **H14 (RDMA against TCP):** supported with a CPU server. Over RDMA, the database node
  uses about half the CPU per GiB that TCP needs, and compaction is 10–25% faster.
- **H16 (cluster CPU):** tested only with compact-once on the primary's CPU, because the
  compact-once-on-GPU configuration needs GPU hardware.
  - Supported for batched writes: 3.4× less cluster CPU per write.
  - Not supported for single writes: 1.8× less, short of the stated 2×.

### Outcomes with a GPU server through a relay (r7; written after the runs)

Still no host has both a GPU and RDMA, so r7 (`rdma_campaign.py`) reaches the GPU host
through a relay: database node → RDMA → relay → TCP over 1 GbE (the only link to the GPU
host) → `cuda_merge` server. This design was not registered in advance; it is reported as
an exploratory measurement.

- **H14 (RDMA against TCP), database-node CPU, now with a GPU server:** supported. Over
  RDMA the database node uses 1.28–1.37 CPU-s per GiB, against 4.46–4.71 when the same jobs
  go to the GPU host over TCP (3.4–3.5× less). The TCP path runs over 1 GbE, not the
  100 GbE network; over 100 GbE with a CPU server (R2), TCP also needed about twice RDMA's
  database-node CPU.
- **H14, throughput:** not testable; the two paths use different links.
- **H13, H15, H17:** still not tested. Compaction time on a GPU+RDMA host is estimated
  from measured hops (1.26–1.54 s per compaction, against 5.28–9.26 s for native on the
  database node), but an estimate is not a test.

## Methodology v2 (added before the v2 campaigns)

All campaigns above are the **v1 methodology** and stay as they are. A methodology audit
(new per-role CPU, I/O and write-path instrumentation; `perf` profiles) found four problems,
which v2 fixes:

1. **Native was under-tuned at high core counts.** With `max_subcompactions = 4`, RocksDB's
   single L0→L1 compaction can use at most 4 threads, and writes are throttled while it
   runs. Pilot (50 M keys, batched writes, 40 s): native on 32 cores went from 0.84 M to
   1.45 M writes/s with subcompactions equal to the cores; full offload was unchanged.
   The v1 equal-cost comparison therefore overstates our advantage at 16–32 native cores.
2. **The per-second L0 file count was always 0** (a string property read as an integer).
   Stall statistics were correct; only that timeline column was wrong.
3. **Fixed-size runs** in the single-compaction benchmark and in YCSB.
4. **Memory-backed storage only** (`/dev/shm`).

### Tuning (Phase 3)

- **Search space, identical for every design** (`configs/system/tune-search.json`):
  4, 8, 16 and 32 physical cores (spilling to a second NUMA node above 18); client threads
  1× and 2× the cores (adaptive yield only at 1×); subcompactions 4 and equal to the cores;
  pipelined write off and on; background jobs equal to the cores; batched writes, LZ4,
  50 M keys; 20 s warm-up and 60 s measurement.
- **Selection rule:** per design and core count, the highest measured throughput among
  settings with stall time below 5% and passing verification.
- The chosen settings are frozen in `configs/system/tuned.json` before any v2 campaign
  runs, and the whole search is published. Trials overlapping other work on the machine are
  re-run and the originals excluded.

### Run length (Phase 4)

- **Whole database:** 120 s warm-up and 600 s measurement. Steady-state throughput is the
  median of per-minute rates over the last 8 minutes; a run whose per-minute trend exceeds
  5% is reported as not steady. Catch-up time and catch-up-inclusive throughput are
  reported separately, as before.
- **Workloads:** 60 s warm-up and 300 s measurement; MixGraph: 300 s per run.
- **Single compaction:** `lsm_compaction_bench --duration 120`.
- **YCSB (both drivers):** `maxexecutiontime = 300` seconds.

### Campaigns

| ID | Campaign | Designs | Cores | Repetitions |
|---|---|---|---|---|
| V1 | Headline, batched writes, LZ4, `/dev/shm`, tuned | native, full | 4, 8, 16, 32 | 3 blocks |
| V2 | Workloads: stream, metadata, time series (`docs/workloads.md`) | native, full | 8, 16 | 3 blocks |
| V3 | MixGraph: `zippydb`, `zippydb_write` (`lsm_db_bench`) | native, full | 8, 16 | 3 |
| V4 | SATA SSD (Zyra), direct I/O, 50 M keys, tuned | native, full | 8, 16 | 3 blocks |
| V5 | Replication R6 with parallel broadcast to backups (otherwise as R6) | as R6 | as R6 | 3 |
| V6 | Remote compaction without RDMA hardware: database on NUMA node 2, compaction server on the GPU's node 0 (8 cores), loopback TCP; native, local GPU, remote GPU, remote CPU (`results/rdma/loopback_gpu.py`) | 4 | database: 4 | 3 |

Every whole-database run records CPU by thread role, device bandwidth and utilisation,
throttled seconds, and GPU fallbacks.

### Hypotheses

- **H18:** with tuned settings for both, full offload sustains at least 1.3× native
  throughput at 8 and 16 cores (V1).
- **H19:** at 32 cores, tuned native spends more seconds throttled (below 25% of its mean
  rate) than full offload, by at least 2× (V1).
- **H20:** full offload gives at least 1.2× throughput or at least 25% less CPU per
  operation on the stream and metadata workloads, and is within ±10% of native on the
  time-series workload, where compaction does little merging (V2).
- **H21:** on MixGraph, `zippydb` (85% reads) is within ±10% of native and `zippydb_write`
  gains at least 1.1× (V3).
- **H22:** on the SATA SSD the gain is smaller than on `/dev/shm`, and the device, not the
  CPU, limits native throughput (utilisation above 80%) (V4).
- **H23:** with parallel broadcast, compact-once replication with single writes is within
  10% of per-replica compaction throughput (it was 25% slower in R6) (V5).
- **H24 (stands in for H13 until GPU-over-RDMA hardware exists):** over loopback TCP, remote
  GPU compaction keeps at least 80% of the local GPU throughput gain over native, and a
  remote GPU server gives higher throughput than a remote CPU server on the same 8 cores
  (V6).

### Outcomes so far (v2)

- **V5 / H23 (replication with parallel broadcast): not supported.** 18 runs, all backups
  identical to the primary. Time spent replicating fell 35% (batched, compact once) and 10%
  (single writes), but throughput did not change: compact once with single writes ran at 46k
  writes/s against 74k for per-replica compaction (38% slower; 25% in R6). Physical
  replication ships each WAL append inside RocksDB's serial write path, so only one round
  trip is in flight whether backups are contacted in parallel or not. Cluster CPU per write
  is unchanged: 3.3× less than per-replica for batched writes, 1.7× less for single writes.

The pilots that motivated v2 (`perf` profiles, subcompaction checks, small MixGraph and
workload runs) are not results and are not reported as such. Analysis, statistics and
exclusions are as in the main plan.

## Methodology v3 (added before the v3 campaigns)

The v2 campaigns were stopped after 6 headline trials, at the authors' decision, to
remove the host-side costs that profiling of the measured compaction exposed (AUDIT items
24–28). v3 measures the resulting system. The v2 tuning choice is reused unchanged for both
designs (`configs/system/tuned.json`; it was chosen before these changes, so if anything it
favours native), and both designs run with `allow_fallocate = false` (a RocksDB option that
helps both on tmpfs).

### Changes since v2 (all verified against native RocksDB with the differential matrix)

- Built blocks written in runs with one write each, instead of block by block.
- Bloom filter bits set on the GPU from the per-file key hashes (RocksDB's sizing and
  metadata unchanged).
- Sorted runs merged on the GPU instead of fully sorted.
- Output block checksums computed by a warp instead of one thread.
- Pinned buffers grow by powers of two.
- Flush compression through the GPU output path.

### Campaigns

| ID | Campaign | Designs | Settings | Repetitions |
|---|---|---|---|---|
| X1 | Isolated compaction: 32 B, 128 B, 1 KB LZ4; 128 B ZSTD; 128 B none; 128 B LZ4 with 10% deletes | native, full | 1 subcompaction (one job, one core for native) | 1 cold + 3 warm |
| X2 | Concurrent compaction, 2.6 GiB input: 128 B LZ4 and ZSTD | native, full | 8 subcompactions on 8 cores | 1 cold + 3 warm |
| X3 | Whole database, LZ4, closed loop, 120 s warm-up + 300 s | native, full | 4, 8, 16 cores, tuned | 2 blocks |
| X4 | Whole database, LZ4 above and ZSTD at the bottom (`bottommost_compression`) | native, full | 4, 8 cores, tuned | 2 blocks |
| X5 | Open-loop tail latency, LZ4, 8 cores, 60 s warm-up + 180 s, at 430k, 600k and 770k writes/s (50%, 70% and 90% of tuned native's pilot maximum) | native, full | tuned | 2 blocks |

### Hypotheses

- **H25 (host CPU per compacted GiB, X1):** full offload uses at least 8× less than native
  for 128 B LZ4, and at least 12× less for 32 B LZ4 and for 128 B ZSTD.
- **H26 (time of one compaction, X1):** at least 7× faster than native's single-threaded
  job for 128 B LZ4, and at least 10× for 32 B LZ4 and 128 B ZSTD.
- **H27 (cores one GPU replaces, X2):** the GPU's concurrent compaction throughput equals
  that of at least 12 native cores for LZ4 and at least 15 for ZSTD (GPU GiB/s divided by
  native GiB/s per core from X1).
- **H28 (whole database, X3/X4):** at 8 cores, full offload gives at least 1.4× throughput
  and at least 1.6× less CPU per write than native, for both codec setups.
- **H29 (tail latency, X5):** at 430k and 600k writes/s, full offload's p99.9 write latency
  is at least 2.5× lower than native's.

Not claimed: a 10× whole-database gain. With compaction removed from the CPU, RocksDB's
own write path (memtable insert and write queue, about 2.3 µs per write) and flush remain,
which bounds the whole-database gain near 2×. The order-of-magnitude results are about
compaction itself (H25–H27).

### Outcomes (v3)

Every run matched native RocksDB (isolated: version and lookup digests; whole database:
online read checks and full verification). Data in `data/v3/`, numbers in
`data/v3/summary.md`.

- **H25 supported.** Host CPU per GiB of input: 8.8× less for 128 B LZ4 (10.58 against
  1.21 CPU-s/GiB), 14.2× for 32 B LZ4, 15.5× for 128 B ZSTD. Also 4.4× for 1 KB LZ4,
  4.3× with no compression and 8.9× with 10% deletes (not pre-registered).
- **H26 supported.** One compaction job: 8.0× faster for 128 B LZ4 (6.96 against
  0.87 s), 11.7× for 32 B LZ4, 10.7× for 128 B ZSTD.
- **H27 supported.** With 8 subcompactions the GPU compacted 1.42 GiB/s of LZ4 input
  (using 2.0 host cores) and 0.89 GiB/s of ZSTD input (1.3 cores): the throughput of
  15.0 and 17.7 native cores.
- **H28 supported.** At 8 cores: 1.51× throughput and 1.90× less CPU per write with LZ4;
  1.67× and 2.23× with LZ4 above and ZSTD at the bottom. At 4 and 16 cores: 1.53× and
  1.79× throughput with LZ4, 1.64× at 4 cores with ZSTD at the bottom.
- **H29 not supported.** p99.9 write latency (median of 2 runs) at 430k writes/s: 73.0 ms
  for full offload against 6.0 ms for native; at 600k: 290.5 against 44.0 ms.
  - One run at each of these rates had a p99.9 of 3.4 and 10.5 ms. The other run had a
    single 0.3–0.6 s pause of all writers, which by itself sets the p99.9.
  - During the pauses RocksDB recorded no write stall, and other users' load was below
    1% of the machine.
  - A post-hoc diagnostic (2 more runs at 600k with pinned allocations traced) had no
    pause (p99.9 12.6 and 9.4 ms), so the cause is not yet known.
  - p99 was lower at every rate (1.9 against 2.5 ms; 3.9 against 15.7; 14.7 against
    102.8). At 770k writes/s (90% of native's maximum) p99.9 was 39.3 against 172.0 ms.
- **Not pre-registered:** in the 16-core closed-loop runs, p99.9 write latency was 9.2 ms
  against 209.7 ms for native.

### Additions after the v3 outcomes (registered before these runs)

- **Deviation: more repetitions.** X3, X4 and X5 had 2 blocks each. Three more blocks of
  each are run (`*-more`, schedule seed 2) with the same build and settings. Outcomes are
  reported over all 5 blocks, and over the first 2 as registered; a hypothesis is called
  supported only if it holds in both.
- **The pause (H29): cause found and fixed.** The benchmark now records where every thread
  sleeps whenever no write completes for 150 ms. In 4 runs every warm-up had 3–4 pauses of
  0.2–0.5 s. During each, writers were blocked in page faults (`do_user_addr_fault`) and
  `mmap` (`rwsem_down_write_slowpath`) while a compaction pinned 512 MiB–1 GiB of host
  memory with `cudaHostAlloc`, which holds the process's memory-map lock. Fix: with
  `LSM_GPU_PINNED_PREALLOC_MB=1024`, each GPU merge engine is created and pins its buffers
  when the database opens. In 4 runs with the fix (430k and 600k writes/s) there was no
  pause and no pinned allocation after startup; p99.9 was 3.0–3.2 and 11.0–12.6 ms.
  - Consequence: X5 is re-run in full (5 blocks, schedule seed 3, `v3-openloop-fixed`)
    with the fix. H29 is judged on that re-run; the original X5 stays reported.
  - The 3 extra blocks of X3 and X4 also use the fixed build. The fix changes only when
    pinned memory is allocated, not compaction itself.
- **X6, workloads** (stream, metadata, time series; `docs/workloads.md`): 8 and 16 cores,
  60 s warm-up + 300 s, 3 blocks; tests H20 as stated in v2.
- **X7, MixGraph** (`zippydb`, `zippydb_write`, `lsm_db_bench`): 8 and 16 cores, 300 s,
  3 repetitions; tests H21 as stated in v2. `db_bench` keeps RocksDB's default
  `allow_fallocate` for both designs.
- **X8, SATA SSD with direct I/O:** LZ4, 8 and 16 cores, 3 blocks; tests H22 as stated in
  v2. Our path still reads compaction input with buffered reads (`docs/gpu-compaction.md`,
  Limits).

### Outcomes of the additions

All runs verified against native RocksDB. Numbers: `data/v3/summary.md`.

- **H28 (with 5 blocks): supported in both the first 2 and all 5 blocks.**
  - 8 cores: 1.51× throughput and 1.90× less CPU per write (LZ4); 1.67× and 2.21×
    (ZSTD at the bottom).
  - 4 and 16 cores: 1.56× and 1.79× (LZ4); 4 cores with ZSTD at the bottom: 1.65×.
- **H29 (X5 re-run with the fix, 5 blocks each): not supported**, because it required both
  rates.
  - p99.9 at 430k writes/s: 3.15 against 5.51 ms (1.75×, below the 2.5× threshold).
  - At 600k: 11.0 against 41.9 ms (3.8×).
  - Not pre-registered: at 770k, 26.2 against 251.7 ms (9.6×); p99 was 1.4×, 5.4× and
    10.9× lower at the three rates.
  - No run had a pause.
- **H20 (workloads): partly supported.**
  - Stream state store: 1.38× (8 cores) and 1.55× (16 cores) throughput, 1.30× and 1.53×
    less CPU per operation.
  - Metadata: 1.48× and 1.77× throughput, 1.40× and 1.69× less CPU.
  - Both meet the prediction. The time-series prediction (within ±10% of native) was wrong:
    it gained 1.42× and 1.48× throughput with 1.59× and 1.54× less CPU. Native spent
    2.5–2.75 µs of CPU per operation compacting (about 37% of its CPU), ours 0.2 µs:
    retention deletes and batched appends still make compaction a large share.
  - Write amplification during the window was higher for ours in every workload (time
    series 4.8 against 2.4; stream 2.7 against 2.3; metadata 3.9 against 3.6). The likely
    reason is that native postpones compaction beyond the window.
- **H21 (MixGraph): supported.**
  - `zippydb` (85% reads): 1.05× and 1.10× (within ±10%).
  - `zippydb_write`: 1.34× and 1.70× (at least 1.1×).
  - CPU per operation is not compared: `db_bench` client threads keep their cores busy in
    both designs.
- **H22 (SATA SSD): supported.**
  - Both designs keep the disk 100% busy (about 220–250 MiB/s of writes and 210–270 MiB/s
    of reads, from `iostat`), so throughput is equal (0.93× and 1.04×).
  - Ours uses 1.87× and 2.00× less CPU per write, and p99.9 write latency was 28 against
    48 ms (8 cores) and 84 against 336 ms (16 cores).
  - The disk statistics come from the campaign's `iostat` log (`v3-ssd/device.json`),
    because btrfs hides the disk from the benchmark's own device lookup.

### NVMe follow-up on malphite (registered 2026-11-10, before these runs)

The tuned NVMe confirmation on malphite (`data/malphite-nvme/retune`, 8 cores, 3 blocks)
gave 1.07× throughput (95% CI 1.06–1.12) and 1.46× less CPU per write. Before any of the
runs below, analysis of that data showed:

- Ours had 863 memtable-limit write delays against native's 98, while native had more
  L0-file-count delays (194 against 100).
- Ours drove the NVMe harder: median write latency 3.0 against 0.8 ms, read latency 11.4
  against 5.3 ms, queue depth 43 against 24, and `%util` 81% against 69%.
- Write amplification over ingest plus catch-up compaction was 3.8–4.0 for ours against
  3.2–3.3 for native. The selected settings differ in subcompactions (4 for ours, 8 for
  native).

Four checks follow, all on malphite with the build and golden database of the confirmation.
They use 50 M records, 120 s warm-up plus 300 s, 3 blocks and schedule seed 5. Settings are
frozen from `tuned-malphite.json` unless stated. The confirmation stays the reported NVMe
result; these checks explain it and do not replace it, and no setting is re-selected from
them.

- **N1, same machine in memory.** The confirmation grid with data on `/dev/shm`. tmpfs has
  no direct I/O, so I/O is buffered, as in the v3 in-memory campaigns.
  - Rule: if ours' throughput ratio is at least 1.30×, the small NVMe gain is attributed
    to the storage path. If it is at most 1.15×, it is attributed to the machine.
    Otherwise it is reported as mixed.
- **N2, subcompactions.** The NVMe confirmation with ours at subcompactions = 8 (every other
  setting frozen), paired with frozen native in each block.
  - Rule: if ours' ingest-plus-compaction write amplification comes within 5% of native's,
    the gap is attributed to the subcompaction setting. Throughput is reported either way.
- **N3, 4 cores on NVMe.** Direct I/O, with the registered 4-core settings of
  `v3-headline.json` for both designs. These were tuned on the main server, not malphite;
  this is stated wherever N3 is reported.
  - Descriptive: tests whether a CPU-bound regime exists on this NVMe drive.
- **N4, device ceiling.** `fio` on the same file system with direct I/O, 1 MiB requests,
  queue depth 32, 60 s each: sequential write, then mixed workloads at the read:write byte
  ratios of the confirmation (`iostat` medians): 32% reads (ours, about 765 MB/s in
  total) and 50% reads (native, about 880 MB/s in total).
  - Rule: if ours' combined read plus write bandwidth is at least 80% of the mixed
    ceiling, the drive is called saturated for ours.
- **Change before N3 ran (2026-11-10, 09:45):** N3 is dropped and not run, to make time
  for N5. N2 runs to completion as registered.
- **N5, fixed rates on NVMe.** Open loop at 400k, 630k and 800k writes/s, about 45%, 70%
  and 90% of native's measured NVMe maximum (0.89–0.92 M). Both designs use the frozen
  settings in `tuned-malphite.json`, with direct I/O. 60 s warm-up plus 180 s, as X5;
  2 blocks, schedule seed 6.
  - Reported per rate: achieved rate, p99 and p99.9 write latency, CPU per write, cores
    busy, stall fraction, and the drive's write latency, queue depth and bytes from
    `iostat`.
  - No directional hypothesis. If ours' extra write amplification loads the drive more
    at the same rate (higher latency or queue depth), that is reported as a cost.
  - A rate a design cannot sustain (achieved below 98% of offered) is reported as such,
    not dropped.
- **Change before N5 ran (2026-11-10, 10:10): equal I/O paths.** With direct I/O on,
  native RocksDB reads compaction input from the drive (`use_direct_reads`), while our
  GPU path reads its input through the page cache. The confirmation shows this: native
  compaction read 3.1–3.3 bytes per user byte through RocksDB's reader against 0.05 for
  ours, and the drive served about 445 against 245 MB/s of reads. That favours ours on
  disk. So:
  - **N6, equal-I/O confirmation:** the confirmation grid with `use-direct-io` false for
    both designs, so both read and write through the page cache and both still write to
    the NVMe drive. 3 blocks, schedule seed 8, every other setting frozen. This is the
    like-for-like NVMe result; the direct-I/O confirmation stays reported, with the
    asymmetry stated.
  - **N5 also runs with `use-direct-io` false** for both designs, after N6.
  - A direct-read path for our GPU input, which would allow a like-for-like run with
    direct I/O, is future work.

### Whole database over a direct RDMA link (registered 2026-11-10, before these runs)

r8 ran only through the relay and its 1 Gb/s hop (210k against 545k writes/s). It is
repeated with the database on pyke and the GPU server on malphite over 100 Gb/s RoCE.

- **Designs:** native, and `remote_gpu_rdma` (`cuda_merge` server reached directly).
- **Settings:** as r8 (50 M records, 4 writer threads, 4 background jobs, batches of 100,
  WAL off, 30 s warm-up plus 120 s, full verification, 3 repetitions in shuffled order).
  One change: max_subcompactions = 4 for both designs, because r8 used RocksDB's default
  of 1 (AUDIT item 16).
- **Starts** after the malphite NVMe follow-up ends, so the two do not share the GPU host.
- **Reported:** throughput, write stalls, and database-node CPU per write, with every run's
  verification and fallback count.
- **Hypothesis H13b:** remote GPU throughput is at least native's (median ratio ≥ 1.0),
  with less database-node CPU per write. If it is not, that is reported as a loss.
- **Outcome (2026-11-10, `data/rdma/r8-direct`): supported.** Medians of 3: 802k against
  459k writes/s (1.75×); database-node CPU per write 4.04 against 8.23 µs (2.04× less);
  4.46 µs including the GPU server's 51 s of CPU. Every run verified all 50 M keys. The
  first attempt did not start (missing `/dev/shm/sa29ali` on pyke) and produced no data.

### Write amplification: when level 0 is compacted (added 2026-11-10, before these runs)

On NVMe (N6) ours writes 5.5 bytes per user byte against RocksDB's 4.0, and in memory
(N1) 6.4 against 4.05. Flush bytes are equal; the extra is compaction. N2 ruled out
subcompactions. Hypothesis for the cause: ours finishes compactions sooner, so each level
0 to level 1 compaction starts as soon as the trigger (4 files) is reached and takes less
new data, and level 1 is rewritten more often per user byte.

- **Run** (zyra, in memory, 8 cores, LZ4, batched writes, the paused shared-GPU chain
  resumes after): native and full offload with their `v3-headline.json` 8-core settings,
  each at `l0-trigger` in {4, 8, 12} with `max-bytes-for-level-base` = trigger x 64 MiB
  (the write buffer size, so level 1 matches a full level 0); 4 is RocksDB's default.
  30 s warm-up plus 120 s, verification on, 2 blocks, schedule seed 110.
- **Reported:** write amplification over the window, throughput, CPU per write, stall
  fraction, for every cell.
- **Hypothesis H29:** some trigger brings ours' write amplification within 10% of
  native's at the same trigger, with ours' throughput still at least native's.
- **Selection rule, fixed now:** each design's setting is the trigger with the lowest
  write amplification whose throughput is within 5% of that design's best trigger.
  Both designs get the same choice of settings; the README reports both selections.
- **Extension (added 12:10, after block 0 and before block 1 finished):** block 0 showed
  ours' write amplification still falling at trigger 12 (6.35, 5.24, 4.22). Added trigger
  16 for both designs (`max-bytes-for-level-base` 1 GiB), same protocol, 2 blocks, seed 111,
  run after the main grid. Trigger 20 is not run: the benchmark leaves RocksDB's slowdown
  trigger at its default of 20 files. The selection rule above applies to all four triggers.

### Cost at matched throughput, tuned (added 2026-11-10, before these runs)

Tuned RocksDB tops out near 1.60M writes/s on 32 cores (`shared-mps/native32`). The cost
question at matched throughput is how few cores ours needs to reach that.

- **Run** (zyra, in memory, LZ4, batched writes, X3 protocol: 120 s warm-up plus 300 s,
  verification on, 3 blocks, schedule seed 112), after the level-0 trigger grid:
  - Ours on 12 cores: `v3-headline.json` 16-core settings with threads and background
    jobs scaled to 12 cores (24 and 12).
  - Ours on 32 cores: the same settings scaled to 32 cores (64 and 32).
  - RocksDB on 32 cores with `l0-trigger` 8 and `max-bytes-for-level-base` 512 MiB,
    otherwise its tuned 32-core settings: the trigger RocksDB preferred in the level-0
    grid, so it is not held to a setting it does better without.
- **Reported:** writes/s, CPU per write, stalls, cost per hour and per billion writes at
  `prices.json`, and the cost of ours at the core count that matches RocksDB's best
  32-core throughput (its default-trigger and trigger-8 runs, whichever is higher).
- **Hypothesis H30:** ours on 12 cores reaches at least RocksDB's best 32-core throughput,
  at less than 0.7x its cost per hour. If it does not, the README says so.
- **Changes before any cost data (12:18):** the level-0 trigger grid was stopped after
  block 0 and 3 runs of block 1, to start the cost runs sooner. Ours on 12 cores uses 25
  threads, not 24: the benchmark requires the 50 M records to divide evenly by the thread
  count, and the first attempt with 24 failed at start-up with no data. The 12-core runs
  go first, then the 32-core pair.

### NVMe: where the CPU, not the drive, is the limit (added 2026-11-10, before these runs)

On the NVMe server at 8 cores with LZ4 both designs reach the drive's limit (N6: 1.02x).
Three cells test whether ours' throughput gain returns when the CPU is the limit or when
ours writes fewer bytes.

- **Run** (NVMe server, after N5, same I/O path for both: `use-direct-io` false; 50 M
  records, batched writes, verification on; 60 s warm-up plus 180 s; 2 blocks, schedule
  seed 113). Settings are the NVMe server's tuned 8-core settings (N6), with threads and
  background jobs halved at 4 cores:
  - **C4:** 4 cores, LZ4.
  - **BZ:** 8 cores, LZ4 with ZSTD on the last level (`bottommost-compression zstd`).
  - **TR:** 8 cores, LZ4, each design at its best level-0 trigger from the trigger grid
    (RocksDB 8, ours 12; `max-bytes-for-level-base` trigger x 64 MiB).
- **Reported:** writes/s, CPU per write, write amplification, drive MB/s and utilisation,
  every cell, whatever the outcome.
- **Hypothesis H31:** in at least one cell ours' throughput is at least 1.2x RocksDB's.
- **Measured outcome (2026-11-10, `data/malphite-nvme/h31`):** all 12 trials passed
  verification of 50 M keys with zero GPU merge fallbacks. Medians of two runs per arm:
  C4 713k against 498k writes/s (1.43x), BZ 818k against 751k (1.09x), TR 1,069k against
  956k (1.12x). C4 exceeds the point threshold. The small repetition count, inherited
  settings and cached working set limit generalisation. TR write amplification is
  3.79 against 2.65, so the extra-writing issue is not resolved.
- **N5 completed (`data/malphite-nvme/followup/n5`):** all 12 trials verified 50 M keys.
  At 400k, 630k and 800k offered writes/s, native/full median p99.9 latencies are
  8.91/8.13, 638/336 and 6,174/1,275 ms. Achieved rates are near the offered rates, but
  multi-second tails at 800k are not an acceptable low-latency result.

### Contention, write amplification and baselines on zyra (added 2026-11-10, before these runs)

- **Profile (P):** RocksDB and ours at 8 cores, tuned settings, one 60 s window each with
  `perf record -g` and RocksDB statistics at full level (mutex wait, write stall, write
  group counters). Descriptive; no hypothesis.
- **Write amplification (W):** new benchmark settings for RocksDB's level-0 slowdown and
  stop triggers. Both designs at triggers 16 and 24 with slowdown at 2x and stop at 3x the
  trigger, `max-bytes-for-level-base` trigger x 64 MiB, 8 cores, 30 s plus 120 s, 2 blocks,
  seed 114. Replaces the queued trigger 16 runs. H29's selection rule applies across all
  triggers 4 to 24.
- **Baselines (B):** at 8 cores, RocksDB with universal compaction (new setting) and
  RocksDB with BlobDB (`blob-min-bytes` 64), against ours with its default and its best
  trigger. 30 s plus 120 s, 2 blocks, seed 115. Ours with BlobDB is tried once for 20 s;
  if it falls back or fails it is reported and not run further.
- **Changed before data:** ours on 32 cores is dropped from the cost runs today; RocksDB
  on 32 cores at trigger 8 stays.

- **Implementation amendment (2026-11-10, before P/W/B):** the existing cost32 and
  default-trigger confirmation finish before the new queue starts. Profiling uses
  30 s warm-up plus a 60 s measured window and full verification. CPU samples are
  filtered to the recorded window; raw profiles are retained. The scheduler tracepoint
  is unavailable without changing host permissions, so off-CPU durations are not claimed.
  Full RocksDB statistics span open through catch-up, while sampled write timers cover
  the measured window. Instrumented runs are diagnostic, not throughput comparisons.
  Universal and BlobDB baselines each get a separately preloaded, verified database
  with the same keys and values but their own storage layout. Their settings are
  exploratory baselines, not equally tuned optima. These preloads add runtime.
- **Build-overlap disclosure:** some cost12 trials overlapped the new binary's build or
  CPU smoke tests on another NUMA node. Core separation does not eliminate shared-memory
  or power effects. Retain the measurements, but require clean confirmation before
  treating H30 as final. The original binary remained unchanged throughout cost12/cost32.
- **N6 analysis correction:** aligned device samples show almost no physical reads,
  about 59% busy time for native and 88% for full offload. Cached reads and device busy
  time do not establish that both designs saturate the drive. The previous mixed-I/O
  fio ceilings are not a like-for-like ceiling for buffered N6 or the H31 cells.
- **Registration timing:** H31 was written locally before launch, but its Git commit
  followed launch. P/W/B have not started at this amendment. The earlier trigger-16
  extension completed before the queue hand-off; it is retained separately from W,
  whose slowdown and stop settings differ.
- **Profile execution:** both 60 s profile runs verified 50 M keys. CPU records cover
  each measured window, but some library stack frames produce DWARF decoding warnings;
  raw profiles are retained for re-analysis. They do not provide off-CPU durations.
  BlobDB's write amplification will not be compared using the SST-only counter, which
  omits blob-file writes.
- **W completed (`data/wa-trigger/w`):** all eight runs verified 50 M keys with no GPU
  merge fallbacks. At trigger 16, native/full median write amplification is 2.52/3.64;
  at 24 it is 2.17/3.09, with 971k/1,467k writes/s. Neither setting reaches the within-10%
  target. These measurements do not establish that the higher write amplification is fixed.
- **Baseline retry amendment (2026-11-10, after the first B failures):** kernel logs
  identify `CONSTRAINT_MEMORY_POLICY` OOM kills on NUMA node 0 for BlobDB and universal
  trials, despite free memory on other nodes. All B arms, including both offload arms,
  are rerun in a separate `base-b-preferred` campaign with `memory: preferred`, keeping
  cores, data, settings, durations, repetitions and seed unchanged. The GPU BlobDB smoke
  is retried under the same policy. Verified profile scratch copies are removed first;
  profile artifacts and raw samples remain. The original failed cohort is retained and
  is not mixed into the retry medians.

### GPU multiplexing (registered 2026-11-10, before M1 to M4)

One database uses 23 to 41% of the GPU, and every CPU fallback in the earlier sharing runs
(separate processes and MPS) was a GPU memory allocation failure while the GPU was 51 to 69%
busy. Each database process holds its own CUDA context and engines, and each engine keeps
device buffers sized for the largest job it has run: 11 to 17 times the job's compressed
input (`data/multiplex/memory`). The shared service (`lsm_compaction_server --backend
cuda_merge`, docs/remote-compaction.md, "Sharing one GPU") holds one context and K engines
for every database, schedules jobs (`fifo`, `fair`, `stall`), and re-queues a job that runs
out of device memory instead of falling back.

- **M0 (capacity, before M1, not a hypothesis test):** 138 compaction requests recorded from
  an 8-core whole-database run (median 96 MiB, largest 697 MiB) replayed by 16 concurrent
  clients for 60 s against the service with K in {1, 2, 3, 4, 6, 8}, 3 repetitions each.
  **Rule for K:** M1 to M4 use the smallest K whose median input GiB/s is at least 95% of
  the best K's.
- **M1 (local, zyra):** N databases of 4 cores (N in {1, 2, 4, 6}) and 8 cores (N in {1, 2,
  4}); each database on whole cores within one NUMA node, the same cores in every arm, with
  4 cores of the GPU's node reserved in every arm for the server. Arms: separate processes,
  MPS, service-fifo and service-fair with K engines; RocksDB at the smallest and largest N.
  Tuned settings of `configs/system/v3-headline.json` per core count; 50 M keys per
  database, 30 s warm-up plus 120 s, every key verified, 3 blocks, schedule seed 110
  (`results/multiplex/grids.py`). N = 8 at 4 cores is not run: eight databases do not
  fit in zyra's 94 GB `/dev/shm`.
- **M2 (remote):** N databases of 4 cores on pyke (N in {1, 2, 4, 8}, alternating its two
  NUMA nodes) sharing one service on malphite over 100 Gb/s RDMA (4 server cores, K
  engines), fifo and fair; RocksDB on pyke at N = 1 and 8. Same settings and protocol as M1,
  3 repetitions in shuffled order (`rdma_campaign.py r9`). All databases share pyke's one
  network card.
- **M3 (isolation, zyra):** through the service with K = 1 (deliberately contended), one
  heavy tenant (closed loop) and three light tenants (open loop at 50% of the 4-core
  service-fair N = 1 median from M1), 4 cores each, with each policy, and each light tenant
  alone at the same rate. 3 blocks.
- **M4 (burst, zyra):** as M3 under fair and fifo, with the heavy tenant starting 60 s after
  the light tenants and running for 60 s plus its 30 s warm-up; light tenants run 30 s plus
  180 s. Reported per second from the benchmark timeline. Descriptive.
- **Reported:** per-database writes/s and its share of the same arm's N = 1, aggregate
  writes/s, CPU fallbacks, out-of-memory re-queues, GPU utilisation, database CPU per write
  and server CPU, queue wait per tenant, p99 and p99.9 write latency, and cost per database
  write with the GPU and the server's 4 cores divided by N (`prices.json`).

Hypotheses (medians over blocks or repetitions):

- **X1 (memory):** every service run in M1 and M2 has zero CPU fallbacks.
- **X2 (local capacity):** service-fair, 8 cores, N = 4: each database keeps at least 81% of
  service-fair's N = 1 writes/s; and service-fair is above MPS at N = 4 (MPS kept 62% in the
  quick pass).
- **X3 (remote capacity; H15):** pyke to malphite, 4 cores, N = 4: each database keeps at
  least 80% of the N = 1 remote writes/s.
- **X4 (isolation):** in M3 the light tenants' p99.9 write latency is within 1.5 times
  their solo value under fair and under stall, and not within 1.5 times under fifo.
- **X5 (cost):** service-fair, 8 cores, N = 4 costs no more per write than RocksDB on the
  same cores: each database at least 1,125k writes/s (RocksDB tuned 837k; the GPU and 4
  server cores divided by 4), which is 89% of ours' tuned single-database 1,260k.

**Outcomes (2026-11-12; `data/multiplex`, `multiplex/analyze.py`; every run checked all
keys):**

- **M0:** K = 4 (2.42 GiB/s; the best was 2.48 at K = 6; K = 8 re-queued 14 jobs for
  memory and fell to 2.13).
- **X1 supported:** 0 CPU fallbacks in 66 service runs (42 in M1, 24 in M2).
- **X2 partly supported:** service-fair at 8 cores, N = 4 kept 91% of its N = 1
  (threshold 81%), but its 1,121k writes/s per database is below MPS's 1,149k. MPS kept 89%
  here, not the quick pass's 62%, for two reasons. In this build a GPU out-of-memory error
  inside a database process also releases idle buffers and retries (one MPS database
  re-queued 112 jobs and fell back once). Each database also sits within one NUMA node.
- **X3 supported:** 94% (894k against 947k writes/s per database). At N = 8, 89%, and
  2.10× RocksDB on the same cores.
- **X4 not supported:** the light tenants' worst p99.9 was 4.31× (fifo), 4.46× (fair) and
  4.15× (stall) their solo 13.6 ms. The stall policy never ran a job early, because no
  tenant stalled. The light tenant on another NUMA node stayed at 1.1–1.3× solo; the two on
  the node of the heavy tenant and the service were at 4.2–4.5×. E2 below shows this is not
  the queue.
- **X5 not supported:** 1,121k against 1,125k writes/s per database, $0.238 per billion
  writes against RocksDB's $0.234 (N = 1) and $0.245 (N = 4) measured in M1. FIFO reached
  1,144k ($0.233), and MPS 1,149k ($0.210).
- **M4:** the light tenants kept 363k writes/s throughout. Their combined pending
  compaction rose from 1.43 to 2.13 GiB under fifo and from 1.32 to 1.90 GiB under fair,
  while the heavy tenant ran. Their p99.9 was 44 ms under both.

### Remote capacity curve with GPU telemetry, M5 (registered 2026-11-14, before any M5 run)

M2 counted only the writing window and recorded no GPU utilisation. Re-analysis of M2
(`multiplex/analyze.py`, AUDIT item 36) shows that at N = 8 the service's 4 engines were
86–87% busy and left 10.7–11.3 s of compaction when writing stopped (1.4–2.2 s at N ≤ 4).
Counting that compaction, each database reached 786–788k writes/s against 391k for RocksDB
(2.01×) and kept 84–85% of N = 1. The sharing argument needs the capacity of one service
measured directly, and the claim that one database leaves the GPU mostly idle needs GPU
utilisation in the remote setting.

- **Run** (`rdma_campaign.py r9`, client pyke, service on malphite's RTX PRO 6000 Blackwell
  over 100 Gb/s RDMA, 4 service cores; protocol, settings and builds as M2: tuned 4-core
  settings (`R9_FLAGS`), 50 M keys per database on `/dev/shm`, 30 s warm-up plus 120 s,
  every key verified; 3 repetitions in shuffled order; `--gpu-telemetry`, which samples GPU
  utilisation and memory on malphite once a second, after the databases are copied):
  - **Main:** service-fair with K = 4 at N in {1, 4, 8, 10, 12}; RocksDB at N in {1, 8, 12}:
    `--counts 1,4,8,10,12 --policies fair --engines 4 --native-counts 1,8,12 --seed-base 1000
    --results-name r9-capacity.jsonl --out data/multiplex/m5`.
  - **Engines:** service-fair with K = 8 at N in {8, 12}, RocksDB at N in {8, 12} in the same
    blocks: `--counts 8,12 --policies fair --engines 8 --native-counts 8,12 --seed-base 1100
    --results-name r9-capacity-k8.jsonl --out data/multiplex/m5`. M0 chose K = 4 on the
    48 GiB A6000; the RTX PRO 6000 has 96 GiB.
  - FIFO is not run: it was within 0.5% of fair at every N in M2.
  - **Before the runs:** the hosts file lists 12 four-core database slots on pyke, alternating
    its NUMA nodes. If pyke's `/dev/shm` cannot hold N database copies with 25% headroom
    (checked with `df` against the golden database's size), the largest N that fits is run and
    each larger N is reported as not run, with the reason.
- **Reported** (`multiplex/analyze.py`, every N and arm): per-database writes/s in the window
  and counted to the end of the compaction left when writing stops; share of N = 1;
  that leftover compaction (s); service input GiB/s; engine busy (summed engine device time
  per second, divided by K); mean GPU utilisation inside the window and peak GPU memory; server
  cores busy; CPU fallbacks and out-of-memory re-queues; worst p99.9 write latency; cost per
  write at `prices.json` and the GPU price at which each N breaks even with RocksDB; and cost
  per GiB/s of compaction (`cost_model.py`).
- **Primary measure:** per-database writes/s counted to the end of the leftover compaction
  ("backlog-inclusive").

Hypotheses (medians over repetitions):

- **Y1 (premise: one database leaves the GPU mostly idle):** at N = 1, mean GPU utilisation
  inside the window is at most 40%.
- **Y2 (replication of M2):** at N = 8 with K = 4, backlog-inclusive writes/s per database is
  at least 1.8× RocksDB's at N = 8, with zero CPU fallbacks.
- **Y3 (saturation):** with K = 4, backlog-inclusive writes/s per database at N = 12 is below
  80% of N = 1.
- **Y4 (engines):** at N = 12, K = 8 gives at least 10% more backlog-inclusive writes/s per
  database than K = 4, with zero CPU fallbacks and zero out-of-memory failures.

Decision rules:

- **Capacity** of one service is reported as the largest N, for each K, at which
  backlog-inclusive writes/s per database stays at or above 80% of N = 1, with zero CPU
  fallbacks. The README and the paper state this N in place of "8 databases".
- If Y2 fails, the "2.0× per database" sharing claim is replaced by the M5 value at N = 8,
  whatever it is.
- If Y1 fails, the paper does not claim that one database leaves the GPU mostly idle. It
  states the measured utilisation, and justifies sharing only by the measured cost per GiB/s
  of compaction.
- The cost conclusion follows the measured break-even GPU price at the capacity N. No cost
  hypothesis is registered, because the conclusion depends on the price used.

Pre-run checks (2026-11-14, before the first M5 trial; the registration above was pushed at
11:51 EST):

- `/dev/shm` on pyke: 94 GB free. The 50 M-key database is 4.67 GB (M2's `levels_before`),
  so 12 copies with 25% headroom need 70 GB: every N is run. M2's databases grew to at most
  5.5 GB by the end of a run.
- Builds unchanged since M2: `build/zyra-rdma/lsm_system_bench` (2026-11-10 21:58) and
  `build/zyra-cuda/lsm_compaction_server` (2026-11-10 20:46).
- Database slots: 12 four-core slots on physical cores, alternating NUMA nodes; the first 8
  are M2's.
- Smoke test of the GPU sampler (1 M keys, 5 s plus 10 s, one database, separate output
  directory, not part of M5): 17 timestamped samples covering the database's window, every
  key verified, no CPU fallback.
- Launched at 11:55:57 EST with `~/rdma-private/m5.sh` (the two commands above, in order).
- Analysis clarification, written while the first K = 4 trials ran and before any K = 8 data:
  K = 8 is not run at N = 1, so its capacity rule uses K = 4's N = 1 as the base
  (`multiplex/analyze.py`, `m5_hypotheses`).

**Attempt 1 stopped (2026-11-14, 12:10 EST; data kept in `data/multiplex/m5-attempt1`).**
Three trials completed before the run was stopped:
- **service-fair, N = 8:** every database exited normally, but 17 compactions fell back to the
  CPU ("server disconnected", then "connection rejected"). The server printed no statistics,
  and its CPU time could not be read at the end. GPU utilisation inside the window was 98%,
  with 27 GB of GPU memory in use.
- **RocksDB, N = 1:** normal.
- **service-fair, N = 12:** the server disappeared before the window. Its resident memory had
  reached 25 GB within a minute of starting, and malphite reported 568 MB free. 342
  compactions fell back to the CPU. As compaction fell behind, the databases outgrew pyke's
  94 GB `/dev/shm`: 9 of the 12 exited with "No space left on device" and 1 was killed
  (exit 137). The registered space check (12 copies of the 4.67 GB database plus 25%) did
  not allow for that growth.
- **Diagnosis:** the cause is host memory, not the GPU. The RDMA transport's buffer pool
  (`src/remote/transport_rdma.cpp`, `Pool`) rounds each buffer up to a power of two, registers
  (pins) it, and keeps it on a free list that is never trimmed. Each database opens 4
  connections, so 12 databases hold up to 48 pinned request and reply buffers of up to 1 GiB
  each, against malphite's 95 GB. The clients use the same pool on pyke. M2 at N = 8 stayed
  under the limit in all 6 runs; attempt 1's first N = 8 trial did not.
- **Action:** no M5 outcome is evaluated from attempt 1. How M5 continues is recorded below
  before any further run.

**Continuation, attempt 2 (registered 2026-11-14, before any attempt-2 trial):**
- **Code change (transport only):** the RDMA buffer pool keeps only buffers of at most
  `LSM_RDMA_KEEP_MB` (default 64 MiB) for reuse; larger request and reply buffers are released
  after each call, on the server and the client. The server bounds its large request buffers
  across all connections with `LSM_RDMA_SERVER_BUDGET_MB`: a connection waits for room before
  accepting a large request, which holds its client back. M5 uses 32768 (malphite has 95 GB;
  the 4 engines pin about 12 GB when the server starts). Small requests and replies are
  unchanged.
- **Builds:** `build/zyra-rdma` and `build/zyra-cuda` are rebuilt in place with this change;
  M2's binaries are kept in `build/m2-binaries`.
- **Database counts:** K = 4 at N in {1, 4, 8, 10}, RocksDB at {1, 8, 10}; K = 8 at {8, 10},
  RocksDB at {8, 10}. N = 12 is dropped. The space rule is now N times M2's peak database size
  (5.5 GB) plus 25%, plus the source copy, within pyke's 94 GB `/dev/shm`. N = 12 needs 87 GB,
  too little room for the growth seen in attempt 1; N = 10 needs 74 GB.
- **Hypotheses:** Y3 and Y4 are evaluated at N = 10 instead of 12, with the same thresholds.
  Y1, Y2, the capacity rule and the decision rules are unchanged. Y2 at N = 8 now also tests
  whether M2's result holds with the fixed transport.
- **Run:** as registered above, with `--counts 1,4,8,10 --native-counts 1,8,10 --seed-base 2000`
  (K = 4) and `--counts 8,10 --native-counts 8,10 --seed-base 2100` (K = 8), both with
  `--server-env LSM_RDMA_SERVER_BUDGET_MB=32768`. Results go to fresh files in
  `data/multiplex/m5`.
- **Before the run:** a smoke test of the rebuilt binaries (1 M keys, N = 2, budget set, in a
  separate output directory) must verify every key with no CPU fallback.
- **Smoke test result (about 13:06 EST, before the amendment was pushed at 13:07:52):** both
  databases verified all 1 M keys with no CPU fallback; the server printed its statistics
  (123 compaction jobs). The engine fingerprint (RocksDB commit and engine patch hash) is
  identical to M2's. The CUDA server was rebuilt on zyra, where the build was configured;
  malphite's default gcc 13 is newer than CUDA 12.3 accepts.
- **Launched** at 13:08:14 EST (`~/rdma-private/m5.sh`, the two commands above), after moving
  attempt 1's results file aside so none of its trials are reused.

**Attempt 2 stopped (2026-11-14, 13:24 EST; data kept in `data/multiplex/m5-attempt2`).** Four
trials completed, all in repetition 1:
- **service-fair, N = 4:** every key verified, no CPU fallback; the server's resident memory
  stayed at 17–22 GB, and it printed its statistics. Throughput was 776k writes/s per database
  (744k counting leftover compaction), against M2's 894k (876k). Attempt 1's N = 8 trial, with
  the old transport and the same GPU sampler, matched M2 (840k against 842k), so the drop
  comes from the transport change: large buffers are now registered and pinned for every call
  instead of reused.
- **service-fair, N = 10:** the server stayed up with no CPU fallback, but 8 of the 10
  databases stopped with "No space left on device" in pyke's 94 GB `/dev/shm`. The space rule
  used M2's database size after each run, which is smaller than the size during a run, when
  compaction has not caught up.
- **RocksDB, N = 10 and N = 8:** normal (387k and 416k writes/s per database).
- **GPU sampler:** a sampler loop left over from attempt 1 was still running on malphite, so
  attempt 2's GPU telemetry has about two rows per second instead of one. Both loops sampled the
  same GPU, so the utilisation figures are unaffected.
- **Action:** no M5 outcome is evaluated from attempt 2. The cleanup script
  (`~/rdma-private/m5clean.sh`) now stops sampler loops by their command line, without
  matching itself.

**Continuation, attempt 3 (registered 2026-11-14, before any attempt-3 build, check or trial):**
- **Code change (transport only), replacing attempt 2's:** each process has one RDMA
  protection domain and buffer pool per device, shared by all its connections. Request and
  reply buffers go back to the pool after every call, so any connection can reuse them, and
  idle connections hold none. `LSM_RDMA_CACHE_MB` caps the bytes the pool keeps registered
  between calls, evicting the oldest first; a cached buffer more than 4 times the size needed
  is not handed out. `LSM_RDMA_SERVER_BUDGET_MB` now counts every request buffer in use on the
  server.
- **Settings:** server `LSM_RDMA_SERVER_BUDGET_MB=32768 LSM_RDMA_CACHE_MB=16384`; each database
  `LSM_RDMA_CACHE_MB=4096`.
- **Throughput check before the run (pass rule fixed here):** one service-fair trial at N = 4
  with K = 4 and the M5 protocol, in a separate output directory. It passes if writes/s per
  database in the window is at least 97% of M2's N = 4 median (894k, so at least 867k), with
  every key verified and no CPU fallback. If it fails, M5 does not run and the result is
  recorded here.
- **Database counts:** K = 4 at N in {1, 4, 6, 8}, RocksDB at {1, 8}; K = 8 at N in {6, 8},
  RocksDB at {8}. Eight 50 M-key databases is the most that fits pyke's `/dev/shm` (attempt 2,
  N = 10).
- **Hypotheses:** Y1, Y2 and the decision rules are unchanged. Y4 is evaluated at N = 8
  (K = 8 against K = 4). Y3 is withdrawn: saturation beyond N = 8 cannot be reached on this
  testbed, and at N = 8 M2 already gives 0.84 of N = 1, so Y3 at N = 8 would not test a
  prediction. The capacity rule is applied to N in {1, 4, 6, 8}.
- **Run:** `--counts 1,4,6,8 --native-counts 1,8 --seed-base 3000` (K = 4) and
  `--counts 6,8 --native-counts 8 --seed-base 3100` (K = 8), both with
  `--server-env "LSM_RDMA_SERVER_BUDGET_MB=32768 LSM_RDMA_CACHE_MB=16384"` and
  `--client-env LSM_RDMA_CACHE_MB=4096`; results go to fresh files in `data/multiplex/m5`.
- **Builds:** only `transport_rdma.cpp` was recompiled (client on pyke, CUDA server on zyra).
- **Smoke test (1 M keys, N = 2):** every key verified, no CPU fallback, server statistics
  printed.
- **Throughput check (13:30–13:40 EST, `~/rdma-out/m5-check3`): passed.** service-fair at
  N = 4 reached 870k writes/s per database in the window (863k–879k; 861k counting leftover
  compaction), against the 867k threshold, with every key verified, no CPU fallback and no
  out-of-memory re-queue. The server's resident memory stayed at about 30 GB. The same check
  ran RocksDB at N = 4: 461k per database.
- **Launched** at 13:41:08 EST (`~/rdma-private/m5.sh`); both phases exited 0 (data in
  `data/multiplex/m5`).

**Outcomes, attempt 3 (2026-11-14; `multiplex/analyze.py`, `m5_hypotheses`; 27 trials, every
database exited normally and verified all keys, 0 CPU fallbacks, 0 out-of-memory re-queues or
failures):**

| K | N | Writes/s per database, window | With leftover compaction | Share of N = 1 | Leftover (s) | GPU busy | Engines busy |
|---|---|---|---|---|---|---|---|
| RocksDB | 1 | 475k | 465k | 1.00 | 6.3 | | |
| RocksDB | 8 | 402k | 394k | 0.85 | 8.5 | | |
| 4 | 1 | 918k | 914k | 1.00 | 0.9 | 19% | 8% |
| 4 | 4 | 872k | 865k | 0.95 | 1.7 | 72% | 44% |
| 4 | 6 | 830k | 816k | 0.89 | 3.5 | 96% | 80% |
| 4 | 8 | 837k | 783k | 0.86 | 10.9 | 98% | 86% |
| 8 | 6 | 828k | 814k | 0.89 | 3.9 | 93% | 61% |
| 8 | 8 | 834k | 784k | 0.86 | 10.0 | 96% | 70% |

- **Y1 supported:** 19% GPU utilisation at N = 1 (threshold 40%).
- **Y2 supported:** at N = 8, K = 4, 783k against RocksDB's 394k writes/s per database, 1.99×
  (threshold 1.8×), with no CPU fallback. M2's result (786–788k, 2.01×) holds with the new
  transport.
- **Y3:** withdrawn before the run (see above).
- **Y4 not supported:** at N = 8, K = 8 gives 1.00× K = 4. With 6 or more databases the GPU
  itself is 93–98% busy, so extra engines have nothing to use.
- **Capacity:** by the registered rule, N = 8 for both K, the largest N tested; capacity on this
  testbed is therefore at least 8. The GPU is saturated from about N = 6, and from N = 6 to 8
  the service adds total throughput (4.98M to 6.70M writes/s in the window) mainly by deferring
  compaction: leftover compaction grows from 3.5 to 10.9 s.
- **Cost (not a hypothesis):** at N = 8, $0.258 against RocksDB's $0.246 per billion writes
  (5% more) at the RTX PRO 6000 price ($3.01/h); break-even at $2.73/h.

### Cloud: the compaction service on Kubernetes, Azure CPU track (registered 2026-11-15, before any cloud run)

Every earlier result comes from one lab cluster with RDMA. These runs deploy the shared service
on a public cloud with Kubernetes. The subscription available (Visual Studio Enterprise) cannot
create GPU VMs: every NC, NV and ND size is "NotAvailableForSubscription", an offer restriction
that quota requests do not lift. So the service runs the CPU backend (`cpu_merge`), bounded and
ordered by the same job scheduler as the GPU engines (`--engines K --scheduler P`). These runs
therefore test deployment, compaction shipped over commodity cloud TCP, isolation, elasticity
with drain, and what Kubernetes costs; GPU results stay the lab results. No absolute comparison
with the lab cluster is made (different CPUs, network and database size). A GPU track on the
Digital Research Alliance of Canada cloud will be registered separately, once its GPU is known.

- **Platform** (`infra/azure/aks.sh`, `infra/k8s`, docs/cloud.md): AKS, free control plane,
  southcentralus. Service node Standard_D16ads_v6 (16 vCPUs, 64 GB), tenant nodes
  Standard_D16ds_v6 and Standard_D16s_v6 (16 vCPUs each), system node Standard_D2as_v6[^cloud-sizes]; kubelet
  `cpuManagerPolicy: static`. Each database is a Guaranteed pod with 4 whole vCPUs (2 physical
  cores with their hyperthreads; on the lab cluster a database had 4 physical cores) and 16 GiB,
  at most 3 per tenant node, spread evenly, so N <= 6; its data is on a memory `emptyDir` (as
  `/dev/shm` on the lab cluster). The service pod has 14 whole vCPUs and 32 GiB. Image:
  `ghcr.io/saifaldin14/lsm-tree:cpu-<commit>` (Dockerfile; RocksDB built for `-march=haswell`[^cloud-march]), the
  commit recorded with every trial. Pay-as-you-go prices archived in `prices-azure.json`.
- **Protocol** (`results/cloud/cloud_campaign.py`; records in the r9 format, analysed with
  `multiplex/analyze.py`): M5 settings (`R9_FLAGS`: tuned 4-core settings, batches of 100, WAL
  off, LZ4, 128-byte values), 10 M keys per database preloaded once per slot and copied into
  memory before every trial, 30 s warm-up plus 120 s, every key verified, 3 repetitions in
  shuffled order (seed 2000 + repetition), the service restarted for every trial, clients over
  TCP with 4 connections each.
- **C0 (engines, not a hypothesis):** service-fair at N = 6 with K in {4, 7, 14}, one
  repetition each. **Rule:** C1, C2 and C4 use the smallest K whose backlog-inclusive writes/s
  per database is at least 95% of the best K's; C3 gives each of its two replicas half of it
  (rounded up).
- **C1 (capacity over cloud TCP):** service-fair at N in {1, 2, 4, 6}; RocksDB at N in {1, 4, 6}.
- **C2 (isolation, the service on its own node):** with K = 2 (deliberately contended), one
  heavy tenant (closed loop) and three light tenants (open loop at 50% of C1's service-fair
  N = 1 median writes/s in the window, rounded down to 1,000), under fair and under fifo, and one
  light tenant alone at the same rate under each policy. 3 repetitions.
- **C3 (elastic):** N = 6 databases reach the service through a Kubernetes Service
  (`infra/k8s/compaction-service.yaml`) with `LSM_REMOTE_JOBS_PER_CONNECTION=4`. Replicas of 7
  vCPUs each (two fit on the service node), `--drain-seconds 60`. One replica at the start; a
  second is added 60 s after the warm-up and one is removed at 150 s (it drains); writing stops
  at 240 s. 3 repetitions. Per-second throughput from the databases' timelines.
- **C4 (what Kubernetes costs):** C1's service-fair and RocksDB at N = 4 on plain VMs of the
  same sizes in the same region (`infra/azure/vms.sh`: the same image in Docker, each database
  pinned to 4 vCPUs, the service to 14), 3 repetitions, against C1's N = 4 on AKS.
- **Reported:** as M5 (writes/s in the window and backlog-inclusive, share of N = 1, leftover
  compaction, service input GiB/s, service cores busy, CPU fallbacks, retries, p99/p99.9), plus
  pod placement and, for C3, retries, recycled connections and refused merges per replica.
- **Cost (C5, not a hypothesis):** per billion writes, each database charged its 4/16 of its
  tenant node and 1/N of the service node, against RocksDB charged its 4/16; and the service node
  price at which the service breaks even with RocksDB.

[^cloud-sizes]: Changed from the v5 sizes (D16ads_v5, D16ds_v5, D16s_v5, B2s) on 2026-11-15, before any
    cloud run: AKS rejected them ("not allowed in your subscription in location southcentralus";
    the allowed x86 general-purpose sizes are v6 and v7). The v6 sizes keep the same roles: an AMD
    service node and two tenant nodes with the same Intel CPU, one per VM family because each
    family's quota is 20 vCPUs.

[^cloud-march]: Changed from `x86-64-v3` on 2026-11-15, before any cloud run: that level lacks
    PCLMUL, which RocksDB's CRC32C code uses, so the image did not compile. `haswell` adds PCLMUL
    and nothing the cloud hosts lack.

Hypotheses (medians over repetitions; backlog-inclusive writes/s per database unless stated):

- **Z1 (offload over cloud TCP):** at N = 1, the service gives at least 1.3x RocksDB's
  writes/s, with zero CPU fallbacks.
- **Z2 (sharing):** at N = 6, the service gives at least RocksDB's writes/s at N = 6, with zero
  CPU fallbacks in every C1 trial.
- **Z3 (isolation):** in C2 under fair, the light tenants' worst p99.9 write latency is within
  1.5x their solo value; under fifo it is not.
- **Z4 (elastic):** in every C3 repetition, zero CPU fallbacks across scale-out and the drained
  scale-in; and total writes/s in the 60 s after the second replica is ready is at least 10% above
  the 60 s before the scale-out.
- **Z5 (Kubernetes):** at N = 4, AKS is within 5% of plain VMs for both the service and RocksDB.

Decision rules: the capacity of one 14-vCPU CPU service is reported as in M5 (largest N at or
above 80% of N = 1 with zero fallbacks). If Z1 fails, the paper does not claim that offloading
compaction over commodity cloud networking helps. If Z4 fails because of fallbacks, the paper
does not claim drain-safe scaling. A trial that fails for an infrastructure reason (node lost,
image pull, quota) is rerun and reported.

### YCSB A to F at 4, 8 and 16 cores, E1 (registered 2026-11-26, before any E1 run)

The `ycsb` campaign ran A, B, F and a write-only mix on 4 cores. Reviewers of papers in this
area expect all six YCSB workloads, including scans (E) and reads of recent keys (D), and
results beyond 4 cores.

- **Run** (`system_campaign.py`, zyra's RTX A6000, data on `/dev/shm`, grid
  `configs/system/ycsb-full.json`, 3 blocks, seed 108): the `ycsb` campaign's settings
  (50 M keys, 30 s warm-up plus 90 s, one operation per write, every read checked, adaptive
  yield, the same environment), with `threads` equal to cores and `background-jobs` half the
  cores (at least 2).
  - **Workloads:** A (50% reads, 50% updates), B (95% reads), C (100% reads), D (95% reads,
    5% writes, `--distribution latest`), E (95% scans of 50 keys, 5% updates), F (50%
    read-modify-write). A, B, C, E and F with uniform and Zipfian keys; D with "latest" only.
  - **Cells:** 4 cores for C, D and E (A, B and F are archived); 8 and 16 cores for all six.
    54 cells, both designs, 162 runs.
  - Command: `python3 results/system_campaign.py OUT --golden GOLDEN --grid
    configs/system/ycsb-full.json --blocks 3 --seed 108`.
- **Reported:** operations/s, p50/p99 read latency (scans for E), CPU per operation and write
  stalls, both designs, every cell; ratios paired within blocks.

Hypotheses (medians over blocks):

- **Y-E1a (no harm to reads):** for B, C, D and E at every core count, our operations/s is
  at least 0.95× RocksDB's and our p99 read latency at most 1.10× RocksDB's.
- **Y-E1b (write-heavy gain persists with more cores):** for A and F, uniform keys, our
  operations/s is at least 1.2× RocksDB's at 8 and at 16 cores.
- **Y-E1c (scans):** E's p99 scan latency is at most 1.10× RocksDB's. (GPU-built blocks use
  RocksDB's block size and restart interval, so scans should cost the same.)

### Lab machines for E2 to E5 (checked 2026-11-26, before any of these runs)

- **zyra** (RTX A6000, SATA SSD): busy with E1 until it ends; nothing else runs there.
- **malphite** (10-core Xeon Silver 4210R, 20 threads; RTX PRO 6000 Blackwell; 477 GB NVMe,
  49 GB free; 92 GB memory; 47 GB `/dev/shm`): GPU idle. Another user ran 3 single-thread
  jobs at the first check and 14 an hour later, all free to run on any of the 20 threads.
  Every trial records the load it saw. Reached through rift (`ssh rift ssh malphite...`).
- **pyke** (2 × 24 cores, 186 GB, 94 GB `/dev/shm`, no GPU): idle. Its RoCE port and
  malphite's share 192.168.100.0/24 at 100 Gb/s; its other link is 1 Gb/s.
- **jax** and **nasus** (192 and 256 threads, no GPU): their RoCE ports are on
  192.168.126.0/24, which does not reach pyke or malphite (pings fail), so neither can join
  the RDMA pair.
- **Builds:** M5's binaries (`build/zyra-*`, built 2026-11-14) predate the client's retry on
  a new connection and the bound on concurrent CPU merges (both added 2026-11-15), which E4
  and E5 test. So E2 to E5 use a fresh build of the sources at the commit that adds this
  registration (unchanged since 240740c), in
  its own checkout on malphite's disk (`/tmp/sa29ali/lsm-e`; E1 keeps running from the shared
  checkout): `build/e-cuda` (CUDA and RDMA, for malphite) and `build/e-rdma` (RDMA only,
  copied to pyke), with the same CMake options as `zyra-cuda` and `zyra-rdma` except CUDA 12.8
  instead of 12.3 (12.3 does not accept malphite's gcc 13); sm_86 as before. Their sha256
  sums are kept with the data (`build/SHA256SUMS`). The hosts file (`rdma-hosts-e.json`,
  private) is M5's with `repo` set to this checkout and these two builds.
- **Driver:** runs on malphite, as rift has no port on 192.168.100.0/24. pyke deletes a
  user's files in `/dev/shm` when the user's last login ends (systemd `RemoveIPC`), so one
  login to pyke is held open for the whole campaign.
- **Smoke tests (not reported):** r10 and r11 were run once with 1 M keys, 2 databases and
  windows under a minute, with M5's binaries, to check the driver. They found two driver bugs,
  now fixed (the restarted server did not start; pyke's `/dev/shm` was cleared between
  logins); their output is discarded.
- **Order:** E4, E3, E2, then E5, one after another, as all use malphite. E5 is last because
  its CPU-pool trials may wait for other users.

### Failure of the shared service, E4 (registered 2026-11-26, before any E4 run)

The client falls back to its own CPU when the service fails (`merge_fallbacks`), but no run
has measured it. Service papers show what happens when a worker dies.

- **Run** (`rdma_campaign.py r10`, client pyke, the M5 service on malphite: `cuda_merge`, 4
  engines, fair scheduling, 4 server cores, RDMA; M5's 4-core settings `R9_FLAGS`; 50 M keys
  per database on `/dev/shm`; every key verified): 4 databases (the first 4 M5 slots), 30 s
  warm-up plus 240 s.
  - **Arms, in shuffled order in each of 3 blocks (seed 1300 + block):** `service` (no
    failure), `service-kill` (the server is killed with SIGKILL 60 s into the measurement and
    restarted on the same endpoint 60 s later), and `native` (RocksDB, 4 databases).
  - Clients use the defaults: 2 retries on a new connection, then CPU fallback; 120 s
    without progress counts as a failure.
  - Command: `python3 rdma_campaign.py --hosts rdma-hosts-e.json --experiment r10 --out
    data/rdma/e4 --instances 4 --engines 4 --duration-seconds 240 --kill-at 60 --outage 60`.
- **Reported:** per-second writes/s of every database (timelines) aligned to the kill and
  restart times; CPU fallbacks and retries; write stalls; worst p99.9 write latency; every
  database's exit code and verification.
- **Recovery time:** seconds from the restart until the 5-second moving average of the
  summed writes/s is back to 90% of its mean over measurement seconds 30 to 60.

Hypotheses (medians over blocks):

- **Y-E4a (no data lost):** in every `service-kill` trial every database exits normally and
  verifies all 50 M keys.
- **Y-E4b (degrades, does not stop):** while the server is down (measurement seconds 60 to
  120), summed writes/s is at least 0.7× that of `native` in the same seconds.
- **Y-E4c (recovers):** recovery time is at most 30 s.

If Y-E4a fails, the paper reports the failure and the fallback is called unsafe until fixed.

### A pool of CPU cores as the service, E5 (registered 2026-11-26, before any E5 run)

Reviewers will ask why a GPU and not a pool of remote CPU cores, as in CaaS-LSM. E5 keeps
everything as in M5 and changes only what the server computes with.

- **Run** (`rdma_campaign.py r11`, client pyke, server on malphite over the same RDMA link,
  M5's 4-core database settings, 50 M keys per database, 30 s warm-up plus 120 s, every key
  verified): 8 databases (M5's first 8 slots).
  - **Arms, in shuffled order in each of 3 blocks (seed 1400 + block):** `service-gpu` (M5:
    `cuda_merge`, 4 engines, server pinned to threads 0-3); `service-cpu-4`, `-8` and `-16`
    (`cpu_merge` on 2, 4 and 8 physical cores, pinned to both threads of each core: 0-1,10-11;
    0-3,10-13; 0-7,10-17; one merge per thread); `native` (RocksDB, 8 databases).
  - 8 of malphite's 10 cores is the largest pool on the RDMA pair (see "Lab machines").
  - **Other users:** before each CPU-pool trial the driver waits, for at most 6 hours, until
    other users' busy threads on malphite are at most 20 minus the pool's threads; what it saw
    is recorded with the trial. A trial whose wait ran out is still run and reported, but left
    out of the hypothesis tests; if every block of a pool is left out, that pool is reported
    as not measured.
  - Command: `python3 rdma_campaign.py --hosts rdma-hosts-e.json --experiment r11 --out
    data/rdma/e5 --instances 8 --engines 4`.
- **Reported** (as M5, `multiplex/analyze.py`): per-database writes/s in the window and
  counted to the end of the leftover compaction ("backlog-inclusive", primary); leftover
  compaction (s); server CPU-seconds; CPU fallbacks; worst p99.9 write latency; cost per write
  at `prices.json` (a pool priced at `physical_core_hour` per physical core; the GPU arm as in
  M5).
- **Cores to match the GPU:** if no pool matches the GPU arm, the number of physical cores
  needed is extrapolated from the pools' server CPU-seconds per GiB compacted and is reported
  as an extrapolation, not a measurement.

Hypotheses (medians over blocks):

- **Y-E5a (a GPU does the work of many cores):** backlog-inclusive writes/s per database with
  the 8-core pool is below 0.95× the GPU arm's.
- **Y-E5b (cost):** at the cores needed to match the GPU (measured or extrapolated), the CPU
  pool costs more per write than the GPU service.

If Y-E5a fails, the paper says that at this load a pool of 8 cores does what the GPU does and
compares the two on cost alone.

#### TCP fallback on available hosts (registered before the fallback run)

The planned Malphite/RDMA pair could not be used. Run the same E5 workload as a separate TCP
campaign: Pyke is the database client, Zyra hosts the A6000 service, and Jax hosts the CPU
service. All remote service arms use TCP on the same private lab network; these outcomes
are reported separately from any RDMA campaign and do not support an RDMA claim.

- Keep 8 databases, 50 M keys each, the M5 4-core database settings, 30 s warm-up plus 120 s,
  3 randomized blocks, and full verification.
- Keep the GPU service at 4 engines. Run CPU pools of 2, 4, and 8 physical cores on Jax,
  using both SMT threads per core; record host load and wait for enough free worker threads.
- Record the client/GPU/CPU host models and loads, transport, service CPU time, per-database
  throughput and latency, compaction backlog, and all verification outcomes. Do not describe
  this cross-host comparison as hardware-cost matched.
- Run `python3 results/rdma/rdma_campaign.py --hosts HOSTS.json --experiment r11 --out
  results/data/rdma/e5-tcp --instances 8 --engines 4 --transport tcp --cpu-host cpu_pool
  --pools '0-1,96-97;0-3,96-99;0-7,96-103'`. Save it separately so the original preregistered
  RDMA results, if later available, remain distinct.

### Kvrocks with its compactions sent to the service, E2 (registered 2026-11-26, before any E2 run)

Every result so far uses our own benchmark. E2 runs a real key-value store with a network
protocol, Kvrocks (Redis-compatible, on RocksDB), as peers do.

- **Feasibility, checked on jax:** Kvrocks v2.17.0 pins RocksDB v11.8.1, the version we
  patch, and builds on our patched RocksDB with g++ 11. Sending its compactions to the service
  takes about 75 lines (`results/e2/kvrocks-lsm-hook.diff`): the transform is installed where
  Kvrocks builds its table options. Final hook (+140/−6 lines in 3 files, against Kvrocks
  commit 185d4221df20, our sources at 240740c, built with g++ 11.5 for Haswell and later):
  `LSM_KVROCKS_TABLE_FORMAT=lsm` gives the local arm the same table format without the
  transform, and Kvrocks prints the transform's statistics as one JSON line when stopped
  with SIGTERM. sha256: diff `5575da04…87b2`, binary `1a868336…80a4`, config `23c34680…e208`. The service cannot take every compaction as Kvrocks is
  configured by default, so the service's arm needs:
  - **lz4 instead of snappy:** snappy stops the database ("session block codec is
    unsupported").
  - **no compaction filters:** Kvrocks puts one on every column family, and a compaction with
    one stays local. Without them, expired and stale data is not cleaned up, which is
    harmless here (plain SETs, no expiry).
  - **table format 6, CRC32c checksums and binary-search data blocks** (Kvrocks uses format 5
    and hash-indexed blocks); the hook sets these.

  Even then, Kvrocks' two-level index and table-properties collectors keep the service from
  building finished blocks: it returns the merged entries and Kvrocks compresses and writes
  them. Only TCP is built into the hook, not RDMA.
- **Run** (`results/e2/e2_campaign.py` on pyke; the service on malphite, reached over TCP on
  the 100 Gb/s port; `build/e-cuda`; 3 blocks; arms shuffled with seed 1500 + block):
  - **Kvrocks:** 4 physical cores of pyke's node 0 (threads 0-3 and 48-51), data in
    `/dev/shm`, preloaded with every key of a 50 M-key space (100-byte values).
  - **Load:** `redis-benchmark` on node 1: SETs to random keys (64 connections, pipeline 16,
    8 threads), 60 s warm-up plus 300 s, then 60 s of GETs. Throughput and Kvrocks' CPU come
    from its counters between benchmark runs of 2 M requests.
  - **Check:** after leftover compaction, 1 M random keys are read back.
  - **Arms:**
    - `stock`: Kvrocks' own settings, local compaction (reference);
    - `local`: the settings the service needs, local compaction (the baseline);
    - `service`: as `local`, compactions sent to `cuda_merge` with 4 engines and 4 server cores.
- **Reported:** SETs/s, Kvrocks CPU per SET, p99 SET latency (median and worst over
  benchmark runs), GETs/s and p99, leftover compaction, CPU fallbacks and merge jobs from the
  hook, and the check.

Hypotheses (medians over blocks, `service` against `local`):

- **Y-E2a (works in a real store):** in every `service` trial the check finds every key and
  the hook reports no fallback to local compaction.
- **Y-E2b (no loss):** SETs/s is at least 0.95× `local`'s.
- **Y-E2c (CPU saved):** Kvrocks' CPU per SET is at least 1.2× lower.

`stock` against `local` shows what the required settings cost; it is reported, not tested.

### A dataset much larger than the cache, on NVMe, E3 (registered 2026-11-26, before any E3 run)

The headline keeps the data in memory, and the earlier NVMe runs (`data/malphite-nvme`) had
the 50 M-key database in the page cache. Peers use datasets larger than memory.

- **Why not 500 M keys (~70 GB) as planned:** no GPU host has room. malphite's NVMe has 49 GB
  free, which must hold the database while it grows, and zyra has a SATA drive.
- **Instead:** 100 M keys (about 9.3 GB, 36× the 256 MiB block cache) with direct I/O for
  reads, flushes and compaction, so reads that miss the block cache go to the drive. The golden
  copy lives in `/dev/shm`; each trial's copy is on the NVMe drive.
- **Run** (`system_campaign.py` on malphite, `build/e-cuda`, 3 blocks, seed 109): the
  tuned 8-core settings of the NVMe confirmation (`malphite-nvme/followup/n6`) with
  `records` 100 M, `use-direct-io` true, 60 s warm-up plus 300 s, every key verified.
  - **Workloads:** write-only (batches of 100, WAL off, as n6), and YCSB A, uniform keys
    (E1's 8-core settings: one operation per write, 50% reads, 4 background jobs, 8 threads,
    adaptive yield).
  - Grid `configs/system/e3-nvme-large.json`; script `results/e3_run.sh` (builds the golden
    copy, then `system_campaign.py OUT --golden /dev/shm/$USER/e3/golden-100m --grid
    e3-nvme-large.json --blocks 3 --seed 109 --scratch /tmp/$USER/e3/scratch`; rerun to resume).
- **Reported:** operations/s, CPU per operation, p99 read latency (YCSB A), write stalls,
  drive bandwidth and busy time, both designs.

Hypotheses (medians over blocks):

- **Y-E3a (no loss on the drive, CPU saved):** write-only, our writes/s is at least 0.95×
  RocksDB's and our CPU per write at least 1.3× lower.
- **Y-E3b (reads from the drive):** YCSB A, our operations/s is at least 0.95× RocksDB's and
  our p99 read latency at most 1.10× RocksDB's.

### Energy counted to the end of leftover compaction, E6 (registered 2026-11-26, analysis only)

The measured window ends while compaction is still pending, which flatters whichever design
leaves more of it. Every archived single-database trial also integrated RAPL package and DRAM
energy and the GPU's board power from the first operation to the end of that leftover
compaction (`energy_joules_end_to_end`, with all operations in `ingest_and_compact.ops`).
These have not been analysed.

- **Analysis** (`energy_e2e.py`, no new runs): energy per operation, end to end, ours (`full`)
  against RocksDB (`native`), paired within block and cell, in `v3-headline` and
  `v3-headline-more` (the headline, 4/8/16 cores) and in the `main`, `equal-cost`, `ycsb`,
  `contention`, `prior-lz4`, `prior-zstd`, `prior-none` and `prior-equal-cost` campaigns.
  `read-tradeoff` is left out because its two designs use different settings.
- **Two accountings, both reported:** (i) the same machine, everything measured, including
  the idle GPU during RocksDB's runs; (ii) RocksDB on a machine without a GPU, so its GPU
  energy is left out. (ii) is the primary measure, because it is the less favourable to us.
- **Not counted:** fans, power supply and drive, which RAPL and the GPU sensor do not see.

Hypotheses (medians over blocks, headline):

- **Y-E6a:** under (ii), ours uses at most as much energy per write as RocksDB at 4, 8 and
  16 cores.
- **Y-E6b:** under (i), ours uses less energy per write than RocksDB at 4, 8 and 16 cores.

Whatever the result, the paper states energy under both accountings and drops "no energy
claim" only if Y-E6a holds.

**Outcome (2026-11-26; `energy_e2e.py`, `data/energy-e2e`).** Neither hypothesis holds.
Headline energy per write, ours / RocksDB, medians over 5 blocks:

| Cores | (ii) RocksDB without a GPU (primary) | (i) same machine |
|---|---|---|
| 4 | 1.03× (1.03–1.06) | 0.91× |
| 8 | 1.20× (1.20–1.21) | 1.06× |
| 16 | 1.16× (1.16–1.18) | 1.03× |

- **Y-E6a: not supported** (ours uses 3–20% more).
- **Y-E6b: not supported** (only 4 cores is below 1).
- **Where ours saves energy under (ii):** where RocksDB is short of CPU for compaction:
  - 2 background jobs on 4 cores with batched writes: 0.46× (LZ4), 0.37× (zstd);
  - CPU contention: 0.60–0.77×;
  - YCSB uniform writes: 0.77×.
- **Where it costs energy:** with 4 jobs and single writes, 1.16×; against RocksDB on 2× or
  4× the cores (equal cost), 1.65–1.80×.

The script's first version paired every trial of a cell with every other within a block,
mixing the two background-job settings; it was fixed to pair matching settings (jobs per core)
before these numbers were recorded. The headline has one trial per cell and was not affected.

## Real SATA with synchronous WAL writes (registered before these runs)

The existing SATA campaign disabled WAL, so it did not measure the storage-bound write path
with durable WAL writes. Run native RocksDB and full GPU offload on Zyra's local Micron 5200
SATA device using the direct-I/O table path, with the database and trial copies on that device.
Use 50 M keys, 128-byte values, LZ4, batched writes of 100, 4 client cores, verification on,
120 s warm-up, 300 s measurement, and 3 randomized paired blocks (seed 1601). WAL stays enabled and
`WriteOptions::sync` is enabled for every write batch in both arms. Record `wal_syncs` and
`sync_writes` in every result; a trial cannot support this experiment unless both confirm
synchronous WAL writes and correctness passes. Report window and backlog-inclusive throughput,
p99 write latency, CPU/energy, write amplification, and the loss/gain relative to native. This
is a bounded 4-core storage-path comparison, not a claim that the SATA device is saturated.
The exact grid is `configs/system/sata-sync-wal.json`; raw campaigns are archived separately.

## Changes after data collection

- **Cost analysis repriced (2026-11-14; analysis change only, no new runs).** Every cost
  figure had priced the GPU as an NVIDIA L4 ($0.6263/h), the cheapest AWS GPU, while the
  measured cards were an RTX A6000 (48 GiB) and an RTX PRO 6000 Blackwell (96 GiB). The L4
  has 24 GiB, less than the service's 4 engines need (30.7 GiB in M0), and its performance
  was not measured. The cost analysis now prices each measured GPU as the cheapest AWS GPU
  with at least its memory and bandwidth: the A6000 as an L40S ($1.6825/h, g6e.xlarge) and
  the RTX PRO 6000 as the same card ($3.0061/h, g7e.2xlarge) (`prices.json`,
  `gpu_pricing_rule`). Outcomes recorded above under the L4 price are left as recorded.
  Under the new prices: the tuned top-rate comparison (ours on 16 cores against RocksDB on
  32) still favours ours (23% lower cost per write); the same-rate comparison reverses (ours
  68% more); H30's cost ratio (12 cores, runs not yet final) rises from 0.59× to 0.96× of
  RocksDB's cost per hour, above its 0.7× threshold; X5's cost part and the M2 cost
  comparison reverse (service 24% and 4% more per write than RocksDB). Every comparison's
  break-even GPU price is reported so other prices can be applied. X5 was not supported on
  throughput either way.

- **GPU multiplexing, exploratory runs added after M1 and M3 (2026-11-12; not hypothesis
  tests; `data/multiplex/e1`, `e2`; seeds 113 and 114, 3 blocks):**
  - E1: in M1 the service's jobs waited 550–750 ms for one of its 4 engines while the GPU
    was 64–69% busy, so the service with 4, 6 and 8 engines and MPS were run again at
    4 × 8 and 6 × 4. Per database: 4 engines 1,210k and 578k, 6 engines 1,204k and 514k,
    8 engines 1,080k and 473k (10–14 out-of-memory re-queues per run), MPS 1,156k and 560k
    (17 and 124 CPU fallbacks). The service with 4 engines was 3–5% above MPS here, where
    M1 had it 2–17% below, so the two are not distinguished by these runs.
  - E2: in M3 the light tenant on another NUMA node was barely affected, so the M3 tenants
    were run with the same placement and rates as separate processes (no service) and
    through the service with 4 engines (fair). Light p99.9 on node 0: 23.1 ms (1.7× solo)
    without the service, 79.7 ms (5.85×) with 4 engines, although light jobs waited only
    51 ms. The slowdown comes from the service's work on node 0, not from its queue.

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
- **Earlier-design campaigns, first attempt (stopped, kept as
  `prior-isolated-attempt1` and `prior-lz4-attempt1`):**
  - What happened: in the LZ4 campaign, the sort/merge and pipeline rebuilds
    were killed for lack of memory with batched writes and 4 background jobs.
    Trials are bound to NUMA node 0 (about 47 GB).
  - Cause 1: the CPU-decompress path decoded a whole job's input into one
    extra pinned buffer. It now decodes and uploads in 256 MiB chunks.
  - Cause 2: the pipeline rebuild still needs about 30 GB more pinned host
    memory than full offload, because the GPU returns its blocks uncompressed
    before RocksDB compresses them.
  - Change: every trial of the earlier-design campaigns, native and full
    offload included, now prefers node 0 but may spill to other nodes
    (`"memory": "preferred"`).
  - Both campaigns were restarted from scratch with the fixed build. The
    isolated sweep was re-run as well, so all designs use the same binary.
- **Earlier-design campaigns, second attempt (stopped, kept as
  `prior-isolated-attempt2` and `prior-lz4-attempt2`):**
  - What happened: all 115 completed trials exited cleanly. However, two
    pipeline-rebuild trials (batched writes, 4 background jobs) fell back to
    the CPU for 2–3% of jobs because the GPU ran out of memory.
  - Cause: the CPU-decompressed input was held on the GPU twice (as uploaded
    and as copied into place), which handicapped the rebuilds.
  - Change: the CPU-decoded blocks are now uploaded straight into place. Peak
    GPU memory of the pipeline rebuild fell to 24 GB (full offload: 13 GB),
    with no fallbacks.
  - Both campaigns were restarted from scratch with this build.
- **RDMA experiments with the hardware available (R1, R2, R6):**
  - **Topology:** the database node (Nasus) is connected over 100 Gb/s RoCE to one
    other machine (Jax), which runs the compaction server and both backups on
    separate cores. A third RDMA host on the same network was not available.
  - **CPU only:** no host has both a usable GPU and an RDMA card, so the server
    compacts on the CPU (`cpu_merge`) and every result is labelled "CPU". `cpu_merge`
    is our reference merge and takes about twice as long as RocksDB's own compaction
    for the same job (19.8 s against 10.0 s for 32-byte values). So "remote CPU" here
    understates a CaaS-LSM-style remote RocksDB compaction, and H17 cannot be tested
    until GPU-over-RDMA hardware is available.
  - **Interrupted R2 runs:** the first attempts were interrupted twice when the
    controlling laptop lost its connection. Unfinished runs were discarded and
    re-run, and the experiment driver now runs on the database node itself.
