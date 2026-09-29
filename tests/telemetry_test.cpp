#include "lsm_lab/telemetry.hpp"

#include <atomic>
#include <fstream>
#include <limits>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <gtest/gtest.h>

namespace lsm_lab::telemetry
{
    TEST(Telemetry, MissingRequiredSensorsInvalidateCapture)
    {
        char pattern[] = "/tmp/lsm-telemetry-required-XXXXXX";
        ASSERT_NE(mkdtemp(pattern), nullptr);
        Sampler sampler({.output = std::filesystem::path(pattern) / "resources.jsonl", .role = "test", .required_metrics = {"host_gpu:unavailable_test_requirement"}});
        const auto summary = sampler.stop();
        EXPECT_EQ(summary.at("status"), "incomplete");
        EXPECT_GE(summary.at("error_samples").get<unsigned>(), 2U);
        std::filesystem::remove_all(pattern);
    }

    TEST(Telemetry, SamplerPersistsBoundedRealSamplesAndStopsCleanly)
    {
        char pattern[] = "/tmp/lsm-telemetry-XXXXXX";
        ASSERT_NE(mkdtemp(pattern), nullptr);
        const auto output = std::filesystem::path(pattern) / "resources.jsonl";
        {
            Sampler sampler({.output = output, .role = "test", .include_host = true});
            const auto summary = sampler.stop();
            EXPECT_EQ(summary.at("status"), "complete");
            EXPECT_GE(summary.at("samples").get<unsigned>(), 2U);
            EXPECT_GT(summary.at("sample_collection_wall_ns").get<unsigned>(), 0U);
            EXPECT_EQ(sampler.stop(), summary);
        }
        EXPECT_THROW(Sampler({.output = output, .role = "test"}), std::runtime_error);
        std::ifstream input(output);
        std::string line;
        ASSERT_TRUE(static_cast<bool>(std::getline(input, line)));
        EXPECT_EQ(nlohmann::json::parse(line).at("type"), "header");
        std::uint64_t end = 0;
        unsigned int samples = 0;
        while (std::getline(input, line))
        {
            const auto record = nlohmann::json::parse(line);
            if (record.at("type") == "sample")
            {
                EXPECT_GE(record.at("sample_start_ns").get<std::uint64_t>(), end);
                end = record.at("sample_end_ns").get<std::uint64_t>();
                EXPECT_FALSE(record.at("metrics").empty());
                EXPECT_TRUE(record.at("errors").empty());
                ++samples;
            }
        }
        EXPECT_GE(samples, 2U);
        std::filesystem::remove_all(pattern);
    }

    TEST(Telemetry, PowerIntegrationIsLabelledAndRefusesGaps)
    {
        auto before = metric("power_watts", "host_gpu", "gpu0", "sensor", "watts", "gauge", 10.0);
        auto after = metric("power_watts", "host_gpu", "gpu0", "sensor", "watts", "gauge", 14.0);
        auto interval = power_interval(before, after, 2'000'000'000ULL, 3'000'000'000ULL);
        EXPECT_DOUBLE_EQ(interval.at("joules").get<double>(), 24.0);
        EXPECT_FALSE(interval.at("hardware_energy_counter").get<bool>());
        EXPECT_EQ(power_interval(before, after, 4'000'000'000ULL, 3'000'000'000ULL).at("status"), "unavailable");
        after["status"] = "unavailable";
        after["value"] = nullptr;
        EXPECT_TRUE(power_interval(before, after, 1, 10).at("joules").is_null());
    }

    TEST(Telemetry, RealSensorsPreserveSourcesAndAvailability)
    {
        const auto sensors = sensor_metrics();
        ASSERT_FALSE(sensors.empty());
        for (const auto &sample : sensors)
        {
            EXPECT_FALSE(sample.at("source").get<std::string>().empty());
            if (sample.at("status") == "unavailable")
            {
                EXPECT_TRUE(sample.at("value").is_null());
                EXPECT_FALSE(sample.at("reason").get<std::string>().empty());
            }
            else
                EXPECT_TRUE(sample.at("value").is_number());
        }
    }

