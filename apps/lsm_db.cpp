#include "lsm_lab/block_transform.hpp"
#include "lsm_lab/dataset.hpp"
#include "lsm_lab/remote_compaction.hpp"
#include "lsm_lab/telemetry.hpp"
#include "benchmark.grpc.pb.h"

#include <algorithm>
#include <iostream>
#include <charconv>
#include <shared_mutex>
#include <vector>
#include <rocksdb/db.h>
#include <rocksdb/listener.h>

namespace
{
    namespace wire = lsm_lab::rpc::v1;
    namespace fs = std::filesystem;
    using Json = nlohmann::json;

    class CompactionMetrics final : public rocksdb::EventListener
    {
    public:
        const char *Name() const override { return "lsm_lab_compaction_metrics_v1"; }

        void OnCompactionCompleted(rocksdb::DB *, const rocksdb::CompactionJobInfo &info) override
        {
            const std::lock_guard guard(mutex_);
            if (!info.status.ok())
            {
                ++failed_jobs_;
                return;
            }
            elapsed_micros_.push_back(info.stats.elapsed_micros);
            if (jobs_.size() < 4096)
            {
                jobs_.push_back({
                    {"elapsed_micros", info.stats.elapsed_micros},
                    {"cpu_micros", info.stats.cpu_micros},
                    {"input_records", info.stats.num_input_records},
                    {"output_records", info.stats.num_output_records},
                    {"input_files", info.stats.num_input_files},
                    {"output_files", info.stats.num_output_files},
                    {"input_bytes", info.stats.total_input_bytes},
                    {"output_bytes", info.stats.total_output_bytes},
                    {"remote", info.stats.is_remote_compaction}});
            }
            else
            {
                ++dropped_job_samples_;
            }
            cpu_micros_ += info.stats.cpu_micros;
            input_records_ += info.stats.num_input_records;
            output_records_ += info.stats.num_output_records;
            input_files_ += info.stats.num_input_files;
            output_files_ += info.stats.num_output_files;
            input_bytes_ += info.stats.total_input_bytes;
            output_bytes_ += info.stats.total_output_bytes;
            remote_jobs_ += info.stats.is_remote_compaction;
        }

        Json summary() const
        {
            const std::lock_guard guard(mutex_);
            auto elapsed = elapsed_micros_;
            std::sort(elapsed.begin(), elapsed.end());
            const auto percentile = [&](std::size_t value)
            {
                if (elapsed.empty())
                    return std::uint64_t{0};
                const auto index = std::min(elapsed.size() - 1, (elapsed.size() * value + 99) / 100 - 1);
                return elapsed[index];
            };
            std::uint64_t total = 0;
            for (const auto value : elapsed)
                total += value;
            return {{"successful_jobs", elapsed.size()}, {"failed_jobs", failed_jobs_},
                    {"remote_jobs", remote_jobs_}, {"elapsed_micros_total", total},
                    {"elapsed_micros_p50", percentile(50)}, {"elapsed_micros_p95", percentile(95)},
                    {"elapsed_micros_p99", percentile(99)}, {"elapsed_micros_max", percentile(100)},
                    {"cpu_micros_total", cpu_micros_}, {"input_records", input_records_},
                    {"output_records", output_records_}, {"input_files", input_files_},
                    {"output_files", output_files_}, {"input_bytes", input_bytes_},
                    {"output_bytes", output_bytes_}, {"jobs", jobs_},
                    {"dropped_job_samples", dropped_job_samples_}};
        }

    private:
        mutable std::mutex mutex_;
        std::vector<std::uint64_t> elapsed_micros_;
        std::vector<Json> jobs_;
        std::uint64_t dropped_job_samples_ = 0;
        std::uint64_t failed_jobs_ = 0;
        std::uint64_t remote_jobs_ = 0;
        std::uint64_t cpu_micros_ = 0;
        std::uint64_t input_records_ = 0;
        std::uint64_t output_records_ = 0;
        std::uint64_t input_files_ = 0;
        std::uint64_t output_files_ = 0;
        std::uint64_t input_bytes_ = 0;
        std::uint64_t output_bytes_ = 0;
    };

