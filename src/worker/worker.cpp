#include "lsm_lab/worker.hpp"
#include "lsm_lab/dataset.hpp"

#include <algorithm>

#include <rocksdb/db.h>
#include <rocksdb/sst_file_reader.h>
#include <rocksdb/statistics.h>

namespace lsm_lab
{
    namespace fs = std::filesystem;
    namespace wire = rpc::v1;
    namespace
    {
        constexpr std::size_t payload_limit = 4 * 1024 * 1024;
        constexpr std::size_t retained_job_limit = 1024;

        bool terminal(wire::AttemptState state)
        {
            return state == wire::ATTEMPT_OUTPUT_READY || state == wire::ATTEMPT_FAILED ||
                   state == wire::ATTEMPT_CANCELLED || state == wire::ATTEMPT_ABANDONED;
        }

        void validate_identity(const wire::AttemptIdentity &identity)
        {
            require(identity.has_job() && valid_id(identity.attempt_id()) && valid_id(identity.job().run_id()) &&
                        valid_id(identity.job().db_id()) && valid_id(identity.job().db_session_id()),
                    "invalid job identity");
        }
    }

    Worker::Worker(WorkerOptions options) : options_(std::move(options))
    {
        require(options_.threads >= 1 && options_.threads <= 64 && options_.queue_limit >= 1 && options_.queue_limit <= 1024,
                "invalid worker limits");
        options_.shared_root = fs::canonical(options_.shared_root);
        const auto marker = nlohmann::json::parse(read_file(options_.shared_root / ".lsm-shared.json"));
        require(marker.at("owner") == "lsm-lab" && marker.at("schema_version") == 1, "unowned shared root");
        shared_id_ = marker.at("shared_root_id").get<std::string>();
        require(valid_id(shared_id_), "invalid shared root identity");
        require(fs::is_directory(options_.shared_root / ".lsm-staging"), "shared staging directory missing");
        options_.journal = fs::absolute(options_.journal).lexically_normal();
        if (fs::create_directory(options_.journal))
        {
            durable_json(options_.journal / "worker.json", {{"owner", "lsm-lab"}, {"schema_version", 1}, {"shared_root_id", shared_id_}, {"engine_fingerprint", engine_fingerprint()}});
            sync_directory(options_.journal.parent_path());
        }
        const auto journal = nlohmann::json::parse(read_file(options_.journal / "worker.json"));
        require(journal.at("owner") == "lsm-lab" && journal.at("shared_root_id") == shared_id_ &&
                    journal.at("engine_fingerprint") == engine_fingerprint(),
                "worker journal identity mismatch");
        check(rocksdb::Env::Default()->LockFile((options_.journal / "LOCK").string(), &journal_lock_));
        try
        {
            for (const auto &entry : fs::directory_iterator(options_.journal))
            {
                if (!entry.is_directory())
                {
                    continue;
                }
                require(records_.size() < retained_job_limit, "worker journal retention limit exceeded");
                auto record = std::make_shared<Record>();
                record->directory = entry.path();
                const auto state_path = entry.path() / "state.pb";
                if (!fs::exists(state_path))
                {
                    continue;
                }
                require(record->status.ParseFromString(read_file(state_path)), "invalid worker state record");
                validate_identity(record->status.identity());
                require(record->status.identity().attempt_id() == entry.path().filename(), "journal path mismatch");
                record->request_digest = digest_hex(read_file(entry.path() / "request.pb"));
                if (!terminal(record->status.state()) || record->status.state() == wire::ATTEMPT_OUTPUT_READY)
                {
                    record->status.set_state(wire::ATTEMPT_ABANDONED);
                    record->status.set_failure_code("WORKER_RESTART");
                    record->status.set_failure_detail("previous attempt is fenced; submit a new attempt");
                }
                record->status.set_worker_instance_id(instance_);
                persist(*record);
                records_.emplace(record->status.identity().attempt_id(), record);
            }
            for (std::uint32_t index = 0; index < options_.threads; ++index)
            {
                threads_.emplace_back([this]
                                      { run(); });
            }
        }
        catch (...)
        {
            stop();
            rocksdb::Env::Default()->UnlockFile(journal_lock_).PermitUncheckedError();
            journal_lock_ = nullptr;
            throw;
        }
    }

    Worker::~Worker()
    {
        stop();
        if (journal_lock_ != nullptr)
        {
            rocksdb::Env::Default()->UnlockFile(journal_lock_).PermitUncheckedError();
        }
    }

    wire::WorkerCapabilitiesResponse Worker::capabilities() const
    {
        wire::WorkerCapabilitiesResponse result;
        result.set_wire_version(wire_version);
        result.set_engine_fingerprint(engine_fingerprint());
        result.set_worker_instance_id(instance_);
        result.add_backends("cpu_native");
        result.set_maximum_active_jobs(options_.threads);
        result.set_maximum_queued_jobs(options_.queue_limit);
        result.set_maximum_engine_payload_bytes(payload_limit);
        result.set_shared_root(options_.shared_root.string());
        result.set_shared_root_id(shared_id_);
        return result;
    }

