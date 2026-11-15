#!/usr/bin/env bash
# End-to-end remote compaction test: starts lsm_compaction_server (cpu_merge backend) on a
# free port and runs lsm_compaction_bench through it, directly and through a relay server
# (--backend remote_merge). Every run must match native RocksDB (run first in the same
# process) exactly and must not fall back to local compaction.
#
# Usage: remote_compaction_test.sh SERVER BENCH [ENDPOINT_PREFIX] [BACKEND]
#   ENDPOINT_PREFIX defaults to tcp://127.0.0.1 (use rdma://<RoCE address> on RDMA hosts, or
#   unix:// for the shared-memory transport, with sockets in a temporary directory).
set -euo pipefail
server=${1:?server binary}
bench=${2:?bench binary}
prefix=${3:-tcp://127.0.0.1}
backend=${4:-cpu_merge}
work=$(mktemp -d)
relay=
# A listen address for the server named $1: port 0 for TCP/RDMA, a socket file for unix.
listen_on() {
  case "$prefix" in
    unix://*) echo "unix://$work/$1.sock" ;;
    *) echo "$prefix:0" ;;
  esac
}
trap 'kill $pid $relay ${forward:-} ${drain_a_pid:-} ${drain_b_pid:-} 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$work"' EXIT

"$server" --listen "$(listen_on server)" --backend "$backend" > "$work/server.out" 2> "$work/server.err" &
pid=$!
for _ in $(seq 1 100); do
  grep -q '^listening ' "$work/server.out" 2>/dev/null && break
  sleep 0.1
done
endpoint=$(awk '/^listening /{print $2; exit}' "$work/server.out")
[ -n "$endpoint" ] || { cat "$work/server.err"; echo "server did not start"; exit 1; }
export LSM_REMOTE_ENDPOINT=$endpoint

failures=0
cat > "$work/check_shared.py" <<'PY'
import json, sys
runs = [json.loads(line) for line in open(sys.argv[1])]
remote = runs[-1]
t = remote.get("transform") or {}
ok = remote["matches_reference"] and t.get("merge_fallbacks") == 0 and t.get("merge_jobs", 0) > 0
print(f"shared client {sys.argv[2]}: match={remote['matches_reference']} jobs={t.get('merge_jobs')} "
      f"fallbacks={t.get('merge_fallbacks')}")
sys.exit(0 if ok else 1)
PY
run_case() {
  rm -rf "$work/db"
  # shellcheck disable=SC2086
  "$bench" --dir "$work/db" --modes native_cpu,remote_merge --records 50000 --l0-files 4 --target-file-bytes 1048576 \
    $1 > "$work/run.json" 2> "$work/run.err" || { cat "$work/run.err"; failures=$((failures + 1)); return; }
  if ! python3 - "$work/run.json" "$1" <<'EOF'
import json, sys
ok = True
for line in open(sys.argv[1]):
    run = json.loads(line)
    if run["mode"] == "native_cpu":
        continue
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
}
for args in "--delete-fraction 0" "--delete-fraction 0.05 --snapshots 3" \
            "--delete-fraction 0.2 --snapshots 2 --subcompactions 4" "--delete-fraction 0.05 --compression zstd" \
            "--delete-fraction 0.1 --compression none" "--delete-fraction 0.05 --value-bytes 1000 --block-bytes 16384" \
            "--delete-fraction 0.05 --snapshots 2 --ttl-ms 3600000"; do
  run_case "$args"
done

# Relay: a second server forwards every job to the first one.
LSM_REMOTE_ENDPOINT=$endpoint "$server" --listen "$(listen_on relay)" --backend remote_merge > "$work/relay.out" 2> "$work/relay.err" &
relay=$!
for _ in $(seq 1 100); do
  grep -q '^listening ' "$work/relay.out" 2>/dev/null && break
  sleep 0.1
done
LSM_REMOTE_ENDPOINT=$(awk '/^listening /{print $2; exit}' "$work/relay.out")
[ -n "$LSM_REMOTE_ENDPOINT" ] || { cat "$work/relay.err"; echo "relay did not start"; exit 1; }
for args in "--delete-fraction 0.05 --snapshots 2 --subcompactions 4" "--delete-fraction 0.05 --ttl-ms 3600000"; do
  run_case "$args"
