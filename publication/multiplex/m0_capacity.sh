#!/usr/bin/env bash
# M0 (PREREGISTRATION.md, "GPU multiplexing"): capacity of one shared GPU compaction server.
# Replays compaction requests recorded from a whole-database run (LSM_REMOTE_RECORD_DIR) from
# CLIENTS concurrent connections for SECONDS, against lsm_compaction_server --backend cuda_merge
# with K engines, for each K. One JSON line per K: jobs/s, input GiB/s, latency, mean GPU
# utilisation, and the server's statistics (device memory per engine).
#
# Usage: publication/multiplex/m0_capacity.sh RECORDED_DIR OUT_JSONL [K...]
set -euo pipefail
recorded=${1:?recorded requests}
out=${2:?output jsonl}
shift 2
if [ $# -gt 0 ]; then engines=("$@"); else engines=(1 2 3 4 6 8); fi
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${LSM_BUILD_DIR:-$root/build/zyra-cuda}
clients=${CLIENTS:-16}
seconds=${SECONDS_PER_K:-60}
work=${WORK:-/dev/shm/$USER/m0}
server_cpus=${SERVER_CPUS:-0-7}
mkdir -p "$work"
export LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024 LSM_MERGE_READ_THREADS=4 \
       LSM_GPU_PINNED_PREALLOC_MB=1024
for k in "${engines[@]}"; do
  for repetition in 1 2 3; do
    log=$work/server-$k-$repetition.out
    numactl --physcpubind="$server_cpus" --preferred=0 "$build/lsm_compaction_server" --listen "unix://$work/s.sock" \
      --backend cuda_merge --engines "$k" --scheduler fifo > "$log" 2>&1 &
    server=$!
    for _ in $(seq 1 1200); do grep -q '^listening' "$log" && break; sleep 0.1; done
    numactl --physcpubind=18-35 --preferred=1 "$build/lsm_rpc_bench" --endpoint "unix://$work/s.sock" \
      --replay "$recorded" --clients "$clients" --duration "$seconds" > "$work/replay.json" 2> "$work/replay.err" &
    bench=$!
    # Sample the GPU only while requests are being replayed, not while the client loads them.
    for _ in $(seq 1 6000); do grep -q '^replaying' "$work/replay.err" && break; sleep 0.1; done
    nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits -lms 500 \
      > "$work/gpu-$k-$repetition.csv" &
    monitor=$!
    wait "$bench" || true
    kill "$monitor"
    kill "$server"
    wait "$server" || true
    python3 - "$k" "$repetition" "$work" "$out" <<'PY'
import json, sys
k, repetition, work, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
replay = json.loads(open(f"{work}/replay.json").read())
util, memory = [], []
for line in open(f"{work}/gpu-{k}-{repetition}.csv"):
    try:
        u, m = (float(x) for x in line.split(","))
        util.append(u); memory.append(m)
    except ValueError:
        pass
stats = [json.loads(line) for line in open(f"{work}/server-{k}-{repetition}.out") if line.startswith("{")]
server = stats[-1] if stats else {}
record = {"experiment": "m0", "engines": k, "repetition": repetition, "replay": replay,
          "gpu_utilization_mean": sum(util) / len(util) if util else None,
          "gpu_memory_used_max_mib": max(memory) if memory else None,
          "server": {key: server.get(key) for key in ("gpu_memory", "merge_jobs", "merge_fallbacks", "merge_oom_retries",
                                                        "merge_oom_failures", "merge_input_bytes", "merge_device_ns")}}
open(out, "a").write(json.dumps(record) + "\n")
print(f"K={k} rep {repetition}: {replay['input_gib_per_second']:.2f} GiB/s, {replay['jobs_per_second']:.1f} jobs/s, "
      f"p50 {replay['latency_p50_s']:.2f} s, GPU {record['gpu_utilization_mean']:.0f}%, "
      f"failures {replay['failures']}, oom retries {server.get('merge_oom_retries')}")
PY
  done
done
