#include "lsm_lab/merged_input.hpp"
#include "lsm_lab/remote_merge.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <future>
#include <thread>

#include <pthread.h>
#include <cstring>
#include <fcntl.h>
#include <lz4.h>
#include <zstd.h>
#include <unistd.h>

#include "util/coding.h"
#include "util/crc32c.h"
#include "util/hash.h"

namespace lsm_lab
{
    namespace
    {
        constexpr std::uint32_t kTrailer = 5;
        constexpr std::uint32_t kHashIndexBit = 1u << 31;
        constexpr std::uint32_t kReservedBit = 1u << 30;
        constexpr std::uint32_t kUniformKeysBit = 1u << 29;
        constexpr std::uint32_t kSeparatedKVBit = 1u << 28;
        constexpr unsigned char kTypeDeletion = 0x0;
        constexpr unsigned char kTypeValue = 0x1;

        class FileDescriptor
        {
        public:
            explicit FileDescriptor(const std::string &path) : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {}
            ~FileDescriptor()
            {
                if (fd_ >= 0)
                    ::close(fd_);
            }
            FileDescriptor(const FileDescriptor &) = delete;
            FileDescriptor &operator=(const FileDescriptor &) = delete;
            int get() const { return fd_; }

        private:
            int fd_;
        };

        rocksdb::Status read_exact(int fd, char *destination, std::uint64_t bytes, std::uint64_t offset)
        {
            while (bytes > 0)
            {
                const auto got = ::pread(fd, destination, bytes, static_cast<off_t>(offset));
                if (got < 0)
                {
                    if (errno == EINTR)
                        continue;
                    return rocksdb::Status::IOError("merged-input pread failed", std::strerror(errno));
                }
                if (got == 0)
                    return rocksdb::Status::Corruption("merged-input read past end of file");
                destination += got;
                bytes -= static_cast<std::uint64_t>(got);
                offset += static_cast<std::uint64_t>(got);
            }
            return rocksdb::Status::OK();
        }

        struct Candidate
        {
            const char *key;
            std::uint32_t key_bytes;
            const char *value;
            std::uint32_t value_bytes;
        };

        bool internal_key_less(const Candidate &left, const Candidate &right)
        {
            const auto left_user = left.key_bytes - 8;
            const auto right_user = right.key_bytes - 8;
            const auto common = std::min(left_user, right_user);
            const auto order = std::memcmp(left.key, right.key, common);
            if (order != 0)
                return order < 0;
            if (left_user != right_user)
                return left_user < right_user;
            // Same user key: larger packed (sequence << 8 | type) sorts first.
            return rocksdb::DecodeFixed64(left.key + left_user) > rocksdb::DecodeFixed64(right.key + right_user);
        }

        // Index of the earliest snapshot that can see sequence (snapshots ascending);
        // snapshots.size() when no snapshot is at or above it.
        std::size_t snapshot_stripe(const std::vector<std::uint64_t> &snapshots, std::uint64_t sequence)
        {
            return static_cast<std::size_t>(std::lower_bound(snapshots.begin(), snapshots.end(), sequence) -
                                            snapshots.begin());
        }

        bool same_user_key(const Candidate &left, const Candidate &right)
        {
            return left.key_bytes == right.key_bytes &&
                   std::memcmp(left.key, right.key, left.key_bytes - 8) == 0;
        }

        int compare_user(const char *key, std::uint32_t bytes, const std::string &bound)
        {
            const auto common = std::min<std::size_t>(bytes, bound.size());
            const auto order = std::memcmp(key, bound.data(), common);
            if (order != 0)
                return order;
            return bytes < bound.size() ? -1 : (bytes > bound.size() ? 1 : 0);
        }

