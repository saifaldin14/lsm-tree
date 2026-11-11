#include "lsm_lab/merge_scheduler.hpp"

#include <algorithm>
#include <stdexcept>

namespace lsm_lab
{
    namespace
    {
        constexpr std::size_t kLogLimit = 50000;

        std::uint64_t nanoseconds(std::chrono::steady_clock::duration duration)
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
        }
    }

    SchedulePolicy parse_schedule_policy(const std::string &name)
    {
        if (name == "fifo")
            return SchedulePolicy::fifo;
        if (name == "fair")
            return SchedulePolicy::fair;
        if (name == "stall")
            return SchedulePolicy::stall;
        throw std::invalid_argument("scheduler policy must be fifo, fair or stall");
    }

    std::string schedule_policy_name(SchedulePolicy policy)
    {
        switch (policy)
        {
        case SchedulePolicy::fifo:
            return "fifo";
        case SchedulePolicy::fair:
            return "fair";
        case SchedulePolicy::stall:
            return "stall";
        }
        return "unknown";
    }

    bool merge_job_urgent(bool write_stalled, bool reads_level0, int level0_files, int level0_slowdown_trigger)
    {
        return write_stalled || (reads_level0 && level0_slowdown_trigger > 0 && level0_files >= level0_slowdown_trigger);
    }

    MergeScheduler::MergeScheduler(SchedulePolicy policy, std::size_t slots, std::uint64_t memory_budget)
        : policy_(policy), memory_budget_(memory_budget), slots_(std::max<std::size_t>(slots, 1), false)
    {
    }

    MergeScheduler::Grant::Grant(Grant &&other) noexcept : owner_(other.owner_), job_(other.job_), slot_(other.slot_)
    {
        other.owner_ = nullptr;
    }

    MergeScheduler::Grant &MergeScheduler::Grant::operator=(Grant &&other) noexcept
    {
        if (this != &other)
        {
            release();
            owner_ = other.owner_;
            job_ = other.job_;
            slot_ = other.slot_;
            other.owner_ = nullptr;
        }
        return *this;
    }

    MergeScheduler::Grant::~Grant() { release(); }

    void MergeScheduler::Grant::release()
    {
        if (owner_ == nullptr)
            return;
        owner_->finish(job_, false);
        owner_ = nullptr;
    }

    void MergeScheduler::Grant::requeue()
    {
        if (owner_ == nullptr)
            throw std::logic_error("requeue without a grant");
        owner_->finish(job_, true);
        std::unique_lock lock(owner_->mutex_);
        slot_ = owner_->wait_for_grant(job_, lock);
    }

    MergeScheduler::Grant MergeScheduler::acquire(const MergeJobInfo &info)
    {
        std::unique_lock lock(mutex_);
        const auto id = next_id_++;
        auto &job = jobs_[id];
        job.id = id;
        job.info = info;
        job.arrival = next_arrival_++;
        enqueue(job);
        Grant grant;
        grant.owner_ = this;
        grant.job_ = id;
        grant.slot_ = wait_for_grant(id, lock);
        return grant;
    }

    void MergeScheduler::enqueue(Job &job)
    {
        // Start-time fair queuing: a tenant's next job starts where its previous one finished,
        // or at the current virtual time if the tenant had nothing queued.
        auto &finish = tenant_finish_[job.info.tenant];
        job.start_tag = std::max(virtual_time_, finish);
        job.finish_tag = job.start_tag + static_cast<double>(std::max<std::uint64_t>(job.info.input_bytes, 1));
        finish = job.finish_tag;
        job.queued = Clock::now();
        waiting_.push_back(job.id);
        peak_waiting_ = std::max<std::uint64_t>(peak_waiting_, waiting_.size());
        dispatch();
    }

    std::size_t MergeScheduler::wait_for_grant(std::uint64_t id, std::unique_lock<std::mutex> &lock)
    {
        granted_.wait(lock, [&] { return jobs_.at(id).granted; });
        auto &job = jobs_.at(id);
        job.started = Clock::now();
        const auto waited = nanoseconds(job.started - job.queued);
        auto &stats = stats_[job.info.tenant];
        stats.wait_ns += waited;
        stats.max_wait_ns = std::max(stats.max_wait_ns, waited);
        return job.slot;
    }

    MergeScheduler::Job *MergeScheduler::pick()
    {
        Job *best = nullptr;
        for (const auto id : waiting_)
        {
            auto &job = jobs_.at(id);
            if (job.requeued && completions_ == job.completions_at_requeue && running_ > 0)
                continue;
            if (best == nullptr)
            {
                best = &job;
                continue;
            }
            bool better = false;
            switch (policy_)
            {
            case SchedulePolicy::fifo:
                better = job.arrival < best->arrival;
                break;
            case SchedulePolicy::stall:
                if (job.info.urgent != best->info.urgent)
                {
                    better = job.info.urgent;
                    break;
                }
                [[fallthrough]];
            case SchedulePolicy::fair:
                better = job.start_tag < best->start_tag ||
                         (job.start_tag == best->start_tag && job.arrival < best->arrival);
                break;
            }
            if (better)
                best = &job;
        }
        return best;
    }

    bool MergeScheduler::fits(const Job &job) const
    {
        return memory_budget_ == 0 || running_ == 0 || memory_in_use_ + job.info.device_bytes <= memory_budget_;
    }

    void MergeScheduler::dispatch()
    {
        bool changed = false;
        for (;;)
        {
            const auto free_slot = std::find(slots_.begin(), slots_.end(), false);
            if (free_slot == slots_.end())
                break;
            Job *job = pick();
            if (job == nullptr || !fits(*job))
                break;
            *free_slot = true;
            job->slot = static_cast<std::size_t>(free_slot - slots_.begin());
            job->granted = true;
            job->running = true;
            job->requeued = false;
            memory_in_use_ += job->info.device_bytes;
            peak_memory_ = std::max(peak_memory_, memory_in_use_);
            ++running_;
            virtual_time_ = std::max(virtual_time_, job->start_tag);
            waiting_.erase(std::find(waiting_.begin(), waiting_.end(), job->id));
            changed = true;
        }
        if (changed)
            granted_.notify_all();
    }

    void MergeScheduler::finish(std::uint64_t id, bool requeue)
    {
        const std::lock_guard lock(mutex_);
        auto &job = jobs_.at(id);
        const auto now = Clock::now();
        slots_[job.slot] = false;
        memory_in_use_ -= job.info.device_bytes;
        --running_;
        auto &stats = stats_[job.info.tenant];
        stats.service_ns += nanoseconds(now - job.started);
        if (requeue)
        {
            job.granted = false;
            job.running = false;
            job.queued = now;
            job.requeued = true;
            job.completions_at_requeue = completions_;
            ++job.requeues;
            ++stats.requeues;
            waiting_.push_back(id);
        }
        else
        {
            ++completions_;
            ++stats.jobs;
            stats.input_bytes += job.info.input_bytes;
            stats.urgent_jobs += job.info.urgent ? 1 : 0;
            log_.push_back({{"end_ms", nanoseconds(now - created_) / 1e6},
                            {"start_ms", nanoseconds(job.started - created_) / 1e6},
                            {"tenant", job.info.tenant},
                            {"input_bytes", job.info.input_bytes},
                            {"urgent", job.info.urgent},
                            {"slot", job.slot},
                            {"requeues", job.requeues}});
            if (log_.size() > kLogLimit)
                log_.pop_front();
            jobs_.erase(id);
        }
        dispatch();
    }

    std::size_t MergeScheduler::running() const
    {
        const std::lock_guard lock(mutex_);
        return running_;
    }

    std::size_t MergeScheduler::waiting() const
    {
        const std::lock_guard lock(mutex_);
        return waiting_.size();
    }

    nlohmann::json MergeScheduler::statistics(bool include_log) const
    {
        const std::lock_guard lock(mutex_);
        nlohmann::json tenants = nlohmann::json::object();
        for (const auto &[tenant, stats] : stats_)
            tenants[tenant] = {{"jobs", stats.jobs},
                               {"input_bytes", stats.input_bytes},
                               {"wait_ns", stats.wait_ns},
                               {"max_wait_ns", stats.max_wait_ns},
                               {"service_ns", stats.service_ns},
                               {"requeues", stats.requeues},
                               {"urgent_jobs", stats.urgent_jobs}};
        nlohmann::json result = {{"policy", schedule_policy_name(policy_)},
                                  {"slots", slots_.size()},
                                  {"memory_budget", memory_budget_},
                                  {"peak_waiting", peak_waiting_},
                                  {"peak_memory_estimate", peak_memory_},
                                  {"tenants", tenants}};
        if (include_log)
        {
            auto &log = result["log"] = nlohmann::json::array();
            for (const auto &entry : log_)
                log.push_back(entry);
        }
        return result;
    }
}
