#!/usr/bin/env bash
# Meta production workloads (MixGraph; Cao et al., FAST'20) through RocksDB's own db_bench,
# built as lsm_db_bench from the pinned RocksDB source with our compaction transform installed
# by LSM_DB_BENCH_MODE (bench/db_bench/transform_hook.cc). The same binary, options and
# database run every design; native_cpu doubles as the db_bench cross-check of native RocksDB.
#
# Workloads:
#   zippydb        the ZippyDB model published with mixgraph (RocksDB wiki, "Synthetic Workload
#                  Generation based on Models"): 50 M keys of 48 B, Pareto value sizes
#                  (k 0.2615, sigma 25.45), 30 hot key ranges, 85% get / 14% put / 1% seek.
#   zippydb_write  the same model with a write-heavy mix (49% get / 50% put / 1% seek), ours,
#                  because a 14% put rate barely exercises compaction.
# The published sine rate limit is left out, so each run measures the highest sustained rate.
# Each run is fixed-time (--duration) on a fresh copy of one preloaded database.
#
# Usage: publication/mixgraph/run.sh OUT_DIR [SCRATCH_DIR]
# Environment: LSM_BUILD_DIR (default build/zyra-cuda), MODES, WORKLOADS, CORES (list),
#              DURATION (s, default 300), REPETITIONS (default 3), DIRECT_IO (0/1),
#              SUBCOMPACTIONS (default: equal to the cores), NUM (keys, default 50000000).
set -euo pipefail
out=${1:?usage: run.sh OUT_DIR [SCRATCH_DIR]}
scratch=${2:-/dev/shm/$USER/mixgraph}
root=$(cd "$(dirname "$0")/../.." && pwd)
bench=${LSM_BUILD_DIR:-$root/build/zyra-cuda}/lsm_db_bench
modes=${MODES:-native_cpu cuda_merge}
workloads=${WORKLOADS:-zippydb zippydb_write}
cores_list=${CORES:-8 16}
duration=${DURATION:-300}
repetitions=${REPETITIONS:-3}
direct=${DIRECT_IO:-0}
mkdir -p "$out" "$scratch"
[ -x "$bench" ] || { echo "missing $bench (configure with -DLSM_BUILD_DB_BENCH=ON)" >&2; exit 1; }

# The database shape of every other experiment (format 6 and CRC32C are what the GPU path reads
# and writes), with the ZippyDB key size and mean value size.
shape=(--key_size=48 --value_size=35 --num=${NUM:-50000000} --compression_type=lz4 --block_size=4096
       --write_buffer_size=67108864 --max_write_buffer_number=4 --target_file_size_base=67108864
       --max_bytes_for_level_base=268435456 --bloom_bits=10 --cache_size=268435456 --format_version=6
       --checksum_type=1 --compression_ratio=0.5 --statistics=1 --stats_per_interval=0 --histogram=1
       --use_direct_io_for_flush_and_compaction=$direct --use_direct_reads=$direct)
zippydb=(--keyrange_dist_a=14.18 --keyrange_dist_b=-2.917 --keyrange_dist_c=0.0164 --keyrange_dist_d=-0.08082
         --keyrange_num=30 --value_k=0.2615 --value_sigma=25.45 --iter_k=2.517 --iter_sigma=14.236)
# GPU settings used by every campaign (publication/system_campaign.py, GPU_ENV).
export LSM_DISABLE_GPU_INPUT=1 LSM_CUDA_SYNC=blocking LSM_GPU_OUTPUT_MIN_BLOCK_BYTES=1024 \
       LSM_MERGE_READ_THREADS=4 LSM_GPU_MERGE_STREAMS=2

cpus_for() {
  python3 -c "
import sys; sys.path.insert(0, '$root/publication')
from system_campaign import physical_cores
cpus, nodes = physical_cores(0, $1)
print(','.join(map(str, cpus)), ','.join(map(str, nodes)))"
}

golden=$scratch/golden-${NUM:-50000000}
if [ ! -f "$golden.done" ]; then
  rm -rf "$golden"
  read -r cpus nodes < <(cpus_for 8)
  numactl --physcpubind="$cpus" --membind="$nodes" "$bench" --db="$golden" --benchmarks=fillrandom,waitforcompaction \
    --threads=8 --disable_wal=1 --max_background_jobs=8 --seed=1 "${shape[@]}" > "$out/load.txt" 2>&1
  touch "$golden.done"
fi

for repetition in $(seq 1 "$repetitions"); do
  for cores in $cores_list; do
    read -r cpus nodes < <(cpus_for "$cores")
    for workload in $workloads; do
      case $workload in
        zippydb) mix=(--mix_get_ratio=0.85 --mix_put_ratio=0.14 --mix_seek_ratio=0.01) ;;
        zippydb_write) mix=(--mix_get_ratio=0.49 --mix_put_ratio=0.50 --mix_seek_ratio=0.01) ;;
        *) echo "unknown workload $workload" >&2; exit 1 ;;
      esac
      for mode in $modes; do
        name=$workload-$mode-c$cores-r$repetition
        [ -f "$out/$name.txt" ] && grep -q "^mixgraph" "$out/$name.txt" && continue
        database=$scratch/run
        rm -rf "$database" && cp -a "$golden" "$database"
        # /usr/bin/time adds the process CPU time (db_bench reports only operations).
        LSM_DB_BENCH_MODE=$mode /usr/bin/time -f "lsm_cpu user_s=%U system_s=%S wall_s=%e" \
          numactl --physcpubind="$cpus" --membind="$nodes" "$bench" --db="$database" \
          --use_existing_db=1 --benchmarks=mixgraph --duration="$duration" --reads=1000000000000 \
          --threads="$cores" --max_background_jobs="$cores" --subcompactions="${SUBCOMPACTIONS:-$cores}" \
          --seed=$((1000 + repetition)) "${shape[@]}" "${zippydb[@]}" "${mix[@]}" > "$out/$name.txt" 2>&1 \
          || echo "$name failed" >&2
        grep -h "^mixgraph" "$out/$name.txt" | sed "s/^/$name /" || true
      done
    done
  done
done
rm -rf "$scratch/run"
