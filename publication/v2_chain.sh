#!/usr/bin/env bash
# Runs the v2 campaigns on the GPU host (PREREGISTRATION.md, "Methodology v2"), in order:
# V1 headline, V2 workloads, V3 MixGraph, V4 SATA SSD. Needs configs/system/tuned.json
# (publication/v2_summary.py --select) and the grids from publication/v2_grids.py.
# Every campaign resumes where it stopped.
#
# Usage: publication/v2_chain.sh OUT_ROOT
# Environment: LSM_BUILD_DIR (default build/zyra-cuda), SHM (default /dev/shm/$USER/v2),
#              SSD (a directory on the SSD, default /tmp/$USER/v2-ssd), BLOCKS (default 3),
#              ONLY (space-separated subset of: headline stream metadata timeseries mixgraph ssd loopback)
set -euo pipefail
out=${1:?usage: v2_chain.sh OUT_ROOT}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export LSM_BUILD_DIR=${LSM_BUILD_DIR:-$root/build/zyra-cuda}
shm=${SHM:-/dev/shm/$USER/v2}
ssd=${SSD:-/tmp/$USER/v2-ssd}
blocks=${BLOCKS:-3}
only=${ONLY:-headline stream metadata timeseries mixgraph ssd loopback}
bench=$LSM_BUILD_DIR/lsm_system_bench
mkdir -p "$out" "$shm/scratch" "$ssd/scratch"

wanted() { [[ " $only " == *" $1 "* ]]; }

# Preloads a golden database with the key and value shape of a grid's first trial.
golden() {
  local directory=$1 grid=$2
  [ -f "$directory.done" ] && return
  local shape
  shape=$(python3 -c "
import json, sys
flags = json.load(open('$grid'))['trials'][0]['flags']
keys = ['workload', 'retention', 'key-bytes', 'key-bytes-max', 'value-bytes', 'value-bytes-max', 'key-families']
print(' '.join(f'--{k} {flags[k]}' for k in keys if k in flags))")
  rm -rf "$directory"
  # shellcheck disable=SC2086
  "$bench" --phase preload --dir "$directory" --records 50000000 --threads 8 $shape > "$directory.json"
  touch "$directory.done"
}

campaign() {
  local name=$1 golden_dir=$2 grid=$3 scratch=$4
  local resume=()
  [ -f "$out/$name/schedule.json" ] && resume=(--resume)
  echo "== $name $(date)"
  python3 publication/system_campaign.py "$out/$name" --golden "$golden_dir" --grid "$grid" --blocks "$blocks" \
    --scratch "$scratch" --idle-seconds 0 "${resume[@]}"
}

if wanted headline; then
  golden "$shm/golden-50m" configs/system/v2-headline.json
  campaign v2-headline "$shm/golden-50m" configs/system/v2-headline.json "$shm/scratch"
fi
for workload in stream metadata timeseries; do
  if wanted "$workload"; then
    golden "$shm/golden-$workload" "configs/system/v2-$workload.json"
    campaign "v2-$workload" "$shm/golden-$workload" "configs/system/v2-$workload.json" "$shm/scratch"
  fi
done
if wanted mixgraph; then
  echo "== v2-mixgraph $(date)"
  CORES="8 16" REPETITIONS=$blocks DURATION=300 publication/mixgraph/run.sh "$out/v2-mixgraph" "$shm/mixgraph"
fi
if wanted ssd; then
  # The same database as the headline campaign, copied to the SSD; direct I/O keeps the page
  # cache out of the way.
  golden "$shm/golden-50m" configs/system/v2-headline.json
  if [ ! -f "$ssd/golden-50m.done" ]; then
    rm -rf "$ssd/golden-50m" && cp -a "$shm/golden-50m" "$ssd/golden-50m" && touch "$ssd/golden-50m.done"
  fi
  campaign v2-ssd "$ssd/golden-50m" configs/system/v2-ssd.json "$ssd/scratch"
fi
if wanted loopback; then
  echo "== v2-loopback $(date)"
  golden "$shm/golden-50m" configs/system/v2-headline.json
  python3 publication/rdma/loopback_gpu.py "$out/v2-loopback" --golden "$shm/golden-50m" \
    --flags configs/system/tuned.json --repetitions "$blocks"
fi
echo "== v2 chain done $(date)"
