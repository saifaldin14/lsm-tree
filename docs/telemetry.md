# Resource Accounting

Collection is native C++20. Python only starts collectors and analyzes their recorded output. The primary, worker, and load generator sample their own processes once per second by default. A separate collector samples each host, including a dedicated storage host's kernel and file-cache activity. Local trials use one host collector, since all roles share the same machine.

## Sources and Scope

| Measurement | macOS source | Linux source | Scope and limits |
| --- | --- | --- | --- |
| Process CPU, memory, I/O | `proc_pid_rusage`, `proc_pidinfo`, `getrusage` | `/proc/PID/stat,status,io`, `getrusage` | All process threads; children excluded. CPU time is nanoseconds. RSS and footprint are different memory measurements. |
| Host CPU and memory | Mach host statistics, `hw.memsize` | `/proc/stat`, `/proc/meminfo` | All activity, including other applications and kernel work. Cache, compressed/wired memory, or slab fields are retained where available. |
| Network traffic | 64-bit `NET_RT_IFLIST2` counters | `/sys/class/net/IF/statistics` | Per-interface RX/TX bytes, packets, errors, and exposed drops. Includes real protocol traffic and retransmissions counted by that interface. These are not inferred from request sizes. |
| Device traffic | `IOBlockStorageDriver.Statistics` | `/sys/block/DEVICE/stat` | Whole-device read/write bytes and operations. Linux sectors use the kernel's fixed 512-byte accounting unit. Partitions are not added to parent devices. |
| NFS server bytes | Explicitly unavailable from this collector | `/proc/net/rpc/nfsd`, `io` fields | NFS server bytes are distinct from network and physical-device traffic. Storage-host CPU/memory totals include server and kernel work without pretending it all belongs to one userspace PID. |
| GPU utilization and memory | `IOAccelerator.PerformanceStatistics` | NVML from `libnvidia-ml.so.1` | Whole GPU, all processes. macOS fields are driver-specific. NVML memory utilization is memory-busy time, not allocated-byte percentage. |
| Power | Operator-supplied `powermetrics` capture | NVML milliwatts; hwmon microwatts | Converted to watts, retaining device or sensor domain. Apple's tool reports OS estimates, not calibrated power-meter values. |
| Energy | Integration of imported explicit power readings, when duration is present | NVML millijoules; hwmon/RAPL microjoules | Hardware counter deltas and sampled-power integrals remain separate. No invented readings when a source is absent. |
| Temperature | No numerical Celsius source validated here | NVML GPU temperature; hwmon and thermal-zone millidegrees | Converted to Celsius. Sensor labels are retained; thermal pressure is not a numerical temperature. |

Network interfaces can overlap through bridges, tunnels, or loopback. Do not sum every interface and call the result link traffic. NIC counters do not establish exact on-wire bits including all framing, offloads, or switch traffic.

Device bytes are OS-observed block-device traffic, not physical NAND writes or SSD internal write amplification. On macOS, file-backed virtual disks are classified separately using IOKit transport metadata. Linux excludes partition and software-stack entries without a device link; a VM's exposed disk can still be hypervisor-backed. Counter identities include registry IDs or kernel disk sequence where available. Select the actual storage devices and interfaces for the deployment instead of summing overlapping layers. Delayed writeback after collection stops is outside the captured interval.

Apple GPU memory is unified with host memory. It must not be added to RSS or physical footprint as if it were separate RAM. GPU utilization is a driver sample, not the sum of kernel dispatch durations. Whole-GPU readings include the desktop and other applications; exclusive allocation and separate useful-compaction counters remain necessary for attribution. NVML enumerates accessible devices with UUIDs; it is not filtered by `CUDA_VISIBLE_DEVICES`.

## Capture Commands

The standalone executable links no RocksDB or RPC service. Its parent output directory must exist, and the output file must be new. It stops on SIGINT/SIGTERM, or after a specified duration:

```sh
build/dev/lsm_telemetry --role storage --duration-seconds 30 \
  --output artifacts/storage-resources.jsonl --interval-ms 1000 \
  --max-output-bytes 33554432 \
  --require-metrics host:cpu_user_ns,host:memory_total_bytes,host_interface:network_rx_bytes,host_interface:network_tx_bytes,host_block_device:disk_read_bytes,host_block_device:disk_write_bytes
```

`--pid` optionally samples another process on that same host. Without it, the standalone executable labels its own process metrics as collector overhead, not as the storage service. The native services do not spawn measured child processes; future subprocess work would need separate accounting. A non-self Linux PID's context-switch fields are explicitly main-thread scoped.

Core process CPU, RSS, and storage counters are required. Extra `scope:name` requirements are comma-separated. A requirement needs an available reading from at least one entity in that scope in every sample; inspect entity identities when multiple GPUs or devices are present. Use an exclusive GPU allocation for the hardware gate. Missing required readings or collection errors produce an incomplete footer and exit code 3. Unsupported optional sensors remain null with a reason. A complete log does not certify that every optional sensor was available.

For GPU sensor qualification, add these requirements on the worker host:

```text
host_gpu:gpu_utilization_percent,host_gpu:gpu_memory_used_bytes
```

For a supported NVIDIA device, additionally require:

