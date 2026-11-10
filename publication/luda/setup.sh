#!/bin/bash
# Builds LUDA (https://github.com/xpsair/luda, LevelDB 1.22 + CUDA) and its db_bench for the
# head-to-head (PREREGISTRATION.md, "LUDA head-to-head"), plus stock LevelDB 1.22 (the release LUDA
# forked) as the baseline. LUDA's own switch, GPU_ACCELERATE at 0, deadlocks in the first compaction.
#
# Build system: CMake with CUDA enabled for this GPU, standing in for the README's hand edits of
# the generated makefiles. moderngpu is pinned to the last commit before LUDA's code (2019-10-14).
#
# Two source changes to LUDA, both needed to build and run on this machine:
# - Its single mgpu::merge call passes launch_params_t<128, 11>, moderngpu's own sm_35 launch
#   shape. moderngpu's default for sm_52 and later (128 x 15) needs 61,440 bytes of shared memory
#   for LUDA's 32-byte WpSlice, over the 48 KiB static limit, so it does not link. Only the tile
#   size changes, not the merge result.
# - thread_write_file (db/db_impl.cc) is declared void* but never returns. GCC 13 treats the
#   missing return as unreachable and falls through into the next function, crashing the first
#   compaction.
#
# Usage: publication/luda/setup.sh [WORK_DIR]   (default /tmp/$USER)
set -euo pipefail
work=${1:-/tmp/$USER}
commit=89792ac
moderngpu_commit=${MODERNGPU_COMMIT:-01aabbe}
arch=${CUDA_ARCH:-86}
nvcc=${NVCC:-/usr/local/cuda-12.3/bin/nvcc}
cd "$work"

[ -d moderngpu ] || git clone -q https://github.com/moderngpu/moderngpu.git moderngpu
git -C moderngpu checkout -q "$moderngpu_commit"
[ -d luda ] || git clone -q https://github.com/xpsair/luda.git luda
cd luda
git checkout -q "$commit"
git checkout -q -- .
rm -rf build ycsbc
sed -i "s|/home/xp/flying/moderngpu/src/|$work/moderngpu/src/|" cuda/decode_kv.cu
sed -i 's|^    merge(low_slices, low_num|    merge<mgpu::launch_params_t<128, 11> >(low_slices, low_num|' cuda/decode_kv.cu
grep -q 'merge<mgpu::launch_params_t<128, 11> >(low_slices' cuda/decode_kv.cu
sed -i '/^void\* thread_write_file(void\* arg) {/,/^}/ s|^  delete pencode;$|  delete pencode;\n  return nullptr;|' db/db_impl.cc
grep -A12 '^void\* thread_write_file' db/db_impl.cc | grep -q 'return nullptr;'

cat >> CMakeLists.txt <<EOF

# --- LUDA CUDA build (publication/luda/setup.sh) ---
enable_language(CUDA)
set(CMAKE_CUDA_ARCHITECTURES $arch)
set(CMAKE_CUDA_STANDARD 14)
set_source_files_properties("\${PROJECT_SOURCE_DIR}/cuda/util.cu" PROPERTIES HEADER_FILE_ONLY ON)
set_target_properties(leveldb PROPERTIES CUDA_SEPARABLE_COMPILATION ON CUDA_RESOLVE_DEVICE_SYMBOLS ON)
target_compile_options(leveldb PRIVATE \$<\$<COMPILE_LANGUAGE:CUDA>:--extended-lambda -Xptxas -dlcm=ca>)
target_include_directories(leveldb PRIVATE "$work/moderngpu/src")
find_package(CUDAToolkit REQUIRED)
target_link_libraries(leveldb CUDA::cudart)
EOF

build() {
  cmake -S "$1" -B "$1/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER="$nvcc" \
    -DLEVELDB_BUILD_TESTS=OFF -DLEVELDB_BUILD_BENCHMARKS=ON -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  cmake --build "$1/build" -j 16 --target db_bench
  ls -la "$1/build/db_bench"
}
build "$work/luda"

# The baseline: stock LevelDB 1.22, built the same way (no snappy on this machine, as for LUDA).
cd "$work"
[ -d leveldb ] || git clone -q https://github.com/google/leveldb.git leveldb
git -C leveldb checkout -q 1.22
build "$work/leveldb"