    class DatabaseService final : public wire::BenchmarkService::Service
    {
    public:
        DatabaseService(fs::path dataset, std::string run_id, const std::string &mode,
                        const lsm_lab::RemoteOptions &remote_options, std::shared_ptr<lsm_lab::EventLog> events)
            : root_(fs::canonical(dataset)), run_id_(std::move(run_id)), spec_(lsm_lab::dataset_spec(root_)),
              mode_(mode), events_(std::move(events))
        {
            lsm_lab::require(lsm_lab::valid_id(run_id_), "invalid run identity");
            const auto manifest = Json::parse(lsm_lab::read_file(root_ / "dataset.json"));
            lsm_lab::require(!manifest.at("parent_id").get<std::string>().empty(), "server requires a trial clone, not a baseline");
            dataset_id_ = manifest.at("dataset_id").get<std::string>();
            const auto spec_hash = lsm_lab::digest_hex(lsm_lab::spec_json(spec_).dump());
            if (fs::exists(root_ / "run.json"))
            {
                const auto prior = Json::parse(lsm_lab::read_file(root_ / "run.json"));
                lsm_lab::require(prior.at("owner") == "lsm-lab" && prior.at("run_id") == run_id_ &&
                                     prior.at("dataset_id") == dataset_id_ && prior.at("spec_sha256") == spec_hash &&
                                     prior.at("engine_fingerprint") == lsm_lab::engine_fingerprint() && prior.at("mode") == mode_,
                                 "trial restart identity mismatch");
            }
            else
            {
                lsm_lab::verify_dataset(root_);
            }
            auto options = lsm_lab::engine_options(spec_);
            if (mode_ == "remote_worker")
            {
                auto resolved = remote_options;
                resolved.database = root_ / "db";
                resolved.run_id = run_id_;
                resolved.spec_sha256 = spec_hash;
                remote_ = std::make_shared<lsm_lab::RemoteCompaction>(std::move(resolved), events_);
                options.compaction_service = remote_;
                options.listeners.push_back(remote_);
            }
            else if (mode_ == "cuda_local")
            {
                local_transform_ = lsm_lab::make_transform("cuda_hybrid");
                options = lsm_lab::engine_options(spec_, local_transform_);
            }
            else
            {
                lsm_lab::require(mode_ == "native_cpu", "mode must be native_cpu, cuda_local, or remote_worker");
            }
            compactions_ = std::make_shared<CompactionMetrics>();
            options.listeners.push_back(compactions_);
            lsm_lab::check(rocksdb::DB::Open(options, (root_ / "db").string(), &database_));
            lsm_lab::check(database_->GetDbSessionId(session_));
            run_record_ = {{"schema_version", 1}, {"owner", "lsm-lab"}, {"run_id", run_id_}, {"dataset_id", dataset_id_}, {"spec_sha256", spec_hash}, {"engine_fingerprint", lsm_lab::engine_fingerprint()}, {"db_session_id", session_}, {"state", "active"}, {"mode", mode_}};
            lsm_lab::durable_json(root_ / "run.json", run_record_);
            events_->append({{"role", "primary"}, {"phase", "opened"}, {"run_id", run_id_}, {"db_session_id", session_}});
        }

        grpc::Status Describe(grpc::ServerContext *context, const wire::DescribeRequest *request,
                              wire::DescribeResponse *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     {
                lsm_lab::require(request->wire_version() == lsm_lab::wire_version, "incompatible wire version");
                response->set_wire_version(lsm_lab::wire_version);
                response->set_run_id(run_id_);
                response->set_dataset_id(dataset_id_);
                response->set_db_session_id(session_);
                response->set_engine_fingerprint(lsm_lab::engine_fingerprint());
                response->set_resolved_spec_sha256(lsm_lab::digest_hex(lsm_lab::spec_json(spec_).dump()));
                response->set_maximum_key_bytes(spec_.dataset.key_bytes);
                response->set_maximum_value_bytes(spec_.dataset.value_bytes);
                response->set_compaction_backend(mode_);
                response->set_sync_writes(true);
                response->set_dataset_spec_json(lsm_lab::spec_json(spec_).dump()); });
        }

