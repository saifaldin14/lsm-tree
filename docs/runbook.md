# Build and Run

## Setup

C++20 implementation; Python 3.12 or newer for orchestration and analysis
only. Linux and macOS are supported.

```sh
bash scripts/bootstrap.sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.lock
.venv/bin/python -m unittest discover -s tests -p "test_*.py"
```

`scripts/bootstrap.sh` checks out a pinned vcpkg revision into `.deps/vcpkg`
and builds dependencies in `~/.cache/lsm-lab` (OpenSSL cannot build in a path
with spaces). It does not change system packages. If the build directory
existed before the toolchain was selected, run `cmake --fresh --preset dev`
once.

### CUDA build

On a Linux x86_64 host with an NVIDIA driver and CUDA 12 toolkit:

```sh
bash scripts/bootstrap.sh
bash scripts/bootstrap-cuda.sh          # pinned nvCOMP 5.3.0.16, SHA-256 checked
cmake --preset dev -DLSM_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=native -DLSM_ROCKSDB_PORTABLE=0
cmake --build --preset dev -j
build/dev/lsm_gpu_check cuda_batched --require
ctest --preset dev --output-on-failure
```

`LSM_ROCKSDB_PORTABLE=0` builds RocksDB with `-march=native`. The default
(`ON`) is RocksDB's portable build, which uses software CRC32C and makes
native compaction about 16% slower; never compare GPU results against a
portable CPU baseline. The CTest `worker.lifecycle` test exercises the GPU
input path and fails if `LSM_DISABLE_GPU_INPUT` is exported in the shell.

GPU offload modes and environment variables are documented in
[gpu-compaction.md](gpu-compaction.md).

## GPU compaction benchmarks

Both benchmarks run inside one process and take `native_cpu`, `gpu_output`,
`cpu_merge` and `cuda_merge` modes. Set the variables used in the published
runs:

```sh
export LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024
```

### Isolated compaction: `lsm_compaction_bench`

Builds the same database for each mode (a base level with every key, then
`--l0-files` overlapping flushes of overwrites and deletes, optional
`--snapshots`), runs one full `CompactRange`, and prints one JSON line per
mode and repetition: wall and whole-process CPU time, per-job statistics,
transform statistics, and a digest of every internal version and of up to
200,000 point lookups. It exits with status 3 if any mode's digest differs
from the first mode's.

```sh
build/dev/lsm_compaction_bench --dir /dev/shm/$USER/cb --modes native_cpu,gpu_output,cuda_merge \
  --records 4000000 --value-bytes 128 --l0-files 8 --block-bytes 4096 --delete-fraction 0 --repetitions 4
```

Other options: `--key-bytes`, `--overwrite-fraction`, `--compression
lz4|zstd|none`, `--target-file-bytes`, `--subcompactions`, `--no-base`,
`--seed`, `--keep`. Repetition 0 includes GPU buffer allocation and should be
excluded. [publication/compaction_sweep.sh](../publication/compaction_sweep.sh)
runs the published sweep.

### Whole database: `lsm_system_bench` and `system_campaign.py`

`--phase preload` builds and verifies a key space; `--phase run` opens a copy
with the selected backend and runs closed-loop client threads for
`--warmup-seconds` plus `--duration-seconds`. It then waits for all pending
compaction and verifies every key and value. Each thread owns the keys
congruent to its index and checks every read online.

```sh
build/dev/lsm_system_bench --phase preload --dir /dev/shm/$USER/sys/golden --records 50000000 --threads 8
cp -r /dev/shm/$USER/sys/golden /dev/shm/$USER/sys/trial
numactl --physcpubind=0,1,2,3 --membind=0 build/dev/lsm_system_bench --phase run --mode cuda_merge \
  --dir /dev/shm/$USER/sys/trial --records 50000000 --threads 4 --batch-size 100 --disable-wal true \
  --background-jobs 4 --warmup-seconds 30 --duration-seconds 120
```

The result reports throughput and latency during the measurement window and
an `ingest_and_compact` section: all operations divided by the time to issue
them **plus** the time to finish the compaction they caused, with total CPU
and write amplification. Use `ingest_and_compact` to compare backends; window
throughput rewards a backend that postpones compaction. Other options:
`--read-percent`, `--value-bytes`, `--compression`, `--block-bytes`,
`--write-buffer-bytes`, `--target-file-bytes`, `--max-bytes-for-level-base`,
`--max-write-buffer-number`, `--subcompactions`, `--block-cache-bytes`,
`--adaptive-yield` (on by default; keep one client thread per core), `--verify`.

`publication/system_campaign.py` runs a grid in randomized blocks. Each trial
copies a golden database, pins the benchmark to N physical cores of the GPU's
NUMA node, and optionally runs `lsm_burn` background load on the same cores.
It samples RAPL package and DRAM energy (`perf stat -a`) and GPU power
(`nvidia-smi`) at 1 Hz.

```sh
LSM_BUILD_DIR=build/dev python3 publication/system_campaign.py OUT \
  --golden /dev/shm/$USER/sys/golden --grid configs/system/batched-ingest.json --blocks 5 --seed 41
```

Grid entries set `mode`, `cores`, optional `burn_percent` and `node`, and
benchmark `flags`; `common` holds flags shared by all trials. Results go to
`OUT/results.jsonl`. `perf_event_paranoid` must allow system-wide RAPL
events.

### Figures

`python3 publication/make_figures.py` regenerates `publication/figures/` and
`publication/data/summary.md` from the raw results in `publication/data/`. It
refuses data from a run that failed its correctness check.

