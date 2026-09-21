#include "lsm_lab/remote_compaction.hpp"

#include <algorithm>
#include <vector>

namespace lsm_lab
{
    namespace wire = rpc::v1;
    namespace fs = std::filesystem;
    using JobStatus = rocksdb::CompactionServiceJobStatus;

    RemoteCompaction::RemoteCompaction(RemoteOptions options, std::shared_ptr<EventLog> events)
        : options_(std::move(options)), events_(std::move(events))
    {
        require(valid_id(options_.run_id) && events_ != nullptr && options_.job_timeout.count() > 0,
                "invalid remote compaction options");
        options_.shared_root = fs::canonical(options_.shared_root);
        options_.database = fs::canonical(options_.database);
        contained_path(options_.shared_root, options_.database.lexically_relative(options_.shared_root));
        stub_ = wire::CompactionWorkerService::NewStub(connect(options_.endpoint, options_.tls));
        wire::WorkerCapabilitiesRequest request;
        request.set_wire_version(wire_version);
        request.set_engine_fingerprint(engine_fingerprint());
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        const auto status = stub_->Capabilities(&context, request, &capabilities_);
        require(status.ok(), "worker handshake failed: " + status.error_message());
        const auto marker = nlohmann::json::parse(read_file(options_.shared_root / ".lsm-shared.json"));
        require(marker.at("owner") == "lsm-lab" && marker.at("schema_version") == 1 &&
                    capabilities_.wire_version() == wire_version &&
                    capabilities_.engine_fingerprint() == engine_fingerprint() &&
                    capabilities_.shared_root() == options_.shared_root.string() &&
                    marker.at("shared_root_id") == capabilities_.shared_root_id(),
                "worker engine or shared filesystem identity mismatch");
        require(std::find(capabilities_.backends().begin(), capabilities_.backends().end(), options_.backend) !=
                    capabilities_.backends().end(),
                "required worker backend unavailable");
    }

    std::shared_ptr<RemoteCompaction::Attempt> RemoteCompaction::find(const std::string &attempt_id)
    {
        const std::lock_guard guard(mutex_);
        const auto found = attempts_.find(attempt_id);
        require(found != attempts_.end(), "unknown primary attempt");
        return found->second;
    }

    wire::AttemptRequest RemoteCompaction::query(const Attempt &attempt) const
    {
        wire::AttemptRequest request;
        request.set_wire_version(wire_version);
        *request.mutable_identity() = attempt.identity;
        request.set_worker_instance_id(capabilities_.worker_instance_id());
        return request;
    }

    void RemoteCompaction::validate_response(const Attempt &attempt, const wire::CompactionAttempt &response) const
    {
        require(response.wire_version() == wire_version &&
                    response.worker_instance_id() == capabilities_.worker_instance_id() &&
                    response.identity().SerializeAsString() == attempt.identity.SerializeAsString() &&
                    response.engine_input_sha256() == attempt.input_digest && response.backend() == options_.backend,
                "worker returned a mismatched or stale attempt");
    }

