#pragma once

// Stand-in for gParaKV's db/my_stats.h: ycsbc.cc sizes gParaKV's value log from these fields.
// RocksDB does not use them.

#include <cstddef>
#include <cstdint>

namespace leveldb
{
    struct MyStats
    {
        std::uint32_t original_value_size = 0;
        std::uint32_t var_key_value_size = 0;
        std::uint64_t max_vlog_size = 128 << 20;
        std::uint64_t clean_threshold = 0;
        std::uint32_t migrate_threshold = 0;
        std::size_t max_num_log_item = 0;
        std::size_t max_num_log = 0;
    };

    inline MyStats my_stats;
}