        grpc::Status Get(grpc::ServerContext *context, const wire::GetRequest *request, wire::GetResponse *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     {
                validate(request->identity(), request->key());
                const std::shared_lock guard(operations_);
                lsm_lab::require(!context->IsCancelled(), "read cancelled before execution");
                std::string value;
                const auto status = database_->Get(rocksdb::ReadOptions(), request->key(), &value);
                if (!status.IsNotFound())
                {
                    lsm_lab::check(status);
                }
                response->set_request_id(request->identity().request_id());
                response->set_found(status.ok());
                response->set_value(std::move(value));
                ++reads_; });
        }

        grpc::Status Put(grpc::ServerContext *context, const wire::PutRequest *request, wire::WriteResponse *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     {
                validate(request->identity(), request->key());
                lsm_lab::require(request->value().size() <= spec_.dataset.value_bytes, "value exceeds configured maximum");
                const std::shared_lock guard(operations_);
                lsm_lab::require(!context->IsCancelled(), "write cancelled before execution");
                rocksdb::WriteOptions options;
                options.sync = true;
                lsm_lab::check(database_->Put(options, request->key(), request->value()));
                response->set_request_id(request->identity().request_id());
                ++writes_; });
        }

        grpc::Status Delete(grpc::ServerContext *context, const wire::DeleteRequest *request, wire::WriteResponse *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     {
                validate(request->identity(), request->key());
                const std::shared_lock guard(operations_);
                lsm_lab::require(!context->IsCancelled(), "delete cancelled before execution");
                rocksdb::WriteOptions options;
                options.sync = true;
                lsm_lab::check(database_->Delete(options, request->key()));
                response->set_request_id(request->identity().request_id());
                ++deletes_; });
        }

        grpc::Status Maintain(grpc::ServerContext *context, const wire::MaintenanceRequest *request,
                              wire::MaintenanceResponse *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     {
                identity(request->identity());
                lsm_lab::require(request->timeout_seconds() >= 1 && request->timeout_seconds() <= 3600,
                                "maintenance timeout must be 1 to 3600 seconds");
                const std::unique_lock guard(operations_);
                lsm_lab::check(database_->Flush(rocksdb::FlushOptions()));
                if (request->compact())
                {
                    rocksdb::CompactRangeOptions compact;
                    compact.bottommost_level_compaction = rocksdb::BottommostLevelCompaction::kForce;
                    lsm_lab::check(database_->CompactRange(compact, nullptr, nullptr));
                }
                rocksdb::WaitForCompactOptions drain;
                drain.wait_for_purge = true;
                drain.timeout = std::chrono::seconds(request->timeout_seconds());
                lsm_lab::check(database_->WaitForCompact(drain));
                if (request->verify_checksums())
                {
                    lsm_lab::check(database_->VerifyChecksum());
                }
                response->set_latest_sequence(database_->GetLatestSequenceNumber());
                response->set_checksums_verified(request->verify_checksums());
                response->set_statistics_json(statistics().dump()); });
        }

        grpc::Status Statistics(grpc::ServerContext *context, const wire::DescribeRequest *request,
                                wire::StatisticsResponse *response) override
        {
            return lsm_lab::rpc_call(*context, [&]
                                     {
                lsm_lab::require(request->wire_version() == lsm_lab::wire_version, "incompatible wire version");
                const std::shared_lock guard(operations_);
                response->set_statistics_json(statistics().dump()); });
        }

        Json close()
        {
            const std::unique_lock guard(operations_);
            lsm_lab::check(database_->Flush(rocksdb::FlushOptions()));
            rocksdb::WaitForCompactOptions drain;
            drain.timeout = std::chrono::seconds(60);
            drain.wait_for_purge = true;
            lsm_lab::check(database_->WaitForCompact(drain));
            lsm_lab::check(database_->VerifyChecksum());
            auto result = statistics();
            lsm_lab::check(database_->Close());
            database_.reset();
            run_record_["state"] = "stopped";
            run_record_["final_statistics"] = result;
            lsm_lab::durable_json(root_ / "run.json", run_record_);
            events_->append({{"role", "primary"}, {"phase", "closed"}, {"run_id", run_id_}, {"statistics", result}});
            return result;
        }

    private:
        void identity(const wire::RequestIdentity &request) const
        {
            lsm_lab::require(request.wire_version() == lsm_lab::wire_version && request.run_id() == run_id_,
                             "incompatible wire version or run identity");
        }

        void validate(const wire::RequestIdentity &request, const std::string &key) const
        {
            identity(request);
            lsm_lab::require(!key.empty() && key.size() <= spec_.dataset.key_bytes, "invalid key size");
        }

        Json statistics() const
        {
            Json properties = Json::object();
            for (const auto *name : {"rocksdb.estimate-pending-compaction-bytes", "rocksdb.num-files-at-level0",
                                     "rocksdb.cur-size-all-mem-tables", "rocksdb.live-sst-files-size"})
            {
                std::uint64_t value = 0;
                if (database_->GetIntProperty(name, &value))
                {
                    properties[name] = value;
                }
                else
                {
                    std::string text;
                    if (database_->GetProperty(name, &text))
                    {
                        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                        properties[name] = parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? Json(value) : Json(nullptr);
                    }
                    else
                        properties[name] = nullptr;
                }
            }
            return {{"schema_version", 1}, {"run_id", run_id_}, {"db_session_id", session_}, {"reads", reads_.load()}, {"writes", writes_.load()}, {"deletes", deletes_.load()}, {"mode", mode_}, {"properties", properties}, {"process", lsm_lab::process_usage()}, {"compactions", compactions_->summary()}, {"remote", remote_ ? remote_->summary() : Json(nullptr)}, {"local_transform", local_transform_ ? local_transform_->statistics() : Json(nullptr)}};
        }

        fs::path root_;
        std::string run_id_;
        lsm_lab::Spec spec_;
        std::string mode_;
        std::string dataset_id_;
        std::string session_;
        std::shared_ptr<lsm_lab::EventLog> events_;
        std::shared_ptr<lsm_lab::RemoteCompaction> remote_;
        std::shared_ptr<lsm_lab::BatchTransform> local_transform_;
        std::shared_ptr<CompactionMetrics> compactions_;
        std::unique_ptr<rocksdb::DB> database_;
        Json run_record_;
        std::shared_mutex operations_;
        std::atomic<std::uint64_t> reads_{0}, writes_{0}, deletes_{0};
    };
}