done
export LSM_REMOTE_ENDPOINT=$endpoint

# Several databases sharing the server at once, each its own tenant: every one must match
# native RocksDB (the server interleaves their jobs on its engines).
clients=()
for client in 1 2 3; do
  (rm -rf "$work/db$client"
   LSM_TENANT="tenant-$client" "$bench" --dir "$work/db$client" --modes native_cpu,remote_merge --records 50000 \
     --l0-files 4 --target-file-bytes 1048576 --delete-fraction 0.05 --subcompactions 4 --seed "$client" \
     > "$work/shared$client.json" 2> "$work/shared$client.err") &
  clients+=($!)
done
for job in "${clients[@]}"; do
  wait "$job" || failures=$((failures + 1))
done
for client in 1 2 3; do
  python3 "$work/check_shared.py" "$work/shared$client.json" "$client" || failures=$((failures + 1))
done
# One job per connection (LSM_REMOTE_JOBS_PER_CONNECTION=1): every job reconnects.
rm -rf "$work/db"
LSM_REMOTE_JOBS_PER_CONNECTION=1 "$bench" --dir "$work/db" --modes native_cpu,remote_merge --records 50000 \
  --l0-files 4 --target-file-bytes 1048576 --delete-fraction 0.05 --subcompactions 4 > "$work/run.json" 2> "$work/run.err" \
  || { cat "$work/run.err"; failures=$((failures + 1)); }
python3 - "$work/run.json" <<'PY' || failures=$((failures + 1))
import json, sys
run = [json.loads(line) for line in open(sys.argv[1])][-1]
t = run["transform"]
print(f"recycled connections: match={run['matches_reference']} jobs={t['merge_jobs']} "
      f"fallbacks={t['merge_fallbacks']} recycled={t.get('merge_connections_recycled')}")
sys.exit(0 if run["matches_reference"] and t["merge_fallbacks"] == 0
         and t.get("merge_connections_recycled", 0) >= t["merge_jobs"] - 1 else 1)
PY
[ "$failures" -eq 0 ] || { echo "$failures remote compaction case(s) failed"; exit 1; }
echo "remote compaction: all cases (direct, relayed and 3 clients at once) match native RocksDB"

# Drain (TCP only): the client reaches server A through a forwarder. Once A has answered a
# merge, the forwarder sends A SIGTERM (A drains: running merges finish, new ones are refused)
# and sends new connections to server B. The client must retry the refused jobs on B: no
# fallbacks, and A must finish its drain.
case "$prefix" in
tcp://*)
  start_server() {
    "$server" --listen "$prefix:0" --backend "$backend" "${@:2}" > "$work/$1.out" 2> "$work/$1.err" &
    eval "$1_pid=$!"
    for _ in $(seq 1 100); do
      grep -q '^listening ' "$work/$1.out" 2>/dev/null && break
      sleep 0.1
    done
    eval "$1_endpoint=\$(awk '/^listening /{print \$2; exit}' \"\$work/$1.out\")"
  }
  start_server drain_a --drain-seconds 20
  # B also bounds its CPU merges with the scheduler (--engines with cpu_merge).
  start_server drain_b --engines 2 --scheduler fair
  a_endpoint=$drain_a_endpoint
  b_endpoint=$drain_b_endpoint
  [ -n "$a_endpoint" ] && [ -n "$b_endpoint" ] || { echo "drain servers did not start"; exit 1; }
  cat > "$work/forward.py" <<'PY'
import os, signal, socket, sys, threading
a, b, a_pid = sys.argv[1], sys.argv[2], int(sys.argv[3])
target = [a]
returned = [0]
lock = threading.Lock()
def address(endpoint):
    host, port = endpoint.split("://", 1)[1].rsplit(":", 1)
    return host, int(port)
