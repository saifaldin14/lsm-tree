#include "lsm_lab/dataset.hpp"
#include "lsm_lab/block_transform.hpp"
#include "lsm_lab/runtime.hpp"
#include "lsm_lab/build_info.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>
#include <rocksdb/convenience.h>
#include <rocksdb/version.h>

int main(int argc, char **argv)
{
    try
    {
        nlohmann::json result;
        const std::string_view command = argc > 1 ? argv[1] : "";
        if (command == "capabilities" && argc == 2)
        {
            const auto &codecs = rocksdb::GetSupportedCompressions();
            const auto available = lsm_lab::available_transforms();
            result = {{"schema_version", 1U}, {"rocksdb_version", rocksdb::GetRocksVersionAsString()}, {"rocksdb_commit", lsm_lab::build::rocksdb_commit}, {"engine_fingerprint", lsm_lab::engine_fingerprint()}, {"backends", available}, {"compiled_backends", lsm_lab::compiled_transforms()}, {"lz4", std::find(codecs.begin(), codecs.end(), rocksdb::kLZ4Compression) != codecs.end()}, {"remote_worker", true}, {"cuda", std::find(available.begin(), available.end(), "cuda_batched") != available.end()}, {"metal", std::find(available.begin(), available.end(), "metal_batched") != available.end()}, {"sst_format", 6}, {"sst_checksum", "crc32c"}, {"dataset_generator", "splitmix64-record-v1"}, {"gpu_execution_qualification", "run lsm_gpu_check BACKEND --require on the actual worker"}};
        }
        else if (command == "validate" && argc == 3)
        {
            const auto spec = lsm_lab::load_spec(argv[2]);
            result = {{"status", "ok"}, {"spec", lsm_lab::spec_json(spec)}, {"planned_logical_write_bytes", lsm_lab::logical_write_bytes(spec.dataset)}};
        }
        else if (command == "create" && argc == 4)
        {
            result = lsm_lab::create_dataset(lsm_lab::load_spec(argv[2]), argv[3]);
        }
        else if (command == "verify" && argc == 3)
        {
            result = lsm_lab::verify_dataset(argv[2]);
        }
        else if (command == "init-shared" && argc == 3)
        {
            result = lsm_lab::initialize_shared_root(argv[2]);
        }
        else if (command == "verify-trial" && argc == 4)
        {
            result = lsm_lab::verify_trial(argv[2], argv[3]);
        }
        else if (command == "clone" && argc == 4)
        {
            result = lsm_lab::clone_dataset(argv[2], argv[3]);
        }
        else
        {
            std::cerr << "Usage: lsm_admin capabilities | validate CONFIG | create CONFIG NEW_DIRECTORY | "
                         "verify DIRECTORY | clone SOURCE NEW_DIRECTORY | init-shared NEW_DIRECTORY | verify-trial DATASET LOAD_OUTPUT\n";
            return 2;
        }
        std::cout << result.dump(2) << '\n';
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << nlohmann::json{{"status", "error"}, {"detail", error.what()}}.dump() << '\n';
        return 1;
    }
}