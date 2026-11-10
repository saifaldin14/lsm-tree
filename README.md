# LSM Lab: GPU-offloaded RocksDB compaction

RocksDB, the storage engine behind many databases and data services, keeps its data sorted by
constantly merging files in the background. This merging, called **compaction**, is the
largest CPU cost of a write-heavy database. **LSM Lab moves the whole compaction job in
RocksDB 11.8 onto an NVIDIA GPU**, and checks every result key by key against unmodified
RocksDB.

## The short answer

- **Compaction itself becomes about 10× cheaper and faster.** One GPU does the compaction
  work of 15–18 CPU cores.
- **The whole database gains less, about 1.5–1.8× more writes for half the CPU per write**,
  because RocksDB's remaining work of accepting writes is not offloaded.
- **It beats earlier GPU and FPGA designs** rebuilt in the same RocksDB, because it is the
  only one that also moves compression to the GPU.
- **It helps write-heavy work on fast storage.** Read-heavy work, a disk that is already
  the bottleneck, or data stored without compression see little or no gain.

## Key results

Unless a row says otherwise, ours is compared with RocksDB tuned for the same machine by
the same search, on the same CPU cores. The GPU is in addition to those cores.

| What was measured | Result |
|---|---|
| One compaction | **8–12× faster, 9–16× less CPU** (small values, compressed); 3–4× with 1 KB values or no compression |
| GPU capacity | **One GPU = 15–18 CPU cores** of compaction work |
| Whole database | **1.5–1.8× more writes, 1.8–2.2× less CPU per write**, and writers are never slowed down |
| Slowest writes (99.9th percentile) | **3.8× lower at 70% load, 9.6× lower at 90%** (1.75× at 50%) |
| On an NVMe drive | **1.53× more writes, 1.70× less CPU** per write |
| A GPU in another machine, over RDMA | Each compaction **4.0–6.5× faster**; the whole database **1.75× more writes, 2.04× less CPU** |
| Against earlier GPU and FPGA designs | **2.0–6.5× less compaction CPU**; their database gains are 1.14–1.41× over RocksDB, ours 1.75× |
| Cost | Without tuning, 8 cores + GPU match RocksDB on 32 cores for **53% less** (AWS prices) |

## How good is this?

**The compaction result is strong.** A 10× drop in compaction cost, and one GPU replacing
about 16 cores, hold across value sizes and compression types, and every compaction matches
RocksDB's output exactly.

**The whole-database result is solid but not 10×.** Once compaction is on the GPU, most of
the CPU left goes to RocksDB accepting writes:

| CPU per write | RocksDB | Ours |
|---|---|---|
| Compaction | 3.5–5.5 µs | **0.8 µs** |
| Flushing memory to files | 1.0 µs | 0.7 µs |
| Accepting the write | 2.3 µs | 2.3 µs |

So the database gains about 2×, and further gains would need the write path itself to be
faster. The gain is largest when the machine is busy: 9.6× lower worst-case latency near
full load, and 2.5× more writes when other programs take 80% of the database's cores.

