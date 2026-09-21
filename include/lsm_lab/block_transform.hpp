#pragma once

#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <rocksdb/compaction_block_transform.h>

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
        nlohmann::json statistics() const;
        virtual std::string name() const = 0;
        virtual std::string device() const { return "host_cpu"; }

    protected:
        virtual rocksdb::Status execute(const std::string &input, const std::vector<BlockDescriptor> &blocks,
                                        std::vector<std::string> *output) = 0;
        std::uint64_t kernel_ns_ = 0;
        std::uint64_t dispatches_ = 0;

    private:
        mutable std::mutex mutex_;
        std::uint64_t batches_ = 0, blocks_ = 0, input_bytes_ = 0, output_bytes_ = 0, wall_ns_ = 0;
    };

    std::shared_ptr<BatchTransform> make_cpu_transform();
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
#endif
}