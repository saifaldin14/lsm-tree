#include "lsm_lab/telemetry.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <unistd.h>
#include <vector>
#include <openssl/evp.h>
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <mach/mach.h>
#include <net/if.h>
#include <net/route.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <uuid/uuid.h>
#endif

namespace lsm_lab::telemetry
{
    namespace fs = std::filesystem;
    using Json = nlohmann::json;
    namespace
    {
        std::string digest(const std::string &text)
        {
            std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
            unsigned int count = 0;
            if (EVP_Digest(text.data(), text.size(), bytes.data(), &count, EVP_sha256(), nullptr) != 1)
                throw std::runtime_error("host identity digest failed");
            constexpr char digits[] = "0123456789abcdef";
            std::string result;
            for (unsigned int index = 0; index < count; ++index)
            {
                result += digits[bytes[index] >> 4];
                result += digits[bytes[index] & 15];
            }
            return result;
        }

#ifdef __linux__
        std::string text_file(const fs::path &path)
        {
            std::ifstream input(path);
            std::string text;
            std::getline(input, text);
            return text;
        }

        Json integer_file(const fs::path &path)
        {
            std::ifstream input(path);
            std::uint64_t value = 0;
            return input >> value ? Json(value) : Json(nullptr);
        }
#endif

#ifdef __APPLE__
        Json cf_number(CFDictionaryRef values, const char *name)
        {
            if (!values)
                return nullptr;
            const auto key = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
            const auto value = CFDictionaryGetValue(values, key);
            CFRelease(key);
            std::int64_t number = 0;
            if (!value || CFGetTypeID(value) != CFNumberGetTypeID() ||
                !CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &number) || number < 0)
                return nullptr;
            return static_cast<std::uint64_t>(number);
        }

