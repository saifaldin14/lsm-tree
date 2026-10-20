#!/bin/bash
# Head-to-head with gParaKV (PREREGISTRATION.md, H12). Every system runs under gParaKV's own YCSB
# driver (ycsbc): gParaKV through its LevelDB binding, RocksDB through bench/ycsbc/rocksdb_db.cc
# (lsm_ycsbc). Each run loads recordcount records (random inserts, one client), then runs YCSB A
# (50% reads, 50% updates, Zipfian) for operationcount operations, on 4 cores of NUMA node 0
# (memory preferred there) and the same GPU, with data on tmpfs.
#
# Usage: publication/gparakv/run.sh OUT_DIR [REPETITIONS]
# Needs publication/gparakv/setup.sh and lsm_ycsbc (configure with -DLSM_YCSBC_DIR=<gparakv>/ycsbc).
set -euo pipefail
out=${1:?usage: run.sh OUT_DIR [REPETITIONS]}
repetitions=${2:-3}
work=${WORK:-/tmp/$USER}
gparakv=$work/gparakv/build/bin/ycsbc
ours=${LSM_BUILD_DIR:-build/dev}/lsm_ycsbc
scratch=${SCRATCH:-/dev/shm/$USER/h2h}
cores=${CORES:-0-3}
export LD_LIBRARY_PATH="$work/pmdk-install/lib:$work/pmdk-install/lib64:${LD_LIBRARY_PATH:-}"
export LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024 \
  LSM_MERGE_READ_THREADS=4 LSM_GPU_MERGE_STREAMS=2
mkdir -p "$out"
spec_template=$work/gparakv/workloads/10/workloada.spec

# name|binary|environment
systems=(
  "gparakv|$gparakv|PMEM_IS_PMEM_FORCE=1 GPARAKV_PMEM_POOL=$scratch/pool/my_pool"
  "rocksdb_native|$ours|LSM_YCSB_MODE=native_cpu"
  "rocksdb_blobdb|$ours|LSM_YCSB_MODE=native_cpu LSM_YCSB_BLOB_MIN_BYTES=1"
  "rocksdb_gpu_full|$ours|LSM_YCSB_MODE=cuda_merge"
  "rocksdb_native_lz4|$ours|LSM_YCSB_MODE=native_cpu LSM_YCSB_COMPRESSION=lz4"
  "rocksdb_gpu_full_lz4|$ours|LSM_YCSB_MODE=cuda_merge LSM_YCSB_COMPRESSION=lz4"
)
# value bytes|records|operations (about 7-10 GB of values each)
sizes=("128|50000000|10000000" "1024|10000000|10000000")

for repetition in $(seq 1 "$repetitions"); do
  for size in "${sizes[@]}"; do
    IFS='|' read -r value records operations <<< "$size"
    spec=$out/workloada-v$value.spec
    sed -e "s/^recordcount=.*/recordcount=$records/" -e "s/^operationcount=.*/operationcount=$operations/" \
        -e "s/^fieldlength=.*/fieldlength=$value/" "$spec_template" > "$spec"
    mapfile -t order < <(printf '%s\n' "${systems[@]}" | shuf --random-source=<(yes "$repetition$value"))
    for entry in "${order[@]}"; do
      IFS='|' read -r name binary environment <<< "$entry"
      tag="$name-v$value-r$repetition"
      [ -f "$out/$tag.done" ] && continue
      rm -rf "$scratch"; mkdir -p "$scratch/db" "$scratch/vlog" "$scratch/pool"
      env $environment /usr/bin/time -f '%e %U %S' -o "$out/$tag.time" \
        numactl --physcpubind="$cores" --preferred=0 "$binary" -filename "$scratch/db" -vlog "$scratch/vlog" \
        -db leveldb -configpath 0 -P "$spec" > "$out/$tag.out" 2> "$out/$tag.err" \
        && touch "$out/$tag.done" || echo "$tag failed" >&2
      echo "$tag $(grep -E 'Transaction throughput' "$out/$tag.err" | tail -1)"
    done
  done
done
rm -rf "$scratch"
