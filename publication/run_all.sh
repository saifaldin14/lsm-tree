#!/bin/bash
# Runs every campaign in PREREGISTRATION.md, in priority order. Rerunning resumes
# where it stopped. Usage: publication/run_all.sh [OUT_DIR]
set -u
cd "$(dirname "$0")/.."
export LSM_BUILD_DIR=${LSM_BUILD_DIR:-build/dev}
C=${1:-/tmp/$USER/campaigns}; mkdir -p "$C"
GOLDEN=${GOLDEN:-/dev/shm/$USER/sys/golden-50m}
SSD=${SSD:-/tmp/$USER/ssd}
log() { echo "$(date '+%F %T') $*" >> "$C/chain.log"; }
campaign() {  # name grid blocks seed [golden]
  local name=$1 grid=$2 blocks=$3 seed=$4 golden=${5:-$GOLDEN}
  [ -f "$C/$name/DONE" ] && return
  log "start $name"
  if [ -d "$C/$name" ]; then
    python3 publication/system_campaign.py "$C/$name" --golden "$golden" --grid "$grid" --resume >> "$C/$name.log" 2>&1
  else
    python3 publication/system_campaign.py "$C/$name" --golden "$golden" --grid "$grid" --blocks "$blocks" --seed "$seed" >> "$C/$name.log" 2>&1
  fi && touch "$C/$name/DONE" && log "done $name"
}
if [ ! -f "$C/isolated-v2/DONE" ]; then
  log "start isolated-v2"; rm -rf "$C/isolated-v2"
  bash publication/compaction_sweep.sh "$C/isolated-v2" "/dev/shm/$USER/compaction-sweep" >> "$C/isolated-v2.log" 2>&1 \
    && touch "$C/isolated-v2/DONE" && log "done isolated-v2"
fi
campaign main configs/system/main.json 5 101
campaign equal-cost configs/system/equal-cost.json 5 102
campaign ycsb configs/system/ycsb.json 3 103
campaign contention configs/system/contention.json 3 104
campaign read-tradeoff configs/system/read-tradeoff.json 3 105
campaign shared-gpu configs/system/shared-gpu.json 3 106
if [ ! -f "$SSD/golden-200m.done" ]; then
  log "start ssd preload"; mkdir -p "$SSD"; rm -rf "$SSD/golden-200m"
  "$LSM_BUILD_DIR/lsm_system_bench" --phase preload --dir "$SSD/golden-200m" --records 200000000 --threads 8 \
    > "$SSD/golden-200m.json" 2>&1 && touch "$SSD/golden-200m.done" && log "done ssd preload"
fi
campaign ssd-scale configs/system/ssd-scale.json 3 107 "$SSD/golden-200m"
if [ ! -f "$C/db-bench/DONE" ]; then
  log "start db-bench"
  bash publication/db_bench_crosscheck.sh "$C/db-bench" >> "$C/db-bench.log" 2>&1 && touch "$C/db-bench/DONE" && log "done db-bench"
fi
log "chain finished"
