#include "lsm_lab/load.hpp"

#include <limits>
#include <set>
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

    TEST(LoadSchedule, InsertsAreOptInDeterministicAndBeyondDataset)
    {
        DatasetSpec dataset;
        LoadOptions options;
        options.rate = 3;
        LoadOptions inserting = options;
        inserting.insert_percent = 40;
        std::uint64_t inserts = 0;
        std::uint64_t writes = 0;
        for (std::uint64_t request_id = 0; request_id < 2000; ++request_id)
        {
            const auto plain = scheduled_operation(dataset, options, request_id);
            EXPECT_FALSE(plain.insert);
            const auto operation = scheduled_operation(dataset, inserting, request_id);
            EXPECT_EQ(operation, scheduled_operation(dataset, inserting, request_id));
            EXPECT_EQ(operation.write, plain.write);
            EXPECT_EQ(operation.generation, plain.generation);
            EXPECT_EQ(operation.intended_ns, plain.intended_ns);
            if (operation.insert)
            {
                EXPECT_TRUE(operation.write);
                EXPECT_EQ(operation.key_index, dataset.records + request_id);
                ++inserts;
            }
            else
            {
                EXPECT_EQ(operation, plain);
            }
            writes += operation.write;
        }
        EXPECT_GT(inserts, writes * 30 / 100);
        EXPECT_LT(inserts, writes * 50 / 100);
        inserting.insert_percent = 100;
        inserting.read_percent = 0;
        EXPECT_TRUE(scheduled_operation(dataset, inserting, 5).insert);
        inserting.insert_percent = 101;
        EXPECT_THROW(scheduled_operation(dataset, inserting, 5), std::runtime_error);
    }

    TEST(LoadSchedule, ClosedLoopClientsOwnDisjointKeys)
    {
        DatasetSpec dataset;
        dataset.records = 103;
        LoadOptions options;
        options.closed_loop_clients = 4;
        options.insert_percent = 25;
        std::set<std::uint64_t> request_ids;
        for (std::uint32_t client = 0; client < 4; ++client)
        {
            for (std::uint64_t sequence = 0; sequence < 500; ++sequence)
            {
                const auto operation = closed_loop_operation(dataset, options, client, sequence);
                EXPECT_EQ(operation, closed_loop_operation(dataset, options, client, sequence));
                EXPECT_EQ(operation.request_id, sequence * 4 + client);
                EXPECT_TRUE(request_ids.insert(operation.request_id).second);
                EXPECT_EQ(operation.generation, dataset.overwrite_rounds + 1 + operation.request_id);
                if (operation.insert)
                    EXPECT_EQ(operation.key_index, dataset.records + operation.request_id);
                else
                {
                    EXPECT_LT(operation.key_index, dataset.records);
                    EXPECT_EQ(operation.key_index % 4, client);
                }
            }
        }
        EXPECT_THROW(closed_loop_operation(dataset, options, 4, 0), std::runtime_error);
        dataset.records = 3;
        EXPECT_THROW(closed_loop_operation(dataset, options, 0, 0), std::runtime_error);
    }

    TEST(LoadHistogram, ReportsTailPercentileOnRequest)
    {
        LatencyHistogram histogram;
        EXPECT_FALSE(histogram.summary().contains("p999"));
        EXPECT_TRUE(histogram.summary(true).at("p999").is_null());
        for (std::uint64_t value = 1; value <= 1000; ++value)
            histogram.record(value * 1000);
        EXPECT_NEAR(histogram.summary(true).at("p999").get<double>(), 999'000.0, 1'000.0);
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