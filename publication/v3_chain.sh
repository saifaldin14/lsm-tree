#!/usr/bin/env bash
# Runs the v3 campaigns on the GPU host (PREREGISTRATION.md, "Methodology v3"): X1/X2 isolated,
# X3 headline, X4 LZ4 + bottommost ZSTD, X5 open loop. Every campaign resumes where it stopped.
#
# Usage: publication/v3_chain.sh OUT_ROOT
# Environment: LSM_BUILD_DIR (default build/zyra-cuda), SHM (default /dev/shm/$USER/v2, which holds
#              golden-50m and golden-50m-lz4zstd), BLOCKS (default 2), SSD (a directory on the SSD,
#              default /tmp/$USER/v3-ssd), ONLY (subset of: isolated headline bottomzstd openloop
#              more stream metadata timeseries mixgraph ssd; default: the first four). "more" adds
#              3 blocks (seed 2) to headline, bottomzstd and openloop (PREREGISTRATION, deviation).
set -euo pipefail
out=${1:?usage: v3_chain.sh OUT_ROOT}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export LSM_BUILD_DIR=${LSM_BUILD_DIR:-$root/build/zyra-cuda}
shm=${SHM:-/dev/shm/$USER/v2}
blocks=${BLOCKS:-2}
only=${ONLY:-isolated headline bottomzstd openloop}
ssd=${SSD:-/tmp/$USER/v3-ssd}
bench=$LSM_BUILD_DIR/lsm_system_bench
mkdir -p "$out" "$shm/scratch"
wanted() { [[ " $only " == *" $1 "* ]]; }
golden() {
  local directory=$1; shift
  [ -f "$directory.done" ] && return
  rm -rf "$directory"
  "$bench" --phase preload --dir "$directory" --records 50000000 --threads 8 "$@" > "$directory.json"
  touch "$directory.done"
}
# The key and value shape of a grid's first trial, as preload flags.
shape_of() {
  python3 -c "
import json
flags = json.load(open('$1'))['trials'][0]['flags']
keys = ['workload', 'retention', 'key-bytes', 'key-bytes-max', 'value-bytes', 'value-bytes-max', 'key-families']
print(' '.join(f'--{k} {flags[k]}' for k in keys if k in flags))"
}
# campaign NAME GOLDEN GRID [BLOCKS [SEED [SCRATCH]]]
campaign() {
  local name=$1 golden_dir=$2 grid=$3 count=${4:-$blocks} seed=${5:-1} scratch=${6:-$shm/scratch}
  local resume=()
  [ -f "$out/$name/schedule.json" ] && resume=(--resume)
  echo "== $name $(date)"
  mkdir -p "$scratch"
  python3 publication/system_campaign.py "$out/$name" --golden "$golden_dir" --grid "$grid" --blocks "$count" \
    --seed "$seed" --scratch "$scratch" --idle-seconds 0 "${resume[@]}"
}
if wanted isolated; then
  echo "== isolated $(date)"
  publication/v3_isolated.sh "$out/v3-isolated" "$shm/isolated"
fi
golden "$shm/golden-50m"
golden "$shm/golden-50m-lz4zstd" --compression lz4 --bottommost-compression zstd
wanted headline && campaign v3-headline "$shm/golden-50m" configs/system/v3-headline.json
wanted bottomzstd && campaign v3-bottomzstd "$shm/golden-50m-lz4zstd" configs/system/v3-bottomzstd.json
wanted openloop && campaign v3-openloop "$shm/golden-50m" configs/system/v3-openloop.json
if wanted more; then
  campaign v3-headline-more "$shm/golden-50m" configs/system/v3-headline.json 3 2
  campaign v3-bottomzstd-more "$shm/golden-50m-lz4zstd" configs/system/v3-bottomzstd.json 3 2
  campaign v3-openloop-more "$shm/golden-50m" configs/system/v3-openloop.json 3 2
fi
for workload in stream metadata timeseries; do
  if wanted "$workload"; then
    # shellcheck disable=SC2046
    golden "$shm/golden-$workload" $(shape_of "configs/system/v3-$workload.json")
    campaign "v3-$workload" "$shm/golden-$workload" "configs/system/v3-$workload.json" 3
  fi
done
if wanted mixgraph; then
  echo "== v3-mixgraph $(date)"
  CORES="8 16" REPETITIONS=3 DURATION=300 publication/mixgraph/run.sh "$out/v3-mixgraph" "$shm/mixgraph"
fi
if wanted ssd; then
  mkdir -p "$ssd"
  if [ ! -f "$ssd/golden-50m.done" ]; then
    rm -rf "$ssd/golden-50m" && cp -a "$shm/golden-50m" "$ssd/golden-50m" && touch "$ssd/golden-50m.done"
  fi
  campaign v3-ssd "$ssd/golden-50m" configs/system/v3-ssd.json 3 1 "$ssd/scratch"
fi
echo "== v3 chain done $(date)"