    void Worker::persist(Record &record)
    {
        durable_bytes(record.directory / "state.pb", record.status.SerializeAsString());
        record.changed.notify_all();
    }

    wire::CompactionAttempt Worker::response(const Record &record) const
    {
        auto result = record.status;
        if (result.state() == wire::ATTEMPT_OUTPUT_READY)
        {
            result.set_engine_result(read_file(record.directory / "result.bin", payload_limit));
        }
        return result;
    }

    wire::CompactionAttempt Worker::submit(const wire::SubmitCompactionRequest &request)
    {
        require(request.wire_version() == wire_version && request.engine_fingerprint() == engine_fingerprint(),
                "incompatible wire or engine version");
        validate_identity(request.identity());
        require(request.shared_root_id() == shared_id_, "shared root identity mismatch");
        require(request.backend() == "cpu_native", "requested backend is not implemented in this build");
        require(!request.engine_input().empty() && request.engine_input().size() <= payload_limit &&
                    request.engine_input_sha256() == digest_bytes(request.engine_input()),
                "invalid input payload or digest");
        const auto database = contained_path(options_.shared_root, request.database_relative_path());
        require(database.filename() == "db", "worker input must be a dataset db directory");
        const auto spec = dataset_spec(database.parent_path());
        require(request.resolved_spec_sha256() == digest_hex(spec_json(spec).dump()), "configuration fingerprint mismatch");
        const auto serialized = request.SerializeAsString();
        const auto request_digest = digest_hex(serialized);
        const std::lock_guard admission(admission_mutex_);
        {
            const std::lock_guard guard(mutex_);
            require(!stopping_, "worker is stopping");
            const auto found = records_.find(request.identity().attempt_id());
            if (found != records_.end())
            {
                const std::lock_guard record_guard(found->second->mutex);
                require(found->second->request_digest == request_digest, "attempt identity reused with different payload");
                return response(*found->second);
            }
            require(queue_.size() < options_.queue_limit && records_.size() < retained_job_limit,
                    "worker queue or retention budget exhausted");
        }
        auto record = std::make_shared<Record>();
        record->directory = options_.journal / request.identity().attempt_id();
        record->request_digest = request_digest;
        require(fs::create_directory(record->directory), "attempt journal already exists");
        auto &state = record->status;
        state.set_wire_version(wire_version);
        *state.mutable_identity() = request.identity();
        state.set_worker_instance_id(instance_);
        state.set_state(wire::ATTEMPT_ACCEPTED);
        state.set_engine_input_sha256(request.engine_input_sha256());
        state.set_backend(request.backend());
        const auto relative = fs::path(".lsm-staging") / request.identity().attempt_id();
        require(fs::create_directory(options_.shared_root / relative), "attempt output already exists");
        sync_directory(options_.shared_root / ".lsm-staging");
        state.set_output_relative_path(relative.generic_string());
        durable_bytes(record->directory / "request.pb", serialized);
        persist(*record);
        sync_directory(options_.journal);
        state.set_state(wire::ATTEMPT_QUEUED);
        persist(*record);
        {
            const std::lock_guard guard(mutex_);
            records_.emplace(request.identity().attempt_id(), record);
            if (stopping_)
            {
                record->canceled.store(true);
            }
            queue_.push_back(record);
        }
        changed_.notify_one();
        const std::lock_guard guard(record->mutex);
        return response(*record);
    }

    std::shared_ptr<Worker::Record> Worker::find(const wire::AttemptIdentity &identity, const std::string &instance)
    {
        validate_identity(identity);
        require(instance == instance_, "worker instance is stale");
        const std::lock_guard guard(mutex_);
        const auto found = records_.find(identity.attempt_id());
        require(found != records_.end(), "unknown attempt");
        require(found->second->status.identity().SerializeAsString() == identity.SerializeAsString(), "job identity mismatch");
        return found->second;
    }

    wire::CompactionAttempt Worker::inspect(const wire::AttemptRequest &request)
    {
        require(request.wire_version() == wire_version && request.wait_ms() <= 1000, "invalid inspect request");
        const auto record = find(request.identity(), request.worker_instance_id());
        std::unique_lock guard(record->mutex);
        record->changed.wait_for(guard, std::chrono::milliseconds(request.wait_ms()),
                                 [&]
                                 { return terminal(record->status.state()); });
        return response(*record);
    }

    wire::CompactionAttempt Worker::cancel(const wire::AttemptRequest &request)
    {
        require(request.wire_version() == wire_version, "incompatible wire version");
        const auto record = find(request.identity(), request.worker_instance_id());
        const std::lock_guard guard(record->mutex);
        require(record->status.installation_state() != wire::INSTALLATION_COMMITTED, "attempt already committed");
        record->canceled.store(true);
        if (record->status.state() == wire::ATTEMPT_OUTPUT_READY)
        {
            require(record->status.installation_state() != wire::INSTALLATION_OUTPUTS_IMPORTED,
                    "primary must settle imported output before canceling");
            record->status.set_state(wire::ATTEMPT_CANCELLED);
            persist(*record);
        }
        return response(*record);
    }

