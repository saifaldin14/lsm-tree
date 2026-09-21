#include "lsm_lab/load.hpp"

#include <cstdlib>
#include <limits>
#include <hdr/hdr_histogram.h>
#include <hdr/hdr_histogram_log.h>

namespace lsm_lab
{
    namespace
    {
        std::uint64_t mix(std::uint64_t value)
        {
            value += 0x9e3779b97f4a7c15ULL;
            value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
            value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
            return value ^ (value >> 31);
        }

        std::uint64_t bounded(std::uint64_t random, std::uint64_t bound)
        {
            const auto threshold = (0ULL - bound) % bound;
            while (random < threshold)
            {
                random = mix(random);
            }
            return random % bound;
        }
    }

    ScheduledOperation scheduled_operation(const DatasetSpec &dataset, const LoadOptions &options,
                                           std::uint64_t request_id)
    {
        require(dataset.records != 0 && options.rate != 0 && options.read_percent <= 100 &&
                    request_id <= std::numeric_limits<std::uint64_t>::max() / 1'000'000'000ULL,
                "invalid schedule arguments");
        const auto random = mix(options.seed ^ (request_id * 0xd1342543de82ef95ULL));
        return {request_id, bounded(random, dataset.records), dataset.overwrite_rounds + 1ULL + request_id,
                request_id * 1'000'000'000ULL / options.rate,
                bounded(mix(random ^ 0xa4093822299f31d0ULL), 100) >= options.read_percent};
    }

    LatencyHistogram::LatencyHistogram()
    {
        require(hdr_init(1, 3'600'000'000'000LL, 3, &histogram_) == 0, "cannot allocate latency histogram");
    }

    LatencyHistogram::~LatencyHistogram()
    {
        hdr_close(histogram_);
    }

    void LatencyHistogram::record(std::uint64_t nanoseconds)
    {
        require(nanoseconds <= 3'600'000'000'000ULL && hdr_record_value(histogram_, static_cast<std::int64_t>(nanoseconds)),
                "latency exceeds histogram range");
    }

    void LatencyHistogram::merge(const LatencyHistogram &other)
    {
        require(hdr_add(histogram_, other.histogram_) == 0, "histogram merge lost observations");
    }

    nlohmann::json LatencyHistogram::summary() const
    {
        nlohmann::json result{{"count", histogram_->total_count}, {"unit", "nanoseconds"}, {"significant_digits", 3}};
        result["p50"] = histogram_->total_count ? nlohmann::json(hdr_value_at_percentile(histogram_, 50)) : nlohmann::json(nullptr);
        result["p95"] = histogram_->total_count ? nlohmann::json(hdr_value_at_percentile(histogram_, 95)) : nlohmann::json(nullptr);
        result["p99"] = histogram_->total_count ? nlohmann::json(hdr_value_at_percentile(histogram_, 99)) : nlohmann::json(nullptr);
        result["maximum"] = histogram_->total_count ? nlohmann::json(hdr_max(histogram_)) : nlohmann::json(nullptr);
        return result;
    }

    std::string LatencyHistogram::encode() const
    {
        char *encoded = nullptr;
        require(hdr_log_encode(histogram_, &encoded) == 0, "histogram encoding failed");
        const std::unique_ptr<char, decltype(&std::free)> buffer(encoded, std::free);
        return encoded;
    }
}