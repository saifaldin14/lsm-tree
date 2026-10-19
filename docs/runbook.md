# Build and Run

## Native setup

The measured implementation is C++20. Python 3.12 or newer is used only for coordination, artifact checks, and plotting. Linux and macOS are supported targets; Linux and CUDA builds have not been executed in the current Mac session.

```sh
bash scripts/bootstrap.sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.lock
.venv/bin/python -m unittest discover -s tests -p test_labctl.py -v
```

If the build directory was created before the vcpkg toolchain was selected, run `cmake --fresh --preset dev` once. This resets generated configuration, not source or datasets. Do not run another command in the same terminal while a long build is still active. Optional GPU hardware tests may be skipped when devices are unavailable; that is not a passed GPU gate.

The dependency bootstrap uses the fixed vcpkg revision and a whitespace-free user cache. It does not install system drivers or require elevated privileges. Linux needs a C++20 compiler, CMake, Make, Git, curl, zip/unzip, pkg-config, and the normal development utilities required by the vcpkg ports.

## Local pilot

All output names below must be new. The run creates and retains a baseline and separate trial copies. A completed run bundle is immutable; analysis writes elsewhere.

```sh
mkdir -p artifacts
.venv/bin/python tools/labctl.py plan configs/local-experiment.json artifacts/pilot-plan.json
.venv/bin/python tools/labctl.py run artifacts/pilot-plan.json artifacts/pilot-run
.venv/bin/python tools/labctl.py audit artifacts/pilot-run
.venv/bin/python tools/labctl.py analyze artifacts/pilot-run artifacts/pilot-analysis
```

The checked-in profile has eight trials and a 300-second wall budget. It tests functionality rather than long-run stability. Existing evidence from this session is under `artifacts/local-cpu-run`; figures are under `artifacts/local-cpu-analysis-v3`. Those results predate the CRC32C/batch engine profile and must not be presented as measurements of the new GPU or CPU-batched path.

Treatment names are `native_cpu` (compaction on the primary), `cpu_native` (native compaction on the remote worker), `cpu_batched`, `metal_batched`, and `cuda_batched`. The last three use the same bounded block-input path. A requested unavailable backend fails clearly. Local processes share one host and storage device, so they cannot qualify a cross-host claim.

Configuration contains integer units, explicit warmup/measurement durations, repetition count, SLOs, workload seed, rates, and budgets. The frozen plan includes its hash and exact randomized order. Logical-write budgets count user key/value bytes, not SSD writes or compaction amplification. The memory budget is a conservative admission estimate, not an OS-enforced memory quota. Artifact budgets include request logs and bounded resource streams; filesystem free-space checks and host quotas remain necessary for database copies and amplification.

The load generator produces arrivals independently of completions. Keys are routed to bounded FIFO lanes to preserve per-key ordering. Queue rejection, pre-send expiry, RPC failures, payload mismatches, and indeterminate writes are counted. No write retries are hidden; later requests on a key with an indeterminate write are blocked and recorded. A failed or indeterminate trial cannot qualify. Native client timing includes intended-arrival delay and client queueing; server-only timings are not used as request latency.

The current workload implements uniform Get/update mixes, including pure reads or writes through `read_percent`. It does not implement the proposed Zipfian condition yet. Raw request logging and value checks are always enabled, so their overhead is included. All writes use synchronous WAL durability. Warmup has separate counters and is excluded from measured histograms. Administrative drain and native checksums happen after the offered-load interval. Offline verification reopens the stopped database and compares every full value with acknowledged final versions.

Analysis audits checksums and paired schedule hashes, retains unsuccessful outcomes, uses the contiguous qualifying rate prefix, and reports lower/upper capacity bounds. It requires at least three in-window compaction-debt samples and no observed debt growth for pilot qualification. This is not proof of steady state. Whole paired repetitions are bootstrapped; individual requests are not replicates. Censored capacities are never silently converted into exact ratios. GPU treatments require recorded useful GPU block work.

## Resource collection

Every new local trial has primary/load process logs, a worker log when used, and one host log shared by the local roles. Analysis includes lifetime and conservative in-window resource summaries, with no inferred readings for old bundles. The standalone `build/dev/lsm_telemetry` executable also runs on a storage host without a database or RPC service. Build it on all four hosts before remote qualification.

