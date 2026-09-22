#include "lsm_lab/telemetry.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#ifdef __linux__
#include <dlfcn.h>
#endif
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#endif

namespace lsm_lab::telemetry
{
    using Json = nlohmann::json;
    namespace fs = std::filesystem;

    Json power_interval(const Json &previous, const Json &current, std::uint64_t elapsed_ns, std::uint64_t maximum_gap_ns)
    {
        const auto unavailable = [](const std::string &reason)
        {
            return Json{{"status", "unavailable"}, {"joules", nullptr}, {"reason", reason}};
        };
        for (const auto *field : {"name", "scope", "entity", "source", "unit", "kind"})
            if (previous.at(field) != current.at(field))
                return unavailable("power sensor identity changed");
        if (elapsed_ns == 0 || elapsed_ns > maximum_gap_ns)
            return unavailable("power sampling gap exceeds allowed interval");
        if (current.at("name") != "power_watts" || current.at("unit") != "watts" || current.at("kind") != "gauge" ||
            previous.at("status") != "ok" || current.at("status") != "ok" ||
            !previous.at("value").is_number() || !current.at("value").is_number())
            return unavailable("power sample unavailable");
        const auto before = previous.at("value").get<double>();
        const auto after = current.at("value").get<double>();
        if (!std::isfinite(before) || !std::isfinite(after) || before < 0 || after < 0)
            return unavailable("invalid power reading");
        return {{"status", "ok"}, {"joules", (before + after) * 0.5 * static_cast<double>(elapsed_ns) / 1e9}, {"method", "trapezoidal_integral_of_sampled_power"}, {"hardware_energy_counter", false}, {"elapsed_ns", elapsed_ns}};
    }

    namespace
    {
#ifdef __linux__
        struct NvmlDevice;
        struct NvmlUtilization
        {
            unsigned int gpu;
            unsigned int memory;
        };
        struct NvmlMemory
        {
            unsigned long long total;
            unsigned long long free;
            unsigned long long used;
        };

        class Nvml
        {
        public:
            Nvml()
            {
                library_ = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
                if (!library_)
                {
                    error_ = "NVIDIA driver library is not installed or visible";
                    return;
                }
                const auto initialize = function<int (*)()>("nvmlInit_v2");
                if (!initialize)
                {
                    error_ = "nvmlInit_v2 unavailable";
                    return;
                }
                const int result = initialize();
                initialized_ = result == 0;
                if (!initialized_)
                    error_ = error(result);
            }
            ~Nvml()
            {
                const auto shutdown = function<int (*)()>("nvmlShutdown");
                if (initialized_ && shutdown)
                    shutdown();
                if (library_)
                    dlclose(library_);
            }

