#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace lsm_lab
{

    struct DatasetSpec
    {
        std::uint64_t records = 1000;
        std::uint64_t seed = 1;
        std::uint32_t key_bytes = 16;
        std::uint32_t value_bytes = 256;
        std::uint32_t overwrite_rounds = 1;
        bool operator==(const DatasetSpec &) const = default;
    };

    struct EngineSpec
    {
        std::string compression = "lz4";
        std::uint64_t write_buffer_bytes = 1ULL << 20;
        std::uint64_t target_file_bytes = 1ULL << 20;
        std::uint64_t block_cache_bytes = 4ULL << 20;
        std::uint32_t block_bytes = 4096;
        std::uint32_t background_jobs = 2;
        std::uint32_t levels = 4;
        bool operator==(const EngineSpec &) const = default;
    };

    struct Limits
    {
        std::uint64_t logical_write_bytes = 64ULL << 20;
        std::uint64_t minimum_free_disk_bytes = 256ULL << 20;
        bool operator==(const Limits &) const = default;
    };

    struct Spec
    {
        DatasetSpec dataset;
        EngineSpec engine;
        Limits limits;
        bool operator==(const Spec &) const = default;
    };

    void validate(const Spec &spec);
    std::uint64_t logical_write_bytes(const DatasetSpec &dataset);
    Spec parse_spec(std::string_view text);
    Spec load_spec(const std::filesystem::path &path);
    nlohmann::json spec_json(const Spec &spec);

}