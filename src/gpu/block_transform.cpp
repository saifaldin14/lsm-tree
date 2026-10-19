#include "lsm_lab/block_transform.hpp"

#include <chrono>
#include <future>
#include <map>
#include <lz4.h>
#include "util/coding.h"
#include "util/crc32c.h"

namespace lsm_lab
{
    namespace
    {
        using SessionKey = std::pair<std::uint64_t, std::uint64_t>;

        class TransformSession final : public rocksdb::CompactionBlockSession
        {
        public:
            explicit TransformSession(BatchTransform *transform) : transform_(transform) {}

            rocksdb::Status Add(const std::vector<rocksdb::CompactionBlockKey> &keys,
                                const std::vector<rocksdb::CompactionBlockInput> &input) noexcept override
            {
                try
                {
                    if (finished_)
                        return rocksdb::Status::InvalidArgument("cannot add blocks after session finish");
                    if (keys.size() != input.size())
                        return rocksdb::Status::InvalidArgument("compaction session key/input count mismatch");
                    for (std::size_t index = 0; index < input.size(); ++index)
                    {
                        const SessionKey key{keys[index].file_context, keys[index].block_offset};
                        if (!seen_.emplace(key, true).second)
                            return rocksdb::Status::Corruption("duplicate block in compaction session");
                        pending_.push_back({key, input[index].serialized.ToString(), input[index].checksum_modifier});
                    }
                    return rocksdb::Status::OK();
                }
                catch (const std::exception &error)
                {
                    return rocksdb::Status::Aborted(error.what());
                }
            }

            rocksdb::Status Finish() noexcept override
            {
                try
                {
                    if (finished_)
                        return status_;
                    finished_ = true;
                    struct Chunk
                    {
                        std::size_t first;
                        std::vector<rocksdb::CompactionBlockInput> input;
                        std::vector<std::string> decoded;
                        rocksdb::Status status;
                    };
                    std::vector<Chunk> chunks;
                    std::size_t first = 0;
                    while (first < pending_.size())
                    {
                        std::size_t count = 0;
                        std::size_t input_bytes = 0;
                        std::size_t output_bytes = 0;
                        while (first + count < pending_.size() &&
                               count < rocksdb::CompactionBlockTransform::max_blocks)
                        {
                            const auto &serialized = pending_[first + count].serialized;
                            if (serialized.size() < 5)
                                return status_ = rocksdb::Status::Corruption("serialized session block is truncated");
                            rocksdb::Slice payload(serialized.data(), serialized.size() - 5);
                            const auto compression = static_cast<unsigned char>(serialized[serialized.size() - 5]);
                            std::uint32_t decoded_bytes = static_cast<std::uint32_t>(payload.size());
                            if (compression == 4 && !rocksdb::GetVarint32(&payload, &decoded_bytes))
                                return status_ = rocksdb::Status::Corruption("invalid session LZ4 size prefix");
                            if (compression != 0 && compression != 4)
                                return status_ = rocksdb::Status::NotSupported("session block codec is unsupported");
                            if (count != 0 &&
                                (serialized.size() > rocksdb::CompactionBlockTransform::max_input_bytes - input_bytes ||
                                 decoded_bytes > rocksdb::CompactionBlockTransform::max_output_bytes - output_bytes))
                                break;
                            input_bytes += serialized.size();
                            output_bytes += decoded_bytes;
                            ++count;
                        }
                        if (count == 0)
                            return status_ = rocksdb::Status::MemoryLimit(
                                "single block exceeds compaction session budget");
                        Chunk chunk{first, {}, {}, rocksdb::Status::OK()};
                        chunk.input.reserve(count);
                        for (std::size_t index = 0; index < count; ++index)
                        {
                            const auto &block = pending_[first + index];
                            chunk.input.push_back({block.serialized, block.checksum_modifier});
                        }
                        chunks.push_back(std::move(chunk));
                        first += count;
                    }
                    for (std::size_t chunk = 0; chunk < chunks.size(); chunk += 2)
                    {
                        auto run = [&](std::size_t index)
                        {
                            chunks[index].status =
                                transform_->Transform(chunks[index].input, &chunks[index].decoded);
                        };
                        auto first_task = std::async(std::launch::async, run, chunk);
                        std::future<void> second_task;
                        if (chunk + 1 < chunks.size())
                            second_task = std::async(std::launch::async, run, chunk + 1);
                        first_task.get();
                        if (second_task.valid())
                            second_task.get();
                    }
                    for (auto &chunk : chunks)
                    {
                        if (!chunk.status.ok())
                            return status_ = chunk.status;
                        for (std::size_t index = 0; index < chunk.decoded.size(); ++index)
                            output_.emplace(pending_[chunk.first + index].key, std::move(chunk.decoded[index]));
                    }
                    pending_.clear();
                    return status_;
                }
                catch (const std::exception &error)
                {
                    return status_ = rocksdb::Status::Aborted(error.what());
                }
            }