        // Parses one decoded data block, materializing full internal keys into
        // key_arena. Values point into the decoded block.
        rocksdb::Status parse_block(const std::string &block, std::string &key_arena,
                                    std::vector<std::pair<std::uint64_t, Candidate>> *entries)
        {
            if (block.size() < 4)
                return rocksdb::Status::Corruption("merged-input data block too small");
            auto packed = rocksdb::DecodeFixed32(block.data() + block.size() - 4);
            if (packed & (kHashIndexBit | kReservedBit | kSeparatedKVBit))
                return rocksdb::Status::NotSupported("merged input requires binary-search data blocks without separated values");
            packed &= ~kUniformKeysBit;
            const std::uint64_t restarts_bytes = 4ULL * packed + 4;
            if (packed == 0 || restarts_bytes > block.size())
                return rocksdb::Status::Corruption("merged-input data block has invalid restart array");
            const char *p = block.data();
            const char *limit = block.data() + block.size() - restarts_bytes;
            std::string previous;
            while (p < limit)
            {
                std::uint32_t shared = 0, non_shared = 0, value_bytes = 0;
                if (limit - p >= 3 && (static_cast<unsigned char>(p[0]) | static_cast<unsigned char>(p[1]) |
                                       static_cast<unsigned char>(p[2])) < 128)
                {
                    shared = static_cast<unsigned char>(p[0]);
                    non_shared = static_cast<unsigned char>(p[1]);
                    value_bytes = static_cast<unsigned char>(p[2]);
                    p += 3;
                }
                else if ((p = rocksdb::GetVarint32Ptr(p, limit, &shared)) == nullptr ||
                         (p = rocksdb::GetVarint32Ptr(p, limit, &non_shared)) == nullptr ||
                         (p = rocksdb::GetVarint32Ptr(p, limit, &value_bytes)) == nullptr)
                {
                    return rocksdb::Status::Corruption("merged-input entry header truncated");
                }
                if (shared > previous.size() || static_cast<std::uint64_t>(limit - p) < std::uint64_t{non_shared} + value_bytes)
                    return rocksdb::Status::Corruption("merged-input entry exceeds block");
                previous.resize(shared);
                previous.append(p, non_shared);
                p += non_shared;
                if (previous.size() < 8)
                    return rocksdb::Status::Corruption("merged-input internal key too short");
                const auto type = static_cast<unsigned char>(previous[previous.size() - 8]);
                if (type != kTypeValue && type != kTypeDeletion)
                    return rocksdb::Status::NotSupported("merged input supports only Put and Delete entries");
                const auto key_offset = key_arena.size();
                key_arena.append(previous);
                entries->push_back({key_offset, {nullptr, static_cast<std::uint32_t>(previous.size()), p, value_bytes}});
                p += value_bytes;
            }
            return rocksdb::Status::OK();
        }

        // Uncompressed size of a serialized block (contents plus trailer), from its type byte and,
        // for compressed blocks, the varint32 size prefix.
        rocksdb::Status block_decoded_bytes(const char *serialized, const rocksdb::CompactionMergeBlock &block,
                                            std::uint32_t *decoded_bytes)
        {
            const auto type = static_cast<unsigned char>(serialized[block.size]);
            if (type == 0)
            {
                *decoded_bytes = static_cast<std::uint32_t>(block.size);
                return rocksdb::Status::OK();
            }
            if (type != 4 && type != 7)
                return rocksdb::Status::NotSupported("merged input supports only uncompressed, LZ4 and ZSTD blocks");
            rocksdb::Slice compressed(serialized, block.size);
            if (!rocksdb::GetVarint32(&compressed, decoded_bytes) || *decoded_bytes == 0 ||
                *decoded_bytes > rocksdb::CompactionBlockTransform::max_block_bytes)
                return rocksdb::Status::Corruption("merged-input invalid compressed size prefix");
            return rocksdb::Status::OK();
        }

