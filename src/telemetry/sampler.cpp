#include "lsm_lab/telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

namespace lsm_lab::telemetry
{
    using Json = nlohmann::json;
    namespace
    {
        std::uint64_t monotonic_ns()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        Json thread_cpu_ns()
        {
#ifdef __APPLE__
            thread_basic_info_data_t info{};
            mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
            const auto thread = mach_thread_self();
            const auto result = thread_info(thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count);
            mach_port_deallocate(mach_task_self(), thread);
            if (result == KERN_SUCCESS)
                return (static_cast<std::uint64_t>(info.user_time.seconds) + info.system_time.seconds) * 1'000'000'000ULL +
                       (static_cast<std::uint64_t>(info.user_time.microseconds) + info.system_time.microseconds) * 1000;
#elif defined(__linux__)
            rusage usage{};
            if (getrusage(RUSAGE_THREAD, &usage) == 0)
                return (static_cast<std::uint64_t>(usage.ru_utime.tv_sec) + usage.ru_stime.tv_sec) * 1'000'000'000ULL +
                       (static_cast<std::uint64_t>(usage.ru_utime.tv_usec) + usage.ru_stime.tv_usec) * 1000;
#endif
            return nullptr;
        }
    }

    struct Sampler::Impl
    {
        explicit Impl(SamplerOptions requested) : options(std::move(requested)) {}
        ~Impl()
        {
            try
            {
                stop();
            }
            catch (...)
            {
            }
            if (descriptor >= 0)
                close(descriptor);
        }

        void write_record(const Json &record, bool footer = false)
        {
            const auto text = record.dump() + '\n';
            if (text.size() + bytes + (footer ? 0 : 8192) > options.max_output_bytes)
                throw std::runtime_error("telemetry output budget exhausted");
            std::size_t offset = 0;
            while (offset < text.size())
            {
                const auto written = write(descriptor, text.data() + offset, text.size() - offset);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written <= 0)
                    throw std::runtime_error("telemetry write: " + std::string(std::strerror(errno)));
                offset += static_cast<std::size_t>(written);
                bytes += static_cast<std::size_t>(written);
            }
        }

        void sample()
        {
            const auto started = monotonic_ns();
            const auto cpu_before = thread_cpu_ns();
            Json values = Json::array();
            Json errors = Json::array();
            const auto collect = [&](const std::string &source, auto &&callback)
            {
                try
                {
                    const auto collected = callback();
                    for (const auto &value : collected)
                        values.push_back(value);
                }
                catch (const std::exception &error)
                {
                    errors.push_back({{"source", source}, {"reason", error.what()}});
                }
            };
            collect("process", [&]
                    { return process_metrics(options.pid,
                                             options.include_host && options.pid == getpid() ? "collector" : options.role); });
            if (options.include_host)
            {
                collect("host", []
                        { return host_metrics(); });
                collect("sensors", []
                        { return sensor_metrics(); });
            }
            for (const auto &required : options.required_metrics)
            {
                if (!std::any_of(values.begin(), values.end(), [&](const Json &value)
                                 { return value.at("scope").get<std::string>() + ":" + value.at("name").get<std::string>() == required &&
                                          value.at("status") == "ok"; }))
                    errors.push_back({{"source", required}, {"reason", "required metric unavailable"}});
            }
            const auto completed = monotonic_ns();
            const auto realtime = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            write_record({{"type", "sample"}, {"sequence", samples}, {"sample_start_ns", started}, {"sample_end_ns", completed}, {"realtime_ns", realtime}, {"metrics", values}, {"errors", errors}});
            const auto cpu_after = thread_cpu_ns();
            if (!cpu_before.is_null() && !cpu_after.is_null() && cpu_after.get<std::uint64_t>() >= cpu_before.get<std::uint64_t>())
                collector_cpu += cpu_after.get<std::uint64_t>() - cpu_before.get<std::uint64_t>();
            else
                cpu_available = false;
            collection_wall += monotonic_ns() - started;
            error_samples += !errors.empty();
            ++samples;
        }

        void start()
        {
            if (options.output.empty() || options.role.empty() || options.interval_ms < 10 || options.interval_ms > 60'000 ||
                options.max_output_bytes < 16'384 || options.max_output_bytes > (16ULL << 30))
                throw std::invalid_argument("invalid telemetry sampler options");
            if (options.pid == 0)
                options.pid = getpid();
            if (options.pid < 1 || options.pid > std::numeric_limits<pid_t>::max())
                throw std::invalid_argument("invalid sampler process id");
            descriptor = open(options.output.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (descriptor < 0)
                throw std::runtime_error("create telemetry output: " + std::string(std::strerror(errno)));
            identity = host_identity();
            started_ns = monotonic_ns();
            write_record({{"type", "header"}, {"schema_version", 1}, {"host", identity}, {"role", options.role}, {"pid", options.pid}, {"collector_pid", getpid()}, {"includes_host", options.include_host}, {"interval_ms", options.interval_ms}, {"max_output_bytes", options.max_output_bytes}, {"monotonic_clock", "std::chrono::steady_clock_host_local"}, {"start_ns", started_ns}, {"required_metrics", options.required_metrics}, {"process_includes_children", false}, {"host_totals_include_background_activity", true}});
            sample();
            thread = std::thread([this]
                                 {
                auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.interval_ms);
                std::unique_lock lock(mutex);
                while (!condition.wait_until(lock, next, [this] { return stopping; }))
                {
                    lock.unlock();
                    try { sample(); }
                    catch (const std::exception &error) { failure = error.what(); return; }
                    next += std::chrono::milliseconds(options.interval_ms);
                    if (next <= std::chrono::steady_clock::now())
                    {
                        ++overruns;
                        next = std::chrono::steady_clock::now() + std::chrono::milliseconds(options.interval_ms);
                    }
                    lock.lock();
                } });
        }

        Json stop()
        {
            if (finished)
                return result;
            {
                const std::lock_guard lock(mutex);
                stopping = true;
            }
            condition.notify_all();
            if (thread.joinable())
                thread.join();
            if (descriptor < 0)
                return Json{};
            try
            {
                if (failure.empty())
                    sample();
            }
            catch (const std::exception &error)
            {
                failure = error.what();
            }
            result = {{"type", "footer"}, {"status", failure.empty() && error_samples == 0 ? "complete" : "incomplete"}, {"error", failure}, {"samples", samples}, {"error_samples", error_samples}, {"overruns", overruns}, {"start_ns", started_ns}, {"end_ns", monotonic_ns()}, {"sample_collection_wall_ns", collection_wall}, {"sample_collection_thread_cpu_ns", cpu_available ? Json(collector_cpu) : Json(nullptr)}, {"bytes_before_footer", bytes}, {"overhead_scope", "collection_and_writing_only_not_whole_host_perturbation"}};
            try
            {
                write_record(result, true);
                if (fsync(descriptor) != 0)
                    throw std::runtime_error("telemetry fsync failed");
            }
            catch (const std::exception &error)
            {
                result["status"] = "incomplete";
                result["error"] = error.what();
            }
            finished = true;
            return result;
        }

        SamplerOptions options;
        Json identity, result;
        int descriptor = -1;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable condition;
        bool stopping = false, finished = false, cpu_available = true;
        std::string failure;
        std::uint64_t bytes = 0, samples = 0, error_samples = 0, overruns = 0;
        std::uint64_t started_ns = 0, collection_wall = 0, collector_cpu = 0;
    };

    Sampler::Sampler(SamplerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) { impl_->start(); }
    Sampler::~Sampler() = default;
    Json Sampler::stop() { return impl_->stop(); }
}