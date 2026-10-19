# GPU-Accelerated RocksDB Compaction Results

> **Superseded for publication.** An independent re-analysis of the raw data is
> in [`../publication/RESULTS.md`](../publication/RESULTS.md). Several claims
> below are corrected in [`../publication/AUDIT.md`](../publication/AUDIT.md):
> - "Compaction CPU time" counts only the compaction thread; whole-process CPU
>   rises 2–3%.
> - "Compaction p99" is the slowest job in each trial.
> - The compaction share of the window is 21/28/35%, not 26/35/44%.
> - The 5,000 writes/s foreground-latency improvement is not supported.

## Summary

The final GPU implementation improves RocksDB compaction while preserving
complete RocksDB semantics. The original 100%-write stress test establishes
the maximum compaction benefit. A separate paired matrix establishes where
that benefit survives realistic mixed read/write traffic.

Across 3,000–5,000 writes per second:

- Median complete-compaction time is **17.8–21.9% lower** than native CPU.
- Total complete-compaction time is **15.4–18.8% lower**.
- Compaction CPU time is **31.8–36.6% lower**.
- Mean GPU compaction p99 is **87.0–90.0 ms**.
- Every GPU p99 measurement in the final paired sweep is below 100 ms.

An independent 5,000 writes/s confirmation produced GPU p99 values of
**83.51 ms, 83.26 ms, and 83.36 ms**, reaching native-p99 parity under normal
execution while preserving approximately a 23% median advantage.

Across 66 valid mixed-workload confirmation trials:

- GPU median compaction time is **3.5–20.0% lower**.
- GPU total compaction time is **5.4–21.9% lower**.
- GPU compaction CPU time is **21.5–39.9% lower**.
- The clearest repeatable mixed-workload result occurs for update-heavy
  **5,334–6,667/s** and balanced **8,000–10,000/s** traffic: compaction and
  compaction CPU cost fall while foreground p99 changes by only 4–12%.
- At the highest requested write pressure, write-heavy and read-dominant
  traffic exceed an operational boundary and must not be compared by averages.

## Test environment

| Item | Configuration |
|---|---|
| Host | Zyra |
| GPU | NVIDIA RTX A6000 |
| CUDA | 12.3 |
| nvCOMP | 5.3.0.16 |
| Storage | Local Btrfs |
| Compression | LZ4 |

## Stress workload

The GPU advantage is repeatable for this compaction stress workload:

| Factor | Value |
|---|---:|
| Workload | 100% writes (synthetic stress case) |
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

This configuration should be interpreted as a compaction saturation test,
similar to sustained ingestion, bulk loading, index construction, log
materialization, or a write backlog. It should not be used to claim that the
GPU improves ordinary read/write application workloads.

## Realistic mixed workloads

The mixed-workload experiment uses the same database layout and compaction
settings as the stress test. Rates were initially matched by write pressure,
with 64 synchronous clients and an 8,192-entry queue. Every retained point was
then run for three paired native/GPU repetitions.

### Operational boundaries

| Workload | Mix | Retained total rates | Excluded boundary |
|---|---:|---:|---|
| Write-heavy ingestion | 10% reads / 90% writes | 3,334 and 4,445/s | 5,556/s: GPU queue p99 467 ms in screening |
| Update-heavy service | 25% reads / 75% writes | 4,000, 5,334, and 6,667/s | None |
| Balanced transactional | 50% reads / 50% writes | 6,000, 8,000, and 10,000/s | None |
| Read-dominant serving | 90% reads / 10% writes | 10,000, 15,000, and 20,000/s | 25,000/s: 91% in-window; 30,000/s: 70–72% in-window |

The requested read-dominant rates of 30,000–50,000/s are outside the system's
capacity with either backend. At 40,000 and 50,000/s, requests reach the
five-second deadline. A lower-rate capacity sweep established 20,000/s as the
highest clean point and 25,000/s as the overload boundary.

All 66 retained confirmation trials completed successfully. At least 99.89% of
offered operations completed inside the measurement window, pending compaction
bytes ended at zero, and every eligible GPU batch stayed on the GPU.