See [telemetry.md](telemetry.md) for sources, sensor requirements, schema, individual-log analysis, and operator-managed macOS power capture. Missing power, energy, or Celsius readings are explicit. GPU memory may overlap host RSS, device bytes do not measure internal NAND traffic, and clocks from different machines cannot be substituted for each other.

## GPU gates

Metal is enabled by default on native macOS. The shader is compiled during the build and embedded into the host binary for runtime pipeline creation. The explicit hardware command is:

```sh
build/dev/lsm_gpu_check metal_batched --require
```

The hardware command and live compaction tests passed on Apple M3 in this session. `MTLCreateSystemDefaultDevice` returned no default device, so the backend selected the Apple M3 exposed by `MTLCopyAllDevices` and recorded its registry identity. There is no software device substitute. The current kernels favor bounded correctness over tuning, and no speedup is assumed. Run this gate on each actual worker before accepting its GPU results.

[../configs/local-metal-experiment.json](../configs/local-metal-experiment.json) is a bounded eight-trial profile comparing primary CPU, remote native CPU, CPU-batched, and Metal on one machine. Its separate [../configs/local-compaction-smoke.json](../configs/local-compaction-smoke.json) dataset profile uses 64 KiB memtables and target SST sizes to exercise compaction during the offered-load interval. It uses one offered rate and cannot establish a capacity curve or cross-host speedup.

The recorded run under `artifacts/local-metal-active-run` completed 48,000 measured requests and 6,000 warmup requests with full offline verification across all trials. The two Metal trials processed 513 and 511 blocks between in-window samples; each committed four remote compactions during that observed interval. Every treatment exceeded the declared 10 ms p99 SLO at 750 ops/s. See `artifacts/local-metal-active-analysis` for CSV/JSON and figures. The earlier `artifacts/local-metal-run` had GPU work only during post-load drain; its corrected analysis under `artifacts/local-metal-analysis-v3` marks the remote treatments unverified for foreground acceleration. Both runs remain preserved.

CUDA uses the official nvCOMP 5.3.0.16 CUDA-12 package, pinned by SHA-256. The CUDA decompressor is selected explicitly; the dedicated hardware decompression mode is not used because its malformed-input guarantees differ. Input CRC32C is computed on-device before decoding, and both per-block status and output length are checked. Device copies, allocation, synchronization, and output materialization are real work inside worker elapsed time.

`cuda_batched` always dispatches to CUDA and is retained for hardware
qualification and crossover measurements. The in-process `cuda_local` mode
uses `cuda_hybrid`: batches with fewer than 32 blocks, less than 2 MiB of
decoded data, or less than 32 KiB average decoded block size remain on CPU.
The selector records `cpu_fallback_batches`, `cpu_fallback_blocks`, and
`cpu_fallback_bytes`. These thresholds come from RTX A6000 microbenchmarks;
4 KiB blocks remained faster on CPU even at 256 blocks. Re-profile when the
GPU, codec, or block-size distribution changes:

```sh
build/dev/lsm_gpu_check cpu_batched --blocks 256 --block-bytes 4096 --repetitions 100
build/dev/lsm_gpu_check cuda_batched --require --blocks 256 --block-bytes 4096 --repetitions 100
```

On an approved native Linux x86_64 NVIDIA worker with an existing compatible driver and CUDA toolkit:

```sh
bash scripts/bootstrap.sh
bash scripts/bootstrap-cuda.sh
cmake --preset dev -DLSM_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=native
cmake --build --preset dev
build/dev/lsm_gpu_check cuda_batched --require
ctest --preset dev -R 'blocks.transforms|worker.lifecycle|hardware.cuda' --output-on-failure
```

After the hardware gate passes, the bounded paired capacity sweep compares
native CPU, CPU-batched, and CUDA-batched compaction at 750, 3,000, 6,000, and
12,000 operations per second:

```sh
mkdir -p artifacts
python3 tools/labctl.py plan configs/cuda-rate-sweep.json artifacts/cuda-rate-sweep-plan.json
python3 tools/labctl.py run artifacts/cuda-rate-sweep-plan.json artifacts/cuda-rate-sweep-run
python3 tools/labctl.py audit artifacts/cuda-rate-sweep-run
python3 tools/labctl.py analyze artifacts/cuda-rate-sweep-run artifacts/cuda-rate-sweep-analysis
```