            bool Take(const rocksdb::CompactionBlockKey &key, std::string *output) noexcept override
            {
                const auto found = output_.find({key.file_context, key.block_offset});
                if (found == output_.end())
                    return false;
                *output = std::move(found->second);
                output_.erase(found);
                return true;
            }

        private:
            struct PendingBlock
            {
                SessionKey key;
                std::string serialized;
                std::uint32_t checksum_modifier;
            };

            BatchTransform *transform_;
            bool finished_ = false;
            rocksdb::Status status_;
            std::vector<PendingBlock> pending_;
            std::map<SessionKey, bool> seen_;
            std::map<SessionKey, std::string> output_;
        };

        class CpuTransform final : public BatchTransform
        {
        public:
            std::string name() const override { return "cpu_batched"; }

        protected:
            rocksdb::Status execute(const std::string &input, const std::vector<BlockDescriptor> &blocks,
                                    std::vector<std::string> *output) override
            {
                return execute_cpu_batch(input, blocks, output);
            }
        };

        class CompletedOutputSubmission final : public rocksdb::CompactionOutputSubmission
        {
        public:
            CompletedOutputSubmission(
                rocksdb::Status status,
                std::vector<rocksdb::CompactionOutputBlock> output)
                : status_(std::move(status)), output_(std::move(output)) {}

            rocksdb::Status Finish(
                std::vector<rocksdb::CompactionOutputBlock> *output) noexcept override
            {
                if (finished_)
                    return rocksdb::Status::InvalidArgument(
                        "compaction output submission already finished");
                finished_ = true;
                output->clear();
                if (status_.ok())
                    *output = std::move(output_);
                return status_;
            }

        private:
            rocksdb::Status status_;
            std::vector<rocksdb::CompactionOutputBlock> output_;
            bool finished_ = false;
        };
    }

    rocksdb::Status execute_cpu_batch(const std::string &input, const std::vector<BlockDescriptor> &blocks,
                                      std::vector<std::string> *output)
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

    rocksdb::Status execute_cpu_output(const std::vector<rocksdb::Slice> &input,
                                       std::vector<rocksdb::CompactionOutputBlock> *output)
    {
        output->clear();
        output->reserve(input.size());
        for (const auto &block : input)
        {
            std::string compressed(LZ4_compressBound(static_cast<int>(block.size())), '\0');
            const auto compressed_bytes = LZ4_compress_default(
                block.data(), compressed.data(), static_cast<int>(block.size()), static_cast<int>(compressed.size()));
            std::string contents;
            const bool use_compressed =
                compressed_bytes > 0 &&
                static_cast<std::size_t>(compressed_bytes) + 5 < block.size() - block.size() / 8;
            if (use_compressed)
            {
                rocksdb::PutVarint32(&contents, static_cast<std::uint32_t>(block.size()));
                contents.append(compressed.data(), compressed_bytes);
            }
            else
            {
                contents.assign(block.data(), block.size());
            }
            std::string checksummed = contents;
            checksummed.push_back(use_compressed ? '\4' : '\0');
            output->push_back({std::move(contents),
                               rocksdb::crc32c::Mask(
                                   rocksdb::crc32c::Value(checksummed.data(), checksummed.size())),
                               use_compressed, std::string()});
        }
        return rocksdb::Status::OK();
    }

