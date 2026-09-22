#include "lsm_lab/telemetry.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
#ifdef __APPLE__
#include <libproc.h>
#endif

namespace lsm_lab::telemetry
{
    using Json = nlohmann::json;

    Json metric(const std::string &name, const std::string &scope, const std::string &entity,
                const std::string &source, const std::string &unit, const std::string &kind,
                Json value, const std::string &reason)
    {
        std::string resolved_reason = reason;
        if (!value.is_null() && (!value.is_number() || !std::isfinite(value.get<double>()) ||
                                 (kind == "counter" && value < 0)))
        {
            value = nullptr;
            resolved_reason = "invalid or nonfinite native reading";
        }
        Json result{{"name", name}, {"scope", scope}, {"entity", entity}, {"source", source}, {"unit", unit}, {"kind", kind}, {"status", value.is_null() ? "unavailable" : "ok"}, {"value", std::move(value)}};
        if (result.at("status") == "unavailable")
            result["reason"] = resolved_reason.empty() ? "not exposed by this source" : resolved_reason;
        return result;
    }

    Json counter_interval(const Json &previous, const Json &current, std::uint64_t elapsed_ns)
    {
        const auto unavailable = [](const std::string &reason)
        {
            return Json{{"status", "unavailable"}, {"delta", nullptr}, {"per_second", nullptr}, {"reason", reason}};
        };
        if (elapsed_ns == 0)
            return unavailable("zero measurement interval");
        for (const auto *field : {"name", "scope", "entity", "source", "unit", "kind"})
        {
            if (previous.at(field) != current.at(field))
                return unavailable("counter identity changed");
        }
        if (current.at("kind") != "counter" || previous.at("status") != "ok" || current.at("status") != "ok")
            return unavailable("counter sample unavailable");
        const auto &before = previous.at("value");
        const auto &after = current.at("value");
        if (!before.is_number() || !after.is_number() || !std::isfinite(before.get<double>()) ||
            !std::isfinite(after.get<double>()) || before < 0 || after < before)
            return unavailable("counter reset, wrap, or invalid value");
        Json delta;
        if (before.is_number_integer() && after.is_number_integer())
            delta = after.get<std::uint64_t>() - before.get<std::uint64_t>();
        else
            delta = after.get<double>() - before.get<double>();
        return {{"status", "ok"}, {"delta", delta}, {"per_second", delta.get<double>() * 1e9 / static_cast<double>(elapsed_ns)}, {"elapsed_ns", elapsed_ns}, {"delta_unit", current.at("unit")}};
    }

