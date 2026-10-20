#!/bin/bash
# Builds gParaKV (SC'25, https://github.com/KV890/gParaKV) for the head-to-head comparison
# (PREREGISTRATION.md, H12). Persistent memory is emulated in DRAM: PMDK is built without ndctl
# and the pool lives on tmpfs with PMEM_IS_PMEM_FORCE=1 at run time.
#
# The only source change is that the hard-coded pool path /mnt/pmem/my_pool is read from
# GPARAKV_PMEM_POOL (no algorithmic change).
#
# Usage: publication/gparakv/setup.sh [WORK_DIR]   (default /tmp/$USER)
set -euo pipefail
work=${1:-/tmp/$USER}
commit=14c94b1
pmdk_tag=2.1.1
mkdir -p "$work"
cd "$work"

if [ ! -f pmdk-install/lib/pkgconfig/libpmemobj.pc ]; then
  [ -d pmdk ] || git clone -q --depth 1 --branch "$pmdk_tag" https://github.com/pmem/pmdk.git pmdk
  flags="NDCTL_ENABLE=n DOC=n PMEMOBJ_IGNORE_DIRTY_SHUTDOWN=y PMEMOBJ_IGNORE_BAD_BLOCKS=y"
  make -C pmdk -j 8 $flags
  make -C pmdk install prefix="$work/pmdk-install" $flags
fi

[ -d gparakv ] || git clone -q https://github.com/KV890/gParaKV.git gparakv
cd gparakv
git checkout -q "$commit"
git submodule update -q --init --recursive
grep -rl '"/mnt/pmem/my_pool"' db | xargs -r sed -i \
  's|"/mnt/pmem/my_pool"|(std::getenv("GPARAKV_PMEM_POOL") ? std::getenv("GPARAKV_PMEM_POOL") : "/mnt/pmem/my_pool")|g'
grep -q '#include <cstdlib>' db/db_impl.cc || sed -i '1i #include <cstdlib>' db/db_impl.cc

export PKG_CONFIG_PATH="$work/pmdk-install/lib/pkgconfig:$work/pmdk-install/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$work/pmdk-install/lib:$work/pmdk-install/lib64:${LD_LIBRARY_PATH:-}"
# gParaKV links -lpmemobj without its directory.
export LIBRARY_PATH="$work/pmdk-install/lib:$work/pmdk-install/lib64:${LIBRARY_PATH:-}"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_COMPILER=/usr/local/cuda-12.3/bin/nvcc \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_POLICY_DEFAULT_CMP0146=OLD
cmake --build build -j 8 --target ycsbc
ls -la build/bin/ycsbc
