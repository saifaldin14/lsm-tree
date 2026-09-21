#pragma once

#include "lsm_lab/dataset.hpp"
#include "lsm_lab/runtime.hpp"

struct hdr_histogram;

namespace lsm_lab
{
    struct LoadOptions
    {
        std::string endpoint;
        std::string run_id;
        std::string required_mode;
        std::filesystem::path output;
        TlsOptions tls;
        std::uint64_t requests = 1000;
        std::uint64_t warmup_requests = 0;
        std::uint64_t rate = 1000;
        std::uint64_t seed = 1;
        std::uint32_t read_percent = 50;
        std::uint32_t threads = 4;
        std::uint32_t queue_capacity = 256;
        std::uint32_t deadline_ms = 5000;
        std::uint64_t write_budget_bytes = 64ULL << 20;
        std::uint64_t memory_budget_bytes = 256ULL << 20;
        std::uint64_t artifact_budget_bytes = 2ULL << 30;
    };

    struct ScheduledOperation
    {
        std::uint64_t request_id;
        std::uint64_t key_index;
        std::uint64_t generation;
        std::uint64_t intended_ns;
        bool write;
        bool operator==(const ScheduledOperation &) const = default;
    };

    ScheduledOperation scheduled_operation(const DatasetSpec &dataset, const LoadOptions &options,
                                           std::uint64_t request_id);
    nlohmann::json run_load(const LoadOptions &options);

    class LatencyHistogram
    {
    public:
        LatencyHistogram();
        ~LatencyHistogram();
        LatencyHistogram(const LatencyHistogram &) = delete;
        LatencyHistogram &operator=(const LatencyHistogram &) = delete;
        void record(std::uint64_t nanoseconds);
        void merge(const LatencyHistogram &other);
        nlohmann::json summary() const;
        std::string encode() const;

    private:
        hdr_histogram *histogram_ = nullptr;
    };
}