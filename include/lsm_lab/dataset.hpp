#pragma once

#include "lsm_lab/spec.hpp"

#include <rocksdb/options.h>
#include <rocksdb/compaction_block_transform.h>

namespace lsm_lab
{
    std::string record_key(const DatasetSpec &spec, std::uint64_t index);
    std::string record_value(const DatasetSpec &spec, std::uint64_t index, std::uint64_t generation);
    rocksdb::Options engine_options(const Spec &spec, std::shared_ptr<rocksdb::CompactionBlockTransform> transform = {});
    nlohmann::json create_dataset(const Spec &spec, const std::filesystem::path &destination);
    nlohmann::json verify_dataset(const std::filesystem::path &directory);
    nlohmann::json verify_trial(const std::filesystem::path &directory, const std::filesystem::path &load_output);
    nlohmann::json initialize_shared_root(const std::filesystem::path &directory);
    nlohmann::json clone_dataset(const std::filesystem::path &source,
                                 const std::filesystem::path &destination);
    Spec dataset_spec(const std::filesystem::path &directory);
}