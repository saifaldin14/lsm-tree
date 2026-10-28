#pragma once

// Run-time accounting for the system benchmark: where CPU goes (by thread role, from
// /proc/self/task), what the process really writes to storage (/proc/self/io), and the
// device activity of the filesystem holding the database (/proc/diskstats). Linux only;
// elsewhere every sample is empty.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace lsm_lab
{
    // Thread role from its name: foreground, flush (rocksdb:high), compaction (rocksdb:low),
    // merge_read, gpu (CUDA runtime threads), replication, other.
    std::string thread_role(const std::string &name);

    struct ProcessSample
    {
        double seconds = 0;                         // monotonic clock
        std::map<std::string, double> role_cpu;     // CPU seconds so far, by role
        std::map<std::string, int> role_threads;    // live threads by role
        std::uint64_t read_bytes = 0, write_bytes = 0; // /proc/self/io (storage, not page cache)
        std::uint64_t cancelled_write_bytes = 0;
        // Bytes passed to read/write system calls (includes tmpfs and the page cache).
        std::uint64_t syscall_read_bytes = 0, syscall_write_bytes = 0;
        // /proc/diskstats for the device under the database directory (empty for tmpfs).
        std::string device;
        std::uint64_t device_read_sectors = 0, device_write_sectors = 0, device_io_ms = 0, device_weighted_ms = 0;
    };

    class ProcessAccounting
    {
    public:
        // directory: the database directory, used to find its block device.
        explicit ProcessAccounting(const std::string &directory);
        ProcessSample sample() const;
        // Difference between two samples as JSON (rates per second, cores per role, device
        // utilisation and mean queue depth).
        static nlohmann::json delta(const ProcessSample &before, const ProcessSample &after);
        const std::string &device() const { return device_; }

    private:
        std::string device_;
    };
}
