#include "lsm_lab/db_options.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace lsm_lab
{
    namespace
    {
        TEST(DbShape, DefaultOptionsAreUnchanged)
        {
            const DbShape shape;
            const rocksdb::Options defaults;
            const auto options = make_db_options(shape, nullptr);
            EXPECT_EQ(shape.l0_trigger, 0U);
            EXPECT_EQ(shape.l0_slowdown, 0U);
            EXPECT_EQ(shape.l0_stop, 0U);
            EXPECT_EQ(shape.compaction_style, "level");
            EXPECT_FALSE(shape.stats_all);
            EXPECT_EQ(options.level0_file_num_compaction_trigger, defaults.level0_file_num_compaction_trigger);
            EXPECT_EQ(options.level0_slowdown_writes_trigger, defaults.level0_slowdown_writes_trigger);
            EXPECT_EQ(options.level0_stop_writes_trigger, defaults.level0_stop_writes_trigger);
            EXPECT_EQ(options.compaction_style, defaults.compaction_style);
            EXPECT_EQ(options.write_buffer_size, 64ULL << 20);
            EXPECT_EQ(options.max_write_buffer_number, 4);
            EXPECT_EQ(options.target_file_size_base, 64ULL << 20);
            EXPECT_EQ(options.max_bytes_for_level_base, 256ULL << 20);
            EXPECT_EQ(options.max_background_jobs, 4);
            EXPECT_EQ(options.max_background_flushes, defaults.max_background_flushes);
            EXPECT_EQ(options.max_background_compactions, defaults.max_background_compactions);
            EXPECT_EQ(options.max_subcompactions, 1U);
            EXPECT_EQ(options.num_levels, 7);
            EXPECT_EQ(options.max_bytes_for_level_multiplier, defaults.max_bytes_for_level_multiplier);
            EXPECT_EQ(options.compression, rocksdb::kLZ4Compression);
            EXPECT_EQ(options.bottommost_compression, defaults.bottommost_compression);
            EXPECT_TRUE(options.paranoid_checks);
            EXPECT_TRUE(options.allow_fallocate);
            EXPECT_TRUE(options.enable_write_thread_adaptive_yield);
            EXPECT_TRUE(options.allow_concurrent_memtable_write);
            EXPECT_FALSE(options.enable_pipelined_write);
            EXPECT_FALSE(options.use_direct_reads);
            EXPECT_FALSE(options.use_direct_io_for_flush_and_compaction);
            EXPECT_FALSE(options.enable_blob_files);
            ASSERT_NE(options.statistics, nullptr);
            EXPECT_EQ(options.statistics->get_stats_level(), rocksdb::StatsLevel::kExceptDetailedTimers);
        }

        TEST(DbShape, DefaultJsonIncludesNewFields)
        {
            const nlohmann::json document = DbShape{};
            EXPECT_EQ(document.at("l0_trigger"), 0);
            EXPECT_EQ(document.at("l0_slowdown"), 0);
            EXPECT_EQ(document.at("l0_stop"), 0);
            EXPECT_EQ(document.at("compaction_style"), "level");
            EXPECT_EQ(document.at("stats_all"), false);
            EXPECT_EQ(nlohmann::json(nlohmann::json::object().get<DbShape>()), document);
        }

        TEST(DbShape, LegacyJsonDefaultsMissingNewFields)
        {
            nlohmann::json expected = DbShape{};
            expected["l0_trigger"] = 8;
            expected["background_jobs"] = 12;
            expected["compression"] = "zstd";
            auto legacy = expected;
            for (const char *name : {"l0_slowdown", "l0_stop", "compaction_style", "stats_all"})
                legacy.erase(name);
            const auto shape = nlohmann::json::parse(legacy.dump()).get<DbShape>();
            EXPECT_EQ(nlohmann::json(shape), expected);
            const auto options = make_db_options(shape, nullptr);
            const rocksdb::Options defaults;
            EXPECT_EQ(options.level0_file_num_compaction_trigger, 8);
            EXPECT_EQ(options.level0_slowdown_writes_trigger, defaults.level0_slowdown_writes_trigger);
            EXPECT_EQ(options.level0_stop_writes_trigger, defaults.level0_stop_writes_trigger);
            EXPECT_EQ(options.compaction_style, rocksdb::kCompactionStyleLevel);
            ASSERT_NE(options.statistics, nullptr);
            EXPECT_EQ(options.statistics->get_stats_level(), rocksdb::StatsLevel::kExceptDetailedTimers);
        }

        TEST(DbShape, NewFieldsRoundTrip)
        {
            DbShape shape;
            shape.l0_trigger = 24;
            shape.l0_slowdown = 48;
            shape.l0_stop = 64;
            shape.compaction_style = "universal";
            shape.stats_all = true;
            shape.compression = "zstd";
            const nlohmann::json document = shape;
            EXPECT_EQ(document.at("l0_trigger"), 24);
            EXPECT_EQ(document.at("l0_slowdown"), 48);
            EXPECT_EQ(document.at("l0_stop"), 64);
            EXPECT_EQ(document.at("compaction_style"), "universal");
            EXPECT_EQ(document.at("stats_all"), true);
            const auto restored = nlohmann::json::parse(document.dump()).get<DbShape>();
            EXPECT_EQ(nlohmann::json(restored), document);
        }

        TEST(DbShape, AppliesThresholdAndCompactionStyleOverrides)
        {
            DbShape shape;
            shape.l0_trigger = 16;
            shape.l0_slowdown = 32;
            shape.l0_stop = 64;
            shape.compaction_style = "universal";
            const auto options = make_db_options(shape, nullptr);
            EXPECT_EQ(options.level0_file_num_compaction_trigger, 16);
            EXPECT_EQ(options.level0_slowdown_writes_trigger, 32);
            EXPECT_EQ(options.level0_stop_writes_trigger, 64);
            EXPECT_EQ(options.compaction_style, rocksdb::kCompactionStyleUniversal);
        }

        TEST(DbShape, ZeroThresholdsKeepTheirIndividualDefaults)
        {
            const rocksdb::Options defaults;
            const auto compaction = static_cast<std::uint32_t>(defaults.level0_file_num_compaction_trigger);
            const auto slowdown = static_cast<std::uint32_t>(defaults.level0_slowdown_writes_trigger);
            const auto stop = static_cast<std::uint32_t>(defaults.level0_stop_writes_trigger);
            const std::array<std::uint32_t, 3> cases[] = {
                {slowdown, 0, 0}, {0, compaction, 0}, {0, stop, 0}, {0, 0, slowdown}, {slowdown + 1, slowdown + 1, 0}};
            for (const auto &thresholds : cases)
            {
                DbShape shape;
                shape.l0_trigger = thresholds[0];
                shape.l0_slowdown = thresholds[1];
                shape.l0_stop = thresholds[2];
                SCOPED_TRACE(nlohmann::json(shape).dump());
                const auto options = make_db_options(shape, nullptr);
                EXPECT_EQ(options.level0_file_num_compaction_trigger,
                          shape.l0_trigger ? static_cast<int>(shape.l0_trigger) : defaults.level0_file_num_compaction_trigger);
                EXPECT_EQ(options.level0_slowdown_writes_trigger,
                          shape.l0_slowdown ? static_cast<int>(shape.l0_slowdown) : defaults.level0_slowdown_writes_trigger);
                EXPECT_EQ(options.level0_stop_writes_trigger,
                          shape.l0_stop ? static_cast<int>(shape.l0_stop) : defaults.level0_stop_writes_trigger);
            }
        }

        TEST(DbShape, AcceptsEqualThresholdsThroughIntMax)
        {
            const auto maximum = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
            for (const auto threshold : {1U, 32U, maximum})
            {
                SCOPED_TRACE(threshold);
                DbShape shape;
                shape.l0_trigger = threshold;
                shape.l0_slowdown = threshold;
                shape.l0_stop = threshold;
                const auto options = make_db_options(shape, nullptr);
                EXPECT_EQ(options.level0_file_num_compaction_trigger, static_cast<int>(threshold));
                EXPECT_EQ(options.level0_slowdown_writes_trigger, static_cast<int>(threshold));
                EXPECT_EQ(options.level0_stop_writes_trigger, static_cast<int>(threshold));
            }
        }

        TEST(DbShape, RejectsUnknownCompactionStyles)
        {
            for (const char *style : {"", "fifo", "none", "LEVEL", "unknown"})
            {
                SCOPED_TRACE(style);
                DbShape shape;
                shape.compaction_style = style;
                EXPECT_THROW(make_db_options(shape, nullptr), std::invalid_argument);
            }
        }

        TEST(DbShape, RejectsInvalidEffectiveThresholdOrdering)
        {
            const rocksdb::Options defaults;
            const auto compaction = static_cast<std::uint32_t>(defaults.level0_file_num_compaction_trigger);
            const auto slowdown = static_cast<std::uint32_t>(defaults.level0_slowdown_writes_trigger);
            const auto stop = static_cast<std::uint32_t>(defaults.level0_stop_writes_trigger);
            const std::array<std::uint32_t, 3> cases[] = {
                {16, 8, 32}, {8, 32, 16}, {slowdown + 1, 0, 0}, {0, compaction - 1, 0},
                {0, stop + 1, 0}, {0, 0, slowdown - 1}, {slowdown + 1, 0, stop + 1}};
            for (const char *style : {"level", "universal"})
            {
                for (const auto &thresholds : cases)
                {
                    DbShape shape;
                    shape.compaction_style = style;
                    shape.l0_trigger = thresholds[0];
                    shape.l0_slowdown = thresholds[1];
                    shape.l0_stop = thresholds[2];
                    SCOPED_TRACE(nlohmann::json(shape).dump());
                    EXPECT_THROW(make_db_options(shape, nullptr), std::invalid_argument);
                }
            }
        }

        TEST(DbShape, RejectsThresholdOverflowBeforeIntConversion)
        {
            const auto maximum = static_cast<std::uint32_t>(std::numeric_limits<int>::max());
            for (const auto threshold : {maximum + 1, std::numeric_limits<std::uint32_t>::max()})
            {
                for (const auto field : {&DbShape::l0_trigger, &DbShape::l0_slowdown, &DbShape::l0_stop})
                {
                    DbShape shape;
                    shape.*field = threshold;
                    SCOPED_TRACE(nlohmann::json(shape).dump());
                    EXPECT_THROW(make_db_options(shape, nullptr), std::invalid_argument);
                }
            }
        }

        TEST(DbShape, StatsAllSelectsFullStatistics)
        {
            for (const bool stats_all : {true, false})
            {
                SCOPED_TRACE(stats_all);
                DbShape shape;
                shape.stats_all = stats_all;
                const auto options = make_db_options(shape, nullptr);
                ASSERT_NE(options.statistics, nullptr);
                EXPECT_EQ(options.statistics->get_stats_level(),
                          stats_all ? rocksdb::StatsLevel::kAll : rocksdb::StatsLevel::kExceptDetailedTimers);
            }
        }
    }
}