To qualify the in-process CUDA path without starting a remote worker, use
`configs/cuda-local-smoke.json`. A valid write-bearing `cuda_local` trial must
report nonzero `database_statistics.local_transform.blocks` during the
measured window. On Rift machines, keep result artifacts under the persistent
home directory but place the live RocksDB database on local storage:

```sh
mkdir -p /tmp/$USER/lsm-lab
export LSM_LAB_SCRATCH_ROOT=/tmp/$USER/lsm-lab
```

The runner copies the verified database state into the sealed result bundle
and removes its uniquely named scratch directory after the run.

The sweep intentionally retains overload and SLO failures. Its three paired
repetitions and increasing offered rates establish tested capacity brackets;
they do not replace longer steady-state runs.

The CUDA source has not been compiled or executed here. Its API calls were checked against the downloaded, hash-verified official headers. The Linux hardware gate must still establish compiler/runtime compatibility, real dispatch, malformed-input handling, and live-compaction correctness. Record the actual compiler, driver, toolkit, nvCOMP package, and GPU allocation before experiments. GPU selection currently uses the first visible device; use an exclusive allocation and `CUDA_VISIBLE_DEVICES` for CUDA. Do not pool CUDA and Metal speedup results across different hardware suites.

## Cross-host qualification

There are no approved remote hosts in this session. Fill an inventory from the example only after arranging separate load, primary, worker, and storage hosts. Install the same tested source version and pinned engine patch on the service hosts, and build native binaries on each architecture. Python host probes require Python 3.12 or newer. The required work parents and NFS mount must already exist and be writable by the experiment account.

NFS source, canonical path, and observed protocol must match on primary and worker. Use hard mounts and a synchronous export with restricted private clients. Native Mac NFS support must be observed, not inferred from Linux capabilities. The example selects NFSv3 as a candidate, not as a validated deployment; NLM locking must pass the actual two-client test. No script formats disks, edits exports, changes mounts, or installs drivers.

mTLS files are provisioned separately. The CA and certificate/key paths must exist on each host; private keys never enter a run bundle. Server certificates need SANs matching their configured private IP addresses. The primary certificate must permit both server and client authentication because it connects to the worker. SSH host keys must already be trusted; batch mode and strict host-key checking are enforced. Never put passwords or tokens in the inventory.

```sh
.venv/bin/python tools/labctl.py deployment-plan infra/inventory.local.json artifacts/deployment-plan-v1.json
.venv/bin/python tools/labctl.py qualify infra/inventory.local.json artifacts/cluster-qualification --apply
```

The second command also requires `approved: true` and a nonempty `approval_id` in the inventory. It verifies distinct machine identities and the declared NFS mount, requires a real GPU dispatch, writes a full-byte probe, checks lock exclusion across clients, and tests rename visibility. It then runs native CPU, remote CPU, CPU-batched, and GPU smoke trials over the shared storage, with full-value verification on the primary after shutdown. It retains failures and attempts bounded cleanup of only the processes it started. Probe data and trial directories are preserved for inspection; there is no automatic recursive deletion.

This is a functional deployment gate, not the long confirmation study. Physical power-loss durability, deliberate NFS server disconnection/reconnect, production load capacity, and the proposed hundreds-of-hours study remain separate approval and validation tasks. Missing devices or hosts remain unavailable; local loopback evidence is never promoted to cross-host evidence.

## Result and failure handling

Successful load execution exits zero. A fully recorded load with failed accounting/qualification exits 3. Invalid setup or execution failure exits 1. A GPU availability probe without `--require` exits 77 if unavailable, which CTest reports as skipped; with `--require`, unavailability is failure. Failed experiment rows remain in the run and analysis.

Inspect `commands.json`, per-trial process logs, `primary.events.jsonl`, raw request files, and native worker journals. `output_ready`, `outputs_imported`, and `committed` are different phases. A nominal worker result alone is never treated as a committed primary database. Interrupted output directories are retained and cannot be reused as fresh attempts.
## GPU compaction tail-latency sweep

The confirmed compaction-pressure workload uses 128 KiB blocks, 1 KiB values,
a 256 KiB write buffer, one background job, and a pure-write workload.

