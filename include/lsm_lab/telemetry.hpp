#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace lsm_lab::telemetry
{
    nlohmann::json metric(const std::string &name, const std::string &scope, const std::string &entity,
                          const std::string &source, const std::string &unit, const std::string &kind,
                          nlohmann::json value, const std::string &reason = {});
    nlohmann::json counter_interval(const nlohmann::json &previous, const nlohmann::json &current,
                                    std::uint64_t elapsed_ns);
    nlohmann::json process_metrics(std::int64_t pid, const std::string &role);
    nlohmann::json host_identity();
    nlohmann::json host_metrics();
    nlohmann::json sensor_metrics();
    nlohmann::json power_interval(const nlohmann::json &previous, const nlohmann::json &current,
                                  std::uint64_t elapsed_ns, std::uint64_t maximum_gap_ns);

    struct SamplerOptions
    {
        std::filesystem::path output;
        std::string role;
        std::int64_t pid = 0;
        bool include_host = false;
        std::uint64_t interval_ms = 1000;
        std::uint64_t max_output_bytes = 256ULL << 20;
        std::vector<std::string> required_metrics{"process:cpu_user_ns", "process:cpu_system_ns", "process:resident_bytes",
                                                  "process:storage_read_bytes", "process:storage_write_bytes"};
    };

    class Sampler
    {
    public:
        explicit Sampler(SamplerOptions options);
        ~Sampler();
        Sampler(const Sampler &) = delete;
        Sampler &operator=(const Sampler &) = delete;
        nlohmann::json stop();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}