        std::string cf_text(CFTypeRef value)
        {
            char buffer[256]{};
            if (value && CFGetTypeID(value) == CFStringGetTypeID() &&
                CFStringGetCString(static_cast<CFStringRef>(value), buffer, sizeof(buffer), kCFStringEncodingUTF8))
                return buffer;
            return {};
        }
#endif
    }

    Json host_identity()
    {
        char hostname[256]{};
        if (gethostname(hostname, sizeof(hostname) - 1) != 0)
            throw std::runtime_error("gethostname failed");
        std::string machine;
        std::string boot;
        std::string source;
#ifdef __APPLE__
        uuid_t identifier{};
        timespec timeout{1, 0};
        if (gethostuuid(identifier, &timeout) == 0)
        {
            char text[37]{};
            uuid_unparse(identifier, text);
            machine = text;
        }
        timeval time{};
        std::size_t size = sizeof(time);
        if (sysctlbyname("kern.boottime", &time, &size, nullptr, 0) == 0)
            boot = std::to_string(time.tv_sec) + ":" + std::to_string(time.tv_usec);
        source = "gethostuuid,kern.boottime";
#elif defined(__linux__)
        machine = text_file("/etc/machine-id");
        boot = text_file("/proc/sys/kernel/random/boot_id");
        source = "/etc/machine-id,/proc/sys/kernel/random/boot_id";
#endif
        return {{"hostname", hostname}, {"host_id", machine.empty() ? Json(nullptr) : Json(digest(machine))}, {"boot_id", boot.empty() ? Json(nullptr) : Json(digest(boot))}, {"identity_source", source}, {"logical_cpus", sysconf(_SC_NPROCESSORS_ONLN)}};
    }

    Json host_metrics()
    {
        Json samples = Json::array();
        const auto add = [&](const std::string &name, const std::string &scope, const std::string &entity,
                             const std::string &source, const std::string &unit, const std::string &kind, Json value,
                             const std::string &reason = {})
        {
            samples.push_back(metric(name, scope, entity, source, unit, kind, std::move(value), reason));
        };
#ifdef __APPLE__
        host_t host = mach_host_self();
        host_cpu_load_info_data_t cpu{};
        mach_msg_type_number_t cpu_count = HOST_CPU_LOAD_INFO_COUNT;
        const auto ticks = sysconf(_SC_CLK_TCK);
        const bool cpu_ok = ticks > 0 && host_statistics(host, HOST_CPU_LOAD_INFO,
                                                         reinterpret_cast<host_info_t>(&cpu), &cpu_count) == KERN_SUCCESS;
        const std::array<std::pair<const char *, int>, 4> cpu_fields{{{"cpu_user_ns", CPU_STATE_USER},
                                                                      {"cpu_system_ns", CPU_STATE_SYSTEM},
                                                                      {"cpu_idle_ns", CPU_STATE_IDLE},
                                                                      {"cpu_nice_ns", CPU_STATE_NICE}}};
        for (const auto &[name, index] : cpu_fields)
            add(name, "host", "cpu", "host_statistics(HOST_CPU_LOAD_INFO)", "nanoseconds", "counter",
                cpu_ok ? Json(static_cast<std::uint64_t>(cpu.cpu_ticks[index]) * 1'000'000'000ULL / ticks) : Json(nullptr));
        vm_statistics64_data_t vm{};
        mach_msg_type_number_t vm_count = HOST_VM_INFO64_COUNT;
        vm_size_t page_size = 0;
        const bool vm_ok = host_page_size(host, &page_size) == KERN_SUCCESS && host_statistics64(host, HOST_VM_INFO64,
                                                                                                 reinterpret_cast<host_info64_t>(&vm), &vm_count) == KERN_SUCCESS;
        mach_port_deallocate(mach_task_self(), host);
        const std::map<std::string, std::uint64_t> memory{{"memory_free_bytes", vm.free_count}, {"memory_active_bytes", vm.active_count}, {"memory_inactive_bytes", vm.inactive_count}, {"memory_wired_bytes", vm.wire_count}, {"memory_compressed_bytes", vm.compressor_page_count}, {"memory_file_backed_bytes", vm.external_page_count}, {"swap_read_bytes", vm.swapins}, {"swap_write_bytes", vm.swapouts}};
        for (const auto &[name, pages] : memory)
            add(name, "host", "memory", "host_statistics64(HOST_VM_INFO64)", "bytes", name.starts_with("swap_") ? "counter" : "gauge",
                vm_ok ? Json(pages * page_size) : Json(nullptr));
        std::uint64_t memory_size = 0;
        std::size_t memory_length = sizeof(memory_size);
        const bool memory_ok = sysctlbyname("hw.memsize", &memory_size, &memory_length, nullptr, 0) == 0;
        add("memory_total_bytes", "host", "memory", "sysctl(hw.memsize)", "bytes", "gauge", memory_ok ? Json(memory_size) : Json(nullptr));

        int mib[] = {CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0};
        std::size_t length = 0;
        bool network_ok = false;
        if (sysctl(mib, 6, nullptr, &length, nullptr, 0) == 0 && length < 16ULL << 20)
        {
            std::vector<char> buffer(length);
            if (sysctl(mib, 6, buffer.data(), &length, nullptr, 0) == 0)
            {
                for (std::size_t offset = 0; offset + 4 <= length;)
                {
                    std::uint16_t record_bytes = 0;
                    std::memcpy(&record_bytes, buffer.data() + offset, sizeof(record_bytes));
                    if (record_bytes < 4 || record_bytes > length - offset)
                        break;
                    if (static_cast<unsigned char>(buffer[offset + 3]) == RTM_IFINFO2 && record_bytes >= sizeof(if_msghdr2))
                    {
                        if_msghdr2 record{};
                        std::memcpy(&record, buffer.data() + offset, sizeof(record));
                        char name[IF_NAMESIZE]{};
                        if (if_indextoname(record.ifm_index, name))
                        {
                            const std::string entity = std::string(name) + ":" + std::to_string(record.ifm_index);
                            const std::map<std::string, std::uint64_t> counters{{"network_rx_bytes", record.ifm_data.ifi_ibytes},
                                                                                {"network_tx_bytes", record.ifm_data.ifi_obytes},
                                                                                {"network_rx_packets", record.ifm_data.ifi_ipackets},
                                                                                {"network_tx_packets", record.ifm_data.ifi_opackets},
                                                                                {"network_rx_errors", record.ifm_data.ifi_ierrors},
                                                                                {"network_tx_errors", record.ifm_data.ifi_oerrors},
                                                                                {"network_rx_drops", record.ifm_data.ifi_iqdrops}};
                            for (const auto &[key, value] : counters)
                            {
                                add(key, "host_interface", entity, "sysctl(NET_RT_IFLIST2)", key.ends_with("_bytes") ? "bytes" : "count", "counter", value);
                                samples.back()["interface"] = name;
                                samples.back()["loopback"] = (record.ifm_flags & IFF_LOOPBACK) != 0;
                                samples.back()["layer"] = "OS_interface_not_application_or_wire_payload";
                            }
                            network_ok = true;
                        }
                    }
                    offset += record_bytes;
                }
            }
        }
        if (!network_ok)
            add("network_rx_bytes", "host_interface", "unavailable", "sysctl(NET_RT_IFLIST2)", "bytes", "counter", nullptr);

        io_iterator_t drivers = IO_OBJECT_NULL;
        bool disk_ok = false;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching(kIOBlockStorageDriverClass), &drivers) == KERN_SUCCESS)
        {
            for (io_object_t driver = IOIteratorNext(drivers); driver; driver = IOIteratorNext(drivers))
            {
                std::uint64_t registry_id = 0;
                IORegistryEntryGetRegistryEntryID(driver, &registry_id);
                const auto protocol_value = IORegistryEntrySearchCFProperty(driver, kIOServicePlane,
                                                                            CFSTR(kIOPropertyProtocolCharacteristicsKey), kCFAllocatorDefault,
                                                                            kIORegistryIterateRecursively | kIORegistryIterateParents);
                std::string interconnect, location;
                if (protocol_value && CFGetTypeID(protocol_value) == CFDictionaryGetTypeID())
                {
                    const auto protocol = static_cast<CFDictionaryRef>(protocol_value);
                    interconnect = cf_text(CFDictionaryGetValue(protocol, CFSTR(kIOPropertyPhysicalInterconnectTypeKey)));
                    location = cf_text(CFDictionaryGetValue(protocol, CFSTR(kIOPropertyPhysicalInterconnectLocationKey)));
                }
                if (protocol_value)
                    CFRelease(protocol_value);
                const bool virtual_device = interconnect == "Virtual Interface" || location == "File";
                CFTypeRef stats = IORegistryEntryCreateCFProperty(driver, CFSTR(kIOBlockStorageDriverStatisticsKey), kCFAllocatorDefault, 0);
                io_iterator_t children = IO_OBJECT_NULL;
                std::string device;
                if (IORegistryEntryCreateIterator(driver, kIOServicePlane, kIORegistryIterateRecursively, &children) == KERN_SUCCESS)
                {
                    for (io_object_t child = IOIteratorNext(children); child; child = IOIteratorNext(children))
                    {
                        CFTypeRef whole = IORegistryEntryCreateCFProperty(child, CFSTR("Whole"), kCFAllocatorDefault, 0);
                        if (whole && CFEqual(whole, kCFBooleanTrue))
                        {
                            CFTypeRef name = IORegistryEntryCreateCFProperty(child, CFSTR("BSD Name"), kCFAllocatorDefault, 0);
                            if (device.empty())
                                device = cf_text(name);
                            if (name)
                                CFRelease(name);
                        }
                        if (whole)
                            CFRelease(whole);
                        IOObjectRelease(child);
                    }
                    IOObjectRelease(children);
                }
                if (stats && CFGetTypeID(stats) == CFDictionaryGetTypeID() && !device.empty())
                {
                    const auto dictionary = static_cast<CFDictionaryRef>(stats);
                    for (const auto &[key, field] : std::map<std::string, const char *>{{"disk_read_bytes", "Bytes (Read)"},
                                                                                        {"disk_write_bytes", "Bytes (Write)"},
                                                                                        {"disk_read_operations", "Operations (Read)"},
                                                                                        {"disk_write_operations", "Operations (Write)"},
                                                                                        {"disk_read_time_ns", "Total Time (Read)"},
                                                                                        {"disk_write_time_ns", "Total Time (Write)"}})
                    {
                        add(key, virtual_device ? "host_virtual_block_device" : "host_block_device", device + ":" + std::to_string(registry_id), "IOBlockStorageDriver.Statistics",
                            key.ends_with("_bytes") ? "bytes" : (key.ends_with("_ns") ? "nanoseconds" : "count"), "counter", cf_number(dictionary, field));
                        samples.back()["device"] = device;
                        samples.back()["layer"] = "whole_device_driver_not_NAND_media";
                        samples.back()["partition"] = false;
                        samples.back()["physical_interconnect"] = interconnect;
                        samples.back()["device_location"] = location;
                        samples.back()["virtual_device"] = interconnect.empty() ? Json(nullptr) : Json(virtual_device);
                    }
                    disk_ok = disk_ok || !virtual_device;
                }
                if (stats)
                    CFRelease(stats);
                IOObjectRelease(driver);
            }
            IOObjectRelease(drivers);
        }
        if (!disk_ok)
            add("disk_read_bytes", "host_block_device", "unavailable", "IOBlockStorageDriver.Statistics", "bytes", "counter", nullptr);
        add("nfs_server_read_bytes", "host_nfs_server", "nfsd", "macOS", "bytes", "counter", nullptr,
            "macOS nfsd byte counters are not exposed by this collector; use host interface and device counters");
#elif defined(__linux__)
        std::ifstream stat("/proc/stat");
        std::string line;
        if (std::getline(stat, line))
        {
            std::istringstream values(line);
            std::string label;
            values >> label;
            const auto ticks = sysconf(_SC_CLK_TCK);
            for (const auto *name : {"cpu_user_ns", "cpu_nice_ns", "cpu_system_ns", "cpu_idle_ns", "cpu_iowait_ns",
                                     "cpu_irq_ns", "cpu_softirq_ns", "cpu_steal_ns"})
            {
                std::uint64_t value = 0;
                if (label == "cpu" && ticks > 0 && values >> value)
                    add(name, "host", "cpu", "/proc/stat", "nanoseconds", "counter", (value / ticks) * 1'000'000'000ULL + (value % ticks) * 1'000'000'000ULL / ticks);
                else
                    add(name, "host", "cpu", "/proc/stat", "nanoseconds", "counter", nullptr);
            }
        }
        else
            add("cpu_user_ns", "host", "cpu", "/proc/stat", "nanoseconds", "counter", nullptr);
        std::map<std::string, std::uint64_t> memory;
        std::ifstream meminfo("/proc/meminfo");
        while (std::getline(meminfo, line))
        {
            std::istringstream values(line);
            std::string name, unit;
            std::uint64_t value = 0;
            if (values >> name >> value >> unit && unit == "kB")
                memory[name] = value * 1024;
        }
        for (const auto &[name, field] : std::map<std::string, std::string>{{"memory_total_bytes", "MemTotal:"}, {"memory_available_bytes", "MemAvailable:"}, {"memory_free_bytes", "MemFree:"}, {"memory_cached_bytes", "Cached:"}, {"memory_buffers_bytes", "Buffers:"}, {"memory_slab_bytes", "Slab:"}, {"memory_dirty_bytes", "Dirty:"}, {"memory_writeback_bytes", "Writeback:"}, {"swap_total_bytes", "SwapTotal:"}, {"swap_free_bytes", "SwapFree:"}})
            add(name, "host", "memory", "/proc/meminfo", "bytes", "gauge", memory.contains(field) ? Json(memory.at(field)) : Json(nullptr));
        std::error_code error;
        bool network_ok = false;
        for (const auto &entry : fs::directory_iterator("/sys/class/net", error))
        {
            const auto name = entry.path().filename().string();
            const auto index = integer_file(entry.path() / "ifindex");
            const auto entity = name + ":" + index.dump();
            for (const auto &[key, field] : std::map<std::string, std::string>{{"network_rx_bytes", "rx_bytes"}, {"network_tx_bytes", "tx_bytes"}, {"network_rx_packets", "rx_packets"}, {"network_tx_packets", "tx_packets"}, {"network_rx_errors", "rx_errors"}, {"network_tx_errors", "tx_errors"}, {"network_rx_drops", "rx_dropped"}, {"network_tx_drops", "tx_dropped"}})
            {
                add(key, "host_interface", entity, "/sys/class/net/IF/statistics", key.ends_with("_bytes") ? "bytes" : "count", "counter",
                    integer_file(entry.path() / "statistics" / field));
                samples.back()["interface"] = name;
                samples.back()["loopback"] = integer_file(entry.path() / "type") == 772;
                samples.back()["layer"] = "OS_interface_not_application_or_wire_payload";
            }
            network_ok = true;
        }
        if (!network_ok)
            add("network_rx_bytes", "host_interface", "unavailable", "/sys/class/net", "bytes", "counter", nullptr, error.message());
        error.clear();
        bool disk_ok = false;
        for (const auto &entry : fs::directory_iterator("/sys/block", error))
        {
            if (!fs::exists(entry.path() / "device"))
                continue;
            const auto name = entry.path().filename().string();
            const auto entity = name + ":" + text_file(entry.path() / "dev") + ":" + text_file(entry.path() / "diskseq");
            std::ifstream disk(entry.path() / "stat");
            std::vector<std::uint64_t> counters;
            for (std::uint64_t value = 0; disk >> value;)
                counters.push_back(value);
            if (counters.size() < 11)
                continue;
            const std::map<std::string, std::uint64_t> values{{"disk_read_bytes", counters[2] * 512}, {"disk_write_bytes", counters[6] * 512}, {"disk_read_operations", counters[0]}, {"disk_write_operations", counters[4]}, {"disk_read_time_ns", counters[3] * 1'000'000}, {"disk_write_time_ns", counters[7] * 1'000'000}, {"disk_busy_ns", counters[9] * 1'000'000}, {"disk_weighted_io_ns", counters[10] * 1'000'000}};
            for (const auto &[key, value] : values)
            {
                add(key, "host_block_device", entity, "/sys/block/DEVICE/stat", key.ends_with("_bytes") ? "bytes" : (key.ends_with("_ns") ? "nanoseconds" : "count"), "counter", value);
                samples.back()["device"] = name;
                samples.back()["partition"] = false;
                samples.back()["layer"] = "whole_device_block_layer_not_NAND_media";
                samples.back()["virtualization"] = "physical_media_or_hypervisor_backing_not_determined";
            }
            disk_ok = true;
        }
        if (!disk_ok)
            add("disk_read_bytes", "host_block_device", "unavailable", "/sys/block", "bytes", "counter", nullptr, error.message());
        std::ifstream nfs("/proc/net/rpc/nfsd");
        bool nfs_ok = false;
        while (std::getline(nfs, line))
        {
            std::istringstream values(line);
            std::string name;
            std::uint64_t read_bytes = 0, written_bytes = 0;
            if (values >> name >> read_bytes >> written_bytes && name == "io")
            {
                add("nfs_server_read_bytes", "host_nfs_server", "nfsd", "/proc/net/rpc/nfsd:io", "bytes", "counter", read_bytes);
                add("nfs_server_write_bytes", "host_nfs_server", "nfsd", "/proc/net/rpc/nfsd:io", "bytes", "counter", written_bytes);
                nfs_ok = true;
            }
        }
        if (!nfs_ok)
            add("nfs_server_read_bytes", "host_nfs_server", "nfsd", "/proc/net/rpc/nfsd:io", "bytes", "counter", nullptr, "NFS server counters unavailable or nfsd not running");
#endif
        return samples;
    }
}