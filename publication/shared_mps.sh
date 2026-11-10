#!/usr/bin/env bash
# Shared GPU with MPS, tuned (PREREGISTRATION.md), then native RocksDB on 32 cores with the X3
# protocol. Waits for WAIT_LOG (if set) to contain "== DONE" so nothing else is running.
#
# Usage: publication/shared_mps.sh OUT_ROOT
set -euo pipefail
out=${1:?usage: shared_mps.sh OUT_ROOT}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export LSM_BUILD_DIR=${LSM_BUILD_DIR:-$root/build/zyra-cuda}
shm=${SHM:-/dev/shm/$USER/shared-mps}
bench=$LSM_BUILD_DIR/lsm_system_bench
mps=/tmp/$USER/mps
mkdir -p "$out" "$shm/scratch" "$mps/pipe" "$mps/log"

if [ -n "${WAIT_LOG:-}" ]; then
  echo "== waiting for $WAIT_LOG $(date)"
  until grep -q "^== DONE" "$WAIT_LOG" 2>/dev/null; do sleep 60; done
fi

golden() {
  local directory=$1; shift
  [ -f "$directory.done" ] && return
  rm -rf "$directory"
  "$bench" --phase preload --dir "$directory" --records 50000000 --threads 8 "$@" > "$directory.json"
  touch "$directory.done"
}

python3 - "$out" <<'PY'
import json, sys
out = sys.argv[1]
headline = json.load(open("configs/system/v3-headline.json"))
tuned = {(t["design"], t["cores"]): t for t in headline["trials"]}
common = {"records": 50000000, "read-percent": 0, "warmup-seconds": 30, "duration-seconds": 120, "verify": True}
trials = []
for cores in (4, 8):
    for n in (1, 2, 4):
        for design in ("native", "full"):
            t = tuned[(design, cores)]
            trials.append({"design": design, "mode": t["mode"], "cores": cores, "writes": "batched",
                           "flags": t["flags"], "instances": n})
json.dump({"_comment": "Shared GPU with MPS, tuned v3-headline settings; PREREGISTRATION.md.",
           "common": common, "trials": trials}, open(f"{out}/grid-mps.json", "w"), indent=1)
quick = [t for t in trials if (t["design"] == "full" and (t["cores"] == 8 or t["instances"] != 2))
         or (t["design"] == "native" and t["instances"] == 1)]
json.dump({"_comment": "Quick first pass of grid-mps.json, 1 block; PREREGISTRATION.md.",
           "common": common, "trials": quick}, open(f"{out}/grid-mps-quick.json", "w"), indent=1)
native32 = {"design": "native", "mode": "native_cpu", "cores": 32, "writes": "batched",
            "flags": {"batch-size": 100, "disable-wal": True, "background-jobs": 32, "threads": 64,
                      "subcompactions": 32, "pipelined-write": True, "adaptive-yield": False,
                      "allow-fallocate": False}}
json.dump({"_comment": "Native RocksDB, 32 cores, best v2 tuning setting, X3 protocol; PREREGISTRATION.md.",
           "common": headline["common"], "trials": [native32]}, open(f"{out}/grid-native32.json", "w"), indent=1)
print(len(trials), "MPS trials;", json.dumps(common))
PY

golden "$shm/golden-50m"

export CUDA_MPS_PIPE_DIRECTORY=$mps/pipe CUDA_MPS_LOG_DIRECTORY=$mps/log
stop_mps() { echo quit | nvidia-cuda-mps-control 2>/dev/null || true; }
trap stop_mps EXIT
nvidia-cuda-mps-control -d
sleep 2
pgrep -u "$USER" -x nvidia-cuda-mps-control > /dev/null && echo "== MPS control daemon up $(date)"

resume=(); [ -f "$out/mps-quick/schedule.json" ] && resume=(--resume)
echo "== mps-quick $(date)"
python3 publication/system_campaign.py "$out/mps-quick" --golden "$shm/golden-50m" --grid "$out/grid-mps-quick.json" \
  --blocks 1 --seed 108 --scratch "$shm/scratch" --idle-seconds 0 "${resume[@]}" || echo "== mps-quick failed rc=$?"
echo "== mps-quick done, clients seen: $(grep -c 'New client' "$mps/log/control.log" 2>/dev/null || echo 0) $(date)"
stop_mps
unset CUDA_MPS_PIPE_DIRECTORY CUDA_MPS_LOG_DIRECTORY

resume=(); [ -f "$out/native32/schedule.json" ] && resume=(--resume)
echo "== native32 $(date)"
python3 publication/system_campaign.py "$out/native32" --golden "$shm/golden-50m" --grid "$out/grid-native32.json" \
  --blocks 3 --seed 109 --scratch "$shm/scratch" --idle-seconds 0 "${resume[@]}" || echo "== native32 failed rc=$?"

export CUDA_MPS_PIPE_DIRECTORY=$mps/pipe CUDA_MPS_LOG_DIRECTORY=$mps/log
nvidia-cuda-mps-control -d
sleep 2
resume=(); [ -f "$out/mps/schedule.json" ] && resume=(--resume)
echo "== mps $(date)"
python3 publication/system_campaign.py "$out/mps" --golden "$shm/golden-50m" --grid "$out/grid-mps.json" \
  --blocks 2 --seed 108 --scratch "$shm/scratch" --idle-seconds 0 "${resume[@]}" || echo "== mps failed rc=$?"
echo "== mps clients seen: $(grep -c 'New client' "$mps/log/control.log" 2>/dev/null || echo 0) $(date)"
cp "$mps/log/control.log" "$out/mps/mps-control.log" 2>/dev/null || true
stop_mps
trap - EXIT
unset CUDA_MPS_PIPE_DIRECTORY CUDA_MPS_LOG_DIRECTORY

if [ -n "${THEN_PRIOR:-}" ]; then
  echo "== resuming prior-tuned $(date)"
  SHM=$shm publication/prior_tuned.sh "$THEN_PRIOR" || echo "== prior-tuned failed rc=$?"
fi
echo "== DONE $(date)"
