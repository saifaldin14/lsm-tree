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
    };

    std::unique_ptr<CudaMergeEngine> make_cuda_merge_engine();
}