        // Verifies the block checksum and writes its decoded_bytes uncompressed bytes to destination.
        rocksdb::Status decode_block_into(const char *serialized, const rocksdb::CompactionMergeBlock &block,
                                          char *destination, std::uint32_t decoded_bytes)
        {
            const auto type = static_cast<unsigned char>(serialized[block.size]);
            const auto stored = rocksdb::DecodeFixed32(serialized + block.size + 1) - block.checksum_modifier;
            if (rocksdb::crc32c::Mask(rocksdb::crc32c::Value(serialized, block.size + 1)) != stored)
                return rocksdb::Status::Corruption("merged-input block CRC32C mismatch");
            if (type == 0)
            {
                std::memcpy(destination, serialized, block.size);
                return rocksdb::Status::OK();
            }
            rocksdb::Slice compressed(serialized, block.size);
            std::uint32_t prefix = 0;
            rocksdb::GetVarint32(&compressed, &prefix);
            if (type == 4)
            {
                if (LZ4_decompress_safe(compressed.data(), destination, static_cast<int>(compressed.size()),
                                        static_cast<int>(decoded_bytes)) != static_cast<int>(decoded_bytes))
                    return rocksdb::Status::Corruption("merged-input LZ4 decoding failed");
            }
            else if (ZSTD_decompress(destination, decoded_bytes, compressed.data(), compressed.size()) != decoded_bytes)
            {
                return rocksdb::Status::Corruption("merged-input ZSTD decoding failed");
            }
            return rocksdb::Status::OK();
        }

        rocksdb::Status decode_block(const char *serialized, const rocksdb::CompactionMergeBlock &block,
                                     std::string *decoded)
        {
            std::uint32_t decoded_bytes = 0;
            auto status = block_decoded_bytes(serialized, block, &decoded_bytes);
            if (!status.ok())
                return status;
            decoded->resize(decoded_bytes);
            return decode_block_into(serialized, block, decoded->data(), decoded_bytes);
        }

        std::size_t merge_read_threads()
        {
            static const std::size_t configured = [] {
                const char *value = std::getenv("LSM_MERGE_READ_THREADS");
                return value == nullptr ? std::size_t{4} : std::max<std::size_t>(1, std::strtoull(value, nullptr, 10));
            }();
            return configured;
        }

        // Runs work(index) for every index in [0, count) on up to threads threads (the caller is one of
        // them) and returns the first error.
        template <typename Work>
        rocksdb::Status run_parallel(std::size_t count, std::size_t threads, const Work &work)
        {
            threads = std::min(threads, std::max<std::size_t>(1, count));
            std::atomic<std::size_t> next{0};
            const auto worker = [&]() -> rocksdb::Status {
                for (auto index = next++; index < count; index = next++)
                {
                    auto status = work(index);
                    if (!status.ok())
                        return status;
                }
                return rocksdb::Status::OK();
            };
            std::vector<std::future<rocksdb::Status>> helpers;
            for (std::size_t helper = 1; helper < threads; ++helper)
                helpers.push_back(std::async(std::launch::async, [&worker] {
#if defined(__linux__)
                    pthread_setname_np(pthread_self(), "lsm-merge-read");
#endif
                    return worker();
                }));
            auto status = worker();
            for (auto &helper : helpers)
            {
                auto helper_status = helper.get();
                if (status.ok())
                    status = helper_status;
            }
            return status;
        }
    }

    std::uint64_t merge_serialized_bytes(const rocksdb::CompactionMergeRequest &request)
    {
        std::uint64_t total = 0;
        for (const auto &file : request.files)
            for (const auto &block : file.blocks)
                total += block.size + kTrailer;
        return total;
    }

    namespace
    {
        rocksdb::Status read_file_blocks(const rocksdb::CompactionMergeFile &file, char *destination,
                                         std::uint64_t position, std::uint64_t *offsets)
        {
            FileDescriptor fd(file.path);
            if (fd.get() < 0)
                return rocksdb::Status::IOError("merged-input open failed", file.path);
            // Blocks are listed in file order; coalesce contiguous ranges into one read.
            std::size_t index = 0;
            while (index < file.blocks.size())
            {
                auto end = index;
                std::uint64_t bytes = file.blocks[index].size + kTrailer;
                while (end + 1 < file.blocks.size() &&
                       file.blocks[end + 1].offset == file.blocks[end].offset + file.blocks[end].size + kTrailer)
                {
                    ++end;
                    bytes += file.blocks[end].size + kTrailer;
                }
                auto status = read_exact(fd.get(), destination + position, bytes, file.blocks[index].offset);
                if (!status.ok())
                    return status;
                for (auto block = index; block <= end; ++block)
                {
                    *offsets++ = position;
                    position += file.blocks[block].size + kTrailer;
                }
                index = end + 1;
            }
            return rocksdb::Status::OK();
        }
    }