    TEST(Telemetry, HostCountersObserveRealNetworkTraffic)
    {
        const auto identity = host_identity();
        EXPECT_FALSE(identity.at("host_id").is_null());
        EXPECT_FALSE(identity.at("boot_id").is_null());
        const auto before = host_metrics();
        const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_GE(socket_fd, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ASSERT_EQ(bind(socket_fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 0);
        socklen_t length = sizeof(address);
        ASSERT_EQ(getsockname(socket_fd, reinterpret_cast<sockaddr *>(&address), &length), 0);
        timeval timeout{2, 0};
        ASSERT_EQ(setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);
        const std::string payload(4096, 'x');
        char received[4096];
        for (int iteration = 0; iteration < 16; ++iteration)
        {
            EXPECT_EQ(sendto(socket_fd, payload.data(), payload.size(), 0, reinterpret_cast<const sockaddr *>(&address), sizeof(address)), 4096);
            EXPECT_EQ(recv(socket_fd, received, sizeof(received), 0), 4096);
        }
        close(socket_fd);
        const auto after = host_metrics();
        bool network_observed = false;
        bool storage_observed = false;
        for (const auto &sample : after)
        {
            if (sample.at("name") == "disk_read_bytes" && sample.at("scope") == "host_block_device")
            {
                storage_observed = true;
                EXPECT_NE(sample.value("physical_interconnect", ""), "Virtual Interface");
                if (sample.at("status") != "ok")
                {
                    EXPECT_TRUE(sample.at("value").is_null());
                }
            }
            if (sample.at("name") != "network_rx_bytes" || !sample.value("loopback", false))
                continue;
            for (const auto &prior : before)
            {
                if (prior.at("name") == sample.at("name") && prior.at("entity") == sample.at("entity"))
                {
                    EXPECT_GE(sample.at("value").get<std::uint64_t>() - prior.at("value").get<std::uint64_t>(), 65536U);
                    network_observed = true;
                }
            }
        }
        EXPECT_TRUE(network_observed);
        EXPECT_TRUE(storage_observed);
    }

    TEST(Telemetry, CounterIntervalsDoNotInventMissingOrResetTraffic)
    {
        auto previous = metric("read_bytes", "host_block_device", "disk0", "kernel", "bytes", "counter", 1024ULL);
        auto current = metric("read_bytes", "host_block_device", "disk0", "kernel", "bytes", "counter", 5120ULL);
        auto interval = counter_interval(previous, current, 2'000'000'000ULL);
        EXPECT_EQ(interval.at("delta"), 4096);
        EXPECT_DOUBLE_EQ(interval.at("per_second").get<double>(), 2048);
        current["value"] = 12;
        EXPECT_TRUE(counter_interval(previous, current, 1).at("delta").is_null());
        current["value"] = nullptr;
        current["status"] = "unavailable";
        EXPECT_TRUE(counter_interval(previous, current, 1).at("delta").is_null());
        current = previous;
        current["entity"] = "replacement-device";
        EXPECT_EQ(counter_interval(previous, current, 1).at("status"), "unavailable");
        EXPECT_EQ(counter_interval(previous, previous, 0).at("status"), "unavailable");
        current = previous;
        current["value"] = std::numeric_limits<double>::infinity();
        EXPECT_EQ(counter_interval(previous, current, 1).at("status"), "unavailable");
        EXPECT_EQ(metric("reading", "host", "sensor", "source", "watts", "gauge",
                         std::numeric_limits<double>::quiet_NaN())
                      .at("status"),
                  "unavailable");
    }

    TEST(Telemetry, SamplesRealCurrentProcessWithStableIdentity)
    {
        const auto before = process_metrics(getpid(), "test");
        std::atomic<std::uint64_t> work{0};
        for (std::uint64_t index = 0; index < 100000; ++index)
            work.fetch_add(index, std::memory_order_relaxed);
        const auto after = process_metrics(getpid(), "test");
        ASSERT_EQ(before.size(), after.size());
        bool cpu_found = false;
        for (std::size_t index = 0; index < before.size(); ++index)
        {
            EXPECT_EQ(before[index].at("entity"), after[index].at("entity"));
            EXPECT_EQ(after[index].at("role"), "test");
            if (after[index].at("name") == "cpu_user_ns")
            {
                EXPECT_EQ(after[index].at("status"), "ok");
                EXPECT_GE(after[index].at("value").get<std::uint64_t>(), before[index].at("value").get<std::uint64_t>());
                cpu_found = true;
            }
            if (after[index].at("name") == "resident_bytes")
            {
                EXPECT_GT(after[index].at("value").get<std::uint64_t>(), 0U);
            }
            if (after[index].at("name") == "threads")
            {
                EXPECT_GT(after[index].at("value").get<std::uint64_t>(), 0U);
            }
            if (after[index].at("name") == "peak_resident_bytes")
            {
                EXPECT_GT(after[index].at("value").get<std::uint64_t>(), 0U);
            }
            if (after[index].at("status") == "unavailable")
            {
                EXPECT_TRUE(after[index].at("value").is_null());
            }
        }
        EXPECT_TRUE(cpu_found);
        EXPECT_THROW(process_metrics(-1, "test"), std::invalid_argument);
    }
}