```bash
python3 tools/labctl.py plan \
  configs/cuda-compaction-bottleneck-sweep.json \
  /tmp/$USER/cuda-compaction-bottleneck-plan.json

LSM_LAB_SCRATCH_ROOT=/tmp/$USER/lsm-lab \
LSM_DISABLE_GPU_INPUT=1 \
python3 tools/labctl.py run \
  /tmp/$USER/cuda-compaction-bottleneck-plan.json \
  /tmp/$USER/cuda-compaction-bottleneck-run

python3 tools/labctl.py audit \
  /tmp/$USER/cuda-compaction-bottleneck-run
```

The production output path submits 64-block or 8 MiB batches asynchronously,
keeps at most two submissions in flight, and drains output in FIFO order.
Do not set `LSM_FORCE_CPU_OUTPUT` or `LSM_DISABLE_GPU_OUTPUT` for the GPU
qualification run.

## Revision experiments: sync mode, NUMA placement, profiling

`LSM_DISABLE_GPU_INPUT=1` is part of the published configuration: only output
compression and checksums are offloaded. With the input path enabled, compaction
jobs are slower than native CPU on Zyra. Every run now records all `LSM_*`
environment variables in `build.json` under `lsm_environment`.

Two opt-in controls support the placement and synchronization ablation:

- `LSM_CUDA_SYNC=auto|spin|blocking` selects how host threads wait for the GPU
  (`auto` is CUDA's default and the published behavior; `blocking` sets
  `cudaDeviceScheduleBlockingSync` and blocking-sync events). The chosen mode is
  appended to `local_transform.device` in each primary summary.
- `LSM_LAB_PRIMARY_NUMA_NODE=N` launches only `lsm_db` under
  `numactl --cpunodebind=N --membind=N`. On Zyra the GPU is attached to node 0.

```bash
export LSM_LAB_SCRATCH_ROOT=/tmp/$USER/lsm-lab
# Randomized blocks: 2 workloads x 5 conditions + write-heavy 5,556 ops/s, with RAPL energy.
.venv/bin/python publication/campaign.py /tmp/$USER/rev-campaign --blocks 6
# Per-thread/per-symbol CPU attribution for one paired trial (fixed-period cpu-clock sampling).
LSM_DISABLE_GPU_INPUT=1 publication/profile.sh /tmp/$USER/rev-profile-auto auto
```

`perf_event_paranoid` must allow system-wide sampling and RAPL events
(`power/energy-pkg/`, `power/energy-ram/`).

## Harness options (all optional; defaults reproduce earlier behavior)

Absent fields keep the canonical dataset spec, dataset fingerprints, and plan
checksums unchanged. Optional spec fields are written to `spec_json` only when
they differ from the default.

Dataset spec (`lsm_admin validate|create`):

| Field | Default | Meaning |
| --- | --- | --- |
| `engine.compression` | required | `lz4`, `zstd` (CPU only; GPU block transforms accept only LZ4/none), or `none` |
| `engine.max_subcompactions` | 1 | 1–256 |
| `engine.max_bytes_for_level_base` | `target_file_bytes * 4` | 64 KiB–1 TiB |
| `engine.max_write_buffer_number` | 4 | 2–64 |
| `engine.level0_file_num_compaction_trigger` / `level0_slowdown_writes_trigger` / `level0_stop_writes_trigger` | RocksDB defaults (4/20/36, not set) | 1–1024; effective values must satisfy compaction <= slowdown <= stop |
| `engine.use_direct_io` | false | sets `use_direct_reads` and `use_direct_io_for_flush_and_compaction` (not supported on tmpfs) |
| `engine.verify_output` | true | `false` selects the RocksDB defaults `paranoid_file_checks=false` and `verify_compression=false`. The GPU output path never performs the CPU decompress-and-compare that `verify_compression` adds, so use `false` when comparing CPU and GPU compaction cost |
| `dataset.load_mode` | `sync` | `bulk`: every generation written with the WAL disabled in 1 MiB batches from up to 16 parallel producers (64 MiB write groups), flushed per generation, uncapped compaction wait, more background jobs during creation only. `ingest`: only the final generation, written as sorted non-overlapping SSTs by up to 16 threads and ingested with `IngestExternalFile` (fully compacted shape, no obsolete versions or compaction history) |

Both fast modes produce exactly the same keys and final values (including
`overwrite_rounds`) as `sync` and publish the same manifest; `lsm_admin
verify` still checks every key and full value. `bulk` keeps the natural LSM
shape (all generations pass through flush and compaction); `ingest` is much
faster for tens of GB but starts from a fully compacted tree. `lsm_admin create` prints a
`creation` report (records/s, MiB/s, load and compaction-wait seconds) that is
not stored in `dataset.json`. `lsm_admin capabilities` reports `zstd`. The ZSTD
port is part of `vcpkg.json`; rerun `scripts/bootstrap.sh` once on existing hosts.

