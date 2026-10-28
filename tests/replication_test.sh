#!/usr/bin/env bash
# Replication test: a primary (lsm_system_bench) with two backups (lsm_replica_server) in
# physical and logical mode. After the run, every backup's digest of all keys and values
# must equal the primary's (lsm_system_bench fails otherwise).
#
# Usage: replication_test.sh SYSTEM_BENCH REPLICA_SERVER [ENDPOINT_HOST] [MODE...]
#   ENDPOINT_HOST defaults to tcp://127.0.0.1 (use rdma://<RoCE address> on RDMA hosts).
set -euo pipefail
bench=${1:?system bench binary}
replica=${2:?replica server binary}
host=${3:-tcp://127.0.0.1}
shift $(( $# >= 3 ? 3 : $# ))
modes=("$@")
[ ${#modes[@]} -gt 0 ] || modes=(physical logical)
work=$(mktemp -d)
pids=()
cleanup() {
  for pid in ${pids[@]+"${pids[@]}"}; do kill "$pid" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -rf "$work"
}
trap cleanup EXIT

"$bench" --phase preload --dir "$work/golden" --records 200000 --threads 4 > "$work/preload.json" 2>&1

# Starts a backup (the primary bootstraps its empty directory) and sets $endpoint. Not run in a
# subshell, so its PID is recorded for cleanup.
start_replica() {
  local name=$1
  rm -rf "$work/$name"
  mkdir -p "$work/$name"
  "$replica" --listen "$host:0" --dir "$work/$name" > "$work/$name.out" 2> "$work/$name.err" &
  pids+=($!)
  for _ in $(seq 1 100); do
    grep -q '^listening ' "$work/$name.out" 2>/dev/null && break
    sleep 0.1
  done
  endpoint=$(awk '/^listening /{print $2; exit}' "$work/$name.out")
}

for mode in "${modes[@]}"; do
  start_replica "$mode-a"
  a=$endpoint
  start_replica "$mode-b"
  b=$endpoint
  rm -rf "$work/primary"
  cp -R "$work/golden" "$work/primary"
  "$bench" --phase run --mode cpu_merge --dir "$work/primary" --records 200000 --threads 4 \
    --warmup-seconds 1 --duration-seconds 3 --replication "$mode" --replicate-to "$a,$b" \
    > "$work/run-$mode.json" 2> "$work/run-$mode.err" || { cat "$work/run-$mode.err"; cat "$work/run-$mode.json"; exit 1; }
  python3 - "$work/run-$mode.json" "$mode" <<'EOF'
import json, sys
run = json.load(open(sys.argv[1]))
r = run["replication"]
print(f"{sys.argv[2]:<9} ops/s={run['ingest_and_compact']['ops_per_second']:.0f} match={r['match']} "
      f"digest={r['primary_digest']} replica_cpu={[round(b['cpu_seconds'], 2) for b in r['backups']]} "
      f"bytes={[b['bytes'] for b in r['backups']]}")
sys.exit(0 if r["match"] and run["verification"] else 1)
EOF
done
echo "replication: every backup matches the primary"
