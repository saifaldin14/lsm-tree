#pragma once

#include "lsm_lab/runtime.hpp"
#include "compaction.grpc.pb.h"

#include <atomic>
#include <unordered_map>
#include <rocksdb/listener.h>
#include <rocksdb/options.h>

namespace lsm_lab
{
    struct RemoteOptions
    {
        std::filesystem::path shared_root;
        std::filesystem::path database;
        std::string endpoint;
        std::string run_id;
        std::string spec_sha256;
        std::string backend = "cpu_native";
        TlsOptions tls;
        std::chrono::seconds job_timeout{300};
    };

    class RemoteCompaction final : public rocksdb::CompactionService, public rocksdb::EventListener
    {
    public:
        RemoteCompaction(RemoteOptions options, std::shared_ptr<EventLog> events);
        const char *Name() const override { return "lsm_lab_remote_v1"; }
        rocksdb::CompactionServiceScheduleResponse Schedule(const rocksdb::CompactionServiceJobInfo &info,
                                                            const std::string &input) override;
        rocksdb::CompactionServiceJobStatus Wait(const std::string &attempt_id, std::string *result) override;
        void CancelAwaitingJobs() override;
        void OnInstallation(const std::string &attempt_id, rocksdb::CompactionServiceJobStatus status) override;
        void OnCompactionCompleted(rocksdb::DB *, const rocksdb::CompactionJobInfo &info) override;
        nlohmann::json summary() const;

    private:
        struct Attempt
        {
            rpc::v1::AttemptIdentity identity;
            std::string input_digest;
            std::mutex mutex;
            std::string result_digest;
            bool imported = false;
            Clock::time_point started = Clock::now();
        };
        std::shared_ptr<Attempt> find(const std::string &attempt_id);
        rpc::v1::AttemptRequest query(const Attempt &attempt) const;
        void validate_response(const Attempt &attempt, const rpc::v1::CompactionAttempt &response) const;
        void acknowledge(const Attempt &attempt, rpc::v1::InstallationState state);
        void cancel(const Attempt &attempt) noexcept;
        void event(const Attempt &attempt, const std::string &phase);
        void error(const std::string &phase, const std::string &detail) noexcept;
        RemoteOptions options_;
        std::shared_ptr<EventLog> events_;
        std::unique_ptr<rpc::v1::CompactionWorkerService::Stub> stub_;
        rpc::v1::WorkerCapabilitiesResponse capabilities_;
        std::mutex mutex_;
        std::unordered_map<std::string, std::shared_ptr<Attempt>> attempts_;
        std::atomic<bool> cancelled_{false};
        std::atomic<bool> healthy_{true};
        std::atomic<std::uint64_t> submitted_{0};
        std::atomic<std::uint64_t> committed_{0};
        std::atomic<std::uint64_t> fallback_{0};
        std::atomic<std::uint64_t> transformed_blocks_{0}, gpu_blocks_{0}, gpu_input_bytes_{0};
    };
}