    rocksdb::Status BatchTransform::execute_output(
        const std::vector<rocksdb::Slice> &input, std::vector<rocksdb::CompactionOutputBlock> *output)
    {
        return execute_cpu_output(input, output);
    }

    rocksdb::Status BatchTransform::submit_output(
        std::vector<std::string> input,
        std::unique_ptr<rocksdb::CompactionOutputSubmission> *submission)
    {
        std::vector<rocksdb::Slice> slices;
        slices.reserve(input.size());
        for (const auto &block : input)
            slices.emplace_back(block);
        std::vector<rocksdb::CompactionOutputBlock> output;
        auto status = execute_output(slices, &output);
        if (status.ok() && output.size() == input.size())
            for (std::size_t index = 0; index < input.size(); ++index)
                output[index].uncompressed = std::move(input[index]);
        *submission =
            std::make_unique<CompletedOutputSubmission>(status, std::move(output));
        return rocksdb::Status::OK();
    }

    rocksdb::Status BatchTransform::SubmitOutput(
        std::vector<std::string> input,
        std::unique_ptr<rocksdb::CompactionOutputSubmission> *submission) noexcept
    {
        try
        {
            submission->reset();
            if (input.empty() || input.size() > max_output_blocks)
                return rocksdb::Status::InvalidArgument("output block count exceeds limit");
            std::size_t bytes = 0;
            for (const auto &block : input)
            {
                if (block.empty() || block.size() > max_block_bytes ||
                    block.size() > max_output_bytes - bytes)
                    return rocksdb::Status::InvalidArgument(
                        "output block exceeds batch limits");
                bytes += block.size();
            }
            const auto block_count = input.size();
            auto status = submit_output(std::move(input), submission);
            if (!status.ok())
                return status;
            if (!*submission)
                return rocksdb::Status::Corruption(
                    "output submission was not created");
            ++output_batches_;
            output_blocks_ += block_count;
            return rocksdb::Status::OK();
        }
        catch (const std::exception &error)
        {
            submission->reset();
            return rocksdb::Status::Aborted(error.what());
        }
    }

    rocksdb::Status BatchTransform::BuildOutput(
        const std::vector<rocksdb::Slice> &input,
        std::vector<rocksdb::CompactionOutputBlock> *output) noexcept
    {
        try
        {
            std::vector<std::string> owned;
            owned.reserve(input.size());
            for (const auto &block : input)
                owned.emplace_back(block.data(), block.size());
            std::unique_ptr<rocksdb::CompactionOutputSubmission> submission;
            auto status = SubmitOutput(std::move(owned), &submission);
            if (!status.ok())
                return status;
            return submission->Finish(output);
        }
        catch (const std::exception &error)
        {
            output->clear();
            return rocksdb::Status::Aborted(error.what());
        }
    }

    std::shared_ptr<rocksdb::CompactionBlockSession> BatchTransform::CreateSession()
    {
        ++compaction_sessions_;
        return std::make_shared<TransformSession>(this);
    }