    void RemoteCompaction::event(const Attempt &attempt, const std::string &phase)
    {
        events_->append({{"role", "primary"}, {"phase", phase}, {"run_id", options_.run_id}, {"attempt_id", attempt.identity.attempt_id()}, {"job_id", attempt.identity.job().job_id()}, {"parent_job_id", attempt.identity.job().job_id() >> 32}, {"db_session_id", attempt.identity.job().db_session_id()}, {"backend", options_.backend}, {"since_submit_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - attempt.started).count()}});
    }

    void RemoteCompaction::error(const std::string &phase, const std::string &detail) noexcept
    {
        healthy_.store(false);
        try
        {
            events_->append({{"role", "primary"}, {"phase", phase}, {"detail", detail}, {"run_id", options_.run_id}});
        }
        catch (...)
        {
        }
    }

    rocksdb::CompactionServiceScheduleResponse RemoteCompaction::Schedule(
        const rocksdb::CompactionServiceJobInfo &info, const std::string &input)
    {
        std::shared_ptr<Attempt> attempt;
        try
        {
            require(!cancelled_.load(), "primary is stopping");
            require(fs::canonical(info.db_name) == options_.database, "scheduled database mismatch");
            require(input.size() <= capabilities_.maximum_engine_payload_bytes(), "native input exceeds worker limit");
            attempt = std::make_shared<Attempt>();
            attempt->identity.set_attempt_id(unique_id());
            auto *job = attempt->identity.mutable_job();
            job->set_run_id(options_.run_id);
            job->set_db_id(info.db_id);
            job->set_db_session_id(info.db_session_id);
            job->set_column_family_id(info.cf_id);
            job->set_job_id(info.job_id);
            attempt->input_digest = digest_bytes(input);
            wire::SubmitCompactionRequest request;
            request.set_wire_version(wire_version);
            request.set_engine_fingerprint(engine_fingerprint());
            request.set_resolved_spec_sha256(options_.spec_sha256);
            *request.mutable_identity() = attempt->identity;
            request.set_shared_root_id(capabilities_.shared_root_id());
            request.set_database_relative_path(options_.database.lexically_relative(options_.shared_root).generic_string());
            request.set_backend(options_.backend);
            request.set_engine_input(input);
            request.set_engine_input_sha256(attempt->input_digest);
            {
                const std::lock_guard guard(mutex_);
                require(attempts_.size() < 1024, "primary attempt budget exhausted");
                attempts_.emplace(attempt->identity.attempt_id(), attempt);
            }
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
            wire::CompactionAttempt response;
            const auto status = stub_->Submit(&context, request, &response);
            require(status.ok(), "worker admission failed: " + status.error_message());
            validate_response(*attempt, response);
            ++submitted_;
            event(*attempt, "submitted");
            return {attempt->identity.attempt_id(), JobStatus::kSuccess};
        }
        catch (const std::exception &failure)
        {
            if (attempt)
            {
                cancel(*attempt);
            }
            error("schedule_failed", failure.what());
        }
        catch (...)
        {
            error("schedule_failed", "unknown exception");
        }
        return rocksdb::CompactionServiceScheduleResponse(JobStatus::kFailure);
    }

    JobStatus RemoteCompaction::Wait(const std::string &attempt_id, std::string *result)
    {
        try
        {
            const auto attempt = find(attempt_id);
            const auto deadline = attempt->started + options_.job_timeout;
            auto request = query(*attempt);
            request.set_wait_ms(1000);
            while (!cancelled_.load() && Clock::now() < deadline)
            {
                grpc::ClientContext context;
                context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
                wire::CompactionAttempt response;
                const auto status = stub_->Inspect(&context, request, &response);
                require(status.ok(), "worker inspection failed: " + status.error_message());
                validate_response(*attempt, response);
                if (response.state() == wire::ATTEMPT_OUTPUT_READY)
                {
                    require(!response.engine_result().empty(), "worker returned empty native result");
                    *result = response.engine_result();
                    {
                        const std::lock_guard guard(attempt->mutex);
                        attempt->result_digest = digest_bytes(*result);
                    }
                    event(*attempt, "output_ready");
                    return JobStatus::kSuccess;
                }
                require(response.state() == wire::ATTEMPT_ACCEPTED || response.state() == wire::ATTEMPT_QUEUED ||
                            response.state() == wire::ATTEMPT_RUNNING,
                        "worker execution failed: " + response.failure_detail());
            }
            cancel(*attempt);
            error("wait_aborted", "worker deadline exceeded or primary cancelled");
            return JobStatus::kAborted;
        }
        catch (const std::exception &failure)
        {
            error("wait_failed", failure.what());
        }
        catch (...)
        {
            error("wait_failed", "unknown exception");
        }
        return JobStatus::kFailure;
    }

    void RemoteCompaction::cancel(const Attempt &attempt) noexcept
    {
        try
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
            wire::CompactionAttempt response;
            const auto status = stub_->Cancel(&context, query(attempt), &response);
            require(status.ok(), "remote cancellation unconfirmed: " + status.error_message());
            validate_response(attempt, response);
        }
        catch (const std::exception &failure)
        {
            error("cancel_failed", failure.what());
        }
        catch (...)
        {
            error("cancel_failed", "unknown exception");
        }
    }

    void RemoteCompaction::CancelAwaitingJobs()
    {
        cancelled_.store(true);
        try
        {
            std::vector<std::shared_ptr<Attempt>> pending;
            {
                const std::lock_guard guard(mutex_);
                for (const auto &[identity, attempt] : attempts_)
                {
                    static_cast<void>(identity);
                    pending.push_back(attempt);
                }
            }
            for (const auto &attempt : pending)
            {
                cancel(*attempt);
            }
        }
        catch (...)
        {
            error("cancel_failed", "could not cancel all pending attempts");
        }
    }

    void RemoteCompaction::acknowledge(const Attempt &attempt, wire::InstallationState state)
    {
        wire::InstallationAcknowledgement request;
        request.set_wire_version(wire_version);
        *request.mutable_identity() = attempt.identity;
        request.set_worker_instance_id(capabilities_.worker_instance_id());
        request.set_state(state);
        request.set_engine_result_sha256(attempt.result_digest);
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        wire::CompactionAttempt response;
        const auto status = stub_->Acknowledge(&context, request, &response);
        require(status.ok(), "installation acknowledgement failed: " + status.error_message());
        validate_response(attempt, response);
        require(response.installation_state() == state, "installation acknowledgement state mismatch");
    }

    void RemoteCompaction::OnInstallation(const std::string &attempt_id, JobStatus status)
    {
        try
        {
            const auto attempt = find(attempt_id);
            const auto state = status == JobStatus::kSuccess ? wire::INSTALLATION_OUTPUTS_IMPORTED : (status == JobStatus::kUseLocal ? wire::INSTALLATION_LOCAL_FALLBACK : wire::INSTALLATION_FAILED);
            acknowledge(*attempt, state);
            {
                const std::lock_guard guard(attempt->mutex);
                attempt->imported = status == JobStatus::kSuccess;
            }
            event(*attempt, status == JobStatus::kSuccess ? "outputs_imported" : "installation_failed");
        }
        catch (const std::exception &failure)
        {
            error("installation_ack_failed", failure.what());
        }
        catch (...)
        {
            error("installation_ack_failed", "unknown exception");
        }
    }

    void RemoteCompaction::OnCompactionCompleted(rocksdb::DB *, const rocksdb::CompactionJobInfo &info)
    {
        try
        {
            std::vector<std::shared_ptr<Attempt>> completed;
            {
                const std::lock_guard guard(mutex_);
                for (auto iterator = attempts_.begin(); iterator != attempts_.end();)
                {
                    const auto &job = iterator->second->identity.job();
                    if ((job.job_id() >> 32) == static_cast<std::uint32_t>(info.job_id) && job.column_family_id() == info.cf_id)
                    {
                        completed.push_back(iterator->second);
                        iterator = attempts_.erase(iterator);
                    }
                    else
                    {
                        ++iterator;
                    }
                }
            }
            for (const auto &attempt : completed)
            {
                const std::lock_guard guard(attempt->mutex);
                if (info.status.ok() && attempt->imported)
                {
                    acknowledge(*attempt, wire::INSTALLATION_COMMITTED);
                    ++committed_;
                    event(*attempt, "committed");
                }
                else
                {
                    if (info.status.ok())
                    {
                        ++fallback_;
                    }
                    if (!attempt->result_digest.empty())
                    {
                        acknowledge(*attempt, info.status.ok() ? wire::INSTALLATION_LOCAL_FALLBACK : wire::INSTALLATION_FAILED);
                    }
                    error(info.status.ok() ? "local_fallback" : "compaction_failed", info.status.ToString());
                }
            }
        }
        catch (const std::exception &failure)
        {
            error("completion_failed", failure.what());
        }
        catch (...)
        {
            error("completion_failed", "unknown exception");
        }
    }

    nlohmann::json RemoteCompaction::summary() const
    {
        return {{"healthy", healthy_.load()}, {"submitted", submitted_.load()}, {"committed", committed_.load()}, {"local_fallback", fallback_.load()}};
    }
}