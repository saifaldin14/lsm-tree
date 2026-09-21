#include "lsm_lab/load.hpp"
#include "benchmark.grpc.pb.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <set>
#include <thread>
#include <vector>
#include <openssl/evp.h>

namespace lsm_lab
{
    namespace wire = rpc::v1;
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    namespace
    {
        struct Counts
        {
            std::uint64_t completed = 0, failed = 0, expired = 0, blocked = 0;
            std::uint64_t indeterminate = 0, mismatched = 0, in_window = 0;
        };

        struct Lane
        {
            std::mutex mutex;
            std::condition_variable changed;
            std::deque<ScheduledOperation> queue;
            bool closed = false;
            LatencyHistogram reads, writes, queue_delay, rpc_latency;
            std::map<std::uint64_t, std::uint64_t> versions;
            std::set<std::uint64_t> uncertain;
            std::array<Counts, 2> counts;
            std::exception_ptr exception;
        };

        std::int64_t elapsed(Clock::time_point start)
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
        }

        void sync_stream(std::ofstream &stream, const fs::path &path)
        {
            stream.flush();
            require(stream.good(), "artifact write failed: " + path.string());
            stream.close();
            check(rocksdb::Env::Default()->SyncFile(path.string(), rocksdb::EnvOptions(), true));
        }

