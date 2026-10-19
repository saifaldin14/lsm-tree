# LSM Lab: GPU-offloaded RocksDB compaction

LSM Lab moves RocksDB 11.8 compaction onto an NVIDIA GPU and measures what
that buys the database. The GPU reads, decompresses, merges and garbage-collects
the input, then builds, compresses and checksums the output data blocks. RocksDB
keeps file cuts, indexes, filters, metadata and installation. Output is
checked against native RocksDB for every stored key version and through point
lookups.

![Whole database: batched writes on 4 CPU cores](publication/figures/3_database_batched.png)

## Results

Measured on Zyra (Xeon Gold 5220, RTX A6000, data on `/dev/shm`), against
RocksDB built with hardware CRC32C. Full write-up:
[publication/RESULTS.md](publication/RESULTS.md); plain-language version:
[publication/PLAIN-SUMMARY.md](publication/PLAIN-SUMMARY.md).

**A single compaction is 2.3–6.1× faster and uses 2.9–7.3× less CPU.** The
gain is largest for small values:

| Values | GPU compression only | GPU merge | GPU merge + block building |
|---|---|---|---|
| 32 B | 1.12× faster | 1.74× | **6.06× faster, 7.3× less CPU** |
| 128 B | 1.22× | 1.97× | **4.63×, 5.2× less CPU** |
| 1 KB | 1.09× | 1.76× | **2.70×, 2.9× less CPU** |
| 128 B, no compression | 0.99× | 1.54× | **2.32×, 2.9× less CPU** |

**The whole database is faster when compaction is the bottleneck.** 50 M keys,
128-byte values, 4 CPU cores. Throughput counts the time to finish all the
compaction the writes caused:

| | Native RocksDB | GPU merge + block building |
|---|---|---|
| Batched writes (100 per batch) | 349k ops/s, p99 7.1 ms | **602k ops/s (1.72×), p99 1.1 ms** |
| Single writes | 121k ops/s | **232k ops/s (1.92×)** |
| Compaction left over after the run | 68–114 s | 3–4 s |

Offloading only compression and checksums, this project's original design,
gives at most 1.2× on a single compaction and does not help the database. The whole-database numbers are single runs; repetitions, energy
measured through the end of catch-up compaction, and a fix for CPU fallback on
very large jobs are still to do.

## Repository

| Path | Contents |
|---|---|
| `src/gpu/`, `include/lsm_lab/` | GPU merge and block building (`cuda_merge.cu`), CPU reference (`merge_cpu.cpp`), output compression (`cuda_transform.cu`), Metal backend, RocksDB integration headers |
| `cmake/ApplyEnginePatch.cmake` | Version-locked patch that adds the offload hooks to RocksDB |
| `apps/` | `lsm_compaction_bench`, `lsm_system_bench`, `lsm_burn`, and the harness services (`lsm_db`, `lsm_worker`, `lsm_loadgen`, `lsm_admin`, `lsm_telemetry`, `lsm_gpu_check`) |
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

```sh
export LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024
publication/compaction_sweep.sh artifacts/compaction                     # isolated compaction
build/dev/lsm_system_bench --phase preload --dir /dev/shm/$USER/sys/golden --records 50000000 --threads 8
LSM_BUILD_DIR=build/dev python3 publication/system_campaign.py artifacts/database \
  --golden /dev/shm/$USER/sys/golden --grid configs/system/batched-ingest.json
python3 publication/make_figures.py                                      # figures from publication/data/
```

## Documentation

- [docs/gpu-compaction.md](docs/gpu-compaction.md): what is offloaded, how it plugs into RocksDB, eligibility, configuration and limits
- [docs/runbook.md](docs/runbook.md): building, benchmarks, the `labctl` harness, hardware gates
- [docs/telemetry.md](docs/telemetry.md): resource and energy telemetry
- [publication/](publication/): results, methodology audit and data
