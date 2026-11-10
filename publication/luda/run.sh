#!/bin/bash
# LUDA head-to-head (PREREGISTRATION.md, "LUDA head-to-head"). Each system runs through its own
# db_bench: stock LevelDB 1.22, LUDA, native RocksDB and full offload
# (lsm_db_bench). fillrandom of NUM 1 KiB values from one thread, then readrandom; no compression;
# 4 cores of NUMA node 0 and the same GPU, data on tmpfs.
#
# Usage: publication/luda/run.sh OUT_DIR [REPETITIONS]
# Needs publication/luda/setup.sh (builds WORK/luda/build and WORK/leveldb/build) and lsm_db_bench.
set -euo pipefail
out=${1:?usage: run.sh OUT_DIR [REPETITIONS]}
repetitions=${2:-3}
work=${WORK:-/tmp/$USER}
rocks=${LSM_BUILD_DIR:-build/zyra-cuda}/lsm_db_bench
scratch=${SCRATCH:-/dev/shm/$USER/luda-h2h}
cores=${CORES:-0-3}
num=${NUM:-10000000}
reads=${READS:-1000000}
# GPU settings used by every campaign (publication/system_campaign.py, GPU_ENV).
export LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024 \
  LSM_MERGE_READ_THREADS=4 LSM_GPU_MERGE_STREAMS=2 LSM_GPU_PINNED_PREALLOC_MB=1024
mkdir -p "$out"

common=(--num="$num" --reads="$reads" --value_size=1024 --compression_ratio=0.5 --bloom_bits=10
        --write_buffer_size=15728640 --threads=1 --benchmarks=fillrandom,readrandom)
leveldb_options=(--max_file_size=15728640)
rocksdb_options=(--key_size=16 --compression_type=none --target_file_size_base=15728640
                 --max_background_jobs=4 --subcompactions=4 --seed=301)

# name|binary|environment|engine
systems=(
  "leveldb|$work/leveldb/build/db_bench||leveldb"
  "luda|$work/luda/build/db_bench||leveldb"
  "rocksdb_native|$rocks|LSM_DB_BENCH_MODE=native_cpu|rocksdb"
  "rocksdb_gpu_full|$rocks|LSM_DB_BENCH_MODE=cuda_merge|rocksdb"
)

for repetition in $(seq 1 "$repetitions"); do
  mapfile -t order < <(printf '%s\n' "${systems[@]}" | shuf --random-source=<(yes "luda$repetition"))
  for entry in "${order[@]}"; do
    IFS='|' read -r name binary environment engine <<< "$entry"
    tag="$name-r$repetition"
    [ -f "$out/$tag.done" ] && continue
    if [ "$engine" = leveldb ]; then extra=("${leveldb_options[@]}"); else extra=("${rocksdb_options[@]}"); fi
    rm -rf "$scratch"; mkdir -p "$scratch"
    nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader,nounits -lms 500 \
      > "$out/$tag.gpu.csv" 2>/dev/null & gpu_pid=$!
    env $environment /usr/bin/time -f 'lsm_cpu wall_s=%e user_s=%U system_s=%S' -o "$out/$tag.time" \
      numactl --physcpubind="$cores" --preferred=0 "$binary" --db="$scratch/db" "${common[@]}" "${extra[@]}" \
      > "$out/$tag.out" 2> "$out/$tag.err" && touch "$out/$tag.done" || echo "$tag failed" >&2
    kill "$gpu_pid" 2>/dev/null || true
    echo "$tag $(grep -hE '^(fillrandom|readrandom)' "$out/$tag.out" | tr -s ' ' | tr '\n' ' ') $(cat "$out/$tag.time")"
  done
done
rm -rf "$scratch"
