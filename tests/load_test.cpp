#include "lsm_lab/load.hpp"

#include <limits>
#include <gtest/gtest.h>
#include <hdr/hdr_histogram_log.h>

namespace lsm_lab
{
    TEST(LoadSchedule, DeterministicBoundedAndCompletionIndependent)
    {
        DatasetSpec dataset;
        LoadOptions options;
        options.rate = 3;
        for (std::uint64_t request_id = 0; request_id < 1000; ++request_id)
        {
            const auto operation = scheduled_operation(dataset, options, request_id);
            EXPECT_EQ(operation, scheduled_operation(dataset, options, request_id));
            EXPECT_LT(operation.key_index, dataset.records);
            EXPECT_EQ(operation.intended_ns, request_id * 1'000'000'000ULL / 3);
            EXPECT_EQ(operation.generation, dataset.overwrite_rounds + 1 + request_id);
        }
        options.read_percent = 100;
        EXPECT_FALSE(scheduled_operation(dataset, options, 0).write);
        options.read_percent = 0;
        EXPECT_TRUE(scheduled_operation(dataset, options, 0).write);
        options.rate = 0;
        EXPECT_THROW(scheduled_operation(dataset, options, 0), std::runtime_error);
        options.rate = 1;
        EXPECT_THROW(scheduled_operation(dataset, options, std::numeric_limits<std::uint64_t>::max()), std::runtime_error);
    }

    TEST(LoadHistogram, PreservesCountsAndEncodedValues)
    {
        LatencyHistogram histogram;
        EXPECT_TRUE(histogram.summary().at("p99").is_null());
        for (const auto value : {100, 200, 300, 400, 500})
        {
            histogram.record(value);
        }
        LatencyHistogram other;
        other.record(600);
        histogram.merge(other);
        EXPECT_EQ(histogram.summary().at("count"), 6);
        EXPECT_EQ(histogram.summary().at("p50"), 300);
        auto encoded = histogram.encode();
        hdr_histogram *decoded = nullptr;
        ASSERT_EQ(hdr_log_decode(&decoded, encoded.data(), encoded.size()), 0);
        EXPECT_EQ(decoded->total_count, 6);
        EXPECT_EQ(hdr_max(decoded), 600);
        hdr_close(decoded);
        EXPECT_THROW(histogram.record(3'600'000'000'001ULL), std::runtime_error);
    }
}