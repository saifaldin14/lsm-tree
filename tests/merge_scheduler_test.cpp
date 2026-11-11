// Unit tests for the shared-accelerator job scheduler (lsm_lab/merge_scheduler.hpp).

#include "lsm_lab/merge_scheduler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using lsm_lab::MergeJobInfo;
    using lsm_lab::MergeScheduler;
    using lsm_lab::SchedulePolicy;

    // Starts jobs one at a time on threads, in the given order, each waiting until the
    // scheduler has queued it; records the order in which they are granted.
    class Recorder
    {
    public:
        explicit Recorder(MergeScheduler &scheduler) : scheduler_(scheduler) {}
        ~Recorder()
        {
            for (auto &thread : threads_)
                thread.join();
        }
        void submit(const std::string &name, MergeJobInfo info, std::chrono::milliseconds hold = std::chrono::milliseconds(5))
        {
            const auto before = scheduler_.waiting();
            threads_.emplace_back([this, name, info, hold] {
                auto grant = scheduler_.acquire(info);
                {
                    const std::lock_guard lock(mutex_);
                    order_.push_back(name);
                }
                std::this_thread::sleep_for(hold);
            });
            while (scheduler_.waiting() <= before)
                std::this_thread::yield();
        }
        std::vector<std::string> finish()
        {
            for (auto &thread : threads_)
                thread.join();
            threads_.clear();
            return order_;
        }

    private:
        MergeScheduler &scheduler_;
        std::vector<std::thread> threads_;
        std::mutex mutex_;
        std::vector<std::string> order_;
    };

    MergeJobInfo job(const std::string &tenant, std::uint64_t bytes, bool urgent = false, std::uint64_t memory = 0)
    {
        MergeJobInfo info;
        info.tenant = tenant;
        info.input_bytes = bytes;
        info.urgent = urgent;
        info.device_bytes = memory;
        return info;
    }

    // Order in which queued jobs are granted while one blocking job holds the only slot.
    std::vector<std::string> order(SchedulePolicy policy, const std::vector<std::pair<std::string, MergeJobInfo>> &jobs)
    {
        MergeScheduler scheduler(policy, 1);
        auto blocker = scheduler.acquire(job("heavy", 100));
        Recorder recorder(scheduler);
        for (const auto &[name, info] : jobs)
            recorder.submit(name, info);
        blocker.release();
        return recorder.finish();
    }

    TEST(MergeScheduler, FifoGrantsInArrivalOrder)
    {
        const auto granted = order(SchedulePolicy::fifo, {{"h1", job("heavy", 100)},
                                                          {"h2", job("heavy", 100)},
                                                          {"l1", job("light", 100)}});
        EXPECT_EQ(granted, (std::vector<std::string>{"h1", "h2", "l1"}));
    }

    TEST(MergeScheduler, FairLetsALightTenantAheadOfAHeavyBacklog)
    {
        const auto granted = order(SchedulePolicy::fair, {{"h1", job("heavy", 100)},
                                                          {"h2", job("heavy", 100)},
                                                          {"h3", job("heavy", 100)},
                                                          {"l1", job("light", 100)}});
        EXPECT_EQ(granted, (std::vector<std::string>{"l1", "h1", "h2", "h3"}));
    }

    TEST(MergeScheduler, FairChargesByBytes)
    {
        // Light jobs of 50 bytes: two of them cost the same share as one 100-byte heavy job.
        const auto granted = order(SchedulePolicy::fair, {{"h1", job("heavy", 100)},
                                                          {"h2", job("heavy", 100)},
                                                          {"l1", job("light", 50)},
                                                          {"l2", job("light", 50)},
                                                          {"l3", job("light", 50)}});
        EXPECT_EQ(granted, (std::vector<std::string>{"l1", "l2", "h1", "l3", "h2"}));
    }

    TEST(MergeScheduler, StallRunsUrgentJobsFirst)
    {
        const auto granted = order(SchedulePolicy::stall, {{"l1", job("light", 10)},
                                                           {"u1", job("heavy", 100, true)},
                                                           {"l2", job("light", 10)}});
        EXPECT_EQ(granted.front(), "u1");
    }

    TEST(MergeScheduler, MemoryBudgetHoldsJobsThatDoNotFit)
    {
        MergeScheduler scheduler(SchedulePolicy::fifo, 4, 100);
        auto big = scheduler.acquire(job("a", 10, false, 80));
        Recorder recorder(scheduler);
        recorder.submit("b", job("b", 10, false, 50));
        recorder.submit("c", job("c", 10, false, 10)); // fits, but waits behind b (strict order)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_EQ(scheduler.running(), 1U);
        EXPECT_EQ(scheduler.waiting(), 2U);
        big.release();
        auto granted = recorder.finish();
        std::sort(granted.begin(), granted.end());
        EXPECT_EQ(granted, (std::vector<std::string>{"b", "c"}));
    }

    TEST(MergeScheduler, JobLargerThanTheBudgetRunsAlone)
    {
        MergeScheduler scheduler(SchedulePolicy::fifo, 2, 100);
        auto huge = scheduler.acquire(job("a", 10, false, 500));
        EXPECT_EQ(scheduler.running(), 1U);
    }

    TEST(MergeScheduler, RequeueWaitsForAnotherJobToFinish)
    {
        MergeScheduler scheduler(SchedulePolicy::fifo, 2);
        auto a = scheduler.acquire(job("a", 10));
        auto c = scheduler.acquire(job("c", 10));
        std::vector<std::string> events;
        std::mutex mutex;
        std::thread b_thread([&] {
            auto b = scheduler.acquire(job("b", 10));
            {
                const std::lock_guard lock(mutex);
                events.push_back("b granted");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        });
        while (scheduler.waiting() == 0)
            std::this_thread::yield();
        std::thread c_thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            {
                const std::lock_guard lock(mutex);
                events.push_back("c done");
            }
            c.release();
        });
        a.requeue(); // b takes a's slot; a waits until c (or b) finishes
        {
            const std::lock_guard lock(mutex);
            events.push_back("a granted again");
        }
        a.release();
        b_thread.join();
        c_thread.join();
        EXPECT_EQ(events, (std::vector<std::string>{"b granted", "c done", "a granted again"}));
        const auto stats = scheduler.statistics();
        EXPECT_EQ(stats["tenants"]["a"]["requeues"], 1);
        EXPECT_EQ(stats["tenants"]["a"]["jobs"], 1);
        EXPECT_EQ(stats["tenants"]["b"]["jobs"], 1);
    }

    TEST(MergeScheduler, RequeueAloneIsGrantedAgain)
    {
        MergeScheduler scheduler(SchedulePolicy::fair, 1);
        auto a = scheduler.acquire(job("a", 10));
        a.requeue(); // nothing else runs, so it gets the slot back at once
        EXPECT_EQ(scheduler.running(), 1U);
    }

    TEST(MergeScheduler, SlotsRunJobsConcurrently)
    {
        MergeScheduler scheduler(SchedulePolicy::fair, 3);
        auto a = scheduler.acquire(job("a", 10));
        auto b = scheduler.acquire(job("b", 10));
        auto c = scheduler.acquire(job("c", 10));
        EXPECT_EQ(scheduler.running(), 3U);
        EXPECT_NE(a.slot(), b.slot());
        EXPECT_NE(b.slot(), c.slot());
    }

    TEST(MergeScheduler, Urgency)
    {
        EXPECT_TRUE(lsm_lab::merge_job_urgent(true, false, 0, 20));
        EXPECT_TRUE(lsm_lab::merge_job_urgent(false, true, 20, 20));
        EXPECT_FALSE(lsm_lab::merge_job_urgent(false, true, 19, 20));
        EXPECT_FALSE(lsm_lab::merge_job_urgent(false, false, 40, 20));
    }
}
