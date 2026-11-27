#!/bin/bash
# E3 (PREREGISTRATION.md): 100 M keys with direct I/O on malphite's NVMe, from the E2-E5 checkout
# /tmp/sa29ali/lsm-e (build/e-cuda). Rerun to resume.
set -u
cd /tmp/$USER/lsm-e
export LSM_BUILD_DIR=build/e-cuda
G=/dev/shm/$USER/e3/golden-100m; OUT=/tmp/$USER/e3/out; GRID=configs/system/e3-nvme-large.json
mkdir -p /dev/shm/$USER/e3 /tmp/$USER/e3
if [ ! -f $G.done ]; then
  rm -rf $G
  $LSM_BUILD_DIR/lsm_system_bench --phase preload --dir $G --records 100000000 --threads 8 > $G.json 2>&1 && touch $G.done || exit 1
fi
if [ -d $OUT ]; then
  python3 results/system_campaign.py $OUT --golden $G --grid $GRID --scratch /tmp/$USER/e3/scratch --resume
else
  python3 results/system_campaign.py $OUT --golden $G --grid $GRID --blocks 3 --seed 109 --scratch /tmp/$USER/e3/scratch
fi && touch $OUT/DONE