            template <typename Function>
            Function function(const char *name) const
            {
                return library_ ? reinterpret_cast<Function>(dlsym(library_, name)) : nullptr;
            }
            std::string error(int code) const
            {
                const auto explain = function<const char *(*)(int)>("nvmlErrorString");
                return explain ? explain(code) : "NVML error " + std::to_string(code);
            }
            Json sample()
            {
                const std::lock_guard guard(mutex_);
                Json metrics = Json::array();
                const auto unavailable = [&](const std::string &reason)
                {
                    for (const auto &[name, unit] : std::map<std::string, std::string>{{"gpu_utilization_percent", "percent"},
                                                                                       {"gpu_memory_used_bytes", "bytes"},
                                                                                       {"power_watts", "watts"},
                                                                                       {"energy_joules", "joules"},
                                                                                       {"temperature_celsius", "celsius"}})
                        metrics.push_back(metric(name, "host_gpu", "nvidia:unavailable", "NVML", unit,
                                                 name == "energy_joules" ? "counter" : "gauge", nullptr, reason));
                };
                if (!initialized_)
                {
                    unavailable(error_);
                    return metrics;
                }
                const auto count_devices = function<int (*)(unsigned int *)>("nvmlDeviceGetCount_v2");
                const auto handle = function<int (*)(unsigned int, NvmlDevice **)>("nvmlDeviceGetHandleByIndex_v2");
                unsigned int count = 0;
                if (!count_devices || !handle || count_devices(&count) != 0 || count == 0)
                {
                    unavailable("no accessible NVIDIA devices");
                    return metrics;
                }
                const auto utilization = function<int (*)(NvmlDevice *, NvmlUtilization *)>("nvmlDeviceGetUtilizationRates");
                const auto memory = function<int (*)(NvmlDevice *, NvmlMemory *)>("nvmlDeviceGetMemoryInfo");
                const auto power = function<int (*)(NvmlDevice *, unsigned int *)>("nvmlDeviceGetPowerUsage");
                const auto energy = function<int (*)(NvmlDevice *, unsigned long long *)>("nvmlDeviceGetTotalEnergyConsumption");
                const auto temperature = function<int (*)(NvmlDevice *, int, unsigned int *)>("nvmlDeviceGetTemperature");
                const auto uuid = function<int (*)(NvmlDevice *, char *, unsigned int)>("nvmlDeviceGetUUID");
                const auto name = function<int (*)(NvmlDevice *, char *, unsigned int)>("nvmlDeviceGetName");
                for (unsigned int index = 0; index < count; ++index)
                {
                    NvmlDevice *device = nullptr;
                    if (handle(index, &device) != 0)
                        continue;
                    char identity[128]{}, device_name[128]{};
                    const auto identity_status = uuid ? uuid(device, identity, sizeof(identity)) : -1;
                    if (name)
                        name(device, device_name, sizeof(device_name));
                    const std::string entity = identity_status == 0 ? identity : "nvidia-index:" + std::to_string(index);
                    const auto add = [&](const std::string &key, const char *source, const char *unit, const char *kind, Json value, int code)
                    {
                        auto sample = metric(key, "host_gpu", entity, source, unit, kind, code == 0 ? value : Json(nullptr),
                                             code == -1 ? "function not exposed by installed NVML driver" : error(code));
                        sample["device"] = device_name;
                        sample["index"] = index;
                        sample["memory_model"] = "device_reported_memory_not_host_RSS";
                        sample["measurement_scope"] = "whole_GPU_all_processes";
                        sample["sampling_window"] = "driver_defined";
                        metrics.push_back(std::move(sample));
                    };
                    NvmlUtilization usage{};
                    const auto usage_status = utilization ? utilization(device, &usage) : -1;
                    add("gpu_utilization_percent", "nvmlDeviceGetUtilizationRates.gpu", "percent", "gauge", usage.gpu, usage_status);
                    add("gpu_memory_utilization_percent", "nvmlDeviceGetUtilizationRates.memory", "percent", "gauge", usage.memory, usage_status);
                    NvmlMemory allocation{};
                    const auto memory_status = memory ? memory(device, &allocation) : -1;
                    add("gpu_memory_used_bytes", "nvmlDeviceGetMemoryInfo", "bytes", "gauge", allocation.used, memory_status);
                    add("gpu_memory_total_bytes", "nvmlDeviceGetMemoryInfo", "bytes", "gauge", allocation.total, memory_status);
                    add("gpu_memory_free_bytes", "nvmlDeviceGetMemoryInfo", "bytes", "gauge", allocation.free, memory_status);
                    unsigned int milliwatts = 0, celsius = 0;
                    unsigned long long millijoules = 0;
                    const auto power_status = power ? power(device, &milliwatts) : -1;
                    const auto energy_status = energy ? energy(device, &millijoules) : -1;
                    const auto temperature_status = temperature ? temperature(device, 0, &celsius) : -1;
                    add("power_watts", "nvmlDeviceGetPowerUsage", "watts", "gauge", milliwatts / 1000.0, power_status);
                    add("energy_joules", "nvmlDeviceGetTotalEnergyConsumption", "joules", "counter", millijoules / 1000.0, energy_status);
                    add("temperature_celsius", "nvmlDeviceGetTemperature(GPU)", "celsius", "gauge", celsius, temperature_status);
                }
                return metrics;
            }

        private:
            void *library_ = nullptr;
            bool initialized_ = false;
            std::string error_;
            std::mutex mutex_;
        };

        std::string label(const fs::path &path)
        {
            std::ifstream input(path);
            std::string value;
            std::getline(input, value);
            return value;
        }