    rocksdb::Status BatchTransform::Transform(const std::vector<rocksdb::CompactionBlockInput> &input,
                                              std::vector<std::string> *output) noexcept
    {
        try
        {
            const auto started = std::chrono::steady_clock::now();
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
            {
                const std::lock_guard guard(mutex_);
                ++batches_;
                blocks_ += input.size();
                input_bytes_ += packed.size();
                output_bytes_ += output_size;
                wall_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count();
            }
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

    rocksdb::Status BatchTransform::execute_merge(const rocksdb::CompactionMergeRequest &request,
                                                  std::unique_ptr<rocksdb::CompactionMergedInput> *output,
                                                  MergeRunStats *stats)
    {
        return execute_cpu_merge(request, output, stats);
    }

    rocksdb::Status BatchTransform::MergeCompactionInput(const rocksdb::CompactionMergeRequest &request,
                                                         std::unique_ptr<rocksdb::CompactionMergedInput> *output) noexcept
    {
        try
        {
            const auto started = std::chrono::steady_clock::now();
            MergeRunStats stats;
            auto status = execute_merge(request, output, &stats);
            if (!status.ok())
            {
                output->reset();
                return status;
            }
            const std::lock_guard guard(mutex_);
            ++merge_jobs_;
            merge_entries_ += stats.entries;
            merge_blocks_ += stats.blocks;
            merge_input_bytes_ += stats.input_bytes;
            merge_decoded_bytes_ += stats.decoded_bytes;
            merge_read_ns_ += stats.read_ns;
            merge_device_ns_ += stats.device_ns;
            merge_built_jobs_ += (*output && (*output)->built_output() != nullptr) ? 1 : 0;
            merge_built_blocks_ += stats.built_blocks;
            merge_built_bytes_ += stats.built_bytes;
            merge_wall_ns_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - started).count());
            return rocksdb::Status::OK();
        }
        catch (const std::exception &error)
        {
            output->reset();
            return rocksdb::Status::Aborted(error.what());
        }
        catch (...)
        {
            output->reset();
            return rocksdb::Status::Aborted("unknown merged-input failure");
        }
    }

    void BatchTransform::RecordMergeFallback(const rocksdb::Status &reason) noexcept
    {
        try
        {
            const std::lock_guard guard(mutex_);
            ++merge_fallbacks_;
            merge_last_fallback_ = reason.ToString();
        }
        catch (...)
        {
        }
    }

    nlohmann::json BatchTransform::statistics() const
    {
        const std::lock_guard guard(mutex_);
        return {{"backend", name()}, {"device", device()}, {"batches", batches_}, {"blocks", blocks_}, {"input_bytes", input_bytes_}, {"output_bytes", output_bytes_}, {"wall_ns", wall_ns_}, {"kernel_ns", kernel_ns_.load()}, {"dispatches", dispatches_.load()}, {"cpu_fallback_batches", cpu_fallback_batches_.load()}, {"cpu_fallback_blocks", cpu_fallback_blocks_.load()}, {"cpu_fallback_bytes", cpu_fallback_bytes_.load()}, {"compaction_sessions", compaction_sessions_.load()}, {"output_batches", output_batches_.load()}, {"output_blocks", output_blocks_.load()}, {"output_gpu_batches", output_gpu_batches_.load()}, {"merged_input", merged_input_}, {"merge_jobs", merge_jobs_}, {"merge_fallbacks", merge_fallbacks_}, {"merge_last_fallback", merge_last_fallback_}, {"merge_entries", merge_entries_}, {"merge_blocks", merge_blocks_}, {"merge_input_bytes", merge_input_bytes_}, {"merge_decoded_bytes", merge_decoded_bytes_}, {"merge_wall_ns", merge_wall_ns_}, {"merge_read_ns", merge_read_ns_}, {"merge_device_ns", merge_device_ns_}, {"merge_built_jobs", merge_built_jobs_}, {"merge_built_blocks", merge_built_blocks_}, {"merge_built_bytes", merge_built_bytes_}};
    }

    std::shared_ptr<BatchTransform> make_cpu_transform() { return std::make_shared<CpuTransform>(); }

    std::shared_ptr<BatchTransform> make_transform(const std::string &name)
    {
        if (name == "cpu_native")
            return {};
        if (name == "cpu_batched")
            return make_cpu_transform();
        if (name == "cpu_merge")
        {
            auto transform = make_cpu_transform();
            transform->enable_merged_input();
            return transform;
        }
#ifdef LSM_HAVE_METAL
        if (name == "metal_batched")
            return make_metal_transform();
#endif
#ifdef LSM_HAVE_CUDA
        if (name == "cuda_batched")
            return make_cuda_transform();
        if (name == "cuda_hybrid")
            return make_cuda_hybrid_transform();
        if (name == "cuda_merge")
        {
            auto transform = make_cuda_hybrid_transform();
            transform->enable_merged_input();
            return transform;
        }
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
        {
            result.push_back("cuda_batched");
            result.push_back("cuda_hybrid");
        }
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
        result.push_back("cuda_hybrid");
#endif
        return result;
    }
}