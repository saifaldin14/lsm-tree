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

Configuration contains integer units, explicit warmup/measurement durations, repetition count, SLOs, workload seed, rates, and budgets. The frozen plan includes its hash and exact randomized order. Logical-write budgets count user key/value bytes, not SSD writes or compaction amplification. The memory budget is a conservative admission estimate, not an OS-enforced memory quota. Artifact budgets estimate raw request logs; filesystem free-space checks and host quotas remain necessary for database copies and amplification.

The load generator produces arrivals independently of completions. Keys are routed to bounded FIFO lanes to preserve per-key ordering. Queue rejection, pre-send expiry, RPC failures, payload mismatches, and indeterminate writes are counted. No write retries are hidden; later requests on a key with an indeterminate write are blocked and recorded. A failed or indeterminate trial cannot qualify. Native client timing includes intended-arrival delay and client queueing; server-only timings are not used as request latency.

The current workload implements uniform Get/update mixes, including pure reads or writes through `read_percent`. It does not implement the proposed Zipfian condition yet. Raw request logging and value checks are always enabled, so their overhead is included. All writes use synchronous WAL durability. Warmup has separate counters and is excluded from measured histograms. Administrative drain and native checksums happen after the offered-load interval. Offline verification reopens the stopped database and compares every full value with acknowledged final versions.

Analysis audits checksums and paired schedule hashes, retains unsuccessful outcomes, uses the contiguous qualifying rate prefix, and reports lower/upper capacity bounds. It requires at least three in-window compaction-debt samples and no observed debt growth for pilot qualification. This is not proof of steady state. Whole paired repetitions are bootstrapped; individual requests are not replicates. Censored capacities are never silently converted into exact ratios. GPU treatments require recorded useful GPU block work.

## GPU gates

Metal is enabled by default on native macOS. The shader is compiled during the build and embedded into the host binary for runtime pipeline creation. The explicit hardware command is:

```sh
build/dev/lsm_gpu_check metal_batched --require
```

The hardware command and live compaction tests passed on Apple M3 in this session. `MTLCreateSystemDefaultDevice` returned no default device, so the backend selected the Apple M3 exposed by `MTLCopyAllDevices` and recorded its registry identity. There is no software device substitute. The current kernels favor bounded correctness over tuning, and no speedup is assumed. Run this gate on each actual worker before accepting its GPU results.

[../configs/local-metal-experiment.json](../configs/local-metal-experiment.json) is a bounded eight-trial profile comparing primary CPU, remote native CPU, CPU-batched, and Metal on one machine. Its separate [../configs/local-compaction-smoke.json](../configs/local-compaction-smoke.json) dataset profile uses 64 KiB memtables and target SST sizes to exercise compaction during the offered-load interval. It uses one offered rate and cannot establish a capacity curve or cross-host speedup.

The recorded run under `artifacts/local-metal-active-run` completed 48,000 measured requests and 6,000 warmup requests with full offline verification across all trials. The two Metal trials processed 513 and 511 blocks between in-window samples; each committed four remote compactions during that observed interval. Every treatment exceeded the declared 10 ms p99 SLO at 750 ops/s. See `artifacts/local-metal-active-analysis` for CSV/JSON and figures. The earlier `artifacts/local-metal-run` had GPU work only during post-load drain; its corrected analysis under `artifacts/local-metal-analysis-v3` marks the remote treatments unverified for foreground acceleration. Both runs remain preserved.

CUDA uses the official nvCOMP 5.3.0.16 CUDA-12 package, pinned by SHA-256. The CUDA decompressor is selected explicitly; the dedicated hardware decompression mode is not used because its malformed-input guarantees differ. Input CRC32C is computed on-device before decoding, and both per-block status and output length are checked. Device copies, allocation, synchronization, and output materialization are real work inside worker elapsed time.

On an approved native Linux x86_64 NVIDIA worker with an existing compatible driver and CUDA toolkit:

```sh
bash scripts/bootstrap.sh
bash scripts/bootstrap-cuda.sh
cmake --preset dev -DLSM_ENABLE_CUDA=ON -DCMAKE_CUDA_ARCHITECTURES=native
cmake --build --preset dev
build/dev/lsm_gpu_check cuda_batched --require
ctest --preset dev -R 'blocks.transforms|worker.lifecycle|hardware.cuda' --output-on-failure
```

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