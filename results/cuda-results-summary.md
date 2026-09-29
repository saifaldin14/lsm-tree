# GPU-Accelerated RocksDB Compaction Results

## Summary

The final GPU implementation improves RocksDB compaction on the tested
write-heavy workload while preserving complete RocksDB semantics.

Across 3,000–5,000 writes per second:

- Median complete-compaction time is **17.8–21.9% lower** than native CPU.
- Total complete-compaction time is **15.4–18.8% lower**.
- Compaction CPU time is **31.8–36.6% lower**.
- Mean GPU compaction p99 is **87.0–90.0 ms**.
- Every GPU p99 measurement in the final paired sweep is below 100 ms.

An independent 5,000 writes/s confirmation produced GPU p99 values of
**83.51 ms, 83.26 ms, and 83.36 ms**, reaching native-p99 parity under normal
execution while preserving approximately a 23% median advantage.

## Test environment

| Item | Configuration |
|---|---|
| Host | Zyra |
| GPU | NVIDIA RTX A6000 |
| CUDA | 12.3 |
| nvCOMP | 5.3.0.16 |
| Storage | Local Btrfs |
| Compression | LZ4 |

## Workload

The GPU advantage is repeatable for this compaction-heavy workload:

| Factor | Value |
|---|---:|
| Workload | 100% writes |
| Offered rates | 3,000, 4,000, and 5,000 operations/s |
| Value size | 1 KiB |
| RocksDB block size | 128 KiB |
| Write buffer | 256 KiB |
| Target SST size | 8 MiB |
| Background jobs | 1 |
| Levels | 4 |
| Warmup | 2 seconds |
| Measurement | 8 seconds |
| Repetitions | 3 paired repetitions per rate |

The 256 KiB write buffer creates frequent compactions. The 128 KiB blocks
provide enough work per GPU batch to amortize dispatch and transfer costs. A
single background job keeps compaction pressure visible and avoids overlapping
jobs obscuring the comparison.

## Final implementation

RocksDB retains semantic processing on the CPU, including:

- Key parsing
- Merge processing
- Compaction filters
- Snapshots and sequence-number behavior
- Tombstones
- Timestamps and comparators
- Index construction
- Block-handle and file-layout management

The GPU performs batched output LZ4 compression and CRC32C generation after
RocksDB finalizes each data block.

The final output pipeline includes:

- An owning asynchronous output-submission API.
- One 64-block or 8 MiB GPU submission while RocksDB constructs later blocks.
- At most two submissions in flight across two CUDA streams.
- Strict FIFO completion to preserve SST and index order.
- A typical 129-block job split into 64 + 65 blocks, avoiding a one-block
  tail dispatch.
- Pre-sized pinned host buffers, device buffers, and nvCOMP workspace.
- Cached nvCOMP maximum-output and workspace requirements.
- A 128-thread output-copy and CRC32C kernel per RocksDB block.
- Event-based completion without device-wide synchronization.

Every eligible output batch executes on the GPU. CPU output fallback is not
used in the final qualification runs.

## Complete-compaction performance

Each value below is the mean of three paired repetitions.

| Rate | Native CPU p50 | GPU p50 | GPU p50 improvement | Native CPU p99 | GPU p99 |
|---:|---:|---:|---:|---:|---:|
| 3,000/s | 65.04 ms | **51.00 ms** | **21.6%** | **83.37 ms** | 87.64 ms |
| 4,000/s | 65.08 ms | **53.49 ms** | **17.8%** | **81.55 ms** | 86.99 ms |
| 5,000/s | 66.26 ms | **51.76 ms** | **21.9%** | **81.69 ms** | 89.99 ms |

GPU median complete-compaction time is lower at every tested rate and in every
paired repetition.

## Aggregate compaction cost

| Rate | Native total time | GPU total time | GPU improvement | Native CPU time | GPU CPU time | GPU CPU-time improvement |
|---:|---:|---:|---:|---:|---:|---:|
| 3,000/s | 2,103.83 ms | **1,722.23 ms** | **18.1%** | 1,622.81 ms | **1,053.89 ms** | **35.1%** |
| 4,000/s | 2,823.79 ms | **2,387.58 ms** | **15.4%** | 2,179.50 ms | **1,486.45 ms** | **31.8%** |
| 5,000/s | 3,539.13 ms | **2,872.24 ms** | **18.8%** | 2,735.70 ms | **1,733.72 ms** | **36.6%** |

GPU cumulative compaction time and compaction CPU time are lower in every
paired comparison.

