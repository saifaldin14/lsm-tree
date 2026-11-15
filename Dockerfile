# Container image for the cloud experiments (docs/cloud.md): the benchmark, the compaction server
# and the compaction bench, built for x86-64-v3 (AVX2; every Azure v5 and Alliance cloud host).
#   CPU:  docker build -t lsm-lab:cpu .
#   CUDA: docker build -t lsm-lab:cuda --build-arg VARIANT=cuda \
#           --build-arg BUILD_BASE=nvidia/cuda:12.4.1-devel-ubuntu22.04 \
#           --build-arg RUN_BASE=nvidia/cuda:12.4.1-runtime-ubuntu22.04 .
ARG BUILD_BASE=ubuntu:24.04
ARG RUN_BASE=ubuntu:24.04

FROM ${BUILD_BASE} AS build
ARG VARIANT=cpu
ARG CUDA_ARCHITECTURES=70;75;80;86;89;90
ARG COMMIT=unknown
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential git curl ca-certificates zip unzip tar pkg-config python3 python3-pip python3-venv \
      autoconf automake libtool linux-libc-dev numactl perl \
    && if [ "$VARIANT" = cuda ]; then apt-get install -y --no-install-recommends g++-12; fi \
    && rm -rf /var/lib/apt/lists/*
# The CUDA image is Ubuntu 22.04, whose default GCC 11 is older than the compiler the project is tested with.
ENV CC_cuda=gcc-12 CXX_cuda=g++-12
# CMake >= 3.25 (Ubuntu 22.04 ships 3.22).
RUN python3 -m venv /opt/cmake && /opt/cmake/bin/pip install --no-cache-dir "cmake==3.30.*" \
    && ln -sf /opt/cmake/bin/cmake /usr/local/bin/cmake && ln -sf /opt/cmake/bin/ctest /usr/local/bin/ctest
WORKDIR /src
# Dependencies first, so source changes reuse the cached vcpkg layer.
COPY vcpkg.json CMakePresets.json ./
COPY scripts/bootstrap.sh scripts/bootstrap-cuda.sh scripts/
RUN --mount=type=cache,target=/root/.cache/vcpkg-downloads \
    if [ "$VARIANT" = cuda ]; then export CC=$CC_cuda CXX=$CXX_cuda; fi; \
    VCPKG_DOWNLOADS=/root/.cache/vcpkg-downloads VCPKG_MAX_CONCURRENCY=$(nproc) bash scripts/bootstrap.sh \
    && rm -rf /root/.cache/lsm-lab/*/buildtrees /root/.cache/lsm-lab/*/packages
RUN if [ "$VARIANT" = cuda ]; then bash scripts/bootstrap-cuda.sh; fi
COPY . .
RUN cuda=OFF; if [ "$VARIANT" = cuda ]; then cuda=ON; export CC=$CC_cuda CXX=$CXX_cuda CUDAHOSTCXX=$CXX_cuda; fi; \
    cmake --preset dev -DLSM_ROCKSDB_PORTABLE=x86-64-v3 -DLSM_ENABLE_RDMA=OFF -DLSM_ENABLE_CUDA=$cuda \
      "-DCMAKE_CUDA_ARCHITECTURES=${CUDA_ARCHITECTURES}" \
    && cmake --build build/dev -j "$(nproc)" --target lsm_system_bench lsm_compaction_server \
         lsm_compaction_bench lsm_rpc_bench lsm_drain_tests lsm_scheduler_tests
RUN build/dev/lsm_drain_tests && build/dev/lsm_scheduler_tests \
    && bash tests/remote_compaction_test.sh build/dev/lsm_compaction_server build/dev/lsm_compaction_bench
RUN mkdir -p /opt/lsm/bin /opt/lsm/lib \
    && cp build/dev/lsm_system_bench build/dev/lsm_compaction_server build/dev/lsm_compaction_bench \
          build/dev/lsm_rpc_bench /opt/lsm/bin/ \
    && if [ "$VARIANT" = cuda ]; then cp -L .deps/nvcomp-5.3.0.16/nvidia/libnvcomp/lib*/libnvcomp.so* /opt/lsm/lib/; fi \
    && echo "$COMMIT $VARIANT" > /opt/lsm/COMMIT

FROM ${RUN_BASE}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      numactl procps iproute2 ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/lsm /opt/lsm
ENV PATH=/opt/lsm/bin:$PATH LD_LIBRARY_PATH=/opt/lsm/lib
RUN for b in /opt/lsm/bin/*; do if ldd "$b" | grep -q 'not found'; then ldd "$b"; exit 1; fi; done
CMD ["sleep", "infinity"]