    Json process_metrics(std::int64_t pid, const std::string &role)
    {
        if (pid <= 0 || pid > std::numeric_limits<pid_t>::max())
            throw std::invalid_argument("invalid telemetry process id");
        Json values = Json::object();
        std::map<std::string, std::string> sources;
        std::string identity = std::to_string(pid);
        std::string source;
        std::string error;
#ifdef __APPLE__
        source = "proc_pid_rusage(RUSAGE_INFO_V4)";
        rusage_info_v4 usage{};
        if (proc_pid_rusage(static_cast<pid_t>(pid), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t *>(&usage)) == 0)
        {
            identity += ":" + std::to_string(usage.ri_proc_start_abstime);
            values = {{"cpu_user_ns", usage.ri_user_time}, {"cpu_system_ns", usage.ri_system_time}, {"resident_bytes", usage.ri_resident_size}, {"physical_footprint_bytes", usage.ri_phys_footprint}, {"peak_physical_footprint_bytes", usage.ri_lifetime_max_phys_footprint}, {"storage_read_bytes", usage.ri_diskio_bytesread}, {"storage_write_bytes", usage.ri_diskio_byteswritten}, {"logical_write_bytes", usage.ri_logical_writes}, {"pageins", usage.ri_pageins}, {"cpu_cycles", usage.ri_cycles}, {"cpu_instructions", usage.ri_instructions}};
        }
        else
            error = std::strerror(errno);
        proc_taskinfo task{};
        if (proc_pidinfo(static_cast<pid_t>(pid), PROC_PIDTASKINFO, 0, &task, sizeof(task)) == sizeof(task))
        {
            const std::map<std::string, std::int64_t> counters{{"threads", task.pti_threadnum}, {"page_faults", task.pti_faults}, {"copy_on_write_faults", task.pti_cow_faults}, {"context_switches", task.pti_csw}};
            for (const auto &[name, value] : counters)
            {
                if (value >= 0)
                    values[name] = value;
                sources[name] = "proc_pidinfo(PROC_PIDTASKINFO)";
            }
            values["virtual_bytes"] = task.pti_virtual_size;
            sources["virtual_bytes"] = "proc_pidinfo(PROC_PIDTASKINFO)";
        }
#elif defined(__linux__)
        source = "/proc/PID/stat,status,io";
        const auto root = "/proc/" + std::to_string(pid) + "/";
        std::ifstream stat(root + "stat");
        std::string line;
        if (std::getline(stat, line))
        {
            const auto name_end = line.rfind(')');
            std::vector<std::string> fields;
            if (name_end != std::string::npos)
            {
                std::istringstream rest(line.substr(name_end + 2));
                for (std::string field; rest >> field;)
                    fields.push_back(field);
            }
            const auto ticks = sysconf(_SC_CLK_TCK);
            if (fields.size() >= 22 && ticks > 0)
            {
                identity += ":" + fields.at(19);
                const auto nanoseconds = [ticks](const std::string &field)
                {
                    const auto count = std::stoull(field);
                    return (count / ticks) * 1'000'000'000ULL + (count % ticks) * 1'000'000'000ULL / ticks;
                };
                values["cpu_user_ns"] = nanoseconds(fields.at(11));
                values["cpu_system_ns"] = nanoseconds(fields.at(12));
                values["threads"] = std::stoull(fields.at(17));
                values["minor_faults"] = std::stoull(fields.at(7));
                values["major_faults"] = std::stoull(fields.at(9));
                std::ifstream status(root + "status");
                while (std::getline(status, line))
                {
                    std::istringstream parser(line);
                    std::string name;
                    std::uint64_t value = 0;
                    if (parser >> name >> value)
                    {
                        if (name == "VmRSS:")
                            values["resident_bytes"] = value * 1024;
                        if (name == "VmHWM:")
                            values["peak_resident_bytes"] = value * 1024;
                        if (name == "VmSize:")
                            values["virtual_bytes"] = value * 1024;
                        if (name == "voluntary_ctxt_switches:")
                            values["voluntary_context_switches"] = value;
                        if (name == "nonvoluntary_ctxt_switches:")
                            values["involuntary_context_switches"] = value;
                    }
                }
                std::ifstream io(root + "io");
                const std::map<std::string, std::string> io_fields{{"read_bytes:", "storage_read_bytes"},
                                                                   {"write_bytes:", "storage_write_bytes"},
                                                                   {"rchar:", "logical_read_bytes"},
                                                                   {"wchar:", "logical_write_bytes"},
                                                                   {"cancelled_write_bytes:", "cancelled_write_bytes"},
                                                                   {"syscr:", "read_syscalls"},
                                                                   {"syscw:", "write_syscalls"}};
                for (std::string name; io >> name;)
                {
                    std::uint64_t value = 0;
                    if (!(io >> value))
                        break;
                    const auto field = io_fields.find(name);
                    if (field != io_fields.end())
                        values[field->second] = value;
                }
            }
            else
                error = "malformed process stat or unavailable clock tick frequency";
        }
        else
            error = "process exited or /proc access denied";
#else
        source = "unsupported platform";
        error = "process byte counters require native Linux or macOS";
#endif
        if (pid == getpid())
        {
            rusage usage{};
            if (getrusage(RUSAGE_SELF, &usage) == 0)
            {
#ifdef __APPLE__
                values["peak_resident_bytes"] = usage.ru_maxrss;
#else
                values["peak_resident_bytes"] = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
                values["voluntary_context_switches"] = usage.ru_nvcsw;
                values["involuntary_context_switches"] = usage.ru_nivcsw;
                for (const auto *name : {"peak_resident_bytes", "voluntary_context_switches", "involuntary_context_switches"})
                    sources[name] = "getrusage(RUSAGE_SELF)";
            }
        }
        Json metrics = Json::array();
        for (const auto *name : {"cpu_user_ns", "cpu_system_ns", "resident_bytes", "peak_resident_bytes", "physical_footprint_bytes",
                                 "peak_physical_footprint_bytes", "storage_read_bytes", "storage_write_bytes", "logical_read_bytes",
                                 "logical_write_bytes", "cancelled_write_bytes", "read_syscalls", "write_syscalls", "threads",
                                 "minor_faults", "major_faults", "pageins", "voluntary_context_switches", "involuntary_context_switches",
                                 "cpu_cycles", "cpu_instructions", "virtual_bytes", "page_faults", "copy_on_write_faults", "context_switches"})
        {
            const std::string key = name;
            const bool gauge = key == "threads" || key == "virtual_bytes" || key == "resident_bytes" || key == "peak_resident_bytes" ||
                               key == "physical_footprint_bytes" || key == "peak_physical_footprint_bytes";
            const auto unit = key.ends_with("_ns") ? "nanoseconds" : (key.ends_with("_bytes") ? "bytes" : "count");
            const auto scope = pid != getpid() && (key == "voluntary_context_switches" || key == "involuntary_context_switches")
                                   ? "process_main_thread"
                                   : "process";
            auto sample = metric(key, scope, identity, sources.contains(key) ? sources.at(key) : source, unit, gauge ? "gauge" : "counter",
                                 values.contains(key) ? values.at(key) : Json(nullptr), error);
            sample["role"] = role;
            sample["pid"] = pid;
            sample["includes_children"] = false;
            metrics.push_back(std::move(sample));
        }
        return metrics;
    }
}