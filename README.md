# LSM Lab: GPU-offloaded RocksDB compaction

RocksDB keeps its data sorted by merging files in the background. This merging, called
compaction, is the largest CPU cost of a write-heavy RocksDB database. LSM Lab runs
compaction in RocksDB 11.8 on an NVIDIA GPU. Every compaction's output is checked key by key
against unmodified RocksDB.

## Summary

- One compaction is 8–12× faster and uses 9–16× less CPU with small compressed values. One
  GPU does the compaction work of 15–18 CPU cores.
- The whole database handles 1.5–1.8× more writes and uses 1.8–2.2× less CPU per write. The
  gain is smaller than for compaction alone because the CPU work of accepting writes is not
  offloaded.
- Earlier GPU and FPGA designs, rebuilt in the same RocksDB, leave compression on the CPU.
  With compression on, this design uses 2.0–6.5× less compaction CPU than the best of them.
- Gains are small for read-heavy workloads and for data stored without compression.

## Key results

Compared with RocksDB tuned for the same machine by the same search, on the same CPU cores,
unless stated otherwise. The GPU is in addition to those cores.

| Measurement | Result |
|---|---|
| One compaction | 8–12× faster, 9–16× less CPU (small values, compressed); 3–4× with 1 KB values or no compression |
| GPU capacity | One GPU does the compaction work of 15–18 CPU cores |
| Whole database | 1.5–1.8× more writes, 1.8–2.2× less CPU per write; no write stalls |
| Postponed compaction | After 5 minutes of writing, RocksDB needs 11–69 s to finish postponed compaction, GPU offload 1.3–2.6 s; including this time, 1.55–1.86× more writes |
| Write latency, 99.9th percentile | 3.8× lower at 70% of RocksDB's maximum load, 9.6× lower at 90% (1.75× at 50%) |
| NVMe drive | 1.60× more writes, 1.70× less CPU per write; 99th-percentile write latency 29× lower at 630k writes/s |
| Remote GPU over RDMA | Each compaction 4.0–6.5× faster; whole database 1.75× more writes, 2.04× less CPU per write |
| Earlier GPU and FPGA designs | 2.0–6.5× less compaction CPU than the best; whole-database gain over RocksDB 1.75×, against 1.14–1.41× for earlier designs |
| Cost (AWS prices) | 2.26M writes/s on 16 cores and a GPU at half RocksDB's cost per write; RocksDB reaches at most 1.60M on 32 cores |

## Where the remaining CPU goes

Compaction CPU per write falls from 3.5–4.2 µs to 0.8 µs. The CPU spent accepting writes,
about 2.3 µs, is unchanged, so the whole database gains about 2×. Tuned
runs, LZ4, median of 5:

![CPU per write, split by job](publication/figures/fig39_cpu_per_write.png)

## Comparison with other work

### Earlier work

