#include "lsm_lab/remote_merge.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace lsm_lab
{
    static_assert(std::is_trivially_copyable_v<rocksdb::CompactionBuiltBlock>);
    static_assert(std::is_trivially_copyable_v<rocksdb::CompactionMergedEntry>);
    static_assert(std::is_trivially_copyable_v<ReplyHeader>);
    static_assert(std::is_trivially_copyable_v<RequestHeader>);

    namespace
    {
        constexpr std::uint32_t kMetaVersion = 1;
        constexpr std::size_t kAlign = 64;

        std::size_t align_up(std::size_t value) { return (value + kAlign - 1) / kAlign * kAlign; }

        class Writer
        {
        public:
            template <typename T>
            void put(T value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                out_.append(reinterpret_cast<const char *>(&value), sizeof(T));
            }
            void put_string(const std::string &value)
            {
                put<std::uint64_t>(value.size());
                out_.append(value);
            }
            std::string take() { return std::move(out_); }

        private:
            std::string out_;
        };

        class Reader
        {
        public:
            Reader(const char *data, std::size_t bytes) : p_(data), end_(data + bytes) {}
            template <typename T>
            T get()
            {
                if (static_cast<std::size_t>(end_ - p_) < sizeof(T))
                    throw std::runtime_error("truncated merge request metadata");
                T value;
                std::memcpy(&value, p_, sizeof(T));
                p_ += sizeof(T);
                return value;
            }
            std::string get_string()
            {
                const auto size = get<std::uint64_t>();
                if (static_cast<std::uint64_t>(end_ - p_) < size)
                    throw std::runtime_error("truncated merge request metadata");
                std::string value(p_, size);
                p_ += size;
                return value;
            }

        private:
            const char *p_;
            const char *end_;
        };

        thread_local const PreloadedMergeBlocks *t_preloaded = nullptr;

        std::uint64_t elapsed_ns(std::chrono::steady_clock::time_point since)
        {
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since)
                    .count());
        }

        rocksdb::Status make_status(std::uint32_t code, const std::string &message)
        {
            using Code = rocksdb::Status::Code;
            switch (static_cast<Code>(code))
            {
            case Code::kOk:
                return rocksdb::Status::OK();
            case Code::kNotSupported:
                return rocksdb::Status::NotSupported("remote: " + message);
            case Code::kCorruption:
                return rocksdb::Status::Corruption("remote: " + message);
            case Code::kInvalidArgument:
                return rocksdb::Status::InvalidArgument("remote: " + message);
            case Code::kIOError:
                return rocksdb::Status::IOError("remote: " + message);
            default:
                return rocksdb::Status::Aborted("remote: " + message);
            }
        }

        // Built output or entry stream that lives in a reply buffer.
        class RemoteMergedInput final : public rocksdb::CompactionMergedInput
        {
        public:
            RemoteMergedInput(std::shared_ptr<const char> reply, const ReplyHeader &header)
                : reply_(std::move(reply)), dropped_(header.dropped)
            {
                const char *payload = reply_.get() + header.payload_offset;
                if (header.kind == 1)
                {
                    std::size_t offset = 0;
                    built_.blocks = reinterpret_cast<const rocksdb::CompactionBuiltBlock *>(payload);
                    built_.block_count = header.block_count;
                    offset = align_up(header.block_count * sizeof(rocksdb::CompactionBuiltBlock));
                    built_.contents = payload + offset;
                    built_.contents_capacity = header.contents_bytes;
                    offset = align_up(offset + header.contents_bytes);
                    built_.keys = payload + offset;
                    offset = align_up(offset + header.keys_bytes);
                    built_.hashes = reinterpret_cast<const std::uint64_t *>(payload + offset);
                    built_.entries = header.entries;
                    built_valid_ = true;
                }
                else
                {
                    packed_ = payload;
                    entries_ = reinterpret_cast<const rocksdb::CompactionMergedEntry *>(
                        payload + align_up(header.packed_bytes));
                    count_ = header.entries;
                }
            }
            std::size_t size() const noexcept override { return count_; }
            rocksdb::Slice key(std::size_t index) const noexcept override
            {
                return {packed_ + entries_[index].offset, entries_[index].key_bytes};
            }
            rocksdb::Slice value(std::size_t index) const noexcept override
            {
                return {packed_ + entries_[index].offset + entries_[index].key_bytes, entries_[index].value_bytes};
            }
            const char *packed_data() const noexcept override { return built_valid_ ? nullptr : packed_; }
            const rocksdb::CompactionMergedEntry *packed_entries() const noexcept override
            {
                return built_valid_ ? nullptr : entries_;
            }
            rocksdb::CompactionMergeDropped dropped() const noexcept override { return dropped_; }
            const rocksdb::CompactionBuiltOutput *built_output() const noexcept override
            {
                return built_valid_ ? &built_ : nullptr;
            }

        private:
            std::shared_ptr<const char> reply_;
            rocksdb::CompactionMergeDropped dropped_;
            rocksdb::CompactionBuiltOutput built_;
            bool built_valid_ = false;
            const char *packed_ = nullptr;
            const rocksdb::CompactionMergedEntry *entries_ = nullptr;
            std::size_t count_ = 0;
        };

        class RemoteTransform final : public BatchTransform
        {
        public:
            explicit RemoteTransform(Endpoint endpoint) : endpoint_(std::move(endpoint))
            {
                const char *configured = std::getenv("LSM_REMOTE_CONNECTIONS");
                const auto parsed = configured == nullptr ? 0 : std::strtoull(configured, nullptr, 10);
                limit_ = parsed == 0 ? 2 : static_cast<std::size_t>(parsed);
                // Connect once up front so a missing server fails fast.
                idle_.push_back(connect_rpc(endpoint_));
                created_ = 1;
                description_ = idle_.back()->describe();
            }

            std::string name() const override { return "remote_merge"; }
            std::string device() const override { return description_; }

        protected:
            rocksdb::Status execute(const std::string &input, const std::vector<BlockDescriptor> &blocks,
                                    std::vector<std::string> *output) override
            {
                return execute_cpu_batch(input, blocks, output);
            }

            rocksdb::Status execute_merge(const rocksdb::CompactionMergeRequest &request,
                                          std::unique_ptr<rocksdb::CompactionMergedInput> *output,
                                          MergeRunStats *stats) override
            {
                auto client = acquire();
                bool healthy = false;
                struct Release
                {
                    RemoteTransform *self;
                    std::unique_ptr<RpcClient> *client;
                    bool *healthy;
                    ~Release() { self->release(std::move(*client), *healthy); }
                } release{this, &client, &healthy};

                const auto read_started = std::chrono::steady_clock::now();
                const auto meta = encode_merge_request(request);
                const auto blocks_bytes = merge_serialized_bytes(request);
                RequestHeader header;
                header.kind = RpcKind::merge;
                header.meta_bytes = meta.size();
                header.blocks_offset = align_up(sizeof(RequestHeader) + meta.size());
                header.blocks_bytes = blocks_bytes;
                const auto total = header.blocks_offset + blocks_bytes;
                char *buffer = client->request_buffer(total);
                std::memcpy(buffer, &header, sizeof(header));
                std::memcpy(buffer + sizeof(header), meta.data(), meta.size());
                std::vector<std::uint64_t> offsets;
                auto status = read_merge_blocks(request, buffer + header.blocks_offset, &offsets);
                if (!status.ok())
                {
                    healthy = true;
                    return status;
                }
                const auto read_ns = elapsed_ns(read_started);

                const auto call_started = std::chrono::steady_clock::now();
                std::shared_ptr<const char> reply;
                std::size_t reply_bytes = 0;
                status = client->call(total, &reply, &reply_bytes);
                if (!status.ok())
                    return status;
                const auto remote_ns = elapsed_ns(call_started);
                healthy = true;
                ReplyHeader answer;
                if (reply_bytes < sizeof(answer))
                    return rocksdb::Status::Corruption("short remote merge reply");
                std::memcpy(&answer, reply.get(), sizeof(answer));
                if (answer.magic != ReplyHeader{}.magic)
                    return rocksdb::Status::Corruption("bad remote merge reply");
                const std::string message(reply.get() + sizeof(answer), answer.message_bytes);
                if (answer.status != 0)
                    return make_status(answer.status, message);
                if (stats != nullptr)
                {
                    *stats = answer.stats;
                    stats->read_ns = read_ns;
                    stats->remote_ns = remote_ns;
                    stats->server_ns = answer.server_ns;
                    stats->sent_bytes = total;
                    stats->received_bytes = reply_bytes;
                }
                *output = std::make_unique<RemoteMergedInput>(std::move(reply), answer);
                return rocksdb::Status::OK();
            }

        private:
            std::unique_ptr<RpcClient> acquire()
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [&] { return !idle_.empty() || created_ < limit_; });
                if (!idle_.empty())
                {
                    auto client = std::move(idle_.back());
                    idle_.pop_back();
                    return client;
                }
                ++created_;
                lock.unlock();
                try
                {
                    return connect_rpc(endpoint_);
                }
                catch (...)
                {
                    const std::lock_guard guard(mutex_);
                    --created_;
                    ready_.notify_one();
                    throw;
                }
            }

            void release(std::unique_ptr<RpcClient> client, bool healthy)
            {
                const std::lock_guard guard(mutex_);
                // A connection that failed mid-call is dropped; the next job reconnects.
                if (healthy && client)
                    idle_.push_back(std::move(client));
                else
                    --created_;
                ready_.notify_one();
            }

            Endpoint endpoint_;
            std::string description_;
            std::size_t limit_ = 2;
            std::mutex mutex_;
            std::condition_variable ready_;
            std::vector<std::unique_ptr<RpcClient>> idle_;
            std::size_t created_ = 0;
        };

        void copy_into(char *base, std::size_t &offset, const void *data, std::size_t bytes)
        {
            if (bytes != 0)
                std::memcpy(base + offset, data, bytes);
            offset = align_up(offset + bytes);
        }

        std::size_t write_reply(ReplySink &sink, ReplyHeader header, const std::string &message,
                                const rocksdb::CompactionMergedInput *output)
        {
            header.message_bytes = message.size();
            header.payload_offset = align_up(sizeof(ReplyHeader) + message.size());
            std::size_t payload = 0;
            const rocksdb::CompactionBuiltOutput *built = output ? output->built_output() : nullptr;
            const char *packed = nullptr;
            const rocksdb::CompactionMergedEntry *packed_entries = nullptr;
            std::vector<rocksdb::CompactionMergedEntry> entries;
            if (built != nullptr)
            {
                header.kind = 1;
                header.block_count = built->block_count;
                header.entries = built->entries;
                for (std::size_t index = 0; index < built->block_count; ++index)
                {
                    const auto &block = built->blocks[index];
                    // Includes each block's trailer gap when the producer left one.
                    header.contents_bytes = std::max<std::uint64_t>(
                        header.contents_bytes,
                        std::min<std::uint64_t>(block.contents_offset + block.contents_bytes + rocksdb::kBuiltTrailerGap,
                                                std::max(built->contents_capacity,
                                                         block.contents_offset + block.contents_bytes)));
                    header.keys_bytes = std::max<std::uint64_t>(
                        header.keys_bytes, block.keys_offset + block.first_key_bytes + block.last_key_bytes);
                    header.hash_count = std::max<std::uint64_t>(header.hash_count, block.hash_offset + block.hash_count);
                }
                payload = align_up(header.block_count * sizeof(rocksdb::CompactionBuiltBlock)) +
                          align_up(header.contents_bytes) + align_up(header.keys_bytes) +
                          align_up(header.hash_count * sizeof(std::uint64_t));
            }
            else if (output != nullptr)
            {
                header.kind = 2;
                header.entries = output->size();
                packed = output->packed_data();
                packed_entries = output->packed_entries();
                if (packed == nullptr || packed_entries == nullptr)
                {
                    // Pack the stream ourselves: key then value for each entry.
                    entries.resize(output->size());
                    std::uint64_t bytes = 0;
                    for (std::size_t index = 0; index < output->size(); ++index)
                    {
                        entries[index] = {bytes, static_cast<std::uint32_t>(output->key(index).size()),
                                          static_cast<std::uint32_t>(output->value(index).size())};
                        bytes += entries[index].key_bytes + entries[index].value_bytes;
                    }
                    header.packed_bytes = bytes;
                }
                else
                {
                    for (std::size_t index = 0; index < output->size(); ++index)
                        header.packed_bytes = std::max<std::uint64_t>(
                            header.packed_bytes, packed_entries[index].offset + packed_entries[index].key_bytes +
                                                     packed_entries[index].value_bytes);
                }
                payload = align_up(header.packed_bytes) + header.entries * sizeof(rocksdb::CompactionMergedEntry);
            }
            const auto total = header.payload_offset + payload;
            char *out = sink.reserve(total);
            std::memcpy(out, &header, sizeof(header));
            std::memcpy(out + sizeof(header), message.data(), message.size());
            std::size_t offset = header.payload_offset;
            if (header.kind == 1)
            {
                copy_into(out, offset, built->blocks, header.block_count * sizeof(rocksdb::CompactionBuiltBlock));
                copy_into(out, offset, built->contents, header.contents_bytes);
                copy_into(out, offset, built->keys, header.keys_bytes);
                copy_into(out, offset, built->hashes, header.hash_count * sizeof(std::uint64_t));
            }
            else if (header.kind == 2 && entries.empty())
            {
                copy_into(out, offset, packed, header.packed_bytes);
                copy_into(out, offset, packed_entries, header.entries * sizeof(rocksdb::CompactionMergedEntry));
            }
            else if (header.kind == 2)
            {
                char *base = out + offset;
                for (std::size_t index = 0; index < entries.size(); ++index)
                {
                    const auto key = output->key(index);
                    const auto value = output->value(index);
                    std::memcpy(base + entries[index].offset, key.data(), key.size());
                    std::memcpy(base + entries[index].offset + key.size(), value.data(), value.size());
                }
                offset = align_up(offset + header.packed_bytes);
                copy_into(out, offset, entries.data(), entries.size() * sizeof(rocksdb::CompactionMergedEntry));
            }
            return total;
        }
    }

    Endpoint parse_endpoint(const std::string &text)
    {
        const auto scheme_end = text.find("://");
        const auto port_start = text.rfind(':');
        if (scheme_end == std::string::npos || port_start == std::string::npos || port_start <= scheme_end + 3)
            throw std::invalid_argument("endpoint must be tcp://host:port or rdma://host:port: " + text);
        Endpoint endpoint;
        endpoint.scheme = text.substr(0, scheme_end);
        endpoint.host = text.substr(scheme_end + 3, port_start - scheme_end - 3);
        const auto port = std::stoul(text.substr(port_start + 1));
        if (port > 65535 || (endpoint.scheme != "tcp" && endpoint.scheme != "rdma"))
            throw std::invalid_argument("endpoint must be tcp://host:port or rdma://host:port: " + text);
        endpoint.port = static_cast<std::uint16_t>(port);
        return endpoint;
    }

    std::string encode_merge_request(const rocksdb::CompactionMergeRequest &request)
    {
        Writer w;
        w.put<std::uint32_t>(kMetaVersion);
        w.put<std::uint64_t>(request.files.size());
        for (const auto &file : request.files)
        {
            w.put<std::uint64_t>(file.file_number);
            w.put<std::uint64_t>(file.file_size);
            w.put<std::int32_t>(file.level);
            w.put<std::uint64_t>(file.blocks.size());
            for (const auto &block : file.blocks)
            {
                w.put<std::uint64_t>(block.offset);
                w.put<std::uint32_t>(block.size);
                w.put<std::uint32_t>(block.checksum_modifier);
            }
        }
        w.put<std::uint8_t>(request.has_start);
        w.put<std::uint8_t>(request.has_end);
        w.put_string(request.start_user_key);
        w.put_string(request.end_user_key);
        w.put<std::uint8_t>(request.drop_hidden);
        w.put<std::uint64_t>(request.snapshots.size());
        for (const auto snapshot : request.snapshots)
            w.put<std::uint64_t>(snapshot);
        const auto &o = request.output;
        w.put<std::uint8_t>(o.enabled);
        w.put<std::uint32_t>(o.block_bytes);
        w.put<std::uint32_t>(o.block_size_deviation);
        w.put<std::uint32_t>(o.restart_interval);
        w.put<std::uint32_t>(o.max_compressed_bytes_per_kb);
        w.put<std::uint8_t>(o.lz4);
        w.put<std::uint8_t>(o.zstd);
        w.put<std::uint8_t>(o.zero_sequences);
        w.put<std::uint64_t>(o.earliest_snapshot);
        w.put<std::uint8_t>(o.filter_hashes);
        w.put<std::uint8_t>(o.bottommost);
        w.put<std::uint64_t>(o.earliest_snapshot_seq);
        w.put<std::uint8_t>(o.check_beyond_ranges);
        w.put<std::uint64_t>(o.beyond_ranges.size());
        for (const auto &[smallest, largest] : o.beyond_ranges)
        {
            w.put_string(smallest);
            w.put_string(largest);
        }
        return w.take();
    }

    rocksdb::Status decode_merge_request(const char *data, std::size_t bytes, rocksdb::CompactionMergeRequest *request)
    {
        try
        {
            Reader r(data, bytes);
            if (r.get<std::uint32_t>() != kMetaVersion)
                return rocksdb::Status::NotSupported("merge request metadata version");
            request->files.resize(r.get<std::uint64_t>());
            for (auto &file : request->files)
            {
                file.file_number = r.get<std::uint64_t>();
                file.file_size = r.get<std::uint64_t>();
                file.level = r.get<std::int32_t>();
                file.blocks.resize(r.get<std::uint64_t>());
                for (auto &block : file.blocks)
                {
                    block.offset = r.get<std::uint64_t>();
                    block.size = r.get<std::uint32_t>();
                    block.checksum_modifier = r.get<std::uint32_t>();
                }
            }
            request->has_start = r.get<std::uint8_t>() != 0;
            request->has_end = r.get<std::uint8_t>() != 0;
            request->start_user_key = r.get_string();
            request->end_user_key = r.get_string();
            request->drop_hidden = r.get<std::uint8_t>() != 0;
            request->snapshots.resize(r.get<std::uint64_t>());
            for (auto &snapshot : request->snapshots)
                snapshot = r.get<std::uint64_t>();
            auto &o = request->output;
            o.enabled = r.get<std::uint8_t>() != 0;
            o.block_bytes = r.get<std::uint32_t>();
            o.block_size_deviation = r.get<std::uint32_t>();
            o.restart_interval = r.get<std::uint32_t>();
            o.max_compressed_bytes_per_kb = r.get<std::uint32_t>();
            o.lz4 = r.get<std::uint8_t>() != 0;
            o.zstd = r.get<std::uint8_t>() != 0;
            o.zero_sequences = r.get<std::uint8_t>() != 0;
            o.earliest_snapshot = r.get<std::uint64_t>();
            o.filter_hashes = r.get<std::uint8_t>() != 0;
            o.bottommost = r.get<std::uint8_t>() != 0;
            o.earliest_snapshot_seq = r.get<std::uint64_t>();
            o.check_beyond_ranges = r.get<std::uint8_t>() != 0;
            o.beyond_ranges.resize(r.get<std::uint64_t>());
            for (auto &[smallest, largest] : o.beyond_ranges)
            {
                smallest = r.get_string();
                largest = r.get_string();
            }
            return rocksdb::Status::OK();
        }
        catch (const std::exception &error)
        {
            return rocksdb::Status::Corruption(error.what());
        }
    }

    PreloadedMergeBlocks::PreloadedMergeBlocks(const char *data, std::size_t bytes)
        : data_(data), bytes_(bytes), previous_(t_preloaded)
    {
        t_preloaded = this;
    }
    PreloadedMergeBlocks::~PreloadedMergeBlocks() { t_preloaded = previous_; }
    const PreloadedMergeBlocks *PreloadedMergeBlocks::current() { return t_preloaded; }

    std::shared_ptr<BatchTransform> make_remote_transform(const std::string &endpoint)
    {
        std::string text = endpoint;
        if (text.empty())
        {
            const char *configured = std::getenv("LSM_REMOTE_ENDPOINT");
            if (configured == nullptr)
                throw std::invalid_argument(
                    "remote_merge needs LSM_REMOTE_ENDPOINT (tcp://host:port or rdma://host:port)");
            text = configured;
        }
        auto transform = std::make_shared<RemoteTransform>(parse_endpoint(text));
        transform->enable_merged_input();
        return transform;
    }

    std::string fetch_remote_statistics(const std::string &endpoint)
    {
        auto client = connect_rpc(parse_endpoint(endpoint));
        RequestHeader header;
        header.kind = RpcKind::statistics;
        header.blocks_offset = align_up(sizeof(header));
        std::memcpy(client->request_buffer(header.blocks_offset), &header, sizeof(header));
        std::shared_ptr<const char> reply;
        std::size_t reply_bytes = 0;
        const auto status = client->call(header.blocks_offset, &reply, &reply_bytes);
        if (!status.ok())
            throw std::runtime_error("remote statistics: " + status.ToString());
        ReplyHeader answer;
        std::memcpy(&answer, reply.get(), sizeof(answer));
        return std::string(reply.get() + sizeof(answer), answer.message_bytes);
    }

    RpcHandler make_merge_handler(std::shared_ptr<BatchTransform> transform)
    {
        return [transform](const char *data, std::size_t bytes, ReplySink &sink) -> std::size_t {
            const auto started = std::chrono::steady_clock::now();
            ReplyHeader reply;
            RequestHeader header;
            const auto fail = [&](rocksdb::Status::Code code, const std::string &message) {
                reply.status = static_cast<std::uint32_t>(code);
                return write_reply(sink, reply, message, nullptr);
            };
            if (bytes < sizeof(header))
                return fail(rocksdb::Status::Code::kCorruption, "short request");
            std::memcpy(&header, data, sizeof(header));
            if (header.magic != RequestHeader{}.magic)
                return fail(rocksdb::Status::Code::kCorruption, "bad request magic");
            if (header.kind == RpcKind::statistics)
                return write_reply(sink, reply, transform->statistics().dump(), nullptr);
            if (header.kind == RpcKind::echo)
            {
                const auto total = std::max<std::size_t>(header.meta_bytes, sizeof(ReplyHeader));
                std::memcpy(sink.reserve(total), &reply, sizeof(reply));
                return total;
            }
            if (header.kind != RpcKind::merge || header.blocks_offset + header.blocks_bytes > bytes ||
                sizeof(header) + header.meta_bytes > header.blocks_offset)
                return fail(rocksdb::Status::Code::kCorruption, "malformed request");
            rocksdb::CompactionMergeRequest request;
            auto status = decode_merge_request(data + sizeof(header), header.meta_bytes, &request);
            std::unique_ptr<rocksdb::CompactionMergedInput> output;
            if (status.ok())
            {
                const PreloadedMergeBlocks preloaded(data + header.blocks_offset, header.blocks_bytes);
                status = transform->merge_with_stats(request, &output, &reply.stats);
            }
            if (!status.ok())
            {
                reply.subcode = static_cast<std::uint32_t>(status.subcode());
                return fail(status.code(), status.ToString());
            }
            reply.dropped = output->dropped();
            reply.server_ns = elapsed_ns(started);
            return write_reply(sink, reply, {}, output.get());
        };
    }
}
