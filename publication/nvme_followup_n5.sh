#!/bin/bash
# Malphite after N2: N3 dropped; release the direct-RDMA r8 on pyke; then N6 (equal-I/O confirmation)
# and N5 (fixed rates), both with use-direct-io false for both designs (PREREGISTRATION.md).
set -uo pipefail
cd ~/lsm-tree
U=$(whoami); D=/tmp/$U/nvme-followup; R=/tmp/$U/retune; GOLDEN=/tmp/$U/x8/golden-50m
export LSM_BUILD_DIR=build/zyra-cuda
N2_PID=${1:?N2 campaign pid}
exec >> $D/run.log 2>&1

while kill -0 "$N2_PID" 2>/dev/null; do sleep 30; done
echo "== N2 ended $(date)"
echo "== N3 dropped (PREREGISTRATION.md, change before N3 ran)"
rm -rf $D/scratch
echo "== DONE $(date)"

echo "== N6/N5 waiting for r8 on pyke $(date)"
until ssh -o BatchMode=yes 192.168.100.103 'grep -q "^== DONE" /tmp/sa29ali/direct/r8.log'; do sleep 60; done

python3 - "$D" "$R" <<'PY'
import json, sys
D, R = sys.argv[1], sys.argv[2]
confirm = json.load(open(f"{R}/confirm-grid.json"))
buffered = dict(confirm["common"], **{"use-direct-io": False})
json.dump({"_comment": "N6: confirmation grid with buffered I/O for both designs.",
           "common": buffered, "trials": confirm["trials"]}, open(f"{D}/n6-grid.json", "w"), indent=1)
trials = []
for rate in (400000, 630000, 800000):
    for t in confirm["trials"]:
        t = json.loads(json.dumps(t))
        t["writes"] = f"open-{rate}"
        t["flags"]["rate"] = rate
        trials.append(t)
json.dump({"_comment": "N5: fixed rates on malphite NVMe, buffered I/O for both designs.",
           "common": dict(buffered, **{"warmup-seconds": 60, "duration-seconds": 180}), "trials": trials},
          open(f"{D}/n5-grid.json", "w"), indent=1)
for name in ("n6", "n5"):
    g = json.load(open(f"{D}/{name}-grid.json"))
    print(name, json.dumps(g["common"]), [(t["design"], t["writes"]) for t in g["trials"]])
PY

echo "== N6 equal I/O $(date)"
python3 publication/system_campaign.py $D/n6 --golden $GOLDEN --grid $D/n6-grid.json \
  --blocks 3 --seed 8 --scratch $D/scratch --idle-seconds 0
echo "== N6 exit=$? $(date)"
rm -rf $D/scratch

echo "== N5 open loop $(date)"
python3 publication/system_campaign.py $D/n5 --golden $GOLDEN --grid $D/n5-grid.json \
  --blocks 2 --seed 6 --scratch $D/scratch --idle-seconds 0
echo "== N5 exit=$? $(date)"
rm -rf $D/scratch
echo "== N5 DONE $(date)"
