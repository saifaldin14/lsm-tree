#include "lsm_lab/process_accounting.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <unistd.h>

namespace lsm_lab
{
    namespace
    {
        double now_seconds()
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        [[maybe_unused]] std::string read_file(const std::filesystem::path &path)
        {
            std::ifstream in(path);
            std::stringstream out;
            out << in.rdbuf();
            return out.str();
        }

        // Block device name (as in /proc/diskstats) holding path, or "" (tmpfs, unknown).
        std::string device_of(const std::string &path)
        {
#if defined(__linux__)
            struct stat info{};
            if (::stat(path.c_str(), &info) != 0 || major(info.st_dev) == 0)
                return {};
            std::ostringstream link;
            link << "/sys/dev/block/" << major(info.st_dev) << ':' << minor(info.st_dev);
            std::error_code error;
            auto target = std::filesystem::canonical(link.str(), error);
            if (error)
                return {};
            // A partition's parent directory is the whole disk, whose statistics we want.
            if (std::filesystem::exists(target / "partition"))
                target = target.parent_path();
            return target.filename().string();
#else
            (void)path;
            return {};
#endif
        }
    }

    std::string thread_role(const std::string &name)
    {
        if (name == "lsm-foreground")
            return "foreground";
        if (name.rfind("rocksdb:high", 0) == 0)
            return "flush";
        if (name.rfind("rocksdb:low", 0) == 0 || name.rfind("rocksdb:bottom", 0) == 0)
            return "compaction";
        if (name == "lsm-merge-read")
            return "merge_read";
        if (name.rfind("cuda", 0) == 0)
            return "gpu";
        if (name.rfind("lsm-replic", 0) == 0)
            return "replication";
        return "other";
    }

    ProcessAccounting::ProcessAccounting(const std::string &directory) : device_(device_of(directory)) {}

    ProcessSample ProcessAccounting::sample() const
    {
        ProcessSample result;
        result.seconds = now_seconds();
#if defined(__linux__)
        static const double ticks = static_cast<double>(::sysconf(_SC_CLK_TCK));
        std::error_code error;
        for (const auto &task : std::filesystem::directory_iterator("/proc/self/task", error))
        {
            auto name = read_file(task.path() / "comm");
            while (!name.empty() && (name.back() == '\n' || name.back() == ' '))
                name.pop_back();
            const auto stat = read_file(task.path() / "stat");
            const auto close = stat.rfind(')');
            if (close == std::string::npos)
                continue;
            std::istringstream fields(stat.substr(close + 2));
            std::string field;
            double user = 0, system = 0;
            // After "pid (comm) ": state is field 3; utime and stime are fields 14 and 15.
            for (int index = 3; index <= 15 && (fields >> field); ++index)
            {
                if (index == 14)
                    user = std::stod(field);
                if (index == 15)
                    system = std::stod(field);
            }
            const auto role = thread_role(name);
            result.role_cpu[role] += (user + system) / ticks;
            ++result.role_threads[role];
        }
        std::istringstream io(read_file("/proc/self/io"));
        std::string key;
        std::uint64_t value = 0;
        while (io >> key >> value)
        {
            if (key == "read_bytes:")
                result.read_bytes = value;
            else if (key == "write_bytes:")
                result.write_bytes = value;
            else if (key == "cancelled_write_bytes:")
                result.cancelled_write_bytes = value;
            else if (key == "rchar:")
                result.syscall_read_bytes = value;
            else if (key == "wchar:")
                result.syscall_write_bytes = value;
        }
        if (!device_.empty())
        {
            std::istringstream disks(read_file("/proc/diskstats"));
            std::string line;
            while (std::getline(disks, line))
            {
                std::istringstream parts(line);
                unsigned major_number = 0, minor_number = 0;
                std::string name;
                parts >> major_number >> minor_number >> name;
                if (name != device_)
                    continue;
                std::uint64_t v[11] = {};
                for (auto &item : v)
                    parts >> item;
                // reads, reads merged, sectors read, ms reading, writes, writes merged,
                // sectors written, ms writing, in flight, ms doing I/O, weighted ms
                result.device = device_;
                result.device_read_sectors = v[2];
                result.device_write_sectors = v[6];
                result.device_io_ms = v[9];
                result.device_weighted_ms = v[10];
                break;
            }
        }
#endif
        return result;
    }

    nlohmann::json ProcessAccounting::delta(const ProcessSample &before, const ProcessSample &after)
    {
        const double seconds = std::max(1e-9, after.seconds - before.seconds);
        nlohmann::json cores = nlohmann::json::object();
        double total = 0;
        for (const auto &[role, cpu] : after.role_cpu)
        {
            const auto it = before.role_cpu.find(role);
            // Threads that exit between samples drop out; clamp at zero.
            const double used = std::max(0.0, cpu - (it == before.role_cpu.end() ? 0.0 : it->second));
            cores[role] = used / seconds;
            total += used / seconds;
        }
        nlohmann::json result{{"seconds", seconds},
                              {"cores_by_role", cores},
                              {"cores", total},
                              {"threads_by_role", after.role_threads},
                              {"process_read_mib_s", (after.read_bytes - before.read_bytes) / 1048576.0 / seconds},
                              {"process_write_mib_s", (after.write_bytes - before.write_bytes) / 1048576.0 / seconds},
                              {"syscall_read_mib_s", (after.syscall_read_bytes - before.syscall_read_bytes) / 1048576.0 / seconds},
                              {"syscall_write_mib_s", (after.syscall_write_bytes - before.syscall_write_bytes) / 1048576.0 / seconds}};
        if (!after.device.empty())
        {
            result["device"] = {
                {"name", after.device},
                {"read_mib_s", (after.device_read_sectors - before.device_read_sectors) * 512.0 / 1048576.0 / seconds},
                {"write_mib_s", (after.device_write_sectors - before.device_write_sectors) * 512.0 / 1048576.0 / seconds},
                {"utilisation", (after.device_io_ms - before.device_io_ms) / 1000.0 / seconds},
                {"mean_queue_depth", (after.device_weighted_ms - before.device_weighted_ms) / 1000.0 / seconds}};
        }
        return result;
    }
}
