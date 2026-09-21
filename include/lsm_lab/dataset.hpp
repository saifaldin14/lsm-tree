#pragma once

#include "lsm_lab/spec.hpp"

#include <rocksdb/options.h>

namespace lsm_lab
{
    std::string record_key(const DatasetSpec &spec, std::uint64_t index);
    std::string record_value(const DatasetSpec &spec, std::uint64_t index, std::uint64_t generation);
    rocksdb::Options engine_options(const Spec &spec);
    nlohmann::json create_dataset(const Spec &spec, const std::filesystem::path &destination);
    nlohmann::json verify_dataset(const std::filesystem::path &directory);
    nlohmann::json clone_dataset(const std::filesystem::path &source,
                                 const std::filesystem::path &destination);
    Spec dataset_spec(const std::filesystem::path &directory);
}