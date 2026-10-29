#!/usr/bin/env bash
# Runs the v3 campaigns on the GPU host (PREREGISTRATION.md, "Methodology v3"): X1/X2 isolated,
# X3 headline, X4 LZ4 + bottommost ZSTD, X5 open loop. Every campaign resumes where it stopped.
#
# Usage: publication/v3_chain.sh OUT_ROOT
# Environment: LSM_BUILD_DIR (default build/zyra-cuda), SHM (default /dev/shm/$USER/v2, which holds
#              golden-50m and golden-50m-lz4zstd), BLOCKS (default 2), ONLY (subset of:
#              isolated headline bottomzstd openloop)
set -euo pipefail
out=${1:?usage: v3_chain.sh OUT_ROOT}
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
export LSM_BUILD_DIR=${LSM_BUILD_DIR:-$root/build/zyra-cuda}
shm=${SHM:-/dev/shm/$USER/v2}
blocks=${BLOCKS:-2}
only=${ONLY:-isolated headline bottomzstd openloop}
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
campaign() {
  local name=$1 golden_dir=$2 grid=$3
  local resume=()
  [ -f "$out/$name/schedule.json" ] && resume=(--resume)
  echo "== $name $(date)"
  python3 publication/system_campaign.py "$out/$name" --golden "$golden_dir" --grid "$grid" --blocks "$blocks" \
    --scratch "$shm/scratch" --idle-seconds 0 "${resume[@]}"
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
echo "== v3 chain done $(date)"
