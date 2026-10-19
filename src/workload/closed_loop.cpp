#include "lsm_lab/load.hpp"
#include "lsm_lab/telemetry.hpp"
#include "benchmark.grpc.pb.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

namespace lsm_lab
{
    namespace wire = rpc::v1;
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    namespace
    {
        struct PhaseCounts
        {
            std::uint64_t issued = 0, completed = 0, failed = 0, blocked = 0, indeterminate = 0, mismatched = 0;
            std::uint64_t in_window = 0, reads_in_window = 0, writes_in_window = 0, inserts_in_window = 0;
        };

        struct Second
        {
            std::uint64_t reads = 0, writes = 0, inserts = 0, failed = 0;
        };

        struct Client
        {
            LatencyHistogram reads, writes;
            std::map<std::uint64_t, std::uint64_t> versions;
            std::set<std::uint64_t> uncertain;
            std::array<PhaseCounts, 2> counts;
            std::vector<Second> timeline;
            std::uint64_t highest_request_id = 0;
            std::exception_ptr exception;
        };

        std::int64_t since(Clock::time_point start)
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

        Json phase_json(const PhaseCounts &counts)
        {
            require(counts.completed + counts.failed + counts.blocked == counts.issued, "closed-loop request accounting mismatch");
            return {{"offered", counts.issued}, {"completed", counts.completed}, {"failed", counts.failed},
                    {"expired_before_send", 0}, {"blocked_by_indeterminate_write", counts.blocked}, {"queue_rejected", 0},
                    {"indeterminate_writes", counts.indeterminate}, {"payload_mismatches", counts.mismatched},
                    {"completed_in_window", counts.in_window}};
        }

        enum class Exhausted : int
        {
            none = 0,
            write_budget = 1,
            memory_budget = 2
        };
    }