Load generator (`lsm_loadgen`):

- `--insert-percent P` (default 0): P% of writes insert a new key with index
  `dataset.records + request_id` (deterministic from the seed). The load
  manifest records `key_index_limit`; `lsm_admin verify-trial` checks every
  inserted key and value and requires the row count to equal records plus inserts.
- `--closed-loop N` selects saturation mode with N back-to-back clients
  (`--duration-seconds`, default 10; `--warmup-duration-seconds`, default 0).
  Each client owns the keys congruent to its index, so final versions stay
  verifiable. `result.json` has `throughput` (ops/reads/writes/inserts per
  second over the measurement window), `read_latency`/`write_latency` with
  p50/p95/p99/p999 (send to response), and a per-second `timeline`. The run
  stops and is marked invalid if it exhausts `--write-budget-bytes` or the
  expected-version memory budget. Open-loop options (`--rate`, `--requests`,
  `--threads`, ...) are rejected in this mode. Open loop remains the default.

Experiment configuration (`labctl plan|run`):

| Field | Meaning |
| --- | --- |
| `load_mode` | `open_loop` (default) or `closed_loop`. Closed loop replaces `rates`, `warmup_seconds`, `measurement_seconds`, `queue_capacity` with `duration_seconds` and `warmup_duration_seconds`; `threads` is the client count (1–1024). Each trial gets an equal share of `max_logical_write_bytes` after the baseline. `labctl analyze` reports sustained throughput per trial and paired throughput ratios. |
| `insert_percent` | passed to `lsm_loadgen --insert-percent` |
| `core_budget` | pins `lsm_db` and `lsm_worker` with `numactl --physcpubind=<cpus> --membind=<node>` to N physical cores (lowest hardware thread of each core from `/sys/devices/system/cpu/cpu*/topology`). Takes precedence over `LSM_LAB_PRIMARY_NUMA_NODE`. `lsm_loadgen` runs on the remaining online CPUs, excluding the sibling threads of the reserved cores. Requires Linux and `numactl`. |
| `numa_node` | node for `core_budget` (default 0) |
| `background_load_percent` | runs `lsm_burn` on the same cores at P% duty cycle (10 ms period) for the whole trial; requires `core_budget`. `lsm_burn` is only required when P > 0. |
| `trial_root` | e.g. `/dev/shm/$USER/lsm-trials` (`$VARS` and `~` expanded; must be absolute). The baseline stays in the scratch root; each trial clone is created in a per-run shared root under `trial_root` and deleted after verification (its `dataset.json`/`run.json` are kept in `trials/<id>/dataset-metadata/`). The per-run directory is removed at the end even on failure. |

Every trial outcome in `run.json` also records `host_load_before` and
`host_load_after` (1-minute load average and the CPU share of all host CPUs
used by processes outside the runner's process tree over 0.5 s, with the top
foreign processes; `unavailable` without Linux `/proc`), plus `cpu_placement`
and `burner` when configured.

`primary-summary.json` and each 1 Hz `primary-metrics.jsonl` sample now include
`write_stalls` (`stall_micros`, `rocksdb.db-write-stall-stats`,
`rocksdb.cf-write-stall-stats`, `io_stalls.*`), `io` (user, WAL, flush, and
compaction bytes, `write_amplification` since open, and RocksDB's cumulative
`compaction.Sum` read/write GB and W-Amp), and more properties
(`num-running-compactions`, `num-running-flushes`, `estimate-num-keys`,
`actual-delayed-write-rate`, `is-write-stopped`, ...). `labctl analyze` adds
in-window deltas (`window_*`) and whole-trial totals (`total_*`) of these
counters to `trials.csv`.

```bash
# 300 M records of 16 B keys / 128 B values: "load_mode": "bulk" or "ingest" in the dataset spec.
build/dev/lsm_admin create configs/my-large.json /scratch/$USER/large
build/dev/lsm_loadgen --endpoint HOST:PORT --run-id RUN --output OUT \
  --closed-loop 32 --warmup-duration-seconds 30 --duration-seconds 120 --read-percent 50 --insert-percent 20
```