```text
host_gpu:power_watts,host_gpu:energy_joules,host_gpu:temperature_celsius
```

Some NVIDIA devices do not expose cumulative energy. Strict qualification reports that absence; it does not substitute a power integral and call it a hardware counter. NVML is dynamically loaded from the installed driver, so collection does not require a CUDA toolkit. Linux energy access may require operator-managed permissions. Nothing changes sensor permissions, invokes privilege elevation, or installs drivers.

Primary and worker CLI options are `--resources`, `--resource-interval-ms`, and `--resource-budget-bytes`. The load generator always writes a process resource stream inside its output directory and accounts for it in the artifact budget. New frozen plans include host and process log budgets. Legacy plans remain analyzable but must be regenerated before another instrumented run.

## Records and Analysis

Each JSONL stream has a header, ordered sample records, and a footer. The header identifies role, PID, hashed host and boot identities, interval, budget, and required metrics. Samples have host-local monotonic start/end bounds, a realtime timestamp for correlation, and explicit collection errors. Each metric carries `name`, `scope`, `entity`, `source`, `unit`, `kind`, `status`, and `value`; unavailable values are null, never zero substitutes. Process start identity protects against PID reuse across samples.

The footer records sample counts, overruns, output bytes, and collection/writing wall and thread-CPU time. Those overhead counters are not an instrumented-versus-uninstrumented performance comparison. Embedded sampler CPU is already included in the process total and must not be added again. Output files use exclusive creation and are synced at orderly shutdown. A killed process can leave a partial log; a missing footer cannot pass completeness checks.

`labctl analyze` includes resource JSON/CSV reports beside the existing latency and capacity reports. Individual logs can also be summarized:

```sh
.venv/bin/python tools/labctl.py resources artifacts/storage-resources.jsonl \
  --output artifacts/storage-resource-analysis
```

To select a recorded foreground measurement window on the same host and boot:

```sh
.venv/bin/python tools/labctl.py resources artifacts/pilot-run/trials/trial-0000/host-resources.jsonl \
  --load-result artifacts/pilot-run/trials/trial-0000/load/result.json \
  --output artifacts/window-resource-analysis
```

Only samples wholly inside the requested window are used. Counter resets, decreases, missing readings, identity changes, and gaps longer than three configured intervals are not bridged. Counter deltas retain integer precision. Gauges use time-weighted trapezoidal averages; sampled power is integrated in joules and explicitly labeled as an estimate. Hardware energy counters remain separate. Wrapping RAPL counters are rejected at the wrap; the reported range is retained, but wrap multiplicity is not guessed.

Reports include per-family availability, per-metric coverage, observed ranges, and excluded intervals. Deltas are not extrapolated to unsampled edges. Very short trials may contain too few in-window samples for a rate or energy estimate. Host, process, and device scopes are never automatically summed; overlapping energy domains are likewise kept separate. Lifetimes include setup and shutdown, while measurement-window reports exclude those phases where a common clock is established.

Approved SSH qualification starts one native collector on each of the four distinct hosts, retrieves logs after stopping processes, and seals resource summaries with the bundle. Captures currently have host-local clocks only. Foreign-host logs cannot be filtered using the load host's monotonic clock, and cross-host window alignment remains unverified. Functional cross-host qualification does not imply complete power or thermal sensor availability.

## macOS Power Capture

The local unprivileged collector reads actual GPU utilization and shared-memory statistics. It does not have a validated numerical power, energy, or temperature source on this Apple M3. No privileged capture has been executed in this session.

An operator with appropriate privileges can run the installed macOS tool directly, outside the agent, using a fresh output path:

```sh
sudo /usr/bin/powermetrics --samplers cpu_power,gpu_power,thermal \
  --format plist --sample-rate 1000 --sample-count 30 \
  --output-file "$PWD/artifacts/power-capture.plist"
```

After collection has stopped:

```sh
.venv/bin/python tools/labctl.py import-power artifacts/power-capture.plist artifacts/power-import
```

The importer preserves and hashes the original NUL-separated plist data. It converts explicit milliwatt fields (`cpu_power`, `gpu_power`, `ane_power`, `combined_power`) when present and uses `elapsed_ns` for sample-energy estimates. Legacy energy-named fields are left unconverted because their semantics vary. Combined and component power overlap. Thermal-pressure labels are retained without fabricating Celsius. Host binding and alignment with benchmark clocks are unverified for external captures. The importer has parser tests, but no real privileged capture has been validated here.

## Validation Status

Native macOS collection has been tested against the live process APIs, real loopback traffic, disk-driver counters, and the Apple M3 GPU registry. Real primary/worker/load trials verify resource-log lifecycles and report generation. Unit checks cover resets, gaps, missing metrics, and power arithmetic; these constructed arithmetic cases are not benchmark measurements.

Linux collectors and NVML have been built and run on Zyra (Linux, RTX A6000) during the CUDA harness experiments. The in-process GPU compaction campaigns (`publication/system_campaign.py`) measure energy separately with RAPL (`perf stat -a`) and `nvidia-smi` power sampling. Cross-host qualification still requires an approved inventory. Earlier recorded CPU/Metal pilots predate this telemetry and have no retroactively inferred resource measurements.