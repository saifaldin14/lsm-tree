# LSM Lab

A C++20 research platform for real RocksDB compaction experiments. It includes native database and worker services, an open-loop load generator, immutable dataset administration, bounded compaction-block transforms, and Python orchestration/analysis outside the measured data path.

Native CPU, CPU-batched, and actual Metal execution on Apple M3 have passed block and separate-process compaction tests on macOS. The Metal tests include full-byte comparison, malformed input, snapshots, and native CPU reopen after multi-batch compaction. CUDA/nvCOMP source is implemented against verified 5.3.0.16 headers; compilation and execution require an approved Linux CUDA host and remain unverified. No cross-host inventory has been supplied.

## Setup

Prerequisites are Git, curl, CMake 3.25 or newer, a C++20 compiler, and the native platform development tools. Linux additionally needs the development/build tools required by vcpkg ports. On macOS, use native Xcode command-line tools. No container or GPU is required for the current CPU slice.

Run the project-local dependency bootstrap:

```sh
bash scripts/bootstrap.sh
```

The script checks out an exact vcpkg revision into `.deps/vcpkg`. Generated package builds and installations use `~/.cache/lsm-lab/vcpkg-319504a5326a` because the OpenSSL build cannot handle whitespace in its source or installation paths. The current workspace path may contain spaces. The script does not change system packages, provision infrastructure, or delete a previous installation. The first bootstrap can take substantial time; it builds native dependencies with two parallel jobs by default. `VCPKG_MAX_CONCURRENCY` can reduce or increase that limit.

Use the `dev` configure/build/test presets. The preset uses the pinned vcpkg toolchain. RocksDB is downloaded separately at the exact commit and archive SHA-256 in [toolchains.lock.json](toolchains.lock.json). The current configuration uses Makefiles so Ninja is not required. CMake Tools could not discover this workspace during validation; the CLI presets are the verified fallback. A cache created before selecting the toolchain requires one fresh configuration.

See [docs/runbook.md](docs/runbook.md) for native build, trial execution, GPU
gates, and approved-host qualification commands. See
[docs/rift-cluster-access.md](docs/rift-cluster-access.md) for SSH key
registration, compute-node selection, and VS Code Remote SSH access.

## Dataset Commands

After a successful build, the command-line entry point is `build/dev/lsm_admin`:

```sh
build/dev/lsm_admin capabilities
build/dev/lsm_admin validate configs/local-smoke.json
mkdir -p artifacts/datasets
build/dev/lsm_admin create configs/local-smoke.json artifacts/datasets/baseline
build/dev/lsm_admin verify artifacts/datasets/baseline
build/dev/lsm_admin clone artifacts/datasets/baseline artifacts/datasets/trial-001
build/dev/lsm_admin verify artifacts/datasets/trial-001
```

Creation writes full deterministic binary values using synchronous WAL-protected batches, flushes through RocksDB, waits for ordinary compaction, and publishes a dataset manifest. The manifest records the exact generator/configuration, engine version, and SHA-256 inventory of database files. Verification checks the inventory, native checksums, every generated key and complete value, and the exact record count.

Dataset operations are offline. Verification and cloning acquire the native database lock and reject a live writer. Destinations must not exist, and their parent must already exist. Cloning copies files, including mutable engine metadata; it never hardlinks a live WAL or MANIFEST. Failure leaves an owned directory labeled incomplete for inspection. There is no automatic destructive cleanup command.

The generator is named `splitmix64-record-v1`. Keys contain a mixed 64-bit prefix and a big-endian record index, so they are distinct but insertion order is not sorted order. Each full value has a pseudorandom first half and a repeating generated 16-byte template in its second half. Every overwrite generation changes the data. These are explicitly synthetic payloads, not a production trace.

