#pragma once

#include <cstddef>
#include <cstdint>
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

    class CompactionBlockTransform
    {
    public:
        static constexpr std::size_t max_blocks = 32;
        static constexpr std::size_t max_input_bytes = 4ULL << 20;
        static constexpr std::size_t max_output_bytes = 16ULL << 20;
        static constexpr std::size_t max_block_bytes = 2ULL << 20;
        virtual ~CompactionBlockTransform() = default;
        virtual Status Transform(const std::vector<CompactionBlockInput> &input,
                                 std::vector<std::string> *output) noexcept = 0;
    };
}