    rocksdb::Status read_merge_blocks(const rocksdb::CompactionMergeRequest &request, char *destination,
                                      std::vector<std::uint64_t> *offsets)
    {
        // Files are read concurrently (LSM_MERGE_READ_THREADS, default 4) into
        // disjoint, precomputed ranges of destination.
        std::vector<std::uint64_t> file_position(request.files.size());
        std::vector<std::size_t> file_first_block(request.files.size());
        std::uint64_t position = 0;
        std::size_t blocks = 0;
        for (std::size_t index = 0; index < request.files.size(); ++index)
        {
            file_position[index] = position;
            file_first_block[index] = blocks;
            for (const auto &block : request.files[index].blocks)
                position += block.size + kTrailer;
            blocks += request.files[index].blocks.size();
        }
        offsets->assign(blocks, 0);
        if (const auto *preloaded = PreloadedMergeBlocks::current())
        {
            // Remote compaction server: the blocks arrived over the network in request order.
            if (preloaded->bytes() != position)
                return rocksdb::Status::Corruption("preloaded merge input has the wrong size");
            std::uint64_t offset = 0;
            std::size_t index = 0;
            for (const auto &file : request.files)
                for (const auto &block : file.blocks)
                {
                    (*offsets)[index++] = offset;
                    offset += block.size + kTrailer;
                }
            constexpr std::uint64_t kChunk = 64ULL << 20;
            const auto chunks = static_cast<std::size_t>((position + kChunk - 1) / kChunk);
            return run_parallel(chunks, merge_read_threads(), [&](std::size_t chunk) {
                const auto begin = chunk * kChunk;
                std::memcpy(destination + begin, preloaded->data() + begin, std::min(kChunk, position - begin));
                return rocksdb::Status::OK();
            });
        }
        return run_parallel(request.files.size(), merge_read_threads(), [&](std::size_t index) {
            return read_file_blocks(request.files[index], destination, file_position[index],
                                    offsets->data() + file_first_block[index]);
        });
    }

    rocksdb::Status decoded_merge_layout(const rocksdb::CompactionMergeRequest &request, const char *serialized,
                                         const std::vector<std::uint64_t> &offsets, std::uint64_t alignment,
                                         std::vector<std::uint64_t> *decoded_offsets,
                                         std::vector<std::uint32_t> *decoded_sizes, std::uint64_t *decoded_total)
    {
        decoded_offsets->assign(offsets.size(), 0);
        decoded_sizes->assign(offsets.size(), 0);
        std::uint64_t total = 0;
        std::size_t index = 0;
        for (const auto &file : request.files)
            for (const auto &block : file.blocks)
            {
                std::uint32_t bytes = 0;
                auto status = block_decoded_bytes(serialized + offsets[index], block, &bytes);
                if (!status.ok())
                    return status;
                total = (total + alignment - 1) / alignment * alignment;
                (*decoded_offsets)[index] = total;
                (*decoded_sizes)[index++] = bytes;
                total += bytes;
            }
        *decoded_total = total;
        return rocksdb::Status::OK();
    }

    rocksdb::Status decode_merge_blocks(const rocksdb::CompactionMergeRequest &request, const char *serialized,
                                        const std::vector<std::uint64_t> &offsets,
                                        const std::vector<std::uint64_t> &decoded_offsets,
                                        const std::vector<std::uint32_t> &decoded_sizes, std::size_t first,
                                        std::size_t last, char *destination)
    {
        std::vector<const rocksdb::CompactionMergeBlock *> blocks;
        blocks.reserve(offsets.size());
        for (const auto &file : request.files)
            for (const auto &block : file.blocks)
                blocks.push_back(&block);
        const auto base = decoded_offsets[first];
        return run_parallel(last - first, merge_read_threads(), [&](std::size_t offset) {
            const auto index = first + offset;
            return decode_block_into(serialized + offsets[index], *blocks[index],
                                     destination + (decoded_offsets[index] - base), decoded_sizes[index]);
        });
    }