        Json count_json(const Counts &counts, std::uint64_t offered, std::uint64_t rejected)
        {
            require(counts.completed + counts.failed + counts.expired + counts.blocked + rejected == offered,
                    "request accounting mismatch");
            return {{"offered", offered}, {"completed", counts.completed}, {"failed", counts.failed}, {"expired_before_send", counts.expired}, {"blocked_by_indeterminate_write", counts.blocked}, {"queue_rejected", rejected}, {"indeterminate_writes", counts.indeterminate}, {"payload_mismatches", counts.mismatched}, {"completed_in_window", counts.in_window}};
        }
    }

    Json run_load(const LoadOptions &options)
    {
        require(valid_id(options.run_id) && options.requests >= 1 && options.requests <= 1'000'000'000 &&
                    options.warmup_requests <= 1'000'000'000 && options.rate >= 1 && options.rate <= 10'000'000 &&
                    options.read_percent <= 100 && options.threads >= 1 && options.threads <= 64 &&
                    options.queue_capacity >= 1 && options.queue_capacity <= 65536 && options.deadline_ms >= 1 &&
                    options.deadline_ms <= 60000,
                "invalid load parameters");
        const auto total_requests = options.requests + options.warmup_requests;
        const auto warmup_ns = options.warmup_requests * 1'000'000'000ULL / options.rate;
        const auto end_ns = total_requests * 1'000'000'000ULL / options.rate;
        require(end_ns <= 86'400'000'000'000ULL, "load duration exceeds one day");
        require(total_requests <= options.artifact_budget_bytes / 2048, "request artifacts exceed declared disk budget");
        require(!fs::exists(options.output), "load output already exists");
        const auto parent = fs::canonical(fs::absolute(options.output).parent_path());
        const auto available = fs::space(parent).available;
        require(available >= (256ULL << 20) && total_requests * 2048 <= available - (256ULL << 20), "insufficient artifact disk space");
        auto channel = connect(options.endpoint, options.tls);
        auto control = wire::BenchmarkService::NewStub(channel);
        wire::DescribeRequest describe;
        describe.set_wire_version(wire_version);
        wire::DescribeResponse description;
        grpc::ClientContext handshake;
        handshake.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        const auto connected = control->Describe(&handshake, describe, &description);
        require(connected.ok(), "database handshake failed: " + connected.error_message());
        require(description.wire_version() == wire_version && description.run_id() == options.run_id &&
                    description.engine_fingerprint() == engine_fingerprint() && description.sync_writes(),
                "database identity mismatch");
        require(options.required_mode.empty() || description.compaction_backend() == options.required_mode, "database mode mismatch");
        const auto spec = parse_spec(description.dataset_spec_json());
        require(description.resolved_spec_sha256() == digest_hex(spec_json(spec).dump()), "database specification mismatch");
        const auto estimated_memory = std::min(spec.dataset.records, total_requests) * 192 +
                                      static_cast<std::uint64_t>(options.threads) * (options.queue_capacity * sizeof(ScheduledOperation) + (2ULL << 20));
        require(estimated_memory <= options.memory_budget_bytes, "load exceeds declared memory estimate budget");
        std::uint64_t planned_writes = 0;
        for (std::uint64_t request_id = 0; request_id < total_requests; ++request_id)
        {
            planned_writes += scheduled_operation(spec.dataset, options, request_id).write;
        }
        const auto logical_bytes = planned_writes * (static_cast<std::uint64_t>(spec.dataset.key_bytes) + spec.dataset.value_bytes);
        require(logical_bytes <= options.write_budget_bytes, "planned load exceeds logical write budget");
        const auto output = parent / fs::absolute(options.output).filename();
        require(fs::create_directory(output), "cannot reserve load output");
        sync_directory(parent);
        Json manifest{{"schema_version", 1}, {"owner", "lsm-lab"}, {"state", "incomplete"}, {"run_id", options.run_id}, {"dataset_id", description.dataset_id()}, {"engine_fingerprint", engine_fingerprint()}, {"spec", spec_json(spec)}, {"mode", description.compaction_backend()}, {"seed", options.seed}, {"rate", options.rate}, {"read_percent", options.read_percent}, {"requests", options.requests}, {"warmup_requests", options.warmup_requests}, {"threads", options.threads}, {"queue_capacity", options.queue_capacity}, {"deadline_ms", options.deadline_ms}, {"planned_logical_write_bytes", logical_bytes}, {"memory_budget_bytes", options.memory_budget_bytes}, {"schedule_generator", "splitmix64-uniform-v1"}, {"value_generator", "splitmix64-record-v1"}};
        durable_json(output / "manifest.json", manifest);
        std::ofstream schedule(output / "schedule.jsonl", std::ios::binary);
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
        require(digest && EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) == 1, "cannot initialize schedule digest");
        for (std::uint64_t request_id = 0; request_id < total_requests; ++request_id)
        {
            const auto operation = scheduled_operation(spec.dataset, options, request_id);
            const auto encoded = Json{{"request_id", request_id}, {"key_index", operation.key_index}, {"generation", operation.generation}, {"intended_ns", operation.intended_ns}, {"write", operation.write}}.dump() + "\n";
            schedule << encoded;
            require(EVP_DigestUpdate(digest.get(), encoded.data(), encoded.size()) == 1, "schedule digest failed");
        }
        std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
        unsigned int hash_size = 0;
        require(EVP_DigestFinal_ex(digest.get(), hash.data(), &hash_size) == 1 && hash_size == 32, "schedule digest failed");
        constexpr char hex[] = "0123456789abcdef";
        std::string fingerprint;
        for (unsigned int index = 0; index < hash_size; ++index)
        {
            fingerprint += hex[hash[index] >> 4];
            fingerprint += hex[hash[index] & 15];
        }
        manifest["schedule_sha256"] = fingerprint;
        sync_stream(schedule, output / "schedule.jsonl");
        durable_json(output / "manifest.json", manifest);
        std::vector<std::unique_ptr<Lane>> lanes;
        for (std::uint32_t index = 0; index < options.threads; ++index)
        {
            lanes.push_back(std::make_unique<Lane>());
        }
        const auto start = Clock::now() + std::chrono::milliseconds(250);
        std::vector<std::thread> threads;
        std::thread sampler;
        std::atomic<bool> stop_sampling{false};
        std::mutex sample_mutex;
        std::condition_variable sample_changed;
        std::atomic<std::uint64_t> sample_errors{0};
        const auto close_threads = [&]
        {
            for (auto &lane : lanes)
            {
                const std::lock_guard guard(lane->mutex);
                lane->closed = true;
                lane->changed.notify_all();
            }
            for (auto &thread : threads)
            {
                if (thread.joinable())
                    thread.join();
            }
            stop_sampling.store(true);
            sample_changed.notify_all();
            if (sampler.joinable())
                sampler.join();
        };
        std::array<std::uint64_t, 2> rejected{};
        LatencyHistogram generator_lag;
        try
        {
            sampler = std::thread([&]
                                  {
                try
                {
                    std::ofstream samples(output / "primary-metrics.jsonl", std::ios::binary);
                    while (!stop_sampling.load())
                    {
                        grpc::ClientContext context;
                        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
                        wire::StatisticsResponse response;
                        const auto sample_start = elapsed(start);
                        const auto status = control->Statistics(&context, describe, &response);
                        const auto sample_end = elapsed(start);
                        sample_errors += !status.ok();
                        samples << Json{{"load_sample_start_ns", sample_start}, {"load_elapsed_ns", sample_end}, {"status", status.ok() ? "ok" : "failed"},
                                        {"statistics", status.ok() ? Json::parse(response.statistics_json()) : Json(nullptr)}}.dump() << '\n';
                        std::unique_lock lock(sample_mutex);
                        sample_changed.wait_for(lock, std::chrono::seconds(1), [&] { return stop_sampling.load(); });
                    }
                    sync_stream(samples, output / "primary-metrics.jsonl");
                }
                catch (...) { ++sample_errors; } });
            for (std::size_t index = 0; index < lanes.size(); ++index)
            {
                threads.emplace_back([&, index]
                                     {
                    auto &lane = *lanes[index];
                    try
                    {
                        auto client = wire::BenchmarkService::NewStub(channel);
                        const auto path = output / ("requests-" + std::to_string(index) + ".jsonl");
                        std::ofstream trace(path, std::ios::binary);
                        while (true)
                        {
                            ScheduledOperation operation;
                            {
                                std::unique_lock guard(lane.mutex);
                                lane.changed.wait(guard, [&] { return lane.closed || !lane.queue.empty(); });
                                if (lane.queue.empty()) break;
                                operation = lane.queue.front();
                                lane.queue.pop_front();
                            }
                            const auto phase = operation.request_id < options.warmup_requests ? 0U : 1U;
                            auto &counts = lane.counts[phase];
                            const auto due = static_cast<std::int64_t>(operation.intended_ns);
                            Json record{{"request_id", operation.request_id}, {"key_index", operation.key_index}, {"write", operation.write},
                                        {"phase", phase == 0 ? "warmup" : "measurement"}, {"intended_ns", due}, {"send_ns", nullptr}};
                            if (lane.uncertain.contains(operation.key_index))
                            {
                                ++counts.blocked;
                                record["status"] = "blocked_by_indeterminate_write";
                                record["complete_ns"] = elapsed(start);
                                trace << record.dump() << '\n';
                                continue;
                            }
                            const auto key = record_key(spec.dataset, operation.key_index);
                            const auto value = operation.write ? record_value(spec.dataset, operation.key_index, operation.generation) : std::string();
                            const auto sent = elapsed(start);
                            const auto remaining = due + static_cast<std::int64_t>(options.deadline_ms) * 1'000'000 - sent;
                            if (remaining <= 0)
                            {
                                ++counts.expired;
                                record["status"] = "expired_before_send";
                                record["complete_ns"] = sent;
                                trace << record.dump() << '\n';
                                continue;
                            }
                            record["send_ns"] = sent;
                            grpc::ClientContext context;
                            context.set_deadline(std::chrono::system_clock::now() +
                                std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds(remaining)));
                            grpc::Status status;
                            bool verified = true;
                            std::int64_t completed = 0;
                            if (operation.write)
                            {
                                wire::PutRequest request;
                                request.mutable_identity()->set_wire_version(wire_version);
                                request.mutable_identity()->set_run_id(options.run_id);
                                request.mutable_identity()->set_request_id(operation.request_id);
                                request.set_key(key);
                                request.set_value(value);
                                wire::WriteResponse response;
                                status = client->Put(&context, request, &response);
                                completed = elapsed(start);
                                verified = !status.ok() || response.request_id() == operation.request_id;
                                if (status.ok()) lane.versions[operation.key_index] = operation.generation;
                                else
                                {
                                    ++counts.indeterminate;
                                    lane.uncertain.insert(operation.key_index);
                                }
                            }
                            else
                            {
                                wire::GetRequest request;
                                request.mutable_identity()->set_wire_version(wire_version);
                                request.mutable_identity()->set_run_id(options.run_id);
                                request.mutable_identity()->set_request_id(operation.request_id);
                                request.set_key(key);
                                wire::GetResponse response;
                                status = client->Get(&context, request, &response);
                                completed = elapsed(start);
                                const auto previous = lane.versions.find(operation.key_index);
                                const auto generation = previous == lane.versions.end() ? spec.dataset.overwrite_rounds : previous->second;
                                verified = !status.ok() || (response.request_id() == operation.request_id && response.found() &&
                                    response.value() == record_value(spec.dataset, operation.key_index, generation));
                            }
                            record["complete_ns"] = completed;
                            record["grpc_code"] = status.error_code();
                            record["full_payload_verified"] = !operation.write && status.ok() && verified;
                            if (phase == 1)
                            {
                                lane.queue_delay.record(sent - due);
                                lane.rpc_latency.record(completed - sent);
                            }
                            if (status.ok() && verified)
                            {
                                ++counts.completed;
                                counts.in_window += completed < static_cast<std::int64_t>(phase == 0 ? warmup_ns : end_ns);
                                if (phase == 1) (operation.write ? lane.writes : lane.reads).record(completed - due);
                                record["status"] = "ok";
                            }
                            else
                            {
                                ++counts.failed;
                                counts.mismatched += status.ok() && !verified;
                                record["status"] = status.ok() ? "payload_mismatch" : (operation.write ? "indeterminate_write" : "rpc_failed");
                                record["detail"] = status.error_message().substr(0, 256);
                            }
                            trace << record.dump() << '\n';
                        }
                        sync_stream(trace, path);
                    }
                    catch (...) { lane.exception = std::current_exception(); } });
            }
            std::ofstream rejected_file(output / "rejected.jsonl", std::ios::binary);
            for (std::uint64_t request_id = 0; request_id < total_requests; ++request_id)
            {
                const auto operation = scheduled_operation(spec.dataset, options, request_id);
                std::this_thread::sleep_until(start + std::chrono::nanoseconds(operation.intended_ns));
                const auto phase = request_id < options.warmup_requests ? 0U : 1U;
                if (phase == 1)
                    generator_lag.record(elapsed(start) - operation.intended_ns);
                auto &lane = *lanes[operation.key_index % lanes.size()];
                const std::lock_guard guard(lane.mutex);
                if (lane.queue.size() >= options.queue_capacity)
                {
                    ++rejected[phase];
                    rejected_file << Json{{"request_id", request_id}, {"intended_ns", operation.intended_ns}, {"phase", phase == 0 ? "warmup" : "measurement"}, {"complete_ns", elapsed(start)}, {"status", "queue_full"}}.dump() << '\n';
                }
                else
                {
                    lane.queue.push_back(operation);
                    lane.changed.notify_one();
                }
            }
            sync_stream(rejected_file, output / "rejected.jsonl");
        }
        catch (...)
        {
            close_threads();
            throw;
        }
        close_threads();
        const auto finished_ns = elapsed(start);
        LatencyHistogram reads, writes, queue_delay, rpc_latency;
        std::array<Counts, 2> counts;
        std::ofstream versions(output / "final-versions.jsonl", std::ios::binary);
        for (const auto &lane : lanes)
        {
            if (lane->exception)
                std::rethrow_exception(lane->exception);
            for (std::size_t phase = 0; phase < counts.size(); ++phase)
            {
                counts[phase].completed += lane->counts[phase].completed;
                counts[phase].failed += lane->counts[phase].failed;
                counts[phase].expired += lane->counts[phase].expired;
                counts[phase].blocked += lane->counts[phase].blocked;
                counts[phase].indeterminate += lane->counts[phase].indeterminate;
                counts[phase].mismatched += lane->counts[phase].mismatched;
                counts[phase].in_window += lane->counts[phase].in_window;
            }
            reads.merge(lane->reads);
            writes.merge(lane->writes);
            queue_delay.merge(lane->queue_delay);
            rpc_latency.merge(lane->rpc_latency);
            for (const auto &[index, generation] : lane->versions)
            {
                versions << Json{{"key_index", index}, {"generation", generation}}.dump() << '\n';
            }
        }
        sync_stream(versions, output / "final-versions.jsonl");
        wire::MaintenanceRequest drain;
        drain.mutable_identity()->set_wire_version(wire_version);
        drain.mutable_identity()->set_run_id(options.run_id);
        drain.set_timeout_seconds(60);
        drain.set_verify_checksums(true);
        grpc::ClientContext drain_context;
        drain_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(70));
        wire::MaintenanceResponse drained;
        const auto drain_start = Clock::now();
        const auto drain_status = control->Maintain(&drain_context, drain, &drained);
        const auto drain_ns = elapsed(drain_start);
        const auto stats = drain_status.ok() ? Json::parse(drained.statistics_json()) : Json(nullptr);
        const bool healthy = drain_status.ok() && drained.checksums_verified() &&
                             (stats.at("remote").is_null() || stats.at("remote").at("healthy") == true);
        const auto warmup = count_json(counts[0], options.warmup_requests, rejected[0]);
        const auto measured = count_json(counts[1], options.requests, rejected[1]);
        Json result{{"schema_version", 1}, {"state", "complete"}, {"run_id", options.run_id}, {"valid", healthy && sample_errors == 0 && counts[0].completed == options.warmup_requests && counts[1].completed == options.requests}, {"warmup", warmup}, {"measurement", measured}, {"warmup_ns", warmup_ns}, {"offered_window_ns", end_ns - warmup_ns}, {"load_and_queue_drain_ns", finished_ns}, {"offered_rate", options.rate}, {"read_percent", options.read_percent}, {"mode", description.compaction_backend()}, {"schedule_sha256", fingerprint}, {"read_latency", reads.summary()}, {"write_latency", writes.summary()}, {"queue_delay", queue_delay.summary()}, {"rpc_latency", rpc_latency.summary()}, {"generator_lag", generator_lag.summary()}, {"database_statistics", stats}, {"metrics_errors", sample_errors.load()}, {"drain_status", drain_status.error_message()}, {"checksums_verified", drained.checksums_verified()}, {"database_drain_ns", drain_ns}, {"load_process", process_usage()}, {"offline_verification_required", true}, {"latency_scope", "intended arrival to response, including generator and client queue delay"}};
        durable_bytes(output / "read.hdr", reads.encode() + "\n");
        durable_bytes(output / "write.hdr", writes.encode() + "\n");
        durable_bytes(output / "queue.hdr", queue_delay.encode() + "\n");
        durable_bytes(output / "rpc.hdr", rpc_latency.encode() + "\n");
        durable_bytes(output / "generator.hdr", generator_lag.encode() + "\n");
        durable_json(output / "result.json", result);
        manifest["state"] = "complete";
        durable_json(output / "manifest.json", manifest);
        return result;
    }
}