**It does not help everywhere.** See [Where it does not help](#where-it-does-not-help).

## How it compares with other work

Earlier GPU and FPGA compaction systems each ran on a different store and different
hardware, so their published speedups cannot be compared with ours. Instead, **we rebuilt
each design inside the same RocksDB, on the same machine**, giving it the benefit of the
doubt where a choice mattered. Every rebuild passes the same exact check against RocksDB.

| Design | On the GPU | Left on the CPU | Database gain over RocksDB (LZ4 / ZSTD) |
|---|---|---|---|
| GPU sort/merge (as gLSM, gParaKV) | sorting and merging | decompression, block building, compression | 1.14× / 1.16× |
| GPU pipeline, CPU compression (as LUDA, MSST'24, Alibaba's FPGA design) | everything except compression | decompression, compression | 1.41× / 1.17× |
| **Ours** | **everything except writing the file** | file layout and writing | **1.75× / 2.27×** |

*4 cores, batched writes, same settings for every design, median of 3–5 runs.*

- **The difference is compression.** Earlier designs leave it on the CPU, where it becomes
  the bottleneck. Ours uses 2.0–6.5× less compaction CPU than the best of them with
  compression on.
- **Without compression, ours and the best earlier design are equal.** Production RocksDB
  rarely runs without compression, but this is the honest limit of the contribution.
- **Their own code.** gParaKV (SC'25) is faster than RocksDB with our offload under its own
  benchmark (238k against 182k ops/s), because it stores values outside the sorted files and
  so compacts far less data; ours uses about the same CPU. Combining the two is future work.
  LUDA's own code is 2.6× faster than the LevelDB it is built on, but still slower than plain
  RocksDB (a different store, so this is not a fair test of its compaction).
- **Remote compaction** systems (Nova-LSM, CaaS-LSM) move compaction to other machines'
  CPUs. Ours can send it to a GPU in another machine instead, which is both cheaper for the
  database machine and faster than compacting locally.

Details: [publication/RELATED-WORK.md](publication/RELATED-WORK.md).

## How it works

A compaction has five steps:

1. read and decompress the input files;
2. sort the entries and drop outdated versions;
3. pack the survivors into data blocks;
4. compress and checksum each block, and build the Bloom filter;
5. write the files with their index and metadata, and install them.

**Ours runs steps 1–4 on the GPU.** RocksDB keeps step 5 and every decision about where
files start and end, so the files are exactly what RocksDB would write. Offloading
compression alone gave only 1.2–1.3×; the large gains need the whole job on the GPU.
Details: [docs/gpu-compaction.md](docs/gpu-compaction.md).

## Results

Data in memory on one server (4 × Intel Xeon Gold 5220, NVIDIA RTX A6000) unless stated.
Every table and figure, with full setups and caveats, is in
[publication/FULL-RESULTS.md](publication/FULL-RESULTS.md).

### 1. One compaction: about 10× faster and cheaper

![One compaction, RocksDB against ours](publication/figures/fig17_v3_compaction.png)

| Workload | Time, RocksDB → ours | CPU per GiB, RocksDB → ours |
|---|---|---|
| 32 B values, LZ4 | 11.8 → 1.0 s (**11.7×**) | 16.6 → 1.2 s (**14.2×**) |
| 128 B values, LZ4 | 7.0 → 0.9 s (**8.0×**) | 10.6 → 1.2 s (**8.8×**) |
| 128 B values, ZSTD | 11.8 → 1.1 s (**10.7×**) | 19.9 → 1.3 s (**15.5×**) |
| 1 KB values, LZ4 | 5.2 → 1.3 s (4.0×) | 5.3 → 1.2 s (4.4×) |

Ours costs about 1.2 CPU-seconds per GiB whatever the data. RocksDB's cost depends on it:
highest with small values and ZSTD, lowest with large values or no compression.

**One GPU replaces 15–18 cores.** Running eight compactions at once, the GPU processes
1.42 GiB/s with LZ4 and 0.89 GiB/s with ZSTD, while one RocksDB core manages 0.094 and
0.050 GiB/s.

### 2. The whole database: 1.5–1.8× more writes for half the CPU

![Whole database, RocksDB against ours](publication/figures/fig19_v3_database.png)

*50 million keys, 128-byte values, batches of 100 writes, 5 minutes measured, median of 5
runs.* RocksDB had to slow writers down for up to 6% of the time; ours never did.

### 3. Write latency: the busier the database, the bigger the gain

*Writes arrive on a fixed schedule, and latency counts from when each write was due.
8 cores, median of 5 runs.*

| Load | Slowest 0.1% of writes, RocksDB | Ours | Lower |
|---|---|---|---|
| 50% of RocksDB's maximum | 5.5 ms | 3.2 ms | 1.75× |
| 70% | 41.9 ms | 11.0 ms | **3.8×** |
| 90% | 251.7 ms | 26.2 ms | **9.6×** |

### 4. Real workloads, reads and a busy machine

| Workload | More operations per second |
|---|---|
| Stream-processing state (as in Flink, Kafka Streams) | 1.38–1.55× |
| Storage metadata (as in Ceph, TiKV) | 1.48–1.77× |
| Time series with old data deleted | 1.42–1.48× |
| Meta's production model, write-heavy | 1.34–1.70× |
| Meta's production model, read-heavy (85% reads) | 1.05–1.10× |
| YCSB read-heavy (95% reads) | 1.02–1.05× |
| YCSB read-modify-write | 1.31–1.67×, and read latency halves |
| Writes while other programs use 80% of the cores | **2.48×** |

Gains follow how much of the work is writing. Reads were not slower in any workload.
Workload models: [docs/workloads.md](docs/workloads.md).

### 5. On real disks

*A second server: 10-core Intel Xeon Silver 4210R, NVIDIA RTX PRO 6000 Blackwell, NVMe drive.
Both designs tuned on this machine; 8 cores, median of 3 runs.*

| | Writes/s, RocksDB → ours | CPU per write, RocksDB → ours |
|---|---|---|
| Data in memory | 0.95M → 1.62M (**1.70×**) | 7.7 → 3.7 µs (**2.06× less**) |
| NVMe drive | 0.82M → 1.26M (**1.53×**) | 8.2 → 4.8 µs (**1.70× less**) |
| SATA SSD (first server) | equal: the disk is the limit for both | **1.9–2.0× less** |

### 6. A GPU in another machine, over RDMA

The database can send each compaction over a 100 Gb/s RDMA network to a GPU server on
another machine, so the database machine needs no GPU. *A 96-core database machine and the
second server; median of 3 runs; every compaction matched RocksDB exactly.*

![Compaction on a remote GPU and a remote CPU over RDMA](publication/figures/fig24_rdma_direct.png)

| Workload | One compaction, RocksDB locally → remote GPU | Database machine's CPU per GiB |
|---|---|---|
| 128 B values, LZ4 | 3.66 → 0.91 s (**4.0×**) | 5.65 → 1.00 s (**5.6× less**) |
| 32 B values, LZ4 | 6.38 → 1.10 s (**5.8×**) | 9.11 → 1.10 s (**8.3× less**) |
| 128 B values, ZSTD | 6.77 → 1.05 s (**6.5×**) | 11.61 → 1.05 s (**11× less**) |

- **The whole database** run this way takes **802k writes/s against 459k** (1.75×), with
  **2.04× less CPU per write** on the database machine (1.84× counting the GPU server's CPU).
- **A remote CPU server is not enough:** it saves the same CPU but each compaction takes
  9.6–17.6 s, slower than RocksDB locally.
- **Replicas:** compacting once and sending the files to 2 backups cuts the whole cluster's
  CPU per write 3.4×.

Details: [docs/remote-compaction.md](docs/remote-compaction.md).

### 7. Cost

Priced at AWS on-demand rates (a CPU core at $0.089/h, an NVIDIA L4 GPU at $0.63/h):

- **Without tuning**, 8 cores + GPU write as fast as RocksDB on 32 cores (1.00M against
  0.99M writes/s) for **$11.7k against $25.0k per year (53% less)**, with 99th-percentile
  write latency 17× lower.
- **Tuned RocksDB scales further**, to 1.6M writes/s on 32 cores. In a first tuned
  comparison, 12 cores + GPU reached 1.79M for 41% less; this still needs a clean re-run.
- **Below about 670k writes/s, RocksDB on a few cores is cheaper.** The GPU pays only when
  the target is beyond what adding cores achieves.

## Where it does not help

- **Read-heavy work:** 1.02–1.10× at 85–95% reads, because there is little compaction.
- **A disk that is already the bottleneck:** on a SATA SSD throughput is equal, and only
  the CPU saving remains.
- **No compression:** ours and the best earlier design are equal.
- **Small deployments:** below about 670k writes/s, a few CPU cores are cheaper than a GPU.
- **Extra writing:** ours writes about 1.4× more bytes to storage per byte of user data (5.5
  against 4.0 on NVMe), probably because its faster compactions start sooner on smaller
  batches.
- **Sharing one GPU:** four databases on one GPU kept 77% of their single-database
  throughput, below our 80% target.
- **Energy:** no saving is claimed; energy per write ranges from 0.58× to 1.19× RocksDB's,
  depending on the workload.
- **Coverage:** two GPU models and RocksDB 11.8 only; the remote-GPU database run used one
  pair of machines and one setting.

## How we checked

- **Correct output:** every compaction is compared with RocksDB's, every stored version and
  200,000 lookups; every database run checks each read and all keys and values at the end.
  The GPU code passes NVIDIA's memory, race and synchronisation checkers.
- **Fair comparison:** both designs get their best settings from the same tuning search
  ([configs/system/tuned.json](configs/system/tuned.json)), runs last a fixed time, and
  their order is randomised.
- **Registered in advance:** hypotheses and thresholds were committed before the runs.
  Three were missed, and are reported as measured
  ([publication/PREREGISTRATION.md](publication/PREREGISTRATION.md)).
- **Problems recorded:** every flaw we found in our own measurements, and its fix, is in
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
```

## Documentation

- [publication/FULL-RESULTS.md](publication/FULL-RESULTS.md): every result in detail, with setups and caveats
- [docs/gpu-compaction.md](docs/gpu-compaction.md): what is offloaded, how it plugs into RocksDB, eligibility, configuration and limits
- [docs/remote-compaction.md](docs/remote-compaction.md): remote compaction and replication over TCP and RDMA, and their results
- [publication/RELATED-WORK.md](publication/RELATED-WORK.md): earlier GPU, FPGA and remote compaction designs, measured head to head on this harness
- [docs/runbook.md](docs/runbook.md): building, benchmarks, the `labctl` harness, hardware gates
- [docs/telemetry.md](docs/telemetry.md): resource and energy telemetry
- [publication/](publication/): results, methodology audit and data