### Mixed-workload compaction results

Each value is the mean of three paired repetitions. Improvement is relative to
native CPU.

| Workload | Total rate | Approx. writes/s | Native p50 | GPU p50 | p50 improvement | Total-time improvement | CPU-time improvement |
|---|---:|---:|---:|---:|---:|---:|---:|
| Write-heavy | 3,334/s | 3,000/s | 65.22 ms | **52.49 ms** | **19.5%** | **16.5%** | **35.4%** |
| Write-heavy | 4,445/s | 4,000/s | 65.94 ms | **52.78 ms** | **20.0%** | **21.9%** | **39.9%** |
| Update-heavy | 4,000/s | 3,000/s | 65.22 ms | **62.91 ms** | **3.5%** | **5.4%** | **21.5%** |
| Update-heavy | 5,334/s | 4,000/s | 65.83 ms | **57.37 ms** | **12.8%** | **12.6%** | **30.8%** |
| Update-heavy | 6,667/s | 5,000/s | 65.92 ms | **56.78 ms** | **13.9%** | **12.3%** | **33.5%** |
| Balanced | 6,000/s | 3,000/s | 65.06 ms | **52.28 ms** | **19.6%** | **16.0%** | **33.7%** |
| Balanced | 8,000/s | 4,000/s | 65.77 ms | **54.28 ms** | **17.5%** | **14.7%** | **34.6%** |
| Balanced | 10,000/s | 5,000/s | 65.88 ms | **58.66 ms** | **11.0%** | **8.9%** | **25.0%** |
| Read-dominant | 10,000/s | 1,000/s | 65.47 ms | **56.39 ms** | **13.9%** | **11.4%** | **29.8%** |
| Read-dominant | 15,000/s | 1,500/s | 65.89 ms | **55.97 ms** | **15.1%** | **13.7%** | **32.0%** |
| Read-dominant | 20,000/s | 2,000/s | 67.68 ms | **61.25 ms** | **9.5%** | **7.1%** | **25.9%** |

GPU compaction is cheaper at every retained point. The benefit is largest once
there is enough write traffic to form regular full GPU output batches, but
before foreground queueing overwhelms the system.

### Foreground latency

| Workload | Rate | Native read p99 | GPU read p99 | Native write p99 | GPU write p99 |
|---|---:|---:|---:|---:|---:|
| Write-heavy | 3,334/s | **23.65 ms** | 84.40 ms | **31.02 ms** | 94.17 ms |
| Write-heavy | 4,445/s | 168.68 ms | **12.09 ms** | 176.47 ms | **18.07 ms** |
| Update-heavy | 4,000/s | **8.92 ms** | 9.90 ms | **16.60 ms** | 17.69 ms |
| Update-heavy | 5,334/s | **12.35 ms** | 13.78 ms | **17.52 ms** | 18.27 ms |
| Update-heavy | 6,667/s | **15.46 ms** | 16.44 ms | **18.73 ms** | 19.43 ms |
| Balanced | 6,000/s | **160.44 ms** | 187.67 ms | **173.97 ms** | 198.99 ms |
| Balanced | 8,000/s | **12.83 ms** | 13.96 ms | **17.66 ms** | 18.39 ms |
| Balanced | 10,000/s | **16.10 ms** | 17.25 ms | **18.80 ms** | 19.93 ms |
| Read-dominant | 10,000/s | **1.23 ms** | 1.26 ms | **10.02 ms** | 11.11 ms |
| Read-dominant | 15,000/s | 8.11 ms | **3.49 ms** | 16.73 ms | **14.89 ms** |
| Read-dominant | 20,000/s | 104.25 ms | **16.23 ms** | 108.73 ms | **20.39 ms** |

The 3,334/s write-heavy, 4,445/s write-heavy, and 6,000/s balanced means each
contain an isolated queueing outlier. At the 4,445/s write-heavy point, the
native outlier reached 478 ms queue p99; the three GPU repetitions remained at
11–12 ms. At 3,334/s, one GPU repetition reached 229 ms, while the other two
were approximately 8 ms. At balanced 6,000/s, one repetition in each backend
reached 465–541 ms. These points demonstrate sensitivity to transient
queueing, not a deterministic backend effect.

