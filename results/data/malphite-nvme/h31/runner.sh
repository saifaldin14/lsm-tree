#!/usr/bin/env bash
# H31 NVMe cells (PREREGISTRATION.md). Waits for the N6/N5 chain to exit, then runs C4, BZ, TR.
out=/tmp/sa29ali/nvme-h31
log=/tmp/sa29ali/nvme-followup/run.log
cd ~/lsm-tree
export LSM_BUILD_DIR=$HOME/lsm-tree/build/zyra-cuda
while pgrep -f "[n]6n5.sh" > /dev/null; do sleep 20; done
mkdir -p /tmp/sa29ali/nvme-h31-grids
python3 - <<'PY'
import json
base = json.load(open("/tmp/sa29ali/nvme-followup/n6-grid.json"))
common = dict(base["common"], **{"warmup-seconds": 60, "duration-seconds": 180})
flags = {t["design"]: t["flags"] for t in base["trials"]}
mode = {"native": "native_cpu", "full": "cuda_merge"}
trials = []
for d in ("native", "full"):
    trials.append({"design": d, "mode": mode[d], "cores": 4, "writes": "batched", "cell": "C4",
                   "flags": dict(flags[d], **{"threads": 8, "background-jobs": 4})})
    trials.append({"design": d, "mode": mode[d], "cores": 8, "writes": "batched", "cell": "BZ",
                   "flags": dict(flags[d], **{"bottommost-compression": "zstd"})})
    trig = 8 if d == "native" else 12
    trials.append({"design": d, "mode": mode[d], "cores": 8, "writes": "batched", "cell": "TR",
                   "flags": dict(flags[d], **{"l0-trigger": trig, "max-bytes-for-level-base": trig * 64 * 2**20})})
json.dump({"_comment": "H31 NVMe cells; PREREGISTRATION.md.", "common": common, "trials": trials},
          open("/tmp/sa29ali/nvme-h31-grids/grid.json", "w"), indent=1)
PY
echo "== H31 NVMe cells $(date)" >> "$log"
python3 publication/system_campaign.py "$out" --golden /tmp/sa29ali/x8/golden-50m \
  --grid /tmp/sa29ali/nvme-h31-grids/grid.json --blocks 2 --seed 113 \
  --scratch /tmp/sa29ali/nvme-followup/scratch --idle-seconds 0 >> "$log" 2>&1
echo "== H31 exit=$? $(date)" >> "$log"
