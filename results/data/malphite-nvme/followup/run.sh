#!/bin/bash
# NVMe follow-up on malphite (PREREGISTRATION.md, "NVMe follow-up on malphite"): N4 fio device
# ceiling, then N1 (same grid in memory), N2 (ours at subcompactions 8) and N3 (4 cores), each
# 3 blocks with schedule seed 5.
set -uo pipefail
cd ~/lsm-tree
U=$(whoami); D=/tmp/$U/nvme-followup; R=/tmp/$U/retune; GOLDEN=/tmp/$U/x8/golden-50m
export LSM_BUILD_DIR=build/zyra-cuda
mkdir -p $D
exec >> $D/run.log 2>&1

echo "== N4 fio $(date)"
if [ ! -x /tmp/$U/fio/fio ]; then
  git clone -q --depth 1 --branch fio-3.37 https://github.com/axboe/fio.git /tmp/$U/fio
  (cd /tmp/$U/fio && ./configure > /dev/null && make -j 8 > /dev/null)
fi
F=/tmp/$U/fio/fio
fio_common="--filename=$D/fio.dat --size=16G --direct=1 --bs=1M --iodepth=32 --ioengine=libaio
  --time_based --runtime=60 --ramp_time=5 --group_reporting --output-format=json"
$F --name=prefill --filename=$D/fio.dat --size=16G --direct=1 --bs=1M --rw=write --iodepth=32 \
  --ioengine=libaio > /dev/null
$F --name=seqwrite --rw=write $fio_common > $D/fio-seqwrite.json
$F --name=mix32 --rw=randrw --rwmixread=32 $fio_common > $D/fio-mix32.json
$F --name=mix50 --rw=randrw --rwmixread=50 $fio_common > $D/fio-mix50.json
rm -f $D/fio.dat
echo "== N4 exit $(date)"

python3 - "$D" "$R" <<'PY'
import json, sys
D, R = sys.argv[1], sys.argv[2]
confirm = json.load(open(f"{R}/confirm-grid.json"))
n1 = {"_comment": "N1: confirmation grid with data on /dev/shm (buffered I/O).",
      "common": dict(confirm["common"], **{"use-direct-io": False}), "trials": confirm["trials"]}
n2_trials = json.loads(json.dumps(confirm["trials"]))
for t in n2_trials:
    if t["design"] == "full":
        t["flags"]["subcompactions"] = 8
n2 = {"_comment": "N2: NVMe confirmation with ours at subcompactions 8.",
      "common": confirm["common"], "trials": n2_trials}
headline = json.load(open("configs/system/v3-headline.json"))
n3 = {"_comment": "N3: 4 cores on NVMe with the registered v3-headline 4-core settings (tuned on zyra).",
      "common": confirm["common"],
      "trials": [t for t in headline["trials"] if t["cores"] == 4 and t["design"] in ("native", "full")]}
for name, grid in (("n1", n1), ("n2", n2), ("n3", n3)):
    json.dump(grid, open(f"{D}/{name}-grid.json", "w"), indent=1)
    print(name, json.dumps(grid["common"]), [(t["design"], t["cores"], t["flags"]["subcompactions"]) for t in grid["trials"]])
PY

echo "== N1 in memory $(date)"
SHM=/dev/shm/$U/n1
rm -rf $SHM && mkdir -p $SHM && cp -a $GOLDEN $SHM/golden-50m
python3 publication/system_campaign.py $D/n1 --golden $SHM/golden-50m --grid $D/n1-grid.json \
  --blocks 3 --seed 5 --scratch $SHM/scratch --idle-seconds 0
echo "== N1 exit=$? $(date)"
rm -rf $SHM

echo "== N2 subcompactions $(date)"
python3 publication/system_campaign.py $D/n2 --golden $GOLDEN --grid $D/n2-grid.json \
  --blocks 3 --seed 5 --scratch $D/scratch --idle-seconds 0
echo "== N2 exit=$? $(date)"

echo "== N3 four cores $(date)"
python3 publication/system_campaign.py $D/n3 --golden $GOLDEN --grid $D/n3-grid.json \
  --blocks 3 --seed 5 --scratch $D/scratch --idle-seconds 0
echo "== N3 exit=$? $(date)"
rm -rf $D/scratch
echo "== DONE $(date)"