The read-dominant means at 15,000 and 20,000/s also require caution. One native
repetition reached 16 ms queue p99 at 15,000/s and another reached 281 ms at
20,000/s. The remaining paired repetitions were comparable or favored native
CPU for foreground latency. The lower GPU means therefore do not establish a
repeatable foreground-latency advantage.

The repeatable result is:

- **Update-heavy 5,334–6,667/s and balanced 8,000–10,000/s:** GPU lowers
  compaction cost and CPU time, while foreground p99 is 4–12% higher in
  absolute ranges that remain below 20 ms.
- **Read-dominant 10,000–20,000/s:** GPU lowers compaction and compaction CPU
  cost, but the apparent mean foreground benefit at the two higher rates is
  outlier-driven.
- **Write-heavy:** GPU compaction is consistently faster, but foreground tail
  behavior is outlier-sensitive near the tested capacity boundary.

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
| Mixed-workload confirmation trials | 66 of 66 valid |
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

The results do not establish an advantage for small blocks or workloads where
compaction is infrequent. The mixed tests are short, single-host runs and show
isolated queueing outliers; longer-duration testing is required before making
a production capacity claim.

The original 3,000–5,000 writes/s matching target is not attainable for
read-dominant traffic on this host because 30,000 total ops/s already exceeds
capacity. Its demonstrated range is therefore 1,000–2,000 writes/s.

## Conclusion

GPU-assisted RocksDB compaction is consistently better than native CPU in
median and aggregate compaction cost. In the synthetic saturation test:

- **17.8–21.9% lower median complete-compaction time**
- **15.4–18.8% lower total complete-compaction time**
- **31.8–36.6% lower compaction CPU time**
- **87.0–90.0 ms mean p99 across the full sweep**
- **83.26–83.51 ms p99 in the independent 5,000 writes/s confirmation**

In realistic mixed traffic, the GPU reduces median compaction by **3.5–20.0%**,
total compaction time by **5.4–21.9%**, and compaction CPU time by
**21.5–39.9%**. The strongest repeatable mixed-workload range is update-heavy
traffic at 5,334–6,667/s and balanced traffic at 8,000–10,000/s. These ranges
retain the compaction and CPU benefit with small absolute foreground tail
regressions. Apparent foreground wins in the read-dominant means and
write-heavy foreground tails are sensitive to isolated queueing outliers.

The final asynchronous pipeline therefore establishes a broader compaction
advantage, but does not establish a repeatable application-latency advantage.
Capacity limits and foreground tail behavior must be evaluated for each
workload mix.

## Reproduction

Dataset:

[`../configs/gpu-compaction-bottleneck-128k.json`](../configs/gpu-compaction-bottleneck-128k.json)

Experiment:

[`../configs/cuda-compaction-bottleneck-sweep.json`](../configs/cuda-compaction-bottleneck-sweep.json)

Mixed-workload experiments:

- [`../configs/cuda-realistic-write-heavy-confirm.json`](../configs/cuda-realistic-write-heavy-confirm.json)
- [`../configs/cuda-realistic-update-heavy.json`](../configs/cuda-realistic-update-heavy.json)
- [`../configs/cuda-realistic-balanced.json`](../configs/cuda-realistic-balanced.json)
- [`../configs/cuda-realistic-read-dominant-capacity.json`](../configs/cuda-realistic-read-dominant-capacity.json)

Remote result bundles:

- Final paired sweep: `/tmp/sa29ali/gpu-tail-final`
- Independent 5,000 writes/s confirmation:
  `/tmp/sa29ali/gpu-tail-5k-confirm/run`
- Mixed-workload screening:
  `/tmp/sa29ali/gpu-realistic-screen-64b`
- Read-dominant capacity screen:
  `/tmp/sa29ali/gpu-realistic-read-capacity/run`
- Mixed-workload three-repetition confirmation:
  `/tmp/sa29ali/gpu-realistic-confirm`