    rocksdb::Status execute_cpu_merge(const rocksdb::CompactionMergeRequest &request,
                                      std::unique_ptr<rocksdb::CompactionMergedInput> *output, MergeRunStats *stats)
    {
        const auto read_started = std::chrono::steady_clock::now();
        std::string serialized(merge_serialized_bytes(request), '\0');
        std::vector<std::uint64_t> offsets;
        auto status = read_merge_blocks(request, serialized.data(), &offsets);
        if (!status.ok())
            return status;
        const auto compute_started = std::chrono::steady_clock::now();
        std::vector<std::unique_ptr<std::string>> decoded_blocks;
        std::vector<std::unique_ptr<std::string>> key_arenas;
        std::vector<Candidate> candidates;
        std::size_t block_index = 0;
        std::uint64_t decoded_bytes = 0;
        for (const auto &file : request.files)
        {
            for (const auto &block : file.blocks)
            {
                auto decoded = std::make_unique<std::string>();
                status = decode_block(serialized.data() + offsets[block_index++], block, decoded.get());
                if (!status.ok())
                    return status;
                decoded_bytes += decoded->size();
                auto arena = std::make_unique<std::string>();
                std::vector<std::pair<std::uint64_t, Candidate>> parsed;
                status = parse_block(*decoded, *arena, &parsed);
                if (!status.ok())
                    return status;
                for (auto &[key_offset, candidate] : parsed)
                {
                    candidate.key = arena->data() + key_offset;
                    const auto user_bytes = candidate.key_bytes - 8;
                    if (request.has_start && compare_user(candidate.key, user_bytes, request.start_user_key) < 0)
                        continue;
                    if (request.has_end && compare_user(candidate.key, user_bytes, request.end_user_key) >= 0)
                        continue;
                    candidates.push_back(candidate);
                }
                decoded_blocks.push_back(std::move(decoded));
                key_arenas.push_back(std::move(arena));
            }
        }
        std::sort(candidates.begin(), candidates.end(), internal_key_less);
        rocksdb::CompactionMergeDropped dropped;
        if (request.drop_hidden && !candidates.empty())
        {
            std::vector<Candidate> kept;
            kept.reserve(candidates.size());
            std::size_t previous_stripe = 0;
            for (std::size_t index = 0; index < candidates.size(); ++index)
            {
                const auto &candidate = candidates[index];
                const auto user_bytes = candidate.key_bytes - 8;
                const auto sequence = rocksdb::DecodeFixed64(candidate.key + user_bytes) >> 8;
                const auto stripe = snapshot_stripe(request.snapshots, sequence);
                const bool hidden = index > 0 && same_user_key(candidates[index - 1], candidate) &&
                                    stripe == previous_stripe;
                previous_stripe = stripe;
                if (hidden)
                {
                    ++dropped.entries;
                    dropped.deletions += static_cast<unsigned char>(candidate.key[user_bytes]) == kTypeDeletion;
                    dropped.key_bytes += candidate.key_bytes;
                    dropped.value_bytes += candidate.value_bytes;
                    continue;
                }
                kept.push_back(candidate);
            }
            candidates.swap(kept);
        }
        std::uint64_t packed_bytes = 0;
        for (const auto &candidate : candidates)
            packed_bytes += candidate.key_bytes + candidate.value_bytes;
        std::shared_ptr<char> data(new char[std::max<std::uint64_t>(packed_bytes, 1)], std::default_delete<char[]>());
        std::vector<MergedEntry> entries;
        bool built = false;
        entries.reserve(candidates.size());
        std::uint64_t position = 0;
        for (const auto &candidate : candidates)
        {
            std::memcpy(data.get() + position, candidate.key, candidate.key_bytes);
            std::memcpy(data.get() + position + candidate.key_bytes, candidate.value, candidate.value_bytes);
            entries.push_back({position, candidate.key_bytes, candidate.value_bytes});
            position += candidate.key_bytes + candidate.value_bytes;
        }
        if (request.output.enabled && !entries.empty())
        {
            status = build_output_blocks(data.get(), entries.data(), entries.size(), request.output, dropped, output,
                                         &built, stats);
            if (!status.ok())
                return status;
        }
        if (stats)
        {
            stats->blocks = block_index;
            stats->input_bytes = serialized.size();
            stats->decoded_bytes = decoded_bytes;
            stats->entries = entries.size();
            stats->dropped = dropped.entries;
            stats->read_ns = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(compute_started - read_started).count());
            stats->device_ns = 0;
        }
        if (!built)
            *output = std::make_unique<PackedMergedInput>(std::move(data), std::move(entries), dropped);
        return rocksdb::Status::OK();
    }

    namespace
    {
        std::uint32_t varint_bytes(std::uint64_t value)
        {
            std::uint32_t bytes = 1;
            while (value >= 128)
            {
                value >>= 7;
                ++bytes;
            }
            return bytes;
        }

        struct BuiltBuffers
        {
            std::vector<rocksdb::CompactionBuiltBlock> blocks;
            std::string contents;
            std::string keys;
            std::vector<std::uint64_t> hashes;
        };
    }

    rocksdb::Status build_output_blocks(const char *packed, const MergedEntry *entries, std::size_t count,
                                       const rocksdb::CompactionOutputSpec &spec,
                                       rocksdb::CompactionMergeDropped dropped,
                                       std::unique_ptr<rocksdb::CompactionMergedInput> *output, bool *built,
                                       MergeRunStats *stats)
    {
        *built = false;
        const auto user_key = [&](std::size_t index) {
            return rocksdb::Slice(packed + entries[index].offset, entries[index].key_bytes - 8);
        };
        // Deletion rules of CompactionIterator, applied after hidden versions are gone.
        const auto key_may_exist_beyond = [&](const rocksdb::Slice &key) {
            if (spec.bottommost)
                return false;
            if (!spec.check_beyond_ranges)
                return true;
            auto after = std::upper_bound(spec.beyond_ranges.begin(), spec.beyond_ranges.end(), key,
                                          [](const rocksdb::Slice &value, const auto &range) {
                                              return value.compare(rocksdb::Slice(range.first)) < 0;
                                          });
            if (after == spec.beyond_ranges.begin())
                return false;
            --after;
            return key.compare(rocksdb::Slice(after->second)) <= 0;
        };
        std::vector<MergedEntry> kept;
        kept.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto &entry = entries[index];
            const char *key = packed + entry.offset;
            const auto user_bytes = entry.key_bytes - 8;
            const auto trailer = rocksdb::DecodeFixed64(key + user_bytes);
            const auto type = static_cast<unsigned char>(trailer & 0xff);
            if ((type != kTypeValue && type != kTypeDeletion) || user_bytes > 240)
                return rocksdb::Status::OK();
            if (type == kTypeDeletion)
            {
                const auto sequence = trailer >> 8;
                bool drop = sequence <= spec.earliest_snapshot_seq && !key_may_exist_beyond(user_key(index));
                if (!drop && spec.bottommost)
                    drop = index + 1 == count || user_key(index + 1) != user_key(index);
                if (drop)
                {
                    ++dropped.entries;
                    ++dropped.deletions;
                    dropped.key_bytes += entry.key_bytes;
                    continue;
                }
            }
            kept.push_back(entry);
        }
        entries = kept.data();
        count = kept.size();

        // Output internal keys (after sequence zeroing) and per-entry metadata.
        std::vector<std::string> keys(count);
        std::vector<std::uint64_t> sequences(count);
        std::vector<unsigned char> deletion(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto &entry = entries[index];
            const char *key = packed + entry.offset;
            const auto user_bytes = entry.key_bytes - 8;
            const auto trailer = rocksdb::DecodeFixed64(key + user_bytes);
            const auto type = static_cast<unsigned char>(trailer & 0xff);
            auto sequence = trailer >> 8;
            if (type == kTypeValue && spec.zero_sequences && sequence <= spec.earliest_snapshot)
                sequence = 0;
            sequences[index] = sequence;
            deletion[index] = type == kTypeDeletion;
            keys[index].assign(key, user_bytes);
            rocksdb::PutFixed64(&keys[index], (sequence << 8) | type);
        }
        const auto shared_with_previous = [&](std::size_t index) -> std::uint32_t {
            if (index == 0)
                return 0;
            return static_cast<std::uint32_t>(rocksdb::Slice(keys[index]).difference_offset(keys[index - 1]));
        };

        // Block starts: RocksDB FlushBlockBySizePolicy over BlockBuilder's size
        // estimate, plus a forced start every kBuiltOutputChunkEntries entries.
        const std::uint64_t block_bytes = spec.block_bytes;
        const std::uint64_t deviation_limit = (block_bytes * (100 - spec.block_size_deviation) + 99) / 100;
        const std::uint32_t interval = spec.restart_interval;
        std::vector<std::size_t> starts;
        {
            std::uint64_t estimate = 8;
            std::uint32_t counter = 0;
            bool empty = true;
            for (std::size_t index = 0; index < count; ++index)
            {
                const std::uint64_t key_bytes = keys[index].size();
                const std::uint64_t value_bytes = entries[index].value_bytes;
                bool flush = index % rocksdb::CompactionOutputSpec::kBuiltOutputChunkEntries == 0;
                if (!flush && !empty)
                {
                    if (estimate >= block_bytes)
                        flush = true;
                    else if (deviation_limit != 0)
                    {
                        const auto after = estimate + key_bytes + value_bytes + (counter >= interval ? 4 : 0) + 4 +
                                           varint_bytes(key_bytes) + varint_bytes(value_bytes);
                        flush = after > block_bytes && estimate > deviation_limit;
                    }
                }
                if (flush)
                {
                    starts.push_back(index);
                    estimate = 8;
                    counter = 0;
                    empty = true;
                }
                std::uint32_t shared = 0;
                if (counter >= interval)
                {
                    estimate += 4;
                    counter = 0;
                }
                else if (!empty)
                {
                    shared = shared_with_previous(index);
                }
                const auto non_shared = key_bytes - shared;
                estimate += varint_bytes(shared) + varint_bytes(non_shared) + varint_bytes(value_bytes) + non_shared +
                            value_bytes;
                ++counter;
                empty = false;
            }
        }

        auto buffers = std::make_shared<BuiltBuffers>();
        buffers->blocks.reserve(starts.size());
        std::string raw;
        std::string compressed;
        for (std::size_t block = 0; block < starts.size(); ++block)
        {
            const auto begin = starts[block];
            const auto end = block + 1 < starts.size() ? starts[block + 1] : count;
            raw.clear();
            std::vector<std::uint32_t> restarts;
            rocksdb::CompactionBuiltBlock out;
            out.smallest_seqno = ~0ULL;
            out.hash_offset = buffers->hashes.size();
            for (auto index = begin; index < end; ++index)
            {
                const auto position = index - begin;
                std::uint32_t shared = 0;
                if (position % interval == 0)
                    restarts.push_back(static_cast<std::uint32_t>(raw.size()));
                else
                    shared = shared_with_previous(index);
                const auto &key = keys[index];
                const auto value_bytes = entries[index].value_bytes;
                rocksdb::PutVarint32(&raw, shared);
                rocksdb::PutVarint32(&raw, static_cast<std::uint32_t>(key.size() - shared));
                rocksdb::PutVarint32(&raw, value_bytes);
                raw.append(key.data() + shared, key.size() - shared);
                raw.append(packed + entries[index].offset + entries[index].key_bytes, value_bytes);
                out.raw_key_bytes += key.size();
                out.raw_value_bytes += value_bytes;
                out.deletions += deletion[index];
                out.smallest_seqno = std::min(out.smallest_seqno, sequences[index]);
                out.largest_seqno = std::max(out.largest_seqno, sequences[index]);
                const auto user_bytes = key.size() - 8;
                if (spec.filter_hashes &&
                    (index == 0 || keys[index - 1].size() != key.size() ||
                     std::memcmp(keys[index - 1].data(), key.data(), user_bytes) != 0))
                    buffers->hashes.push_back(rocksdb::Hash64(key.data(), user_bytes));
            }
            for (const auto restart : restarts)
                rocksdb::PutFixed32(&raw, restart);
            rocksdb::PutFixed32(&raw, static_cast<std::uint32_t>(restarts.size()));

            out.entries = static_cast<std::uint32_t>(end - begin);
            out.hash_count = static_cast<std::uint32_t>(buffers->hashes.size() - out.hash_offset);
            out.uncompressed_bytes = static_cast<std::uint32_t>(raw.size());
            out.contents_offset = buffers->contents.size();
            bool use_compressed = false;
            const char codec = spec.zstd ? 7 : (spec.lz4 ? 4 : 0);
            // Earlier-design emulation: compression and its checksum are left to the table builder.
            static const bool cpu_compress = std::getenv("LSM_GPU_CPU_COMPRESS") != nullptr;
            const bool host_compress = cpu_compress && codec != 0;
            if (codec != 0 && !host_compress)
            {
                compressed.clear();
                rocksdb::PutVarint32(&compressed, static_cast<std::uint32_t>(raw.size()));
                const auto prefix = compressed.size();
                std::int64_t body = 0;
                if (codec == 4)
                {
                    compressed.resize(prefix + LZ4_compressBound(static_cast<int>(raw.size())));
                    body = LZ4_compress_default(raw.data(), compressed.data() + prefix, static_cast<int>(raw.size()),
                                                static_cast<int>(compressed.size() - prefix));
                }
                else
                {
                    compressed.resize(prefix + ZSTD_compressBound(raw.size()));
                    const auto written = ZSTD_compress(compressed.data() + prefix, compressed.size() - prefix, raw.data(),
                                                       raw.size(), 3);
                    body = ZSTD_isError(written) ? 0 : static_cast<std::int64_t>(written);
                }
                const auto limit = (static_cast<std::uint64_t>(spec.max_compressed_bytes_per_kb) * raw.size()) >> 10;
                if (body > 0 && prefix + static_cast<std::uint64_t>(body) <= limit)
                {
                    compressed.resize(prefix + static_cast<std::size_t>(body));
                    use_compressed = true;
                }
            }
            const std::string &contents = use_compressed ? compressed : raw;
            const char type = use_compressed ? codec : 0;
            buffers->contents.append(contents);
            // Room for the block trailer, filled in place by the table builder.
            buffers->contents.append(rocksdb::kBuiltTrailerGap, '\0');
            out.contents_bytes = static_cast<std::uint32_t>(contents.size());
            out.compressed = static_cast<unsigned char>(type);
            if (host_compress)
                out.flags = rocksdb::CompactionBuiltBlock::kCompressOnHost;
            else
                out.checksum = rocksdb::crc32c::Mask(
                    rocksdb::crc32c::Extend(rocksdb::crc32c::Value(contents.data(), contents.size()), &type, 1));
            out.keys_offset = buffers->keys.size();
            out.first_key_bytes = static_cast<std::uint32_t>(keys[begin].size());
            out.last_key_bytes = static_cast<std::uint32_t>(keys[end - 1].size());
            buffers->keys.append(keys[begin]);
            buffers->keys.append(keys[end - 1]);
            buffers->blocks.push_back(out);
        }
        rocksdb::CompactionBuiltOutput result;
        result.blocks = buffers->blocks.data();
        result.block_count = buffers->blocks.size();
        result.contents = buffers->contents.data();
        result.contents_capacity = buffers->contents.size();
        result.keys = buffers->keys.data();
        result.hashes = buffers->hashes.data();
        result.entries = count;
        if (stats)
        {
            stats->built_blocks = buffers->blocks.size();
            stats->built_bytes = buffers->contents.size() - buffers->blocks.size() * rocksdb::kBuiltTrailerGap;
            stats->host_compressed_blocks = static_cast<std::uint64_t>(std::count_if(
                buffers->blocks.begin(), buffers->blocks.end(), [](const rocksdb::CompactionBuiltBlock &block) {
                    return (block.flags & rocksdb::CompactionBuiltBlock::kCompressOnHost) != 0;
                }));
        }
        *output = std::make_unique<BuiltMergedInput>(std::move(buffers), result, dropped);
        *built = true;
        return rocksdb::Status::OK();
    }
}
