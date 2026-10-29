#!/usr/bin/env bash
# X1 and X2 (PREREGISTRATION.md, "Methodology v3"): isolated compaction, native against full
# offload, 1 cold + 3 warm repetitions, fallocate off for both. X1: one subcompaction. X2: 8
# subcompactions on 8 cores with 2.6 GiB of input.
#
# Usage: publication/v3_isolated.sh OUT_DIR [SCRATCH_DIR]
set -euo pipefail
out=${1:?usage: v3_isolated.sh OUT_DIR [SCRATCH_DIR]}
scratch=${2:-/dev/shm/$USER/v3-isolated}
bench=${LSM_BUILD_DIR:-build/zyra-cuda}/lsm_compaction_bench
mkdir -p "$out"
export LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024 \
       LSM_MERGE_READ_THREADS=4 LSM_GPU_MERGE_STREAMS=2
cpus=$(python3 -c "
import sys; sys.path.insert(0, 'publication')
from system_campaign import physical_cores
print(','.join(map(str, physical_cores(0, 8)[0])))")
run() {
  local tag=$1 subcompactions=$2; shift 2
  [ -s "$out/$tag.jsonl" ] && return
  rm -rf "$scratch"
  numactl --physcpubind="$cpus" --membind=0 "$bench" --dir "$scratch" --modes native_cpu,cuda_merge \
    --l0-files 8 --block-bytes 4096 --repetitions 4 --allow-fallocate false --subcompactions "$subcompactions" "$@" \
    > "$out/$tag.jsonl.tmp"
  mv "$out/$tag.jsonl.tmp" "$out/$tag.jsonl"
  echo "$tag done $(date)"
}
# X1: value bytes, records, compression, delete fraction
for config in "32 10000000 lz4 0" "128 4000000 lz4 0" "1024 1000000 lz4 0" "128 4000000 zstd 0" \
              "128 4000000 none 0" "128 4000000 lz4 0.1"; do
  set -- $config
  tag="x1-v$1-$3"; [ "$4" != "0" ] && tag="$tag-del$4"
  run "$tag" 1 --records "$2" --value-bytes "$1" --compression "$3" --delete-fraction "$4"
done
# X2
run x2-v128-lz4 8 --records 16000000 --value-bytes 128 --compression lz4 --delete-fraction 0
run x2-v128-zstd 8 --records 16000000 --value-bytes 128 --compression zstd --delete-fraction 0
rm -rf "$scratch"