        Json scaled_sensor(const fs::path &path, double divisor, bool signed_value = false)
        {
            std::ifstream input(path);
            std::int64_t value = 0;
            if (!(input >> value) || (!signed_value && value < 0))
                return nullptr;
            return static_cast<double>(value) / divisor;
        }
#endif

#ifdef __APPLE__
        Json number(CFDictionaryRef dictionary, const char *field)
        {
            const auto key = CFStringCreateWithCString(kCFAllocatorDefault, field, kCFStringEncodingUTF8);
            const auto value = CFDictionaryGetValue(dictionary, key);
            CFRelease(key);
            double output = 0;
            if (!value || CFGetTypeID(value) != CFNumberGetTypeID() ||
                !CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberDoubleType, &output) || !std::isfinite(output))
                return nullptr;
            return output;
        }
#endif
    }

    Json sensor_metrics()
    {
        Json metrics = Json::array();
#ifdef __APPLE__
        io_iterator_t devices = IO_OBJECT_NULL;
        bool gpu_found = false;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &devices) == KERN_SUCCESS)
        {
            for (io_object_t device = IOIteratorNext(devices); device; device = IOIteratorNext(devices))
            {
                std::uint64_t identity = 0;
                IORegistryEntryGetRegistryEntryID(device, &identity);
                CFTypeRef value = IORegistryEntryCreateCFProperty(device, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
                const auto dictionary = value && CFGetTypeID(value) == CFDictionaryGetTypeID() ? static_cast<CFDictionaryRef>(value) : nullptr;
                for (const auto &[name, key] : std::map<std::string, std::string>{{"gpu_utilization_percent", "Device Utilization %"},
                                                                                  {"gpu_renderer_utilization_percent", "Renderer Utilization %"},
                                                                                  {"gpu_tiler_utilization_percent", "Tiler Utilization %"},
                                                                                  {"gpu_memory_used_bytes", "In use system memory"},
                                                                                  {"gpu_memory_allocated_bytes", "Alloc system memory"}})
                {
                    auto sample = metric(name, "host_gpu", "IOAccelerator:" + std::to_string(identity),
                                         "IOAccelerator.PerformanceStatistics:" + key, name.ends_with("_bytes") ? "bytes" : "percent", "gauge",
                                         dictionary ? number(dictionary, key.c_str()) : Json(nullptr));
                    sample["memory_model"] = "unified_shared_with_host_do_not_add_to_RSS";
                    sample["measurement_scope"] = "whole_GPU_all_processes";
                    sample["sampling_window"] = "driver_defined";
                    sample["api_stability"] = "driver_specific_registry_schema";
                    metrics.push_back(std::move(sample));
                }
                for (const auto &[name, unit] : std::map<std::string, std::string>{{"power_watts", "watts"}, {"energy_joules", "joules"}, {"temperature_celsius", "celsius"}})
                    metrics.push_back(metric(name, "host_gpu", "IOAccelerator:" + std::to_string(identity), "IOAccelerator.PerformanceStatistics",
                                             unit, name == "energy_joules" ? "counter" : "gauge", nullptr,
                                             "not exposed by this macOS driver API; privileged powermetrics capture can be imported separately"));
                if (value)
                    CFRelease(value);
                IOObjectRelease(device);
                gpu_found = true;
            }
            IOObjectRelease(devices);
        }
        if (!gpu_found)
            metrics.push_back(metric("gpu_utilization_percent", "host_gpu", "unavailable", "IOAccelerator", "percent", "gauge", nullptr));
        for (const auto &[name, unit] : std::map<std::string, std::string>{{"power_watts", "watts"}, {"energy_joules", "joules"}, {"temperature_celsius", "celsius"}})
            metrics.push_back(metric(name, "host_sensor", "macos:package", "macOS_unprivileged", unit,
                                     name == "energy_joules" ? "counter" : "gauge", nullptr, "requires an operator-provided power/thermal capture on this platform"));
#elif defined(__linux__)
        static Nvml nvml;
        metrics = nvml.sample();
        std::error_code error;
        for (const auto &device : fs::directory_iterator("/sys/class/hwmon", error))
        {
            const auto chip = label(device.path() / "name");
            std::error_code read_error;
            const auto canonical_path = fs::canonical(device.path(), read_error).string();
            if (read_error)
                continue;
            for (const auto &entry : fs::directory_iterator(device.path(), read_error))
            {
                const auto filename = entry.path().filename().string();
                std::smatch match;
                if (!std::regex_match(filename, match, std::regex("(temp|power|energy)([0-9]+)_(input|average)")))
                    continue;
                const auto type = match[1].str();
                const auto stem = type + match[2].str();
                if (type != "power" && match[3].str() != "input")
                    continue;
                if (type == "power" && match[3].str() == "input" && fs::exists(device.path() / (stem + "_average")))
                    continue;
                const auto temperature_type = label(device.path() / (stem + "_type"));
                auto value = scaled_sensor(entry.path(), type == "temp" ? 1000 : 1'000'000, type == "temp");
                std::string reason = "sensor unavailable, disabled, or permission denied";
                if (label(device.path() / (stem + "_fault")) == "1" || label(device.path() / (stem + "_enable")) == "0")
                {
                    value = nullptr;
                    reason = "sensor disabled or hardware fault reported";
                }
                if (type == "temp" && temperature_type == "4")
                {
                    value = nullptr;
                    reason = "thermistor voltage conversion requires board-specific calibration";
                }
                auto sample = metric(type == "temp" ? "temperature_celsius" : (type == "power" ? "power_watts" : "energy_joules"),
                                     "host_sensor", canonical_path + ":" + stem, entry.path().string(), type == "temp" ? "celsius" : (type == "power" ? "watts" : "joules"),
                                     type == "energy" ? "counter" : "gauge", value, reason);
                sample["chip"] = chip;
                sample["label"] = label(device.path() / (stem + "_label"));
                sample["overlap_policy"] = "sensor_domains_may_overlap_do_not_sum";
                metrics.push_back(std::move(sample));
            }
        }
        std::set<std::string> zones;
        error.clear();
        for (const auto &entry : fs::directory_iterator("/sys/class/powercap", error))
        {
            std::error_code read_error;
            const auto path = fs::canonical(entry.path(), read_error);
            if (read_error || !fs::exists(path / "energy_uj") || !zones.insert(path.string()).second)
                continue;
            auto sample = metric("energy_joules", "host_energy_domain", path.string(), (path / "energy_uj").string(), "joules", "counter",
                                 scaled_sensor(path / "energy_uj", 1'000'000), "RAPL energy counter unavailable or access denied");
            sample["label"] = label(path / "name");
            sample["max_range_joules"] = scaled_sensor(path / "max_energy_range_uj", 1'000'000);
            sample["overlap_policy"] = "energy_domains_may_overlap_do_not_sum";
            metrics.push_back(std::move(sample));
        }
        error.clear();
        for (const auto &entry : fs::directory_iterator("/sys/class/thermal", error))
        {
            if (!entry.path().filename().string().starts_with("thermal_zone"))
                continue;
            auto sample = metric("temperature_celsius", "host_sensor", entry.path().string(), (entry.path() / "temp").string(),
                                 "celsius", "gauge", scaled_sensor(entry.path() / "temp", 1000, true), "thermal zone unavailable or access denied");
            sample["label"] = label(entry.path() / "type");
            sample["overlap_policy"] = "may_duplicate_hwmon_temperature_do_not_sum";
            metrics.push_back(std::move(sample));
        }
        for (const auto &[name, unit] : std::map<std::string, std::string>{{"power_watts", "watts"}, {"energy_joules", "joules"}, {"temperature_celsius", "celsius"}})
        {
            const bool found = std::any_of(metrics.begin(), metrics.end(), [&](const Json &sample)
                                           { return sample.at("name") == name && sample.at("scope") != "host_gpu"; });
            if (!found)
                metrics.push_back(metric(name, "host_sensor", "unavailable", "hwmon,powercap", unit,
                                         name == "energy_joules" ? "counter" : "gauge", nullptr, "no exposed hardware sensor or energy domain"));
        }
#endif
        return metrics;
    }
}