## Compaction as a workload bottleneck

Before GPU acceleration, compaction occupied a significant portion of each
8-second measurement window:

| Rate | Native compaction share of measurement window |
|---:|---:|
| 3,000/s | 26.3% |
| 4,000/s | 35.3% |
| 5,000/s | 44.2% |

At 5,000 writes/s, native compaction consumed nearly half of the measurement
window. This is the point where reducing aggregate compaction cost can also
improve foreground write behavior.

## Foreground write latency

| Rate | Native write p50 | GPU write p50 | Native write p99 | GPU write p99 |
|---:|---:|---:|---:|---:|
| 3,000/s | **1.33 ms** | 1.39 ms | **17.38 ms** | 18.22 ms |
| 4,000/s | 1.90 ms | **1.78 ms** | **20.47 ms** | 21.15 ms |
| 5,000/s | 4.47 ms | **3.56 ms** | 28.37 ms | **26.80 ms** |

At 3,000–4,000 writes/s, foreground latency is approximately equal while GPU
reduces background compaction cost. At 5,000 writes/s, GPU improves both
median and p99 write latency.

## Independent 5,000 writes/s confirmation

An additional three-pair run tested the boundary closest to the 90 ms p99
target.

### GPU results

| Repetition | Compaction p50 | Compaction p99 | Total compaction time |
|---:|---:|---:|---:|
| 1 | 51.00 ms | 83.51 ms | 2,786.78 ms |
| 2 | **50.81 ms** | **83.26 ms** | 2,786.79 ms |
| 3 | 51.59 ms | 83.36 ms | 2,841.01 ms |

All three GPU repetitions remained stable:

- p50 range: **50.81–51.59 ms**
- p99 range: **83.26–83.51 ms**
- Total compaction range: **2,786.78–2,841.01 ms**

One native repetition experienced a separate 325.57 ms outlier. The other two
native p99 values were 80.81 ms and 79.67 ms.

## Correctness and validation

| Validation | Result |
|---|---:|
| CUDA-enabled CTest suite | 9 of 9 passed |
| Python test suite | 17 of 17 passed |
| Final paired performance trials | 18 of 18 valid |
| Independent 5,000-rate trials | 6 of 6 valid |
| Final sweep files audited | 871 |
| Async compute-sanitizer tests | 6 passed |
| CUDA memory errors | 0 |
| Failed requests | 0 |
| Payload mismatches | 0 |
| Full payload verification | Passed |
| Native RocksDB checksum verification | Passed |
| CPU output fallback for eligible batches | 0 |

The tests cover:

- Output LZ4 decoding and exact byte comparison
- GPU-generated CRC32C verification
- Two simultaneous output submissions
- One-shot submission completion
- Job-wide input batching
- Mixed eligible and ineligible output ordering
- Snapshots and deletes
- Database reopen
- SST iteration and native checksum verification

## Remaining limitation

The final GPU path meets the 90 ms mean-p99 target and reaches approximately
83.4 ms p99 in the independent 5,000 writes/s confirmation. Native CPU can
still have slightly lower individual compaction-job p99 in normal runs.

The GPU advantage is strongest in:

- Median compaction latency
- Aggregate compaction time
- Compaction CPU consumption
- Sustained write-heavy workloads where compaction occupies a significant
  share of execution time

The results do not establish an advantage for small blocks, read-heavy
workloads, or workloads where compaction is infrequent.

## Conclusion

For the identified compaction-heavy workload, GPU-assisted RocksDB compaction
is consistently better than native CPU in median and aggregate cost:

- **17.8–21.9% lower median complete-compaction time**
- **15.4–18.8% lower total complete-compaction time**
- **31.8–36.6% lower compaction CPU time**
- **87.0–90.0 ms mean p99 across the full sweep**
- **83.26–83.51 ms p99 in the independent 5,000 writes/s confirmation**

The final asynchronous pipeline closes the practical tail-latency gap without
sacrificing the GPU median advantage or moving eligible work back to the CPU.

## Reproduction

Dataset:

[`../configs/gpu-compaction-bottleneck-128k.json`](../configs/gpu-compaction-bottleneck-128k.json)

Experiment:

[`../configs/cuda-compaction-bottleneck-sweep.json`](../configs/cuda-compaction-bottleneck-sweep.json)

Remote result bundles:

- Final paired sweep: `/tmp/sa29ali/gpu-tail-final`
- Independent 5,000 writes/s confirmation:
  `/tmp/sa29ali/gpu-tail-5k-confirm/run`