int main(int argc, char **argv)
{
    try
    {
        lsm_lab::Arguments arguments(argc, argv);
        const auto dataset = arguments.required("dataset");
        const auto run_id = arguments.required("run-id");
        const auto mode = arguments.take("mode", "native_cpu");
        const auto endpoint = arguments.take("listen", "127.0.0.1:50051");
        const auto ready = arguments.required("ready-file");
        const auto events_path = arguments.required("events");
        const auto summary_path = arguments.required("summary");
        lsm_lab::telemetry::SamplerOptions resources;
        resources.output = arguments.take("resources", summary_path + ".resources.jsonl");
        resources.role = "primary";
        resources.interval_ms = arguments.number("resource-interval-ms", 1000, 10, 60000);
        resources.max_output_bytes = arguments.number("resource-budget-bytes", 256ULL << 20, 16384, 16ULL << 30);
        lsm_lab::require(!fs::exists(summary_path), "summary file already exists");
        const auto tls = lsm_lab::TlsOptions::take(arguments);
        lsm_lab::RemoteOptions remote;
        remote.endpoint = arguments.take("worker");
        remote.shared_root = arguments.take("shared-root");
        remote.backend = arguments.take("backend", "cpu_native");
        remote.job_timeout = std::chrono::seconds(arguments.number("job-timeout-seconds", 300, 1, 3600));
        remote.tls = lsm_lab::TlsOptions::take(arguments, "worker-");
        arguments.finish();
        lsm_lab::require(mode != "native_cpu" || (remote.endpoint.empty() && remote.backend == "cpu_native"),
                         "native mode cannot select a remote worker or GPU backend");
        lsm_lab::require(mode == "native_cpu" || mode == "remote_worker" || mode == "cuda_local",
                         "mode must be native_cpu, cuda_local, or remote_worker");
        lsm_lab::block_shutdown_signals();
        lsm_lab::telemetry::Sampler sampler(resources);
        auto events = std::make_shared<lsm_lab::EventLog>(events_path);
        DatabaseService service(dataset, run_id, mode, remote, events);
        auto server = lsm_lab::start_server(endpoint, tls, service, ready);
        lsm_lab::wait_for_shutdown();
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(10));
        server->Wait();
        auto summary = service.close();
        summary["resources"] = sampler.stop();
        summary["resource_file"] = resources.output.string();
        lsm_lab::durable_json(summary_path, summary);
        lsm_lab::durable_json(ready, {{"schema_version", 1}, {"state", "stopped"}});
        lsm_lab::require(summary.at("resources").at("status") == "complete", "primary resource accounting incomplete");
        lsm_lab::require(summary.at("remote").is_null() || summary.at("remote").at("healthy") == true,
                         "trial contains remote compaction or telemetry failures");
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << Json{{"status", "error"}, {"detail", error.what()}}.dump() << '\n';
        return 1;
    }
}