#!/usr/bin/env bash
# End-to-end remote compaction test: starts lsm_compaction_server (cpu_merge backend) on a
# free port and runs lsm_compaction_bench through it. Every run must match native RocksDB
# exactly and must not fall back to local compaction.
#
# Usage: remote_compaction_test.sh SERVER BENCH [ENDPOINT_PREFIX] [BACKEND]
#   ENDPOINT_PREFIX defaults to tcp://127.0.0.1 (use rdma://<RoCE address> on RDMA hosts).
set -euo pipefail
server=${1:?server binary}
bench=${2:?bench binary}
prefix=${3:-tcp://127.0.0.1}
backend=${4:-cpu_merge}
work=$(mktemp -d)
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; rm -rf "$work"' EXIT

"$server" --listen "$prefix:0" --backend "$backend" > "$work/server.out" 2> "$work/server.err" &
pid=$!
for _ in $(seq 1 100); do
  grep -q '^listening ' "$work/server.out" 2>/dev/null && break
  sleep 0.1
done
endpoint=$(awk '/^listening /{print $2; exit}' "$work/server.out")
[ -n "$endpoint" ] || { cat "$work/server.err"; echo "server did not start"; exit 1; }
export LSM_REMOTE_ENDPOINT=$endpoint

failures=0
for args in "--delete-fraction 0" "--delete-fraction 0.05 --snapshots 3" \
            "--delete-fraction 0.2 --snapshots 2 --subcompactions 4" "--delete-fraction 0.05 --compression zstd" \
            "--delete-fraction 0.1 --compression none" "--delete-fraction 0.05 --value-bytes 1000 --block-bytes 16384"; do
  rm -rf "$work/db"
  # shellcheck disable=SC2086
  "$bench" --dir "$work/db" --modes remote_merge --records 50000 --l0-files 4 --target-file-bytes 1048576 \
    $args > "$work/run.json" 2> "$work/run.err" || { cat "$work/run.err"; failures=$((failures + 1)); continue; }
  if ! python3 - "$work/run.json" "$args" <<'EOF'
import json, sys
ok = True
for line in open(sys.argv[1]):
    run = json.loads(line)
    t = run.get("transform") or {}
    good = run["matches_reference"] and t.get("merge_fallbacks") == 0 and t.get("merge_jobs", 0) > 0
    print(f"{sys.argv[2]:<62} match={run['matches_reference']} jobs={t.get('merge_jobs')} "
          f"built={t.get('merge_built_jobs')} fallbacks={t.get('merge_fallbacks')} "
          f"sent={t.get('merge_sent_bytes')} received={t.get('merge_received_bytes')}")
    ok = ok and good
sys.exit(0 if ok else 1)
EOF
  then
    failures=$((failures + 1))
  fi
done
[ "$failures" -eq 0 ] || { echo "$failures remote compaction case(s) failed"; exit 1; }
echo "remote compaction: all cases match native RocksDB"

# Fault injection: the server dies on its second request. Compaction must fall back to the
# local path for that job and every later one, and still match native RocksDB.
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
"$server" --listen "$prefix:0" --backend "$backend" --crash-after 1 > "$work/server.out" 2> "$work/server.err" &
pid=$!
for _ in $(seq 1 100); do
  grep -q '^listening ' "$work/server.out" 2>/dev/null && break
  sleep 0.1
done
export LSM_REMOTE_ENDPOINT=$(awk '/^listening /{print $2; exit}' "$work/server.out")
rm -rf "$work/db"
"$bench" --dir "$work/db" --modes remote_merge --records 50000 --l0-files 4 --target-file-bytes 1048576 \
  --delete-fraction 0.05 --subcompactions 4 > "$work/run.json" 2> "$work/run.err" || { cat "$work/run.err"; exit 1; }
python3 - "$work/run.json" <<'PY'
import json, sys
run = json.loads(open(sys.argv[1]).readline())
t = run["transform"]
print(f"fault injection: match={run['matches_reference']} remote jobs={t['merge_jobs']} fallbacks={t['merge_fallbacks']}")
sys.exit(0 if run["matches_reference"] and t["merge_fallbacks"] >= 1 else 1)
PY
echo "remote compaction: server failure falls back correctly"
