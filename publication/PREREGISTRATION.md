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
| V6 | Remote compaction without RDMA hardware: database on NUMA node 2, compaction server on the GPU's node 0 (8 cores), loopback TCP; native, local GPU, remote GPU, remote CPU (`publication/rdma/loopback_gpu.py`) | 4 | database: 4 | 3 |

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
