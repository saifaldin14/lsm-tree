#pragma once

#include "compaction.pb.h"
#include "lsm_lab/runtime.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lsm_lab
{
    struct WorkerOptions
    {
        std::filesystem::path shared_root;
        std::filesystem::path journal;
        std::uint32_t threads = 1;
        std::uint32_t queue_limit = 16;
    };

    class Worker
    {
    public:
        explicit Worker(WorkerOptions options);
        ~Worker();
        Worker(const Worker &) = delete;
        Worker &operator=(const Worker &) = delete;
        rpc::v1::WorkerCapabilitiesResponse capabilities() const;
        rpc::v1::CompactionAttempt submit(const rpc::v1::SubmitCompactionRequest &request);
        rpc::v1::CompactionAttempt inspect(const rpc::v1::AttemptRequest &request);
        rpc::v1::CompactionAttempt cancel(const rpc::v1::AttemptRequest &request);
        rpc::v1::CompactionAttempt acknowledge(const rpc::v1::InstallationAcknowledgement &request);
        void stop();

    private:
        struct Record
        {
            std::mutex mutex;
            std::condition_variable changed;
            std::atomic<bool> canceled{false};
            rpc::v1::CompactionAttempt status;
            std::filesystem::path directory;
            std::string request_digest;
        };
        std::shared_ptr<Record> find(const rpc::v1::AttemptIdentity &identity, const std::string &instance);
        void persist(Record &record);
        rpc::v1::CompactionAttempt response(const Record &record) const;
        void execute(const std::shared_ptr<Record> &record);
        void run();
        WorkerOptions options_;
        std::string shared_id_;
        std::string instance_ = unique_id();
        rocksdb::FileLock *journal_lock_ = nullptr;
        std::mutex admission_mutex_;
        std::mutex mutex_;
        std::condition_variable changed_;
        bool stopping_ = false;
        std::deque<std::shared_ptr<Record>> queue_;
        std::unordered_map<std::string, std::shared_ptr<Record>> records_;
        std::vector<std::thread> threads_;
    };
}