| Work | What it moves off the database's CPU | How it was compared |
|---|---|---|
| gLSM (ACM TOS'24), gParaKV (SC'25) | Sorting and merging, to a GPU; values kept outside the sorted files | Rebuilt in RocksDB ("GPU sort/merge"); gParaKV's own code also run |
| LUDA (arXiv'20), MSST'24 GPU compaction | Most of the compaction, to a GPU; compression stays on the CPU | Rebuilt in RocksDB ("GPU pipeline"); LUDA's own code also run |
| Alibaba's FPGA compaction (FAST'20) | Most of the compaction, to an FPGA; compression stays on the CPU | Same design as the GPU pipeline rebuild |
| GPComp (TPDS'25), GCLComp | Data movement between the SSD and the GPU | Not run |
| Nova-LSM (SIGMOD'21), CaaS-LSM (SIGMOD'24) | Compaction, to other machines' CPUs | Represented by our remote CPU compaction server |
| Tebis (EuroSys'22), RubbleDB (ATC'23) | Compaction on replicas: compact once, send the result | Represented by our compact-once replication |
| This work | The whole compaction including compression, to a local or remote GPU | |

### Same-machine comparison

The earlier systems were built on different stores and measured on different hardware, so
their published speedups are not comparable. Each design was rebuilt inside the same
RocksDB and run on the same machine. Where a design choice was open, the rebuild used the
faster option (for example, decompression on 4 parallel threads). Every rebuild's output
matches RocksDB's.

![Earlier GPU and FPGA designs rebuilt in RocksDB](publication/figures/fig36_other_work.png)

(a) One compaction, median of 3 runs. (b) Whole database, 4 cores, batched writes, the same
settings for every design, median of 3–5 runs. The gains below are medians of each run
paired with its RocksDB run.

| Design | Whole-database gain over RocksDB (LZ4 / ZSTD / no compression) |
|---|---|
| GPU sort/merge (as gLSM, gParaKV) | 1.14× / 1.16× / 0.98× |
| GPU pipeline, CPU compression (as LUDA, MSST'24, Alibaba's FPGA design) | 1.41× / 1.17× / 1.34× |
| This work | 1.75× / 2.27× / 1.40× |

- The earlier designs compress on the CPU. With compression on, this design uses 2.0–6.5×
  less compaction CPU than the best of them.
- With every design tuned (LZ4, 4 cores): RocksDB 498k writes/s, best earlier design 592k
  (1.19×), this work 756k (1.52×).
- With the write-ahead log on and one write at a time (LZ4, 4 cores): RocksDB 177k writes/s,
  best earlier design 199k, this work 237k.
- Without compression, this design and the pipeline design use the same CPU (1.80 and 1.81
  CPU-s per GiB) and reach the same throughput.
- gParaKV's own code, under its own YCSB driver, reaches 238k ops/s against 182k for RocksDB
  with this offload, because it stores values outside the sorted files and compacts less
  data. CPU use is about the same (350 and 346 CPU-s).
- LUDA's own code is 2.6× faster than LevelDB 1.22, which it is built on, and slower than
  RocksDB. Different stores, so this does not compare the compaction designs.

Details: [publication/RELATED-WORK.md](publication/RELATED-WORK.md).

## How it works

A compaction has five steps:

1. read and decompress the input files;
2. sort the entries and drop outdated versions;
3. pack the remaining entries into data blocks;
4. compress and checksum each block, and build the Bloom filter;
5. write the files with their index and metadata, and install them.

Steps 1–4 run on the GPU. RocksDB runs step 5 and decides where each output file starts and
ends, so the output files are the ones RocksDB would write. Offloading compression alone
made a compaction 1.2–1.3× faster. Details: [docs/gpu-compaction.md](docs/gpu-compaction.md).

## Results

Data in memory on one server (4 × Intel Xeon Gold 5220, NVIDIA RTX A6000) unless stated.
"Ours" in the figures is the GPU offload. All tables, setups and caveats are in
[publication/FULL-RESULTS.md](publication/FULL-RESULTS.md).

### 1. One compaction

One full compaction of a bottom level plus 8 newer overlapping files (0.6–0.7 GiB),
16-byte keys, median of 3 runs. Log scale.

![One compaction, RocksDB against GPU offload](publication/figures/fig17_v3_compaction.png)

| Workload | Time, RocksDB → GPU | CPU per GiB, RocksDB → GPU |
|---|---|---|
| 32 B values, LZ4 | 11.8 → 1.0 s (11.7×) | 16.6 → 1.2 s (14.2×) |
| 128 B values, LZ4 | 7.0 → 0.9 s (8.0×) | 10.6 → 1.2 s (8.8×) |
| 128 B values, ZSTD | 11.8 → 1.1 s (10.7×) | 19.9 → 1.3 s (15.5×) |
| 1 KB values, LZ4 | 5.2 → 1.3 s (4.0×) | 5.3 → 1.2 s (4.4×) |

The GPU version uses about 1.2 CPU-seconds per GiB in every case. RocksDB uses the most CPU
with small values and with ZSTD, and the least with large values or no compression.

With eight compactions running at once on 8 cores, the GPU processes 1.42 GiB/s with LZ4 and
0.89 GiB/s with ZSTD, using 2.0 and 1.3 CPU cores. One RocksDB core processes 0.094 and
0.050 GiB/s, so the GPU matches 15 and 18 cores.

![Compaction throughput: one GPU against RocksDB on 8 cores](publication/figures/fig18_v3_cores.png)

### 2. Whole database

50 million keys, 128-byte values, batches of 100 writes, 5 minutes measured, median of 5
runs.

![Whole database, RocksDB against GPU offload](publication/figures/fig19_v3_database.png)

- RocksDB slowed writers for up to 6% of the time because compaction fell behind. The GPU
  version did not slow writers.
- On 16 cores, the 99.9th-percentile write latency is 8.9 ms with the GPU version and 210 ms
  with RocksDB.

**Postponed compaction.** Under heavy writing, RocksDB postpones compaction and continues
accepting writes. When writing stops, the postponed compaction still has to run: after
5 minutes of writing, RocksDB needs another 11–44 s with LZ4 (32–69 s with ZSTD on the last
level). The GPU version needs 1.3–2.6 s. Including this time, the GPU version handles
1.55–1.86× RocksDB's writes, against 1.51–1.79× counting only the writing period. All other
throughputs in this README count only the writing period.

![Postponed compaction after writing stops, and the gain including it](publication/figures/fig37_catch_up.png)

### 3. Write latency at fixed loads

Writes arrive on a fixed schedule, and latency is measured from when each write was
scheduled. 8 cores, median of 5 runs. Left: 99th percentile; right: 99.9th percentile. Log
scale.

![Write latency at fixed loads](publication/figures/fig20_v3_tail.png)

| Load | 99.9th percentile, RocksDB | GPU | Ratio |
|---|---|---|---|
| 430k writes/s (50% of RocksDB's maximum) | 5.5 ms | 3.2 ms | 1.75× |
| 600k (70%) | 41.9 ms | 11.0 ms | 3.8× |
| 770k (90%) | 251.7 ms | 26.2 ms | 9.6× |

### 4. Workloads, reads and shared machines

Workloads modelled on RocksDB users ([docs/workloads.md](docs/workloads.md)), Meta's
published production models (FAST'20), and YCSB. Median of 3 runs.

![Gain over RocksDB per workload](publication/figures/fig32_workload_gains.png)

- Write-heavy workloads: 1.30–1.77×.
- With another program using 40% or 80% of each of the database's 4 cores: 2.14× and 2.48×.
  Energy per write, for the whole server, is 1.3–1.5× lower in these runs (GPU energy
  counted for the GPU version only).
- Read-heavy workloads (85–95% reads): 1.02–1.10×. No workload had slower reads. With
  read-modify-write, 99th-percentile read latency is about half of RocksDB's.

**Compaction settings.** Compacting more often (level-0 trigger 2, level multiplier 6) and
using ZSTD reduce RocksDB's throughput more than the GPU version's. Half reads and half
writes, 4 cores, the same untuned settings for both, median of 3 runs. Changing from LZ4 to
ZSTD lowers RocksDB's throughput by 42% and the GPU version's by 9%.

![Throughput with different compaction settings](publication/figures/fig40_settings.png)

### 5. Drives

Data in memory and on the NVMe drive: a second server (10-core Intel Xeon Silver 4210R,
NVIDIA RTX PRO 6000 Blackwell), both designs tuned on it, 8 cores, median of 3 runs. SATA
SSD: the first server, 8 cores, median of 3 runs. Every key verified in every run.

![Throughput and CPU per write in memory, on NVMe and on a SATA SSD](publication/figures/fig33_storage_v2.png)

- Throughput: 1.60× RocksDB on the NVMe drive and 1.45× on the SATA SSD. The raw data for
  these two runs is not yet in the repository.
- CPU per write: 1.7× lower on NVMe, 1.9× lower on the SATA SSD.
- At a fixed 630k writes/s on NVMe, the 99th-percentile write latency is 17 ms with the GPU
  version and 503 ms with RocksDB (median of 2 runs per load; log scale):

![NVMe write latency at fixed loads](publication/figures/fig30_nvme_latency.png)

### 6. Remote GPU over RDMA

The database machine can send each compaction over a 100 Gb/s RDMA network to a GPU server
on another machine. Database machine: 96 cores; GPU server: the second server. Median of 3
runs; every compaction matched RocksDB.

![One compaction on a remote GPU over RDMA against RocksDB](publication/figures/fig31_rdma_direct.png)

- One compaction is 4.0–6.5× faster than RocksDB on the database machine, and the database
  machine uses 5.6–11× less CPU for it. The network transfer takes about 0.2 s of the
  remote GPU's 0.9–1.1 s.
- Whole database: 802k writes/s against 459k (1.75×), and 2.04× less CPU per write on the
  database machine (1.84× including the GPU server's CPU).
- A remote CPU compaction server saves the same database-machine CPU, but each compaction
  takes 9.6–17.6 s, longer than RocksDB on the database machine.
- Replication with 2 backups (earlier testbed, compaction on the primary's CPU): compacting
  once and sending the files to the backups uses 3.4× less CPU across all three machines
  per write with batched writes, and 1.8× less with single writes.

![CPU of all machines per write with 2 backups](publication/figures/fig41_replication.png)

Details: [docs/remote-compaction.md](docs/remote-compaction.md).

### 7. Cost

Both designs tuned for each core count; median of 5 runs (RocksDB on 32 cores: 3). AWS
on-demand prices: $0.089 per CPU core-hour and, for the GPU version, $0.63 per hour for one
NVIDIA L4. The figure shows the two cases where the GPU version is cheaper.

![Cases where the GPU version costs less than RocksDB](publication/figures/fig35_cost_cheaper.png)

- Highest throughput: RocksDB reaches 1.60M writes/s on 32 cores ($25.0k a year, $0.50 per
  billion writes). The GPU version reaches 2.26M writes/s on 16 cores and a GPU ($18.0k a
  year, $0.25 per billion writes).
- Same throughput: 1.26M writes/s takes 16 cores with RocksDB and 8 cores and a GPU with the
  GPU version, which costs 6% less.

Below about 1.3M writes/s, RocksDB costs less per write, because the GPU costs more than the
cores it replaces. All prices and configurations:
[publication/FULL-RESULTS.md](publication/FULL-RESULTS.md#7-what-it-costs-and-what-it-saves).

## Limitations

- Read-heavy workloads: 1.02–1.10× at 85–95% reads.
- No compression: the same CPU as the best earlier design.
- Low write rates: below about 1.3M writes/s, RocksDB costs less per write.
- Write amplification: about 1.4× higher (5.5 against 4.0 bytes written per user byte on
  NVMe). The likely cause is that faster compactions start earlier with less data each.
- Shared GPU: four databases on one GPU kept 77% of their single-database throughput,
  below the 80% registered in advance.
- Energy: lower per write on a shared machine (1.3–1.5×). In the other whole-database runs it
  ranges from 0.69× to 1.19× RocksDB's per write, so no general energy saving is claimed.
- Coverage: two GPU models and RocksDB 11.8; the remote-GPU whole-database run used one pair
  of machines and one configuration.

## Methodology

- Correctness: each compaction is compared with RocksDB's output, every stored version and
  200,000 lookups. Each database run checks every read and all keys and values at the end.
  The GPU code passes NVIDIA's compute-sanitizer memory, race and synchronisation checks.
- Tuning: both designs use the best settings from the same search
  ([configs/system/tuned.json](configs/system/tuned.json)). Runs have a fixed duration and
  are run in random order.
- Registration: hypotheses and thresholds were committed before the runs. Three were not
  met and are reported as measured ([publication/PREREGISTRATION.md](publication/PREREGISTRATION.md)).
- Measurement problems found during the project, and their fixes, are listed in
  [publication/AUDIT.md](publication/AUDIT.md).

## Repository

| Path | Contents |
|---|---|
| `src/gpu/`, `include/lsm_lab/` | GPU merge and block building (`cuda_merge.cu`), CPU reference (`merge_cpu.cpp`), output compression (`cuda_transform.cu`), Metal backend, RocksDB integration headers |
| `src/remote/` | Remote compaction (`remote_merge`) and replication over TCP and RDMA |
| `cmake/ApplyEnginePatch.cmake` | Version-locked patch that adds the offload hooks to RocksDB |
| `apps/` | `lsm_compaction_bench`, `lsm_system_bench`, `lsm_compaction_server`, `lsm_replica_server`, `lsm_rpc_bench`, `lsm_burn`, and the harness services (`lsm_db`, `lsm_worker`, `lsm_loadgen`, `lsm_admin`, `lsm_telemetry`, `lsm_gpu_check`) |
| `tools/` | `labctl` harness: plans, runs, audits and analysis |
| `publication/` | Results, raw data, figures, and the scripts that produce them |
| `configs/` | Dataset and experiment configurations |
| `docs/` | Design and operating documentation |
| `tests/` | C++ (CTest) and Python tests |

## Build

```sh
bash scripts/bootstrap.sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
```

For the GPU build (Linux, CUDA 12, nvCOMP fetched by
`scripts/bootstrap-cuda.sh`) add `-DLSM_ENABLE_CUDA=ON
-DCMAKE_CUDA_ARCHITECTURES=native -DLSM_ROCKSDB_PORTABLE=0`. Prerequisites:
Git, curl, CMake 3.25+, a C++20 compiler and Python 3.12+. RocksDB is fetched
at the commit pinned in [toolchains.lock.json](toolchains.lock.json).

## Reproduce

On a Linux machine with an NVIDIA GPU, after the GPU build:

```sh
python3 publication/v3_grids.py configs/system/tuned.json   # experiment grids from the tuned settings
ONLY="isolated headline bottomzstd openloop stream metadata timeseries mixgraph ssd" \
  LSM_BUILD_DIR=build/dev publication/v3_chain.sh artifacts/v3   # about 30 hours
python3 publication/make_figures_v3.py                       # figures 17-21 from publication/data/v3/
python3 publication/make_figures.py                          # figures 10-16 and 22
python3 publication/make_readme_figures.py                   # figures 32, 33, 35-37 (README summaries)
python3 publication/make_readme_figures_more.py              # figures 38-41 (README summaries)
python3 publication/rdma/analyze_r7.py                       # figures 23 and 31 (remote GPU over RDMA)
```

## Documentation

- [publication/FULL-RESULTS.md](publication/FULL-RESULTS.md): every result in detail, with setups and caveats
- [docs/gpu-compaction.md](docs/gpu-compaction.md): what is offloaded, how it plugs into RocksDB, eligibility, configuration and limits
- [docs/remote-compaction.md](docs/remote-compaction.md): remote compaction and replication over TCP and RDMA, and their results
- [publication/RELATED-WORK.md](publication/RELATED-WORK.md): earlier GPU, FPGA and remote compaction designs, measured head to head on this harness
- [docs/runbook.md](docs/runbook.md): building, benchmarks, the `labctl` harness, hardware gates
- [docs/telemetry.md](docs/telemetry.md): resource and energy telemetry
- [publication/](publication/): results, methodology audit and data
