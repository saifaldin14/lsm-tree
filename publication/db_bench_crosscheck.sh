#!/usr/bin/env bash
# Cross-checks the native baseline of lsm_system_bench against RocksDB's own
# db_bench, built from the same pinned (patched) RocksDB source with the same
# options. Run on the GPU host when it is otherwise idle.
#
# Usage: publication/db_bench_crosscheck.sh OUT_DIR [SCRATCH_DIR]
set -euo pipefail
out=${1:?usage: db_bench_crosscheck.sh OUT_DIR [SCRATCH_DIR]}
scratch=${2:-/dev/shm/$USER/db-bench}
root=$(cd "$(dirname "$0")/.." && pwd)
source_dir=$(ls -d "$root"/build/*/_deps/rocksdb-src | head -1)
vcpkg_root="$root/.deps/vcpkg"
gflags_root="$HOME/.cache/lsm-lab/gflags"
bench_build="$HOME/.cache/lsm-lab/rocksdb-db-bench"
mkdir -p "$out"

# gflags (needed only by db_bench) in a separate vcpkg install root.
if [ ! -d "$gflags_root/x64-linux/include/gflags" ]; then
  (cd /tmp && "$vcpkg_root/vcpkg" install gflags lz4 --triplet x64-linux --classic --x-install-root="$gflags_root")
fi
if [ ! -x "$bench_build/db_bench" ]; then
  cmake -S "$source_dir" -B "$bench_build" -DCMAKE_BUILD_TYPE=Release -DPORTABLE=0 -DWITH_GFLAGS=ON \
    -DWITH_LZ4=ON -DWITH_ZSTD=OFF -DWITH_SNAPPY=OFF -DWITH_ZLIB=OFF -DWITH_BZ2=OFF -DWITH_TESTS=OFF \
    -DWITH_TOOLS=OFF -DWITH_BENCHMARK_TOOLS=ON -DFAIL_ON_WARNINGS=OFF \
    -DCMAKE_PREFIX_PATH="$gflags_root/x64-linux" -DCMAKE_CXX_FLAGS="-I$root/include"
  cmake --build "$bench_build" --target db_bench -j "$(nproc)"
fi

# Same shape as the lsm_system_bench runs: 50 M keys, 16 B keys, 128 B values,
# 4 KiB blocks, LZ4, 64 MiB memtables and files, 10 bits/key Bloom filter,
# 4 client threads and 4 physical cores on the GPU's NUMA node.
common=(--num=50000000 --key_size=16 --value_size=128 --compression_type=lz4 --block_size=4096
        --write_buffer_size=67108864 --target_file_size_base=67108864 --max_bytes_for_level_base=268435456
        --max_write_buffer_number=4 --bloom_bits=10 --cache_size=268435456 --format_version=6
        --compression_ratio=0.5 --statistics=1 --stats_per_interval=0 --histogram=1)
cpus=$(python3 -c "
import sys; sys.path.insert(0, '$root/publication')
from system_campaign import physical_cores
print(','.join(map(str, physical_cores(0, 4)[0])))")
rm -rf "$scratch"
numactl --physcpubind="$cpus" --membind=0 "$bench_build/db_bench" --db="$scratch" --benchmarks=fillrandom \
  --threads=8 --disable_wal=1 --max_background_jobs=8 "${common[@]}" > "$out/load.txt" 2>&1
for repetition in 1 2 3; do
  for jobs in 2 4; do
    rm -rf "$scratch-run" && cp -a "$scratch" "$scratch-run"
    numactl --physcpubind="$cpus" --membind=0 "$bench_build/db_bench" --db="$scratch-run" --use_existing_db=1 \
      --benchmarks=overwrite,waitforcompaction --threads=4 --duration=150 --max_background_jobs=$jobs "${common[@]}" \
      > "$out/overwrite-bg$jobs-rep$repetition.txt" 2>&1
  done
done
rm -rf "$scratch" "$scratch-run"
grep -h "^overwrite" "$out"/overwrite-*.txt