    wire::CompactionAttempt Worker::acknowledge(const wire::InstallationAcknowledgement &request)
    {
        require(request.wire_version() == wire_version, "incompatible wire version");
        const auto record = find(request.identity(), request.worker_instance_id());
        const std::lock_guard guard(record->mutex);
        require(record->status.state() == wire::ATTEMPT_OUTPUT_READY, "attempt has no installable result");
        require(request.engine_result_sha256() == digest_bytes(read_file(record->directory / "result.bin", payload_limit)),
                "result digest mismatch");
        const auto previous = record->status.installation_state();
        const auto next = request.state();
        const bool first = previous == wire::INSTALLATION_STATE_UNSPECIFIED;
        const bool imported = previous == wire::INSTALLATION_OUTPUTS_IMPORTED;
        require(next == previous ||
                    (first && (next == wire::INSTALLATION_OUTPUTS_IMPORTED || next == wire::INSTALLATION_FAILED ||
                               next == wire::INSTALLATION_LOCAL_FALLBACK)) ||
                    (imported && (next == wire::INSTALLATION_COMMITTED || next == wire::INSTALLATION_FAILED)),
                "invalid installation state transition");
        record->status.set_installation_state(next);
        persist(*record);
        return response(*record);
    }

    void Worker::execute(const std::shared_ptr<Record> &record)
    {
        const auto start = Clock::now();
        try
        {
            {
                const std::lock_guard guard(record->mutex);
                record->status.set_state(record->canceled.load() ? wire::ATTEMPT_CANCELLED : wire::ATTEMPT_RUNNING);
                persist(*record);
                if (record->canceled.load())
                {
                    return;
                }
            }
            wire::SubmitCompactionRequest request;
            require(request.ParseFromString(read_file(record->directory / "request.pb")), "invalid persisted request");
            const auto database = contained_path(options_.shared_root, request.database_relative_path());
            const auto output = contained_path(options_.shared_root, record->status.output_relative_path());
            require(fs::is_empty(output), "output directory must be empty");
            const auto spec = dataset_spec(database.parent_path());
            auto options = engine_options(spec);
            rocksdb::CompactionServiceOptionsOverride overrides;
            overrides.table_factory = options.table_factory;
            overrides.statistics = rocksdb::CreateDBStatistics();
            rocksdb::OpenAndCompactOptions compact;
            compact.canceled = &record->canceled;
            compact.allow_resumption = false;
            std::string result;
            const auto status = rocksdb::DB::OpenAndCompact(compact, database.string(), output.string(),
                                                            request.engine_input(), &result, overrides);
            check(status);
            require(result.size() <= payload_limit, "native result exceeds payload limit");
            for (const auto &entry : fs::directory_iterator(output))
            {
                require(fs::is_regular_file(entry.symlink_status()), "unexpected worker output entry");
                if (entry.path().extension() == ".sst")
                {
                    rocksdb::SstFileReader reader(options);
                    check(reader.Open(entry.path().string()));
                    check(reader.VerifyChecksum());
                }
                check(rocksdb::Env::Default()->SyncFile(entry.path().string(), rocksdb::EnvOptions(), true));
            }
            sync_directory(output);
            durable_bytes(record->directory / "result.bin", result);
            const std::lock_guard guard(record->mutex);
            record->status.set_execution_wall_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
            record->status.set_state(record->canceled.load() ? wire::ATTEMPT_CANCELLED : wire::ATTEMPT_OUTPUT_READY);
            persist(*record);
        }
        catch (const std::exception &error)
        {
            const std::lock_guard guard(record->mutex);
            record->status.set_state(record->canceled.load() ? wire::ATTEMPT_CANCELLED : wire::ATTEMPT_FAILED);
            record->status.set_failure_code(record->canceled.load() ? "CANCELLED" : "EXECUTION_FAILED");
            record->status.set_failure_detail(error.what());
            record->status.set_execution_wall_ns(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
            try
            {
                persist(*record);
            }
            catch (...)
            {
                record->status.set_failure_code("JOURNAL_FAILED");
                record->changed.notify_all();
            }
        }
    }

    void Worker::run()
    {
        while (true)
        {
            std::shared_ptr<Record> record;
            {
                std::unique_lock guard(mutex_);
                changed_.wait(guard, [this]
                              { return stopping_ || !queue_.empty(); });
                if (queue_.empty())
                {
                    return;
                }
                record = queue_.front();
                queue_.pop_front();
            }
            execute(record);
        }
    }

    void Worker::stop()
    {
        {
            const std::lock_guard guard(mutex_);
            stopping_ = true;
            for (const auto &[identity, record] : records_)
            {
                static_cast<void>(identity);
                record->canceled.store(true);
            }
        }
        changed_.notify_all();
        for (auto &thread : threads_)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
    }
}