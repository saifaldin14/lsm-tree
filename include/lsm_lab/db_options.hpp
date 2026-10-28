#pragma once

// RocksDB options shared by the system benchmark and logical replicas, so a replica opens
// its database exactly as the primary does.

#include "lsm_lab/block_transform.hpp"

#include <nlohmann/json.hpp>
#include <rocksdb/cache.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace lsm_lab
{
    struct DbShape
    {
        std::uint64_t write_buffer_bytes = 64ULL << 20;
        std::uint32_t max_write_buffer_number = 4;
        std::uint64_t target_file_bytes = 64ULL << 20;
        std::uint64_t max_bytes_for_level_base = 256ULL << 20;
        std::uint32_t background_jobs = 4;
        std::uint32_t subcompactions = 1;
        std::uint32_t levels = 7;
        // 0 keeps RocksDB's default.
        std::uint32_t l0_trigger = 0;
        double level_multiplier = 0;
        bool use_direct_io = false;
        bool adaptive_yield = true;
        std::string compression = "lz4";
        std::uint64_t block_bytes = 4096;
        std::uint64_t block_cache_bytes = 256ULL << 20;
        // RocksDB's integrated key/value separation (BlobDB); 0 disables.
        std::uint32_t blob_min_bytes = 0;
        // Write-path concurrency (tuned equally for every design): RocksDB's pipelined write
        // queue, concurrent memtable inserts, and an explicit flush/compaction thread split
        // (flush_jobs = 0 keeps RocksDB's split of background_jobs / 4 flushes).
        bool pipelined_write = false;
        bool concurrent_memtable = true;
        std::uint32_t flush_jobs = 0;
    };

    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DbShape, write_buffer_bytes, max_write_buffer_number,
                                                    target_file_bytes, max_bytes_for_level_base, background_jobs,
                                                    subcompactions, levels, l0_trigger, level_multiplier,
                                                    use_direct_io, adaptive_yield, compression, block_bytes,
                                                    block_cache_bytes, blob_min_bytes, pipelined_write,
                                                    concurrent_memtable, flush_jobs)

    inline rocksdb::Options make_db_options(const DbShape &shape, const std::shared_ptr<BatchTransform> &transform)
    {
        rocksdb::Options options;
        options.write_buffer_size = shape.write_buffer_bytes;
        options.max_write_buffer_number = static_cast<int>(shape.max_write_buffer_number);
        options.target_file_size_base = shape.target_file_bytes;
        options.max_bytes_for_level_base = shape.max_bytes_for_level_base;
        options.max_background_jobs = static_cast<int>(shape.background_jobs);
        if (shape.flush_jobs > 0)
        {
            options.max_background_flushes = static_cast<int>(shape.flush_jobs);
            options.max_background_compactions =
                std::max<int>(1, static_cast<int>(shape.background_jobs) - static_cast<int>(shape.flush_jobs));
        }
        options.enable_pipelined_write = shape.pipelined_write;
        options.allow_concurrent_memtable_write = shape.concurrent_memtable;
        options.max_subcompactions = shape.subcompactions;
        options.num_levels = static_cast<int>(shape.levels);
        options.paranoid_checks = true;
        if (shape.l0_trigger > 0)
            options.level0_file_num_compaction_trigger = static_cast<int>(shape.l0_trigger);
        if (shape.level_multiplier > 0)
            options.max_bytes_for_level_multiplier = shape.level_multiplier;
        options.use_direct_reads = shape.use_direct_io;
        options.use_direct_io_for_flush_and_compaction = shape.use_direct_io;
        if (shape.blob_min_bytes > 0)
        {
            options.enable_blob_files = true;
            options.min_blob_size = shape.blob_min_bytes;
            options.enable_blob_garbage_collection = true;
        }
        // With more writers than cores, adaptive yield spins waiting writers and burns the CPU budget.
        options.enable_write_thread_adaptive_yield = shape.adaptive_yield;
        options.statistics = rocksdb::CreateDBStatistics();
        options.statistics->set_stats_level(rocksdb::StatsLevel::kExceptDetailedTimers);
        if (shape.compression == "lz4")
            options.compression = rocksdb::kLZ4Compression;
        else if (shape.compression == "zstd")
            options.compression = rocksdb::kZSTD;
        else if (shape.compression == "none")
            options.compression = rocksdb::kNoCompression;
        else
            throw std::invalid_argument("compression must be lz4, zstd or none");
        rocksdb::BlockBasedTableOptions table;
        table.format_version = 6;
        table.checksum = rocksdb::kCRC32c;
        table.block_size = shape.block_bytes;
        table.block_cache = rocksdb::NewLRUCache(shape.block_cache_bytes);
        table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
        table.compaction_block_transform = transform;
        options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
        return options;
    }
}