The small [configs/local-smoke.json](configs/local-smoke.json) configuration is for functional validation, not a representative steady-state database experiment. Its logical write budget counts user keys and values across loading and overwrites. It does not measure or cap physical flash writes, WAL bytes, or compaction amplification. Free-space checks reserve a configured margin and cannot replace device-wide quota enforcement.

## Scope and Validation

The tests cover full values, snapshots, deletion retention, real separate-process RPC, duplicate/cancelled jobs, native and CPU-batched compaction, multi-batch streaming, paired open-loop schedules, overload accounting, independent cloning, and native-reader verification. Hardware tests report unavailable devices separately. CUDA execution is never substituted with CPU execution.

The first local eight-trial pilot completed full-value verification and produced real throughput/p99 figures. It is a functional pilot with one-second warmup and two-second measurement windows, not a steady-state or cross-host performance result. Failed SLOs and censored capacity remain in the analysis. Generated runs and figures are ignored by Git and retained under `artifacts`.

A subsequent four-treatment pilot used 64 KiB memtables to exercise compaction during foreground load. All eight trials passed offline full-value verification, and the two Metal trials processed 513 and 511 GPU blocks between in-window samples. Every treatment exceeded the declared 10 ms p99 SLO at 750 ops/s. This validates real GPU integration, not a positive performance claim. The sealed run is under `artifacts/local-metal-active-run`; its CSV/JSON and figures are under `artifacts/local-metal-active-analysis`.

GPU code accelerates input-block CRC32C verification and LZ4 decompression only. Native RocksDB still owns merging, snapshots, output encoding, and installation. [docs/block-transform.md](docs/block-transform.md) describes the checked engine patch and its limits. No positive GPU speedup, complete security audit, power-loss durability, or storage-reconnect qualification is claimed.

Recovered legacy configuration and [the previous README](legacy/README.pre-restart.md) are archived under `legacy`. Earlier experiment results are separate evidence and do not validate this RocksDB integration. Links inside the archived README describe its old tree and may no longer resolve.

## Resource Accounting

Native samplers record primary, worker, and load-process CPU/memory/I/O, plus per-host CPU/cache memory, interface bytes, and whole-device disk traffic. GPU utilization and memory come from actual macOS driver counters or NVML. Linux sensor readers include power, cumulative energy, and temperature; unsupported or inaccessible fields remain null with their source and reason. Host totals are kept separate from process metrics and virtual disk images.

Local trials collect one host stream, and approved remote qualification collects each host, including storage. Analysis exports per-entity deltas, time-weighted gauges, coverage, and separately labeled sampled-power integration. [docs/telemetry.md](docs/telemetry.md) documents commands, schemas, strict sensor gates, and accounting limits. macOS collection is locally validated; Linux/NVML execution and privileged macOS power capture remain unverified.

## Environment Report

Python is used only for unmeasured administration and analysis. Create an isolated environment with Python 3.12 or newer, then run the read-only doctor:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements.lock
.venv/bin/python tools/labctl.py doctor
```

The report includes command exit statuses, disk capacity, CPU/memory information, and discoverable native GPU tooling. It does not install packages, change mounts, or claim that discovering a compiler validates GPU execution. CUDA, Metal execution, and cross-host storage qualification remain separately reported.

## Experiment Commands

`labctl plan` freezes paired seeds, treatment order, and budgets. `labctl run` executes fresh local trial clones using the native binaries, separates warmup, records complete request outcomes and HDR histograms, stops services, and verifies every final value. `labctl audit` checks a sealed bundle; `labctl analyze` produces CSV/JSON and PNG/SVG/PDF plots outside it.

`labctl deployment-plan` prepares checks without making SSH connections. `labctl qualify` requires an approved inventory and `--apply`, then checks distinct machines, the actual NFS export/protocol, file sync/locking/rename visibility, mTLS, GPU dispatch, and native/remote/batched/GPU correctness trials. It does not provision hosts or modify mounts. See [infra/inventory.example.json](infra/inventory.example.json).