def pipe(source, sink, from_a):
    try:
        while data := source.recv(1 << 16):
            sink.sendall(data)
            if from_a:
                with lock:
                    returned[0] += len(data)
                    # Large replies are merge results, not the connection handshake.
                    if target[0] == a and returned[0] > (1 << 16):
                        target[0] = b
                        os.kill(a_pid, signal.SIGTERM)
    except OSError:
        pass
    for s in (source, sink):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
listener = socket.create_server(("127.0.0.1", 0))
print(f"listening tcp://127.0.0.1:{listener.getsockname()[1]}", flush=True)
while True:
    client, _ = listener.accept()
    with lock:
        chosen = target[0]
    server = socket.create_connection(address(chosen))
    for s in (client, server):
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    threading.Thread(target=pipe, args=(client, server, False), daemon=True).start()
    threading.Thread(target=pipe, args=(server, client, chosen == a), daemon=True).start()
PY
  python3 "$work/forward.py" "$a_endpoint" "$b_endpoint" "$drain_a_pid" > "$work/forward.out" 2> "$work/forward.err" &
  forward=$!
  for _ in $(seq 1 100); do
    grep -q '^listening ' "$work/forward.out" 2>/dev/null && break
    sleep 0.1
  done
  rm -rf "$work/db"
  LSM_REMOTE_ENDPOINT=$(awk '/^listening /{print $2; exit}' "$work/forward.out") "$bench" --dir "$work/db" \
    --modes native_cpu,remote_merge --records 50000 --l0-files 4 --target-file-bytes 1048576 --delete-fraction 0.05 \
    --subcompactions 4 > "$work/run.json" 2> "$work/run.err" || { cat "$work/run.err"; exit 1; }
  wait "$drain_a_pid" 2>/dev/null || true
  kill "$forward" "$drain_b_pid" 2>/dev/null || true
  wait "$forward" "$drain_b_pid" 2>/dev/null || true
  forward=
  python3 - "$work/run.json" "$work/drain_a.out" "$work/drain_b.out" <<'PY'
import json, sys
run = [json.loads(line) for line in open(sys.argv[1])][-1]
t = run["transform"]
a = json.loads(open(sys.argv[2]).read().splitlines()[-1])
b = json.loads(open(sys.argv[3]).read().splitlines()[-1])
scheduled = sum(tenant["jobs"] for tenant in b["scheduler"]["tenants"].values())
print(f"drain: match={run['matches_reference']} jobs={t['merge_jobs']} fallbacks={t['merge_fallbacks']} "
      f"retries={t.get('merge_remote_retries')} refused_by_a={a['drain_refused']} a_drained={a['drain_complete']} "
      f"scheduled_by_b={scheduled}")
sys.exit(0 if run["matches_reference"] and t["merge_fallbacks"] == 0 and t.get("merge_remote_retries", 0) >= 1
         and a["drain_complete"] and scheduled >= 1 else 1)
PY
  echo "remote compaction: a draining server hands its jobs to the next one"
  ;;
esac

# Fault injection: the server dies on its second request. Compaction must fall back to the
# local path for that job and every later one, and still match native RocksDB.
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
"$server" --listen "$(listen_on crash)" --backend "$backend" --crash-after 1 > "$work/server.out" 2> "$work/server.err" &
pid=$!
for _ in $(seq 1 100); do
  grep -q '^listening ' "$work/server.out" 2>/dev/null && break
  sleep 0.1
done
export LSM_REMOTE_ENDPOINT=$(awk '/^listening /{print $2; exit}' "$work/server.out")
rm -rf "$work/db"
"$bench" --dir "$work/db" --modes native_cpu,remote_merge --records 50000 --l0-files 4 --target-file-bytes 1048576 \
  --delete-fraction 0.05 --subcompactions 4 > "$work/run.json" 2> "$work/run.err" || { cat "$work/run.err"; exit 1; }
python3 - "$work/run.json" <<'PY'
import json, sys
run = [json.loads(line) for line in open(sys.argv[1])][-1]
t = run["transform"]
print(f"fault injection: match={run['matches_reference']} remote jobs={t['merge_jobs']} fallbacks={t['merge_fallbacks']}")
sys.exit(0 if run["matches_reference"] and t["merge_fallbacks"] >= 1 else 1)
PY
echo "remote compaction: server failure falls back correctly"
