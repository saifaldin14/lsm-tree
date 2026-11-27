#!/bin/bash
cd ~/lsm-tree
U=$(whoami); D=/tmp/$U/x8
export LSM_BUILD_DIR=build/zyra-cuda
exec > $D/run.log 2>&1
echo "== preload golden on NVMe $(date)"
build/zyra-cuda/lsm_system_bench --phase preload --dir $D/golden-50m --records 50000000 --threads 8 > $D/golden-50m.json
echo "== preload exit=$? $(date) size=$(du -sh $D/golden-50m | cut -f1)"
echo "== campaign $(date)"
python3 publication/system_campaign.py $D/out --golden $D/golden-50m --grid $D/grid.json \
  --blocks 3 --seed 1 --scratch $D/scratch --idle-seconds 0
echo "== campaign exit=$? $(date)"
