#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <rocksdb/compaction_block_transform.h>
#include <string>
#include "lsm_lab/merged_input.hpp"

namespace lsm_lab
{
    struct BlockDescriptor
    {
        std::uint32_t input_offset;
        std::uint32_t checksum_bytes;
        std::uint32_t expected_checksum;
        std::uint32_t compressed_offset;
        std::uint32_t compressed_bytes;
        std::uint32_t output_offset;
        std::uint32_t output_bytes;
        std::uint32_t compression;
    };

    class BatchTransform : public rocksdb::CompactionBlockTransform
    {
    public:
        rocksdb::Status Transform(const std::vector<rocksdb::CompactionBlockInput> &input,
                                  std::vector<std::string> *output) noexcept final;
        rocksdb::Status BuildOutput(const std::vector<rocksdb::Slice> &input,
                                    std::vector<rocksdb::CompactionOutputBlock> *output) noexcept final;
        rocksdb::Status SubmitOutput(
            std::vector<std::string> input,
            std::unique_ptr<rocksdb::CompactionOutputSubmission> *submission) noexcept final;
        std::shared_ptr<rocksdb::CompactionBlockSession> CreateSession() final;
        bool SupportsMergedInput() const noexcept final { return merged_input_; }
        rocksdb::Status MergeCompactionInput(const rocksdb::CompactionMergeRequest &request,
                                             std::unique_ptr<rocksdb::CompactionMergedInput> *output) noexcept final;
        void RecordMergeFallback(const rocksdb::Status &reason) noexcept final;
        void enable_merged_input() { merged_input_ = true; }
        nlohmann::json statistics() const;
        virtual std::string name() const = 0;
        virtual std::string device() const { return "host_cpu"; }

    protected:
        virtual rocksdb::Status execute(const std::string &input, const std::vector<BlockDescriptor> &blocks,
                                        std::vector<std::string> *output) = 0;
        virtual rocksdb::Status execute_output(const std::vector<rocksdb::Slice> &input,
                                               std::vector<rocksdb::CompactionOutputBlock> *output);
        virtual rocksdb::Status submit_output(
            std::vector<std::string> input,
            std::unique_ptr<rocksdb::CompactionOutputSubmission> *submission);
        virtual rocksdb::Status execute_merge(const rocksdb::CompactionMergeRequest &request,
                                              std::unique_ptr<rocksdb::CompactionMergedInput> *output,
                                              MergeRunStats *stats);
        std::atomic<std::uint64_t> kernel_ns_{0};
        std::atomic<std::uint64_t> dispatches_{0};
        std::atomic<std::uint64_t> cpu_fallback_batches_{0};
        std::atomic<std::uint64_t> cpu_fallback_blocks_{0};
        std::atomic<std::uint64_t> cpu_fallback_bytes_{0};
        std::atomic<std::uint64_t> compaction_sessions_{0};
        std::atomic<std::uint64_t> output_batches_{0};
        std::atomic<std::uint64_t> output_blocks_{0};
        std::atomic<std::uint64_t> output_gpu_batches_{0};

    private:
        mutable std::mutex mutex_;
        std::uint64_t batches_ = 0, blocks_ = 0, input_bytes_ = 0, output_bytes_ = 0, wall_ns_ = 0;
        bool merged_input_ = false;
        std::uint64_t merge_jobs_ = 0, merge_fallbacks_ = 0, merge_entries_ = 0, merge_blocks_ = 0,
                      merge_input_bytes_ = 0, merge_decoded_bytes_ = 0, merge_wall_ns_ = 0, merge_read_ns_ = 0,
                      merge_device_ns_ = 0, merge_built_jobs_ = 0, merge_built_blocks_ = 0, merge_built_bytes_ = 0,
                      merge_host_compressed_blocks_ = 0, merge_cpu_decoded_bytes_ = 0;
        std::string merge_last_fallback_;
    };

    std::shared_ptr<BatchTransform> make_cpu_transform();
    rocksdb::Status execute_cpu_batch(const std::string &input, const std::vector<BlockDescriptor> &blocks,
                                      std::vector<std::string> *output);
    rocksdb::Status execute_cpu_output(const std::vector<rocksdb::Slice> &input,
                                       std::vector<rocksdb::CompactionOutputBlock> *output);
    std::shared_ptr<BatchTransform> make_transform(const std::string &name);
    std::vector<std::string> available_transforms();
    std::vector<std::string> compiled_transforms();
#ifdef LSM_HAVE_METAL
    bool metal_available();
    std::shared_ptr<BatchTransform> make_metal_transform();
#endif
#ifdef LSM_HAVE_CUDA
    bool cuda_available();
    std::shared_ptr<BatchTransform> make_cuda_transform();
    std::shared_ptr<BatchTransform> make_cuda_hybrid_transform();
#endif
}