#!/bin/bash
# Runs the earlier-design comparison campaigns (PREREGISTRATION.md, H8-H11) in order.
# Rerunning resumes where it stopped. Usage: publication/run_prior.sh [OUT_DIR]
set -u
cd "$(dirname "$0")/.."
export LSM_BUILD_DIR=${LSM_BUILD_DIR:-build/dev}
C=${1:-/tmp/$USER/campaigns}; mkdir -p "$C"
SYS=${SYS:-/dev/shm/$USER/sys}
log() { echo "$(date '+%F %T') $*" >> "$C/prior-chain.log"; }
campaign() {  # name grid blocks seed golden
  local name=$1 grid=$2 blocks=$3 seed=$4 golden=$5
  [ -f "$C/$name/DONE" ] && return
  log "start $name"
  if [ -d "$C/$name" ]; then
    python3 publication/system_campaign.py "$C/$name" --golden "$golden" --grid "$grid" --resume >> "$C/$name.log" 2>&1
  else
    python3 publication/system_campaign.py "$C/$name" --golden "$golden" --grid "$grid" --blocks "$blocks" --seed "$seed" >> "$C/$name.log" 2>&1
  fi && touch "$C/$name/DONE" && log "done $name"
}
golden() {  # name compression
  [ -f "$SYS/$1.done" ] && return
  log "start preload $1"; rm -rf "$SYS/$1"
  "$LSM_BUILD_DIR/lsm_system_bench" --phase preload --dir "$SYS/$1" --records 50000000 --threads 8 --compression "$2" \
    > "$SYS/$1.json" 2>&1 && touch "$SYS/$1.done" && log "done preload $1"
}
if [ ! -f "$C/prior-isolated/DONE" ]; then
  log "start prior-isolated"; rm -rf "$C/prior-isolated"
  bash publication/prior_compaction_sweep.sh "$C/prior-isolated" "/dev/shm/$USER/prior-sweep" >> "$C/prior-isolated.log" 2>&1 \
    && touch "$C/prior-isolated/DONE" && log "done prior-isolated"
fi
campaign prior-lz4 configs/system/prior-lz4.json 5 111 "$SYS/golden-50m"
campaign prior-equal-cost configs/system/prior-equal-cost.json 5 114 "$SYS/golden-50m"
golden golden-50m-zstd zstd
campaign prior-zstd configs/system/prior-zstd.json 3 112 "$SYS/golden-50m-zstd"
golden golden-50m-none none
campaign prior-none configs/system/prior-none.json 3 113 "$SYS/golden-50m-none"
# gParaKV head-to-head (needs publication/gparakv/setup.sh and lsm_ycsbc in YCSBC_BUILD_DIR).
if [ ! -f "$C/gparakv/DONE" ]; then
  log "start gparakv"
  LSM_BUILD_DIR=${YCSBC_BUILD_DIR:-$LSM_BUILD_DIR} bash publication/gparakv/run.sh "$C/gparakv" 3 >> "$C/gparakv.log" 2>&1 \
    && touch "$C/gparakv/DONE" && log "done gparakv"
fi
log "prior chain finished"
