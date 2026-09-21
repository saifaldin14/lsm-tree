# LSM Lab

A C++20 research platform being built for real RocksDB compaction experiments. The current implementation provides a native CPU engine check and offline dataset administration. Remote CPU, CUDA, and Metal execution are subsequent implementation milestones, not implemented capabilities in this version.

## Setup

Prerequisites are Git, curl, CMake 3.25 or newer, a C++20 compiler, and the native platform development tools. Linux additionally needs the development/build tools required by vcpkg ports. On macOS, use native Xcode command-line tools. No container or GPU is required for the current CPU slice.

Run the project-local dependency bootstrap:

```sh
bash scripts/bootstrap.sh
```

The script checks out an exact vcpkg revision into `.deps/vcpkg`. Generated package builds and installations use `~/.cache/lsm-lab/vcpkg-319504a5326a` because the OpenSSL build cannot handle whitespace in its source or installation paths. The current workspace path may contain spaces. The script does not change system packages, provision infrastructure, or delete a previous installation. The first bootstrap can take substantial time; it builds native dependencies with two parallel jobs by default. `VCPKG_MAX_CONCURRENCY` can reduce or increase that limit.

In VS Code CMake Tools, select the `dev` configure/build/test presets, configure, build, and run the tests. The preset uses the pinned vcpkg toolchain. RocksDB is downloaded separately at the exact commit and archive SHA-256 in [toolchains.lock.json](toolchains.lock.json). The current configuration uses Makefiles so Ninja is not required.

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

The original engine check exercises full values, snapshots, deletion retention, LZ4, checksums, and reopen. The configuration and dataset suites add malformed-config rejection, independent cloning, overwrite refusal, active-writer exclusion, and corrupted-file detection. See the actual test output for validation status; source presence alone is not a passed gate.

This code does not yet provide a load generator, a remote worker, GPU compaction, or cross-host evidence. CUDA requires suitable NVIDIA hardware; Metal requires a physical native Apple GPU. No GPU timings, storage timings, or payloads are simulated. Hardware rental and the long confirmation study require separate approval.

Recovered legacy configuration and [the previous README](legacy/README.pre-restart.md) are archived under `legacy`. Earlier experiment results are separate evidence and do not validate this RocksDB integration. Links inside the archived README describe its old tree and may no longer resolve.

## Environment Report

Python is used only for unmeasured administration and analysis. Create an isolated environment with Python 3.12 or newer, then run the read-only doctor:

```sh
python3 -m venv .venv
.venv/bin/python tools/labctl.py doctor
```

The report includes command exit statuses, disk capacity, CPU/memory information, and discoverable native GPU tooling. It does not install packages, change mounts, or claim that discovering a compiler validates GPU execution. CUDA, Metal execution, and cross-host storage qualification remain separately reported.
