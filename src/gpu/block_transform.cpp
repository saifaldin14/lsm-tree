#include "lsm_lab/block_transform.hpp"

#include <chrono>
#include <lz4.h>
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm_lab
{
    namespace
    {
        class CpuTransform final : public BatchTransform
        {
        public:
            std::string name() const override { return "cpu_batched"; }

        protected:
            rocksdb::Status execute(const std::string &input, const std::vector<BlockDescriptor> &blocks,
                                    std::vector<std::string> *output) override
            {
                for (const auto &block : blocks)
                {
                    const auto *serialized = input.data() + block.input_offset;
                    const auto checksum = rocksdb::crc32c::Mask(rocksdb::crc32c::Value(serialized, block.checksum_bytes));
                    if (checksum != block.expected_checksum)
                        return rocksdb::Status::Corruption("batch block CRC32C mismatch");
                    std::string decoded(block.output_bytes, '\0');
                    const auto *compressed = input.data() + block.compressed_offset;
                    if (block.compression == 0)
                    {
                        decoded.assign(compressed, block.compressed_bytes);
                    }
                    else if (LZ4_decompress_safe(compressed, decoded.data(), static_cast<int>(block.compressed_bytes),
                                                 static_cast<int>(block.output_bytes)) != static_cast<int>(block.output_bytes))
                    {
                        return rocksdb::Status::Corruption("batch LZ4 decoding failed");
                    }
                    output->push_back(std::move(decoded));
                }
                return rocksdb::Status::OK();
            }
        };
    }

    rocksdb::Status BatchTransform::Transform(const std::vector<rocksdb::CompactionBlockInput> &input,
                                              std::vector<std::string> *output) noexcept
    {
        try
        {
            const auto started = std::chrono::steady_clock::now();
            const std::lock_guard guard(mutex_);
            output->clear();
            if (input.empty() || input.size() > max_blocks)
                return rocksdb::Status::InvalidArgument("batch block count exceeds limit");
            std::string packed;
            std::vector<BlockDescriptor> descriptors;
            std::uint32_t output_size = 0;
            for (const auto &block : input)
            {
                const auto size = block.serialized.size();
                if (size < 5 || size > max_block_bytes || size > max_input_bytes - packed.size())
                    return rocksdb::Status::Corruption("serialized block outside batch bounds");
                const auto payload_size = static_cast<std::uint32_t>(size - 5);
                const auto compression = static_cast<unsigned char>(block.serialized[payload_size]);
                if (compression != 0 && compression != 4)
                    return rocksdb::Status::NotSupported("batch supports only uncompressed and LZ4 blocks");
                rocksdb::Slice compressed(block.serialized.data(), payload_size);
                std::uint32_t decoded_bytes = payload_size;
                if (compression == 4 && (!rocksdb::GetVarint32(&compressed, &decoded_bytes) || compressed.empty()))
                    return rocksdb::Status::Corruption("invalid LZ4 size prefix");
                if (decoded_bytes == 0 || decoded_bytes > max_block_bytes || decoded_bytes > max_output_bytes - output_size)
                    return rocksdb::Status::Corruption("decoded block exceeds batch bounds");
                const auto offset = static_cast<std::uint32_t>(packed.size());
                descriptors.push_back({offset, payload_size + 1,
                                       rocksdb::DecodeFixed32(block.serialized.data() + payload_size + 1) - block.checksum_modifier,
                                       offset + static_cast<std::uint32_t>(compressed.data() - block.serialized.data()),
                                       static_cast<std::uint32_t>(compressed.size()), output_size, decoded_bytes, compression});
                output_size += decoded_bytes;
                packed.append(block.serialized.data(), size);
            }
            auto status = execute(packed, descriptors, output);
            if (!status.ok())
            {
                output->clear();
                return status;
            }
            if (output->size() != input.size())
                return rocksdb::Status::Corruption("batch returned incorrect block count");
            for (std::size_t index = 0; index < output->size(); ++index)
            {
                if ((*output)[index].size() != descriptors[index].output_bytes)
                    return rocksdb::Status::Corruption("batch returned incorrect decoded length");
            }
            ++batches_;
            blocks_ += input.size();
            input_bytes_ += packed.size();
            output_bytes_ += output_size;
            wall_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count();
            return rocksdb::Status::OK();
        }
        catch (const std::exception &error)
        {
            output->clear();
            return rocksdb::Status::Aborted(error.what());
        }
        catch (...)
        {
            output->clear();
            return rocksdb::Status::Aborted("unknown batch transform failure");
        }
    }

    nlohmann::json BatchTransform::statistics() const
    {
        const std::lock_guard guard(mutex_);
        return {{"backend", name()}, {"device", device()}, {"batches", batches_}, {"blocks", blocks_}, {"input_bytes", input_bytes_}, {"output_bytes", output_bytes_}, {"wall_ns", wall_ns_}, {"kernel_ns", kernel_ns_}, {"dispatches", dispatches_}};
    }

    std::shared_ptr<BatchTransform> make_cpu_transform() { return std::make_shared<CpuTransform>(); }

    std::shared_ptr<BatchTransform> make_transform(const std::string &name)
    {
        if (name == "cpu_native")
            return {};
        if (name == "cpu_batched")
            return make_cpu_transform();
#ifdef LSM_HAVE_METAL
        if (name == "metal_batched")
            return make_metal_transform();
#endif
#ifdef LSM_HAVE_CUDA
        if (name == "cuda_batched")
            return make_cuda_transform();
#endif
        throw std::invalid_argument("requested block backend is not available: " + name);
    }

    std::vector<std::string> available_transforms()
    {
        std::vector<std::string> result{"cpu_native", "cpu_batched"};
#ifdef LSM_HAVE_METAL
        if (metal_available())
            result.push_back("metal_batched");
#endif
#ifdef LSM_HAVE_CUDA
        if (cuda_available())
            result.push_back("cuda_batched");
#endif
        return result;
    }

    std::vector<std::string> compiled_transforms()
    {
        std::vector<std::string> result{"cpu_native", "cpu_batched"};
#ifdef LSM_HAVE_METAL
        result.push_back("metal_batched");
#endif
#ifdef LSM_HAVE_CUDA
        result.push_back("cuda_batched");
#endif
        return result;
    }
}