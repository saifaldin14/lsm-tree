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