### Running on Rift (Zyra)

See [rift-cluster-access.md](rift-cluster-access.md). On Zyra the RTX A6000 is
attached to NUMA node 0. Keep databases on `/dev/shm`, and enable lingering
once (`loginctl enable-linger $USER`); otherwise logind removes your
`/dev/shm` files when your last session ends. The system CMake is too old;
use the vcpkg-downloaded CMake under `.deps/vcpkg/downloads/tools/` and put
`/usr/local/cuda-12.3/bin` on `PATH`.

## Experiment harness (`labctl`)

The harness runs the database, worker and load generator as separate native
processes over gRPC, with frozen paired plans, open- or closed-loop load,
resource telemetry and full offline verification. With the database limited
to 4 cores, a write-heavy closed-loop pilot reached only about 3,800 ops/s for
every backend: request handling, not compaction, used the cores. The GPU
compaction results therefore use the in-process benchmarks above.

All output names below must be new. A completed run bundle is immutable;
analysis writes elsewhere.

```sh
mkdir -p artifacts
.venv/bin/python tools/labctl.py plan configs/local-experiment.json artifacts/pilot-plan.json
.venv/bin/python tools/labctl.py run artifacts/pilot-plan.json artifacts/pilot-run
.venv/bin/python tools/labctl.py audit artifacts/pilot-run
.venv/bin/python tools/labctl.py analyze artifacts/pilot-run artifacts/pilot-analysis
```

Set `LSM_LAB_SCRATCH_ROOT` to place live databases on local storage;
`LSM_LAB_PRIMARY_NUMA_NODE=N` runs `lsm_db` under `numactl` on node N. Every
run records all `LSM_*` environment variables in `build.json`.

Treatment names are `native_cpu` (compaction on the primary), `cuda_local` (output-only GPU offload in the primary), `cuda_merge` (GPU merge and block building in the primary, see [gpu-compaction.md](gpu-compaction.md)), `cpu_native` (native compaction on a remote worker), and `cpu_batched`, `metal_batched`, `cuda_batched` (bounded block-input path on a remote worker). A requested unavailable backend fails clearly. Local processes share one host and storage device, so they cannot qualify a cross-host claim.

Configuration contains integer units, explicit warmup/measurement durations, repetition count, SLOs, workload seed, rates, and budgets. The frozen plan includes its hash and exact randomized order. Logical-write budgets count user key/value bytes, not SSD writes or compaction amplification. The memory budget is a conservative admission estimate, not an OS-enforced memory quota. Artifact budgets include request logs and bounded resource streams; filesystem free-space checks and host quotas remain necessary for database copies and amplification.

The load generator produces arrivals independently of completions. Keys are routed to bounded FIFO lanes to preserve per-key ordering. Queue rejection, pre-send expiry, RPC failures, payload mismatches, and indeterminate writes are counted. No write retries are hidden; later requests on a key with an indeterminate write are blocked and recorded. A failed or indeterminate trial cannot qualify. Native client timing includes intended-arrival delay and client queueing; server-only timings are not used as request latency.

The current workload implements uniform Get/update mixes, including pure reads or writes through `read_percent`. It does not implement the proposed Zipfian condition yet. Raw request logging and value checks are always enabled, so their overhead is included. All writes use synchronous WAL durability. Warmup has separate counters and is excluded from measured histograms. Administrative drain and native checksums happen after the offered-load interval. Offline verification reopens the stopped database and compares every full value with acknowledged final versions.

Analysis audits checksums and paired schedule hashes, retains unsuccessful outcomes, uses the contiguous qualifying rate prefix, and reports lower/upper capacity bounds. It requires at least three in-window compaction-debt samples and no observed debt growth for pilot qualification. This is not proof of steady state. Whole paired repetitions are bootstrapped; individual requests are not replicates. Censored capacities are never silently converted into exact ratios. GPU treatments require recorded useful GPU block work.


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

## Resource collection

Every trial has primary and load process logs, a worker log when used, and
one host log. `build/dev/lsm_telemetry` also runs standalone on a storage
host. See [telemetry.md](telemetry.md) for sources, schemas and accounting
limits.

## GPU hardware gates

```sh
build/dev/lsm_gpu_check cuda_batched --require     # Linux, NVIDIA
build/dev/lsm_gpu_check metal_batched --require    # macOS
```

Without `--require` an unavailable device exits 77, which CTest reports as
skipped; with it, unavailability fails. There is no software device
substitute. CUDA uses the explicit nvCOMP CUDA decompressor, not the hardware
decompression engine, because their malformed-input guarantees differ. Input
CRC32C is checked on the device before decoding.

[../configs/cuda-local-smoke.json](../configs/cuda-local-smoke.json) qualifies
the in-process `cuda_local` path; a write-bearing trial must report nonzero
`database_statistics.local_transform.blocks`.
[../configs/cuda-rate-sweep.json](../configs/cuda-rate-sweep.json) compares
native, CPU-batched and CUDA-batched remote compaction at four offered rates,
and [../configs/local-metal-experiment.json](../configs/local-metal-experiment.json)
does the same with Metal on one Mac. These are functional gates, not
performance results.

## Cross-host qualification

Fill an inventory from the example only after arranging separate load, primary, worker, and storage hosts. Install the same tested source version and pinned engine patch on the service hosts, and build native binaries on each architecture. Python host probes require Python 3.12 or newer. The required work parents and NFS mount must already exist and be writable by the experiment account.

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
