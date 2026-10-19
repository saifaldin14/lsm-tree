#!/usr/bin/env bash
# CPU attribution for one paired write-only trial (native CPU and GPU offload).
# Usage: publication/profile.sh OUTPUT_DIR {auto|spin|blocking} [NUMA_NODE]
# Records system-wide cpu-clock samples (fixed 1 ms period, so each sample is 1 ms
# of on-CPU time) while labctl runs the pair, then reports
# per-thread and per-symbol shares for each trial's lsm_db process.
set -euo pipefail

output=$1
sync=$2
node=${3:-}
root=$(cd "$(dirname "$0")/.." && pwd)
python=${PYTHON:-$root/.venv/bin/python}

if [ "${REPORT_ONLY:-}" = 1 ]; then
    report_only=1
else
    report_only=0
    mkdir "$output"
fi
[ "$report_only" = 1 ] || cat > "$output/config.json" <<EOF
{"schema_version": 1, "name": "profile-$sync${node:+-node$node}", "dataset_spec": "configs/gpu-compaction-bottleneck-128k.json",
 "modes": ["native_cpu", "cuda_local"], "rates": [5000], "repetitions": 1, "seed": 9091,
 "warmup_seconds": 2, "measurement_seconds": 8, "read_percent": 0, "threads": 8, "queue_capacity": 16384,
 "deadline_ms": 5000, "read_slo_ms": 100, "write_slo_ms": 100, "max_wall_seconds": 300,
 "max_logical_write_bytes": 4294967296, "max_artifact_bytes": 4294967296}
EOF
cd "$root"
[ "$report_only" = 1 ] || "$python" tools/labctl.py plan "$output/config.json" "$output/plan.json"
[ "$report_only" = 1 ] || LSM_CUDA_SYNC=$sync LSM_LAB_PRIMARY_NUMA_NODE=$node \
    perf record -a -e cpu-clock -c 1000000 -o "$output/perf.data" -- "$python" tools/labctl.py run "$output/plan.json" "$output/run" \
    2> "$output/perf-record.log"

report() {
    for resources in "$output"/run/trials/*/primary-resources.jsonl; do
        trial=$(basename "$(dirname "$resources")")
        pid=$("$python" -c 'import json,sys; print(json.loads(open(sys.argv[1]).readline())["pid"])' "$resources")
        mode=$("$python" -c 'import json,sys; t=[t for t in json.load(open(sys.argv[1]))["trials"] if t["id"]==sys.argv[2]][0]; print(t["mode"])' \
            "$output/run/run.json" "$trial")
        perf report -i "$output/perf.data" --pid "$pid" --no-children --sort comm --stdio -g none \
            > "$output/$mode-threads.txt" 2>/dev/null
        perf report -i "$output/perf.data" --pid "$pid" --no-children --sort comm,dso,sym --stdio -g none \
            --percent-limit 0.2 > "$output/$mode-symbols.txt" 2>/dev/null
        echo "$trial $mode pid=$pid" >> "$output/trials.txt"
    done
}
report
echo "profile complete: $output"
