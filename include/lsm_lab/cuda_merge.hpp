#pragma once

#include "lsm_lab/merged_input.hpp"

#include <memory>

namespace lsm_lab
{
    // GPU implementation of the merged compaction input (CUDA + nvCOMP).
    // Thread-safe; concurrent calls are serialized on one device stream.
    class CudaMergeEngine
    {
    public:
        virtual ~CudaMergeEngine() = default;
        virtual rocksdb::Status merge(const rocksdb::CompactionMergeRequest &request,
                                      std::unique_ptr<rocksdb::CompactionMergedInput> *output,
                                      MergeRunStats *stats) = 0;
        // Pins host buffers for jobs of up to `bytes` of input now, so later jobs do not pin
        // memory (pinning holds the process's memory-map lock and stalls every thread that
        // page-faults meanwhile).
        virtual void preallocate(std::size_t /*bytes*/) {}
    };

    std::unique_ptr<CudaMergeEngine> make_cuda_merge_engine();

    // LSM_GPU_PINNED_PREALLOC_MB in bytes (0 when unset): pinned host memory each merge engine
    // reserves when it is created.
    std::size_t pinned_prealloc_bytes();

    // Sets the bits of a FastLocalBloom filter on the GPU (see
    // rocksdb::CompactionBlockTransform::BuildBloomFilter). Thread-safe; data is len_bytes zeroed
    // host bytes and receives the filter bits.
    rocksdb::Status cuda_build_bloom_filter(const std::uint64_t *hashes, std::size_t count, std::uint32_t len_bytes,
                                            int num_probes, char *data);
}
