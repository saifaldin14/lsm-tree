#!/usr/bin/env bash
# Earlier-design comparison, isolated compaction (PREREGISTRATION.md, H8-H9): one full
# CompactRange per run with an exact differential check against native RocksDB.
#
#   <tag>-s3.jsonl         native, GPU compression only (LZ4 only), full offload (ours)
#   <tag>-s2.jsonl         GPU merge with CPU block building (our merge-only level)
#   <tag>-sortmerge.jsonl  sort/merge offload as in gLSM and gParaKV: GPU unpack, sort and merge;
#                          RocksDB CPU code checks and decompresses input, builds and compresses output
#   <tag>-pipeline.jsonl   pipeline offload without compression as in LUDA, MSST'24 and the FPGA design:
#                          GPU unpack, sort, merge, deletions and block building; RocksDB CPU code checks
#                          and decompresses input and compresses and checksums output
#
# Usage: publication/prior_compaction_sweep.sh OUT_DIR [SCRATCH_DIR]
# Needs a CUDA build (LSM_BUILD_DIR, default build/dev). Repetition 0 is a cold start; analysis
# uses repetitions 1-3.
set -euo pipefail
out=${1:?usage: prior_compaction_sweep.sh OUT_DIR [SCRATCH_DIR]}
scratch=${2:-/dev/shm/$USER/prior-sweep}
bench=${LSM_BUILD_DIR:-build/dev}/lsm_compaction_bench
mkdir -p "$out"
export LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024

run() {  # file, then environment assignments and bench arguments
  local file=$1; shift
  rm -rf "$scratch"
  env "$@" > "$out/$file"
}

# value bytes, records, compression, fraction of L0 writes that are deletes
for config in "32 10000000 lz4 0" "128 4000000 lz4 0" "1024 1000000 lz4 0" "128 4000000 none 0" \
              "128 4000000 zstd 0" "128 4000000 lz4 0.1"; do
  set -- $config
  tag="v$1-$3"
  [ "$4" != "0" ] && tag="$tag-del$4"
  common=(--records "$2" --value-bytes "$1" --compression "$3" --l0-files 8 --block-bytes 4096
          --delete-fraction "$4" --repetitions 4 --dir "$scratch")
  run "$tag-s3.jsonl" "$bench" --modes native_cpu,gpu_output,cuda_merge "${common[@]}"
  run "$tag-s2.jsonl" LSM_DISABLE_GPU_BUILD=1 "$bench" --modes cuda_merge "${common[@]}"
  run "$tag-sortmerge.jsonl" LSM_DISABLE_GPU_BUILD=1 LSM_GPU_CPU_DECOMPRESS=1 LSM_DISABLE_GPU_OUTPUT=1 \
    "$bench" --modes cuda_merge "${common[@]}"
  run "$tag-pipeline.jsonl" LSM_GPU_CPU_DECOMPRESS=1 LSM_GPU_CPU_COMPRESS=1 "$bench" --modes cuda_merge "${common[@]}"
  echo "$tag done"
done
rm -rf "$scratch"