    Json run_closed_loop(const LoadOptions &options)
    {
        const auto clients = options.closed_loop_clients;
        require(valid_id(options.run_id) && clients >= 1 && clients <= 1024 && options.duration_seconds >= 1 &&
                    options.duration_seconds <= 86400 && options.warmup_duration_seconds <= 86400 &&
                    options.read_percent <= 100 && options.insert_percent <= 100 && options.deadline_ms >= 1 &&
                    options.deadline_ms <= 60000,
                "invalid closed-loop load parameters");
        const auto warmup_ns = static_cast<std::int64_t>(options.warmup_duration_seconds * 1'000'000'000ULL);
        const auto end_ns = warmup_ns + static_cast<std::int64_t>(options.duration_seconds * 1'000'000'000ULL);
        const auto seconds = options.warmup_duration_seconds + options.duration_seconds;
        const auto resource_budget = std::min<std::uint64_t>(256ULL << 20, (seconds + 80) * 16384);
        const auto timeline_budget = (seconds + 2 + options.deadline_ms / 1000) * 256 + (1ULL << 20);
        require(resource_budget + timeline_budget <= options.artifact_budget_bytes, "closed-loop artifacts exceed declared disk budget");
        require(!fs::exists(options.output), "load output already exists");
        const auto parent = fs::canonical(fs::absolute(options.output).parent_path());
        const auto available = fs::space(parent).available;
        require(available >= (256ULL << 20) && resource_budget + timeline_budget <= available - (256ULL << 20), "insufficient artifact disk space");
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
        require(spec.dataset.records >= clients, "closed-loop clients exceed dataset records");
        const auto fixed_memory = static_cast<std::uint64_t>(clients) * (2ULL << 20);
        require(options.memory_budget_bytes > fixed_memory + 192 * clients, "closed-loop clients exceed declared memory budget");
        // Expected-version entries are bounded at run time; exhausting the budget ends the run as invalid.
        const auto versions_per_client = (options.memory_budget_bytes - fixed_memory) / 192 / clients;
        const auto row_bytes = static_cast<std::uint64_t>(spec.dataset.key_bytes) + spec.dataset.value_bytes;
        const auto output = parent / fs::absolute(options.output).filename();
        require(fs::create_directory(output), "cannot reserve load output");
        sync_directory(parent);
        telemetry::Sampler resources({.output = output / "resources.jsonl", .role = "load", .max_output_bytes = resource_budget});
        Json manifest{{"schema_version", 1}, {"owner", "lsm-lab"}, {"state", "incomplete"}, {"run_id", options.run_id}, {"dataset_id", description.dataset_id()}, {"engine_fingerprint", engine_fingerprint()}, {"spec", spec_json(spec)}, {"mode", description.compaction_backend()}, {"load_mode", "closed_loop"}, {"seed", options.seed}, {"clients", clients}, {"read_percent", options.read_percent}, {"insert_percent", options.insert_percent}, {"duration_seconds", options.duration_seconds}, {"warmup_duration_seconds", options.warmup_duration_seconds}, {"deadline_ms", options.deadline_ms}, {"write_budget_bytes", options.write_budget_bytes}, {"memory_budget_bytes", options.memory_budget_bytes}, {"operation_generator", "splitmix64-closed-loop-v1"}, {"value_generator", "splitmix64-record-v1"}};
        durable_json(output / "manifest.json", manifest);
        std::vector<std::unique_ptr<Client>> states;
        for (std::uint32_t index = 0; index < clients; ++index)
            states.push_back(std::make_unique<Client>());
        const auto start = Clock::now() + std::chrono::milliseconds(250);
        const auto start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count();
        const Json resource_window{{"host", telemetry::host_identity()}, {"load_start_ns", start_ns}, {"measurement_start_ns", start_ns + warmup_ns}, {"measurement_end_ns", start_ns + end_ns}, {"clock", "std::chrono::steady_clock_host_local"}};
        manifest["resource_window"] = resource_window;
        manifest["resource_budget_bytes"] = resource_budget;
        durable_json(output / "manifest.json", manifest);
        std::atomic<bool> stop{false};
        std::atomic<int> exhausted{static_cast<int>(Exhausted::none)};
        std::atomic<std::uint64_t> written_bytes{0};
        std::vector<std::thread> threads;
        std::thread sampler;
        std::atomic<bool> stop_sampling{false};
        std::mutex sample_mutex;
        std::condition_variable sample_changed;
        std::atomic<std::uint64_t> sample_errors{0};
        const auto close_threads = [&]
        {
            stop.store(true);
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
                        const auto sample_start = since(start);
                        const auto status = control->Statistics(&context, describe, &response);
                        const auto sample_end = since(start);
                        sample_errors += !status.ok();
                        samples << Json{{"load_sample_start_ns", sample_start}, {"load_elapsed_ns", sample_end}, {"status", status.ok() ? "ok" : "failed"},
                                        {"statistics", status.ok() ? Json::parse(response.statistics_json()) : Json(nullptr)}}.dump() << '\n';
                        std::unique_lock lock(sample_mutex);
                        sample_changed.wait_for(lock, std::chrono::seconds(1), [&] { return stop_sampling.load(); });
                    }
                    sync_stream(samples, output / "primary-metrics.jsonl");
                }
                catch (...) { ++sample_errors; } });
            for (std::uint32_t index = 0; index < clients; ++index)
            {
                threads.emplace_back([&, index]
                                     {
                    auto &client = *states[index];
                    try
                    {
                        auto stub = wire::BenchmarkService::NewStub(channel);
                        std::this_thread::sleep_until(start);
                        for (std::uint64_t sequence = 0; !stop.load(std::memory_order_relaxed); ++sequence)
                        {
                            const auto issued = since(start);
                            if (issued >= end_ns)
                                break;
                            const auto operation = closed_loop_operation(spec.dataset, options, index, sequence);
                            const auto phase = issued < warmup_ns ? 0U : 1U;
                            auto &counts = client.counts[phase];
                            ++counts.issued;
                            client.highest_request_id = operation.request_id;
                            if (client.uncertain.contains(operation.key_index))
                            {
                                ++counts.blocked;
                                continue;
                            }
                            if (operation.write)
                            {
                                if (!client.versions.contains(operation.key_index) && client.versions.size() >= versions_per_client)
                                {
                                    --counts.issued;
                                    exhausted.store(static_cast<int>(Exhausted::memory_budget));
                                    stop.store(true);
                                    break;
                                }
                                if (written_bytes.fetch_add(row_bytes) + row_bytes > options.write_budget_bytes)
                                {
                                    --counts.issued;
                                    exhausted.store(static_cast<int>(Exhausted::write_budget));
                                    stop.store(true);
                                    break;
                                }
                            }
                            const auto key = generated_key(spec.dataset, operation.key_index);
                            grpc::ClientContext context;
                            context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(options.deadline_ms));
                            const auto sent = since(start);
                            grpc::Status status;
                            bool verified = true;
                            if (operation.write)
                            {
                                wire::PutRequest request;
                                request.mutable_identity()->set_wire_version(wire_version);
                                request.mutable_identity()->set_run_id(options.run_id);
                                request.mutable_identity()->set_request_id(operation.request_id);
                                request.set_key(key);
                                request.set_value(generated_value(spec.dataset, operation.key_index, operation.generation));
                                wire::WriteResponse response;
                                status = stub->Put(&context, request, &response);
                                verified = !status.ok() || response.request_id() == operation.request_id;
                                if (status.ok())
                                    client.versions[operation.key_index] = operation.generation;
                                else
                                {
                                    ++counts.indeterminate;
                                    client.uncertain.insert(operation.key_index);
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
                                status = stub->Get(&context, request, &response);
                                const auto previous = client.versions.find(operation.key_index);
                                const auto generation = previous == client.versions.end() ? spec.dataset.overwrite_rounds : previous->second;
                                verified = !status.ok() || (response.request_id() == operation.request_id && response.found() &&
                                                            response.value() == generated_value(spec.dataset, operation.key_index, generation));
                            }
                            const auto completed = since(start);
                            const auto second = static_cast<std::size_t>(std::max<std::int64_t>(0, completed) / 1'000'000'000LL);
                            if (client.timeline.size() <= second)
                                client.timeline.resize(second + 1);
                            if (status.ok() && verified)
                            {
                                ++counts.completed;
                                (operation.write ? client.timeline[second].writes : client.timeline[second].reads) += 1;
                                client.timeline[second].inserts += operation.insert;
                                if (phase == 1)
                                {
                                    (operation.write ? client.writes : client.reads).record(static_cast<std::uint64_t>(completed - sent));
                                    if (completed < end_ns)
                                    {
                                        ++counts.in_window;
                                        (operation.write ? counts.writes_in_window : counts.reads_in_window) += 1;
                                        counts.inserts_in_window += operation.insert;
                                    }
                                }
                                else
                                    counts.in_window += completed < warmup_ns;
                            }
                            else
                            {
                                ++counts.failed;
                                ++client.timeline[second].failed;
                                counts.mismatched += status.ok() && !verified;
                            }
                        }
                    }
                    catch (...)
                    {
                        client.exception = std::current_exception();
                        stop.store(true);
                    } });
            }
            for (auto &thread : threads)
                thread.join();
        }
        catch (...)
        {
            close_threads();
            throw;
        }
        close_threads();
        const auto finished_ns = since(start);
        LatencyHistogram reads, writes;
        std::array<PhaseCounts, 2> counts;
        std::vector<Second> timeline;
        std::uint64_t inserted_keys = 0;
        std::uint64_t key_index_limit = spec.dataset.records;
        std::ofstream versions(output / "final-versions.jsonl", std::ios::binary);
        for (const auto &client : states)
        {
            if (client->exception)
                std::rethrow_exception(client->exception);
            for (std::size_t phase = 0; phase < counts.size(); ++phase)
            {
                const auto &source = client->counts[phase];
                auto &target = counts[phase];
                target.issued += source.issued;
                target.completed += source.completed;
                target.failed += source.failed;
                target.blocked += source.blocked;
                target.indeterminate += source.indeterminate;
                target.mismatched += source.mismatched;
                target.in_window += source.in_window;
                target.reads_in_window += source.reads_in_window;
                target.writes_in_window += source.writes_in_window;
                target.inserts_in_window += source.inserts_in_window;
            }
            reads.merge(client->reads);
            writes.merge(client->writes);
            if (timeline.size() < client->timeline.size())
                timeline.resize(client->timeline.size());
            for (std::size_t second = 0; second < client->timeline.size(); ++second)
            {
                timeline[second].reads += client->timeline[second].reads;
                timeline[second].writes += client->timeline[second].writes;
                timeline[second].inserts += client->timeline[second].inserts;
                timeline[second].failed += client->timeline[second].failed;
            }
            for (const auto &[index, generation] : client->versions)
            {
                versions << Json{{"key_index", index}, {"generation", generation}}.dump() << '\n';
                if (index >= spec.dataset.records)
                {
                    ++inserted_keys;
                    key_index_limit = std::max(key_index_limit, index + 1);
                }
            }
            if (options.insert_percent != 0)
                key_index_limit = std::max(key_index_limit, spec.dataset.records + client->highest_request_id + 1);
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
        const auto drain_ns = since(drain_start);
        const auto stats = drain_status.ok() ? Json::parse(drained.statistics_json()) : Json(nullptr);
        const bool healthy = drain_status.ok() && drained.checksums_verified() &&
                             (stats.at("remote").is_null() || stats.at("remote").at("healthy") == true);
        const auto reason = static_cast<Exhausted>(exhausted.load());
        const auto window_seconds = static_cast<double>(options.duration_seconds);
        const auto &measured = counts[1];
        Json throughput{{"window_ns", end_ns - warmup_ns},
                        {"ops_per_second", measured.in_window / window_seconds},
                        {"reads_per_second", measured.reads_in_window / window_seconds},
                        {"writes_per_second", measured.writes_in_window / window_seconds},
                        {"inserts_per_second", measured.inserts_in_window / window_seconds},
                        {"completed_in_window", measured.in_window},
                        {"basis", "measurement-phase operations issued and completed inside [warmup, warmup + duration)"}};
        Json seconds_json = Json::array();
        for (std::size_t second = 0; second < timeline.size(); ++second)
        {
            const auto &entry = timeline[second];
            const auto offset = static_cast<std::int64_t>(second) * 1'000'000'000LL;
            seconds_json.push_back({{"second", second}, {"phase", offset < warmup_ns ? "warmup" : offset < end_ns ? "measurement" : "drain"},
                                    {"ops", entry.reads + entry.writes}, {"reads", entry.reads}, {"writes", entry.writes},
                                    {"inserts", entry.inserts}, {"failed", entry.failed}});
        }
        const bool clean = counts[0].failed == 0 && counts[1].failed == 0 && counts[0].blocked == 0 && counts[1].blocked == 0;
        Json result{{"schema_version", 1}, {"state", "complete"}, {"run_id", options.run_id}, {"load_mode", "closed_loop"},
                    {"valid", healthy && sample_errors == 0 && clean && reason == Exhausted::none && measured.completed > 0},
                    {"clients", clients}, {"warmup", phase_json(counts[0])}, {"measurement", phase_json(measured)},
                    {"warmup_ns", warmup_ns}, {"offered_window_ns", end_ns - warmup_ns}, {"load_and_queue_drain_ns", finished_ns},
                    {"offered_rate", nullptr}, {"read_percent", options.read_percent}, {"insert_percent", options.insert_percent},
                    {"mode", description.compaction_backend()}, {"throughput", throughput}, {"timeline", seconds_json},
                    {"read_latency", reads.summary(true)}, {"write_latency", writes.summary(true)},
                    {"inserted_keys", inserted_keys}, {"logical_write_bytes", std::min(written_bytes.load(), options.write_budget_bytes)},
                    {"budget_exhausted", reason == Exhausted::none ? Json(nullptr) : Json(reason == Exhausted::write_budget ? "write_budget" : "memory_budget")},
                    {"database_statistics", stats}, {"metrics_errors", sample_errors.load()}, {"drain_status", drain_status.error_message()},
                    {"checksums_verified", drained.checksums_verified()}, {"database_drain_ns", drain_ns}, {"load_process", process_usage()},
                    {"offline_verification_required", true}, {"latency_scope", "client send to response; closed loop without an arrival schedule"}};
        durable_bytes(output / "read.hdr", reads.encode() + "\n");
        durable_bytes(output / "write.hdr", writes.encode() + "\n");
        result["resource_window"] = resource_window;
        result["resources"] = resources.stop();
        result["valid"] = result.at("valid").get<bool>() && result.at("resources").at("status") == "complete";
        durable_json(output / "result.json", result);
        manifest["state"] = "complete";
        manifest["key_index_limit"] = key_index_limit;
        durable_json(output / "manifest.json", manifest);
        return result;
    }
}
