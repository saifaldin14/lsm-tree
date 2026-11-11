#pragma once

// Decides which waiting compaction job gets the next accelerator slot when several databases
// share one GPU (the compaction server, or several databases in one process).
//
//   fifo   arrival order
//   fair   start-time fair queuing over tenants, charged by input bytes: each tenant gets an
//          equal share of the processed bytes while it has work waiting
//   stall  jobs that unblock writers first (writes stalled, or a level-0 job with level 0 at
//          or above its slowdown trigger), each class in fair order
//
// A job starts when a slot is free and its device-memory estimate fits the budget (a job
// always starts when nothing else runs, so one large job cannot wait forever). Jobs are
// granted strictly in policy order: a job that does not fit blocks those behind it.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace lsm_lab
{
    enum class SchedulePolicy
    {
        fifo,
        fair,
        stall
    };
    SchedulePolicy parse_schedule_policy(const std::string &name);
    std::string schedule_policy_name(SchedulePolicy policy);

    struct MergeJobInfo
    {
        std::string tenant;
        std::uint64_t input_bytes = 0;
        std::uint64_t device_bytes = 0; // estimate; 0 when unknown
        bool urgent = false;
    };

    class MergeScheduler
    {
    public:
        // slots: jobs that may run at once; memory_budget: bytes of device memory the running
        // jobs' estimates may add up to (0: no limit).
        MergeScheduler(SchedulePolicy policy, std::size_t slots, std::uint64_t memory_budget = 0);

        // Holds a slot until destroyed (or released). requeue() gives the slot back and waits
        // for another one, keeping the job's place in the policy order, but not before some
        // other job finishes (so memory has been freed); used after a device allocation failure.
        class Grant
        {
        public:
            Grant() = default;
            Grant(Grant &&other) noexcept;
            Grant &operator=(Grant &&other) noexcept;
            Grant(const Grant &) = delete;
            Grant &operator=(const Grant &) = delete;
            ~Grant();
            std::size_t slot() const { return slot_; }
            void release();
            void requeue();

        private:
            friend class MergeScheduler;
            MergeScheduler *owner_ = nullptr;
            std::uint64_t job_ = 0;
            std::size_t slot_ = 0;
        };

        Grant acquire(const MergeJobInfo &job);
        // Jobs running now (including the caller's, if it holds a grant), and jobs waiting.
        std::size_t running() const;
        std::size_t waiting() const;
        SchedulePolicy policy() const { return policy_; }
        std::size_t slots() const { return slots_.size(); }
        // Per-tenant counts and times and the policy; with include_log, the completed jobs in
        // order (bounded) for timeline figures.
        nlohmann::json statistics(bool include_log = false) const;

    private:
        using Clock = std::chrono::steady_clock;
        struct Job
        {
            std::uint64_t id = 0;
            MergeJobInfo info;
            std::uint64_t arrival = 0;
            double start_tag = 0;
            double finish_tag = 0;
            Clock::time_point queued;
            Clock::time_point started;
            bool granted = false;
            bool running = false;
            std::size_t slot = 0;
            std::uint64_t requeues = 0;
            // Not eligible again until completions_ passes this (after a requeue).
            std::uint64_t completions_at_requeue = 0;
            bool requeued = false;
        };
        struct TenantStats
        {
            std::uint64_t jobs = 0, input_bytes = 0, wait_ns = 0, service_ns = 0, requeues = 0, urgent_jobs = 0;
            std::uint64_t max_wait_ns = 0;
        };

        void enqueue(Job &job);
        void dispatch(); // with mutex_ held
        Job *pick();     // with mutex_ held
        bool fits(const Job &job) const;
        void finish(std::uint64_t id, bool requeue);
        std::size_t wait_for_grant(std::uint64_t id, std::unique_lock<std::mutex> &lock);

        const SchedulePolicy policy_;
        const std::uint64_t memory_budget_;
        mutable std::mutex mutex_;
        std::condition_variable granted_;
        std::vector<bool> slots_;
        std::map<std::uint64_t, Job> jobs_;
        std::deque<std::uint64_t> waiting_;
        std::map<std::string, double> tenant_finish_;
        std::map<std::string, TenantStats> stats_;
        double virtual_time_ = 0;
        std::uint64_t next_id_ = 1, next_arrival_ = 0, memory_in_use_ = 0, running_ = 0;
        std::uint64_t peak_waiting_ = 0, peak_memory_ = 0, completions_ = 0;
        std::deque<nlohmann::json> log_;
        const Clock::time_point created_ = Clock::now();
    };

    // Whether a job unblocks writers: writes were stalled, or it compacts level 0 while level 0
    // is at or above its slowdown trigger.
    bool merge_job_urgent(bool write_stalled, bool reads_level0, int level0_files, int level0_slowdown_trigger);
}
