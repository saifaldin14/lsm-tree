#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE
{
    struct CompactionBlockInput
    {
        Slice serialized;
        std::uint32_t checksum_modifier = 0;
    };

    struct CompactionBlockKey
    {
        std::uint64_t file_context = 0;
        std::uint64_t block_offset = 0;
    };

    struct CompactionOutputBlock
    {
        std::string contents;
        std::uint32_t checksum = 0;
        bool compressed = false;
    };

    class CompactionOutputSubmission
    {
    public:
        virtual ~CompactionOutputSubmission() = default;
        virtual Status Finish(std::vector<CompactionOutputBlock> *output) noexcept = 0;
    };

    class CompactionBlockSession
    {
    public:
        virtual ~CompactionBlockSession() = default;
        virtual Status Add(const std::vector<CompactionBlockKey> &keys,
                           const std::vector<CompactionBlockInput> &input) noexcept = 0;
        virtual Status Finish() noexcept = 0;
        virtual bool Take(const CompactionBlockKey &key, std::string *output) noexcept = 0;
    };

    class CompactionBlockTransform
    {
    public:
        static constexpr std::size_t max_blocks = 256;
        static constexpr std::size_t max_input_bytes = 16ULL << 20;
        static constexpr std::size_t max_output_bytes = 64ULL << 20;
        static constexpr std::size_t max_block_bytes = 2ULL << 20;
        virtual ~CompactionBlockTransform() = default;
        virtual Status Transform(const std::vector<CompactionBlockInput> &input,
                                 std::vector<std::string> *output) noexcept = 0;
        virtual Status BuildOutput(const std::vector<Slice> &input,
                                   std::vector<CompactionOutputBlock> *output) noexcept = 0;
        virtual Status SubmitOutput(
            std::vector<std::string> input,
            std::unique_ptr<CompactionOutputSubmission> *submission) noexcept = 0;
        virtual std::shared_ptr<CompactionBlockSession> CreateSession() = 0;
    };
}