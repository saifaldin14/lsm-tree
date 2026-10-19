#!/usr/bin/env bash
# Isolated compaction sweep: one full CompactRange per run, native RocksDB vs
# the three GPU offload levels, with an exact differential check of every
# internal version and 200,000 point lookups against native.
#
# Usage: publication/compaction_sweep.sh OUT_DIR [SCRATCH_DIR]
# Needs a CUDA build (LSM_BUILD_DIR, default build/dev). SCRATCH_DIR should be
# on tmpfs (default /dev/shm/$USER/compaction-sweep). Repetition 0 is a cold
# start; analysis uses repetitions 1-3.
set -euo pipefail
out=${1:?usage: compaction_sweep.sh OUT_DIR [SCRATCH_DIR]}
scratch=${2:-/dev/shm/$USER/compaction-sweep}
bench=${LSM_BUILD_DIR:-build/dev}/lsm_compaction_bench
mkdir -p "$out"
export LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024

# value bytes, records, compression
for config in "32 10000000 lz4" "128 4000000 lz4" "1024 1000000 lz4" "128 4000000 none"; do
  set -- $config
  tag="v$1-$3"
  common=(--records "$2" --value-bytes "$1" --compression "$3" --l0-files 8 --block-bytes 4096
          --delete-fraction 0 --repetitions 4)
  # Native, GPU output only, and GPU merge + block building (Stage 3).
  rm -rf "$scratch"
  "$bench" --dir "$scratch" --modes native_cpu,gpu_output,cuda_merge "${common[@]}" > "$out/$tag-s3.jsonl"
  # GPU merge only (Stage 2): block building disabled.
  rm -rf "$scratch"
  LSM_DISABLE_GPU_BUILD=1 "$bench" --dir "$scratch" --modes cuda_merge "${common[@]}" > "$out/$tag-s2.jsonl"
  echo "$tag done"
done
rm -rf "$scratch"
