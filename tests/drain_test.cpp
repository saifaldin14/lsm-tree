// Unit tests for lsm_lab::Drain: a draining compaction server refuses new merges with Busy,
// still answers statistics requests, and reports when its running merges have finished.

#include "lsm_lab/remote_merge.hpp"

#include <gtest/gtest.h>
#include <rocksdb/status.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <thread>

namespace
{
    using namespace std::chrono_literals;

    class StringSink : public lsm_lab::ReplySink
    {
    public:
        char *reserve(std::size_t bytes) override
        {
            buffer.resize(bytes);
            return buffer.data();
        }
        std::string buffer;
    };

    std::string request(lsm_lab::RpcKind kind)
    {
        lsm_lab::RequestHeader header;
        header.kind = kind;
        return std::string(reinterpret_cast<const char *>(&header), sizeof(header));
    }

    TEST(Drain, RefusesNewMergesAndWaitsForRunningOnes)
    {
        lsm_lab::Drain drain;
        std::promise<void> release;
        auto released = release.get_future().share();
        std::atomic<int> merges{0}, others{0};
        auto handler = drain.wrap([&](const char *data, std::size_t, lsm_lab::ReplySink &) -> std::size_t {
            lsm_lab::RequestHeader header;
            std::memcpy(&header, data, sizeof(header));
            if (header.kind != lsm_lab::RpcKind::merge)
            {
                ++others;
                return 0;
            }
            ++merges;
            released.wait();
            return 0;
        });

        const auto merge = request(lsm_lab::RpcKind::merge);
        std::thread running([&] {
            StringSink sink;
            handler(merge.data(), merge.size(), sink);
        });
        while (merges.load() == 0)
            std::this_thread::sleep_for(1ms);

        EXPECT_FALSE(drain.wait(0ms));
        drain.begin();
        EXPECT_FALSE(drain.wait(50ms));

        StringSink refused;
        const auto bytes = handler(merge.data(), merge.size(), refused);
        ASSERT_GE(bytes, sizeof(lsm_lab::ReplyHeader));
        lsm_lab::ReplyHeader reply;
        std::memcpy(&reply, refused.buffer.data(), sizeof(reply));
        EXPECT_EQ(reply.status, static_cast<std::uint32_t>(rocksdb::Status::Code::kBusy));
        EXPECT_EQ(merges.load(), 1);
        EXPECT_EQ(drain.refused(), 1u);

        const auto statistics = request(lsm_lab::RpcKind::statistics);
        StringSink answered;
        handler(statistics.data(), statistics.size(), answered);
        EXPECT_EQ(others.load(), 1);

        release.set_value();
        running.join();
        EXPECT_TRUE(drain.wait(1s));
    }

    TEST(Drain, IdleServerDrainsAtOnce)
    {
        lsm_lab::Drain drain;
        drain.begin();
        EXPECT_TRUE(drain.wait(0ms));
    }
}
