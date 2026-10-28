// Installs the compaction transform in RocksDB's own db_bench (built as lsm_db_bench from the
// pinned RocksDB source; CMakeLists.txt adds one call in db_bench_tool.cc). The mode comes from
// LSM_DB_BENCH_MODE, with the same names as lsm_system_bench: native_cpu (default, no
// transform), gpu_output, cpu_merge, cuda_merge or remote_merge. Statistics are printed to
// stderr at exit.

#include <lsm_lab/block_transform.hpp>

#include <rocksdb/compaction_block_transform.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace
{
    std::shared_ptr<lsm_lab::BatchTransform> transform;
    std::once_flag once;

    void print_statistics()
    {
        if (transform)
            std::cerr << "lsm_transform: " << transform->statistics().dump() << std::endl;
    }
}

namespace ROCKSDB_NAMESPACE
{
    std::shared_ptr<CompactionBlockTransform> LsmDbBenchTransform()
    {
        std::call_once(once, [] {
            const char *value = std::getenv("LSM_DB_BENCH_MODE");
            const std::string mode = value != nullptr && *value != '\0' ? value : "native_cpu";
            if (mode == "native_cpu")
                return;
            if (mode == "gpu_output")
                transform = lsm_lab::make_transform("cuda_hybrid");
            else if (mode == "cuda_merge" || mode == "cpu_merge" || mode == "remote_merge")
                transform = lsm_lab::make_transform(mode);
            else
                throw std::invalid_argument("LSM_DB_BENCH_MODE must be native_cpu, gpu_output, cpu_merge, cuda_merge or remote_merge");
            std::cerr << "lsm_db_bench: compaction transform " << mode << std::endl;
            std::atexit(print_statistics);
        });
        return transform;
    }
}
