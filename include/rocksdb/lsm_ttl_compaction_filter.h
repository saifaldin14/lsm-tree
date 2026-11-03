#pragma once

// Time-to-live compaction filter with the semantics of Flink's RocksDB TTL compaction filter
// (FlinkCompactionFilter): each value carries a big-endian 8-byte timestamp (milliseconds) at
// a fixed offset, and a value whose timestamp + ttl is at or before the current time is
// removed. Compaction offload recognises this class and applies it on the accelerator;
// RocksDB applies it on the CPU otherwise. Values too short to hold a timestamp are kept.

#include "rocksdb/compaction_filter.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace ROCKSDB_NAMESPACE
{
    class LsmTtlCompactionFilter : public CompactionFilter
    {
    public:
        LsmTtlCompactionFilter(std::uint32_t timestamp_offset, std::uint64_t ttl_ms)
            : offset_(timestamp_offset), ttl_(ttl_ms)
        {
            if (const char *fixed = std::getenv("LSM_TTL_NOW_MS"))
                fixed_now_ = std::strtoull(fixed, nullptr, 10);
        }

        bool Filter(int /*level*/, const Slice & /*key*/, const Slice &value, std::string * /*new_value*/,
                    bool * /*value_changed*/) const override
        {
            return Expired(value, Now());
        }

        const char *Name() const override { return "LsmTtlCompactionFilter"; }

        // Current time in milliseconds; LSM_TTL_NOW_MS (read at construction) fixes it so tests
        // can compare exact output. The accelerator reads it once per compaction job, RocksDB
        // once per key.
        std::uint64_t Now() const
        {
            if (fixed_now_ != 0)
                return fixed_now_;
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::system_clock::now().time_since_epoch())
                                                  .count());
        }

        bool Expired(const Slice &value, std::uint64_t now) const
        {
            if (ttl_ == 0 || value.size() < offset_ + 8)
                return false;
            std::uint64_t timestamp = 0;
            for (std::size_t index = 0; index < 8; ++index)
                timestamp = (timestamp << 8) | static_cast<unsigned char>(value[offset_ + index]);
            return now >= ttl_ && timestamp <= now - ttl_;
        }

        std::uint32_t timestamp_offset() const { return offset_; }
        std::uint64_t ttl() const { return ttl_; }

    private:
        std::uint32_t offset_;
        std::uint64_t ttl_;
        std::uint64_t fixed_now_ = 0;
    };
}
