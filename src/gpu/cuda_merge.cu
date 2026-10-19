// GPU merged compaction input: CRC32C verification, LZ4 decompression
// (nvCOMP), parallel data-block parsing, internal-key merge sort (CUB) and
// packing, producing the same stream as lsm_lab::execute_cpu_merge.

#include "lsm_lab/cuda_merge.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <cub/device/device_merge_sort.cuh>
#include <cub/device/device_reduce.cuh>
#include <cub/device/device_scan.cuh>
#include <cub/device/device_select.cuh>
#include <cuda_runtime.h>
#include <nvcomp.h>
#include <nvcomp/lz4.h>
#include <nvcomp/zstd.h>

namespace lsm_lab
{
    namespace
    {
        constexpr std::uint32_t kTrailer = 5;
        constexpr std::uint32_t kHashIndexBit = 1u << 31;
        constexpr std::uint32_t kReservedBit = 1u << 30;
        constexpr std::uint32_t kUniformKeysBit = 1u << 29;
        constexpr std::uint32_t kSeparatedKVBit = 1u << 28;
        constexpr std::uint32_t kMaxKeyBytes = 512;
        constexpr std::uint64_t kMaxBlockBytes = rocksdb::CompactionBlockTransform::max_block_bytes;

        enum : unsigned int
        {
            kErrorCorrupt = 1,
            kErrorEntryType = 2,
            kErrorKeyTooLong = 4,
            kErrorFooter = 8,
        };

        void cuda_ok(cudaError_t status, const char *what)
        {
            if (status != cudaSuccess)
                throw std::runtime_error(std::string(what) + ": " + cudaGetErrorString(status));
        }

        void nvcomp_ok(nvcompStatus_t status, const char *what)
        {
            if (status != nvcompSuccess)
                throw std::runtime_error(std::string(what) + ": " + nvcompGetStatusString(status));
        }

        template <typename T>
        class DeviceArray
        {
        public:
            DeviceArray() = default;
            ~DeviceArray()
            {
                if (data_ != nullptr)
                    cudaFree(data_);
            }
            DeviceArray(const DeviceArray &) = delete;
            DeviceArray &operator=(const DeviceArray &) = delete;
            T *reserve(std::size_t count)
            {
                count = std::max<std::size_t>(count, 1);
                if (count > capacity_)
                {
                    if (data_ != nullptr)
                        cuda_ok(cudaFree(data_), "cudaFree");
                    data_ = nullptr;
                    const auto grown = std::max(count, capacity_ + capacity_ / 2);
                    cuda_ok(cudaMalloc(reinterpret_cast<void **>(&data_), grown * sizeof(T)), "cudaMalloc");
                    capacity_ = grown;
                }
                return data_;
            }
            T *get() const { return data_; }

        private:
            T *data_ = nullptr;
            std::size_t capacity_ = 0;
        };

        template <typename T>
        class HostArray
        {
        public:
            HostArray() = default;
            ~HostArray()
            {
                if (data_ != nullptr)
                    cudaFreeHost(data_);
            }
            HostArray(const HostArray &) = delete;
            HostArray &operator=(const HostArray &) = delete;
            T *reserve(std::size_t count)
            {
                count = std::max<std::size_t>(count, 1);
                if (count > capacity_)
                {
                    if (data_ != nullptr)
                        cuda_ok(cudaFreeHost(data_), "cudaFreeHost");
                    data_ = nullptr;
                    const auto grown = std::max(count, capacity_ + capacity_ / 2);
                    cuda_ok(cudaHostAlloc(reinterpret_cast<void **>(&data_), grown * sizeof(T), cudaHostAllocPortable),
                            "cudaHostAlloc");
                    capacity_ = grown;
                }
                return data_;
            }
            T *get() const { return data_; }

        private:
            T *data_ = nullptr;
            std::size_t capacity_ = 0;
        };

        // Pinned output buffers outlive a merge call (owned by the returned
        // stream), so they are recycled through a pool instead of freed.
        class PinnedPool : public std::enable_shared_from_this<PinnedPool>
        {
        public:
            ~PinnedPool()
            {
                for (auto &[pointer, bytes] : free_)
                    cudaFreeHost(pointer);
            }

            std::shared_ptr<char> take(std::size_t bytes)
            {
                bytes = std::max<std::size_t>(bytes, 1);
                char *pointer = nullptr;
                std::size_t capacity = 0;
                {
                    const std::lock_guard guard(mutex_);
                    auto best = free_.end();
                    for (auto it = free_.begin(); it != free_.end(); ++it)
                        if (it->second >= bytes && (best == free_.end() || it->second < best->second))
                            best = it;
                    if (best != free_.end())
                    {
                        pointer = best->first;
                        capacity = best->second;
                        free_.erase(best);
                    }
                }
                if (pointer == nullptr)
                {
                    capacity = bytes + bytes / 4;
                    cuda_ok(cudaHostAlloc(reinterpret_cast<void **>(&pointer), capacity, cudaHostAllocPortable),
                            "cudaHostAlloc output");
                }
                std::weak_ptr<PinnedPool> pool = shared_from_this();
                return std::shared_ptr<char>(pointer, [pool, capacity](char *released) {
                    if (auto alive = pool.lock())
                    {
                        const std::lock_guard guard(alive->mutex_);
                        alive->free_.emplace_back(released, capacity);
                        return;
                    }
                    cudaFreeHost(released);
                });
            }

        private:
            std::mutex mutex_;
            std::vector<std::pair<char *, std::size_t>> free_;
        };

        struct GpuBlock
        {
            std::uint64_t serialized_offset;
            std::uint64_t compressed_offset;
            std::uint64_t decoded_offset;
            std::uint32_t checksum_bytes;
            std::uint32_t expected_checksum;
            std::uint32_t compressed_bytes;
            std::uint32_t decoded_bytes;
            std::uint32_t compression;
            std::uint32_t padding;
        };

        struct GpuEntry
        {
            std::uint64_t key_offset;
            std::uint64_t value_offset;
            std::uint32_t key_bytes;
            std::uint32_t value_bytes;
        };

        static_assert(sizeof(MergedEntry) == 16, "host entry layout must match the GPU output");

        struct KeyBounds
        {
            const unsigned char *start;
            const unsigned char *end;
            std::uint32_t start_bytes;
            std::uint32_t end_bytes;
            bool has_start;
            bool has_end;
        };

        __device__ __forceinline__ std::uint32_t load32(const unsigned char *p)
        {
            return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                   (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        }

        __device__ __forceinline__ std::uint64_t load64(const unsigned char *p)
        {
            return static_cast<std::uint64_t>(load32(p)) | (static_cast<std::uint64_t>(load32(p + 4)) << 32);
        }

        __device__ const unsigned char *varint32(const unsigned char *p, const unsigned char *limit,
                                                 std::uint32_t *value)
        {
            std::uint32_t result = 0;
            for (std::uint32_t shift = 0; shift <= 28 && p < limit; shift += 7)
            {
                const std::uint32_t byte = *p++;
                if (byte & 128u)
                {
                    result |= (byte & 127u) << shift;
                }
                else
                {
                    *value = result | (byte << shift);
                    return p;
                }
            }
            return nullptr;
        }

        __device__ int compare_bytes(const unsigned char *left, std::uint32_t left_bytes,
                                     const unsigned char *right, std::uint32_t right_bytes)
        {
            const auto common = left_bytes < right_bytes ? left_bytes : right_bytes;
            for (std::uint32_t index = 0; index < common; ++index)
                if (left[index] != right[index])
                    return left[index] < right[index] ? -1 : 1;
            return left_bytes < right_bytes ? -1 : (left_bytes > right_bytes ? 1 : 0);
        }

        // Slicing-by-8 CRC32C, one thread per block; tables staged in shared memory.
        __global__ void verify_blocks(const unsigned char *serialized, const GpuBlock *blocks, std::uint32_t count,
                                      unsigned char *decoded, const std::uint32_t *tables, std::uint32_t *status)
        {
            __shared__ std::uint32_t table[8][256];
            for (std::uint32_t index = threadIdx.x; index < 8 * 256; index += blockDim.x)
                table[index / 256][index % 256] = tables[index];
            __syncthreads();
            const auto block_index = blockIdx.x * blockDim.x + threadIdx.x;
            if (block_index >= count)
                return;
            const GpuBlock block = blocks[block_index];
            const unsigned char *p = serialized + block.serialized_offset;
            std::uint64_t remaining = block.checksum_bytes;
            std::uint32_t crc = 0xffffffffu;
            while (remaining >= 8)
            {
                const std::uint32_t one = load32(p) ^ crc;
                const std::uint32_t two = load32(p + 4);
                crc = table[7][one & 0xff] ^ table[6][(one >> 8) & 0xff] ^ table[5][(one >> 16) & 0xff] ^
                      table[4][one >> 24] ^ table[3][two & 0xff] ^ table[2][(two >> 8) & 0xff] ^
                      table[1][(two >> 16) & 0xff] ^ table[0][two >> 24];
                p += 8;
                remaining -= 8;
            }
            while (remaining-- > 0)
                crc = table[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
            crc = ~crc;
            const std::uint32_t masked = ((crc >> 15) | (crc << 17)) + 0xa282ead8u;
            const bool ok = masked == block.expected_checksum;
            status[block_index] = ok ? 0u : 1u;
            if (ok && block.compression == 0)
            {
                const unsigned char *source = serialized + block.compressed_offset;
                unsigned char *destination = decoded + block.decoded_offset;
                for (std::uint32_t offset = 0; offset < block.decoded_bytes; ++offset)
                    destination[offset] = source[offset];
            }
        }

        __global__ void stage_compressed(const unsigned char *serialized, const GpuBlock *blocks,
                                         const std::uint32_t *compressed_index, const std::uint64_t *staged_offsets,
                                         std::uint32_t count, unsigned char *staged)
        {
            const auto index = blockIdx.x * blockDim.x + threadIdx.x;
            if (index >= count)
                return;
            const GpuBlock block = blocks[compressed_index[index]];
            for (std::uint32_t offset = 0; offset < block.compressed_bytes; ++offset)
                staged[staged_offsets[index] + offset] = serialized[block.compressed_offset + offset];
        }

        __global__ void count_restarts(const unsigned char *decoded, const GpuBlock *blocks, std::uint32_t count,
                                       std::uint32_t *restarts, unsigned int *error)
        {
            const auto index = blockIdx.x * blockDim.x + threadIdx.x;
            if (index >= count)
                return;
            const GpuBlock block = blocks[index];
            restarts[index] = 0;
            if (block.decoded_bytes < 4)
            {
                atomicOr(error, kErrorCorrupt);
                return;
            }
            std::uint32_t packed = load32(decoded + block.decoded_offset + block.decoded_bytes - 4);
            if (packed & (kHashIndexBit | kReservedBit | kSeparatedKVBit))
            {
                atomicOr(error, kErrorFooter);
                return;
            }
            packed &= ~kUniformKeysBit;
            if (packed == 0 || 4ull * packed + 4 > block.decoded_bytes)
            {
                atomicOr(error, kErrorCorrupt);
                return;
            }
            restarts[index] = packed;
        }

        __global__ void expand_intervals(const std::uint32_t *restarts, const std::uint64_t *first_interval,
                                         std::uint32_t count, std::uint32_t *interval_block)
        {
            const auto index = blockIdx.x * blockDim.x + threadIdx.x;
            if (index >= count)
                return;
            const auto first = first_interval[index];
            for (std::uint32_t restart = 0; restart < restarts[index]; ++restart)
                interval_block[first + restart] = index;
        }

        // One thread per restart interval. Count mode records the number of
        // in-range entries and their key bytes; write mode materializes keys.
        template <bool Write>
        __global__ void parse_intervals(const unsigned char *decoded, const GpuBlock *blocks,
                                        const std::uint32_t *interval_block, const std::uint64_t *first_interval,
                                        std::uint64_t intervals, KeyBounds bounds, std::uint32_t *counts,
                                        std::uint64_t *key_bytes, const std::uint64_t *entry_offsets,
                                        const std::uint64_t *key_offsets, GpuEntry *entries,
                                        unsigned char *key_arena, unsigned int *error)
        {
            const std::uint64_t interval = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (interval >= intervals)
                return;
            const auto block_index = interval_block[interval];
            const GpuBlock block = blocks[block_index];
            const auto restart = static_cast<std::uint32_t>(interval - first_interval[block_index]);
            const unsigned char *base = decoded + block.decoded_offset;
            const std::uint32_t restart_count = load32(base + block.decoded_bytes - 4) & ~kUniformKeysBit;
            const std::uint32_t restarts_begin = block.decoded_bytes - 4 - 4 * restart_count;
            const std::uint32_t begin = load32(base + restarts_begin + 4 * restart);
            const std::uint32_t end =
                restart + 1 < restart_count ? load32(base + restarts_begin + 4 * (restart + 1)) : restarts_begin;
            if (begin > end || end > restarts_begin)
            {
                atomicOr(error, kErrorCorrupt);
                return;
            }
            unsigned char key[kMaxKeyBytes];
            std::uint32_t key_length = 0;
            const unsigned char *p = base + begin;
            const unsigned char *limit = base + end;
            std::uint32_t count = 0;
            std::uint64_t bytes = 0;
            std::uint64_t entry_out = Write ? entry_offsets[interval] : 0;
            std::uint64_t key_out = Write ? key_offsets[interval] : 0;
            bool first = true;
            while (p < limit)
            {
                std::uint32_t shared = 0, non_shared = 0, value_bytes = 0;
                if (limit - p >= 3 && (p[0] | p[1] | p[2]) < 128)
                {
                    shared = p[0];
                    non_shared = p[1];
                    value_bytes = p[2];
                    p += 3;
                }
                else if ((p = varint32(p, limit, &shared)) == nullptr ||
                         (p = varint32(p, limit, &non_shared)) == nullptr ||
                         (p = varint32(p, limit, &value_bytes)) == nullptr)
                {
                    atomicOr(error, kErrorCorrupt);
                    return;
                }
                if ((first && shared != 0) || shared > key_length)
                {
                    atomicOr(error, kErrorCorrupt);
                    return;
                }
                if (non_shared > kMaxKeyBytes - shared)
                {
                    atomicOr(error, kErrorKeyTooLong);
                    return;
                }
                if (static_cast<std::uint64_t>(limit - p) < static_cast<std::uint64_t>(non_shared) + value_bytes)
                {
                    atomicOr(error, kErrorCorrupt);
                    return;
                }
                for (std::uint32_t index = 0; index < non_shared; ++index)
                    key[shared + index] = p[index];
                key_length = shared + non_shared;
                p += non_shared;
                if (key_length < 8)
                {
                    atomicOr(error, kErrorCorrupt);
                    return;
                }
                if (key[key_length - 8] > 1)
                {
                    atomicOr(error, kErrorEntryType);
                    return;
                }
                const auto user_bytes = key_length - 8;
                const bool in_range =
                    (!bounds.has_start || compare_bytes(key, user_bytes, bounds.start, bounds.start_bytes) >= 0) &&
                    (!bounds.has_end || compare_bytes(key, user_bytes, bounds.end, bounds.end_bytes) < 0);
                if (in_range)
                {
                    if (Write)
                    {
                        for (std::uint32_t index = 0; index < key_length; ++index)
                            key_arena[key_out + index] = key[index];
                        entries[entry_out] = {key_out, static_cast<std::uint64_t>(p - decoded), key_length,
                                              value_bytes};
                        ++entry_out;
                        key_out += key_length;
                    }
                    else
                    {
                        ++count;
                        bytes += key_length;
                    }
                }
                p += value_bytes;
                first = false;
            }
            if (!Write)
            {
                counts[interval] = count;
                key_bytes[interval] = bytes;
            }
        }

        struct InternalKeyLess
        {
            const GpuEntry *entries;
            const unsigned char *keys;

            __device__ bool operator()(const std::uint32_t &left_index, const std::uint32_t &right_index) const
            {
                const GpuEntry left = entries[left_index];
                const GpuEntry right = entries[right_index];
                const unsigned char *left_key = keys + left.key_offset;
                const unsigned char *right_key = keys + right.key_offset;
                const auto left_user = left.key_bytes - 8;
                const auto right_user = right.key_bytes - 8;
                const int order = compare_bytes(left_key, left_user, right_key, right_user);
                if (order != 0)
                    return order < 0;
                return load64(left_key + left_user) > load64(right_key + right_user);
            }
        };

        __device__ std::uint32_t snapshot_stripe(const std::uint64_t *snapshots, std::uint32_t count,
                                                 std::uint64_t sequence)
        {
            std::uint32_t low = 0, high = count;
            while (low < high)
            {
                const auto middle = (low + high) / 2;
                if (snapshots[middle] < sequence)
                    low = middle + 1;
                else
                    high = middle;
            }
            return low;
        }

        // keep[j] = 0 when sorted entry j has the same user key as entry j-1
        // (its newer neighbour) and both fall in the same snapshot stripe.
        __global__ void mark_hidden(const std::uint32_t *order, const GpuEntry *entries, const unsigned char *keys,
                                    std::uint64_t count, const std::uint64_t *snapshots, std::uint32_t snapshot_count,
                                    unsigned char *keep, unsigned long long *dropped)
        {
            const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (index >= count)
                return;
            const GpuEntry entry = entries[order[index]];
            bool hidden = false;
            if (index > 0)
            {
                const GpuEntry previous = entries[order[index - 1]];
                if (previous.key_bytes == entry.key_bytes &&
                    compare_bytes(keys + previous.key_offset, previous.key_bytes - 8, keys + entry.key_offset,
                                  entry.key_bytes - 8) == 0)
                {
                    const auto sequence = load64(keys + entry.key_offset + entry.key_bytes - 8) >> 8;
                    const auto previous_sequence = load64(keys + previous.key_offset + previous.key_bytes - 8) >> 8;
                    hidden = snapshot_stripe(snapshots, snapshot_count, sequence) ==
                             snapshot_stripe(snapshots, snapshot_count, previous_sequence);
                }
            }
            keep[index] = hidden ? 0 : 1;
            if (hidden)
            {
                atomicAdd(&dropped[0], 1ULL);
                if (keys[entry.key_offset + entry.key_bytes - 8] == 0)
                    atomicAdd(&dropped[1], 1ULL);
                atomicAdd(&dropped[2], static_cast<unsigned long long>(entry.key_bytes));
                atomicAdd(&dropped[3], static_cast<unsigned long long>(entry.value_bytes));
            }
        }

        __global__ void iota(std::uint32_t *values, std::uint64_t count)
        {
            const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (index < count)
                values[index] = static_cast<std::uint32_t>(index);
        }

        __global__ void sorted_sizes(const std::uint32_t *order, const GpuEntry *entries, std::uint64_t count,
                                     std::uint64_t *sizes)
        {
            const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (index < count)
            {
                const GpuEntry entry = entries[order[index]];
                sizes[index] = static_cast<std::uint64_t>(entry.key_bytes) + entry.value_bytes;
            }
        }

        // One warp per entry.
        __global__ void gather(const std::uint32_t *order, const GpuEntry *entries, const unsigned char *keys,
                               const unsigned char *decoded, const std::uint64_t *offsets, std::uint64_t count,
                               unsigned char *packed, MergedEntry *output)
        {
            const std::uint64_t warp = (static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x) / 32;
            const unsigned int lane = threadIdx.x & 31u;
            if (warp >= count)
                return;
            const GpuEntry entry = entries[order[warp]];
            const auto offset = offsets[warp];
            for (std::uint32_t index = lane; index < entry.key_bytes; index += 32)
                packed[offset + index] = keys[entry.key_offset + index];
            for (std::uint32_t index = lane; index < entry.value_bytes; index += 32)
                packed[offset + entry.key_bytes + index] = decoded[entry.value_offset + index];
            if (lane == 0)
                output[warp] = {offset, entry.key_bytes, entry.value_bytes};
        }

        // ---- Stage 3: finished data blocks --------------------------------

        // RocksDB Hash64 = XXPH3 (XXH3 preview) with seed 0, inputs <= 240 bytes.
        __constant__ unsigned char kXxph3Secret[192] = {
            0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c,
            0xde, 0xd4, 0x6d, 0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f,
            0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0, 0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21,
            0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0, 0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c,
            0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b, 0x1b, 0x53, 0x2e, 0xa3,
            0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac, 0xd8,
            0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d,
            0x8a, 0x51, 0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64,
            0xea, 0xc5, 0xac, 0x83, 0x34, 0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb,
            0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49, 0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e,
            0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8, 0xd1, 0x7a, 0xd0, 0x31, 0xce,
            0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b, 0x40, 0x7e,
        };
        constexpr std::uint64_t kPrime64_1 = 0x9E3779B185EBCA87ULL;
        constexpr std::uint64_t kPrime64_2 = 0xC2B2AE3D27D4EB4FULL;
        constexpr std::uint64_t kPrime64_3 = 0x165667B19E3779F9ULL;
        constexpr std::uint64_t kPrime32_1 = 0x9E3779B1ULL;

        __device__ __forceinline__ std::uint64_t secret64(std::uint32_t offset)
        {
            std::uint64_t value = 0;
            for (int index = 7; index >= 0; --index)
                value = (value << 8) | kXxph3Secret[offset + index];
            return value;
        }

        __device__ __forceinline__ std::uint64_t fold64(std::uint64_t left, std::uint64_t right)
        {
            return (left * right) ^ __umul64hi(left, right);
        }

        __device__ __forceinline__ std::uint64_t avalanche(std::uint64_t hash)
        {
            hash ^= hash >> 37;
            hash *= kPrime64_3;
            return hash ^ (hash >> 32);
        }

        __device__ __forceinline__ std::uint64_t mix16(const unsigned char *input, std::uint32_t secret)
        {
            return fold64(load64(input) ^ secret64(secret), load64(input + 8) ^ secret64(secret + 8));
        }

        __device__ std::uint64_t xxph3(const unsigned char *input, std::uint32_t length)
        {
            if (length == 0)
                return fold64(secret64(0), kPrime64_2);
            if (length <= 3)
            {
                const std::uint32_t combined = static_cast<std::uint32_t>(input[0]) |
                                               (static_cast<std::uint32_t>(input[length >> 1]) << 8) |
                                               (static_cast<std::uint32_t>(input[length - 1]) << 16) | (length << 24);
                const std::uint64_t keyed = static_cast<std::uint64_t>(combined) ^ (secret64(0) & 0xffffffffULL);
                return avalanche(keyed * kPrime64_1);
            }
            if (length <= 8)
            {
                const std::uint64_t input64 =
                    static_cast<std::uint64_t>(load32(input)) | (static_cast<std::uint64_t>(load32(input + length - 4)) << 32);
                const std::uint64_t keyed = input64 ^ secret64(0);
                const std::uint64_t mixed = length + ((keyed ^ (keyed >> 51)) * kPrime32_1);
                return avalanche((mixed ^ (mixed >> 47)) * kPrime64_2);
            }
            if (length <= 16)
            {
                const std::uint64_t low = load64(input) ^ secret64(0);
                const std::uint64_t high = load64(input + length - 8) ^ secret64(8);
                return avalanche(length + (low + high) + fold64(low, high));
            }
            std::uint64_t accumulator = length * kPrime64_1;
            if (length <= 128)
            {
                if (length > 32)
                {
                    if (length > 64)
                    {
                        if (length > 96)
                        {
                            accumulator += mix16(input + 48, 96);
                            accumulator += mix16(input + length - 64, 112);
                        }
                        accumulator += mix16(input + 32, 64);
                        accumulator += mix16(input + length - 48, 80);
                    }
                    accumulator += mix16(input + 16, 32);
                    accumulator += mix16(input + length - 32, 48);
                }
                accumulator += mix16(input, 0);
                accumulator += mix16(input + length - 16, 16);
                return avalanche(accumulator);
            }
            for (std::uint32_t round = 0; round < 8; ++round)
                accumulator += mix16(input + 16 * round, 16 * round);
            accumulator = avalanche(accumulator);
            for (std::uint32_t round = 8; round < length / 16; ++round)
                accumulator += mix16(input + 16 * round, 16 * (round - 8) + 3);
            accumulator += mix16(input + length - 16, 136 - 17);
            return avalanche(accumulator);
        }

        struct BuildSpec
        {
            std::uint64_t block_bytes;
            std::uint64_t deviation_limit;
            std::uint64_t zero_limit;
            std::uint32_t interval;
            std::uint32_t zero_sequences;
            std::uint32_t hashes;
            std::uint32_t chunk;
        };

        __device__ __forceinline__ std::uint32_t varint_length(std::uint64_t value)
        {
            std::uint32_t bytes = 1;
            while (value >= 128)
            {
                value >>= 7;
                ++bytes;
            }
            return bytes;
        }

        __device__ __forceinline__ unsigned char *put_varint(unsigned char *p, std::uint32_t value)
        {
            while (value >= 128)
            {
                *p++ = static_cast<unsigned char>(value | 128u);
                value >>= 7;
            }
            *p++ = static_cast<unsigned char>(value);
            return p;
        }

        __device__ __forceinline__ void store32(unsigned char *p, std::uint32_t value)
        {
            p[0] = static_cast<unsigned char>(value);
            p[1] = static_cast<unsigned char>(value >> 8);
            p[2] = static_cast<unsigned char>(value >> 16);
            p[3] = static_cast<unsigned char>(value >> 24);
        }

        // Byte j of the output internal key: user key, then the 8-byte trailer.
        __device__ __forceinline__ unsigned char ikey_byte(const unsigned char *key, std::uint32_t user,
                                                           std::uint64_t trailer, std::uint32_t index)
        {
            return index < user ? key[index] : static_cast<unsigned char>(trailer >> (8 * (index - user)));
        }

        __device__ __forceinline__ std::uint64_t output_trailer(const unsigned char *key, std::uint32_t user,
                                                                const BuildSpec &spec, std::uint64_t *sequence)
        {
            const auto trailer = load64(key + user);
            auto value = trailer >> 8;
            // Only Puts are zeroed; deletions keep their sequence (CompactionIterator::PrepareOutput).
            if (spec.zero_sequences && value <= spec.zero_limit && (trailer & 0xff) == 1)
                value = 0;
            *sequence = value;
            return (value << 8) | (trailer & 0xff);
        }

        struct DeletionSpec
        {
            const unsigned char *range_bytes;
            const std::uint64_t *range_offsets; // 4 per range: start offset, start bytes, end offset, end bytes
            std::uint64_t earliest_snapshot;
            std::uint32_t ranges;
            std::uint32_t bottommost;
            std::uint32_t check_ranges;
        };

        __device__ bool key_may_exist_beyond(const unsigned char *key, std::uint32_t bytes, const DeletionSpec &spec)
        {
            if (spec.bottommost)
                return false;
            if (!spec.check_ranges)
                return true;
            // Last range whose start <= key.
            std::uint32_t low = 0, high = spec.ranges;
            while (low < high)
            {
                const auto middle = (low + high) / 2;
                const auto *range = spec.range_offsets + 4 * middle;
                if (compare_bytes(spec.range_bytes + range[0], static_cast<std::uint32_t>(range[1]), key, bytes) <= 0)
                    low = middle + 1;
                else
                    high = middle;
            }
            if (low == 0)
                return false;
            const auto *range = spec.range_offsets + 4 * (low - 1);
            return compare_bytes(key, bytes, spec.range_bytes + range[2], static_cast<std::uint32_t>(range[3])) <= 0;
        }

        // keep[i] = 0 for deletions that CompactionIterator drops (see CompactionOutputSpec).
        __global__ void drop_deletions(const std::uint32_t *order, const GpuEntry *entries, const unsigned char *keys,
                                       std::uint64_t count, DeletionSpec spec, unsigned char *keep,
                                       unsigned long long *dropped)
        {
            const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (index >= count)
                return;
            const GpuEntry entry = entries[order[index]];
            const unsigned char *key = keys + entry.key_offset;
            const auto user = entry.key_bytes - 8;
            bool drop = false;
            if (key[user] == 0)
            {
                const auto sequence = load64(key + user) >> 8;
                drop = sequence <= spec.earliest_snapshot && !key_may_exist_beyond(key, user, spec);
                if (!drop && spec.bottommost)
                {
                    if (index + 1 == count)
                        drop = true;
                    else
                    {
                        const GpuEntry next = entries[order[index + 1]];
                        drop = next.key_bytes != entry.key_bytes ||
                               compare_bytes(keys + next.key_offset, next.key_bytes - 8, key, user) != 0;
                    }
                }
            }
            keep[index] = drop ? 0 : 1;
            if (drop)
            {
                atomicAdd(&dropped[0], 1ULL);
                atomicAdd(&dropped[1], static_cast<unsigned long long>(entry.key_bytes));
            }
        }

        // Per sorted entry: output sequence, bytes shared with the previous
        // output internal key, whether its user key starts a new key, and the
        // filter hash of new user keys. Flags entries the encoder cannot take.
        __global__ void build_entry_info(const std::uint32_t *order, const GpuEntry *entries, const unsigned char *keys,
                                         std::uint64_t count, BuildSpec spec, std::uint64_t *sequences,
                                         std::uint32_t *shared, unsigned char *distinct, std::uint64_t *hashes,
                                         unsigned int *error)
        {
            const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (index >= count)
                return;
            const GpuEntry entry = entries[order[index]];
            const unsigned char *key = keys + entry.key_offset;
            const auto user = entry.key_bytes - 8;
            if (key[user] > 1 || user > 240)
                atomicOr(error, 1u);
            std::uint64_t sequence = 0;
            const auto trailer = output_trailer(key, user, spec, &sequence);
            sequences[index] = sequence;
            std::uint32_t common = 0;
            bool same_user = false;
            if (index > 0)
            {
                const GpuEntry previous = entries[order[index - 1]];
                const unsigned char *previous_key = keys + previous.key_offset;
                const auto previous_user = previous.key_bytes - 8;
                std::uint64_t ignored = 0;
                const auto previous_trailer = output_trailer(previous_key, previous_user, spec, &ignored);
                const auto limit = min(entry.key_bytes, previous.key_bytes);
                while (common < limit && ikey_byte(key, user, trailer, common) ==
                                             ikey_byte(previous_key, previous_user, previous_trailer, common))
                    ++common;
                same_user = user == previous_user && common >= user;
            }
            shared[index] = common;
            distinct[index] = same_user ? 0 : 1;
            hashes[index] = spec.hashes && !same_user ? xxph3(key, user) : 0;
        }

        // One thread per chunk: replays BlockBuilder's size estimate and
        // FlushBlockBySizePolicy; a block always starts at a chunk boundary.
        __global__ void cut_blocks(const std::uint32_t *order, const GpuEntry *entries, const std::uint32_t *shared,
                                   std::uint64_t count, BuildSpec spec, std::uint32_t *start)
        {
            const std::uint64_t chunk = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
            const std::uint64_t begin = chunk * spec.chunk;
            if (begin >= count)
                return;
            const std::uint64_t end = min(count, begin + spec.chunk);
            std::uint64_t estimate = 8;
            std::uint32_t counter = 0;
            bool empty = true;
            for (auto index = begin; index < end; ++index)
            {
                const GpuEntry entry = entries[order[index]];
                const std::uint64_t key_bytes = entry.key_bytes;
                const std::uint64_t value_bytes = entry.value_bytes;
                bool flush = index == begin;
                if (!flush && !empty)
                {
                    if (estimate >= spec.block_bytes)
                        flush = true;
                    else if (spec.deviation_limit != 0)
                    {
                        const auto after = estimate + key_bytes + value_bytes + (counter >= spec.interval ? 4 : 0) + 4 +
                                           varint_length(key_bytes) + varint_length(value_bytes);
                        flush = after > spec.block_bytes && estimate > spec.deviation_limit;
                    }
                }
                start[index] = flush ? 1u : 0u;
                if (flush)
                {
                    estimate = 8;
                    counter = 0;
                    empty = true;
                }
                std::uint64_t common = 0;
                if (counter >= spec.interval)
                {
                    estimate += 4;
                    counter = 0;
                }
                else if (!empty)
                {
                    common = shared[index];
                }
                estimate += varint_length(common) + varint_length(key_bytes - common) + varint_length(value_bytes) +
                            key_bytes - common + value_bytes;
                ++counter;
                empty = false;
            }
        }

        // Inclusive scan of block-start flags is 1-based; make it a 0-based block index.
        __global__ void zero_base(std::uint32_t *values, std::uint64_t count)
        {
            const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (index < count)
                --values[index];
        }

        __global__ void block_firsts(const std::uint32_t *start, const std::uint32_t *block_of, std::uint64_t count,
                                     std::uint32_t *first)
        {
            const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (index < count && start[index])
                first[block_of[index]] = static_cast<std::uint32_t>(index);
        }

        __device__ __forceinline__ std::uint32_t entry_shared(std::uint64_t index, std::uint64_t first,
                                                              const std::uint32_t *shared, std::uint32_t interval)
        {
            return (index - first) % interval == 0 ? 0u : shared[index];
        }

        // Encoded bytes of every entry; bytes[count] = 0 so the scan yields the total.
        __global__ void entry_bytes(const std::uint32_t *order, const GpuEntry *entries, const std::uint32_t *shared,
                                    const std::uint32_t *block_of, const std::uint32_t *first, std::uint64_t count,
                                    std::uint32_t interval, std::uint64_t *bytes)
        {
            const std::uint64_t index = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
            if (index > count)
                return;
            if (index == count)
            {
                bytes[index] = 0;
                return;
            }
            const GpuEntry entry = entries[order[index]];
            const auto common = entry_shared(index, first[block_of[index]], shared, interval);
            bytes[index] = varint_length(common) + varint_length(entry.key_bytes - common) +
                           varint_length(entry.value_bytes) + entry.key_bytes - common + entry.value_bytes;
        }

        // Per block: raw size, statistics and key/hash counts.
        __global__ void block_summary(const std::uint32_t *order, const GpuEntry *entries, const unsigned char *keys,
                                      const std::uint32_t *first,
                                      const std::uint64_t *position, const std::uint64_t *sequences,
                                      const unsigned char *distinct, std::uint32_t blocks, std::uint32_t interval,
                                      rocksdb::CompactionBuiltBlock *built, std::uint64_t *raw_bytes,
                                      std::uint64_t *key_bytes, std::uint64_t *hash_counts)
        {
            const std::uint32_t block = blockIdx.x * blockDim.x + threadIdx.x;
            if (block >= blocks)
                return;
            const std::uint64_t begin = first[block];
            const std::uint64_t end = first[block + 1];
            const auto entries_in_block = end - begin;
            const auto restarts = (entries_in_block + interval - 1) / interval;
            const auto raw = position[end] - position[begin] + 4 * restarts + 4;
            rocksdb::CompactionBuiltBlock out{};
            out.uncompressed_bytes = static_cast<std::uint32_t>(raw);
            out.entries = static_cast<std::uint32_t>(entries_in_block);
            out.smallest_seqno = ~0ULL;
            std::uint32_t hashes = 0;
            for (auto index = begin; index < end; ++index)
            {
                const GpuEntry entry = entries[order[index]];
                out.raw_key_bytes += entry.key_bytes;
                out.raw_value_bytes += entry.value_bytes;
                out.smallest_seqno = min(out.smallest_seqno, sequences[index]);
                out.largest_seqno = max(out.largest_seqno, sequences[index]);
                hashes += distinct[index];
                out.deletions += keys[entries[order[index]].key_offset + entries[order[index]].key_bytes - 8] == 0 ? 1u : 0u;
            }
            out.first_key_bytes = entries[order[begin]].key_bytes;
            out.last_key_bytes = entries[order[end - 1]].key_bytes;
            out.hash_count = hashes;
            built[block] = out;
            raw_bytes[block] = (raw + 15) / 16 * 16;
            key_bytes[block] = out.first_key_bytes + out.last_key_bytes;
            hash_counts[block] = hashes;
        }

        // One warp per entry writes its header, key suffix and value.
        __global__ void encode_entries(const std::uint32_t *order, const GpuEntry *entries, const unsigned char *keys,
                                       const unsigned char *decoded, const std::uint32_t *shared,
                                       const std::uint64_t *sequences, const std::uint32_t *block_of,
                                       const std::uint32_t *first, const std::uint64_t *position,
                                       const std::uint64_t *raw_offset, std::uint64_t count, std::uint32_t interval,
                                       unsigned char *raw)
        {
            const std::uint64_t index = (static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x) / 32;
            const unsigned int lane = threadIdx.x & 31u;
            if (index >= count)
                return;
            const GpuEntry entry = entries[order[index]];
            const auto block = block_of[index];
            const std::uint64_t begin = first[block];
            const auto common = entry_shared(index, begin, shared, interval);
            const unsigned char *key = keys + entry.key_offset;
            const auto user = entry.key_bytes - 8;
            const std::uint64_t trailer = (sequences[index] << 8) | key[user];
            unsigned char *out = raw + raw_offset[block] + (position[index] - position[begin]);
            const auto header = varint_length(common) + varint_length(entry.key_bytes - common) +
                                varint_length(entry.value_bytes);
            if (lane == 0)
            {
                auto *p = put_varint(out, common);
                p = put_varint(p, entry.key_bytes - common);
                put_varint(p, entry.value_bytes);
            }
            for (std::uint32_t offset = common + lane; offset < entry.key_bytes; offset += 32)
                out[header + offset - common] = ikey_byte(key, user, trailer, offset);
            unsigned char *value = out + header + entry.key_bytes - common;
            for (std::uint32_t offset = lane; offset < entry.value_bytes; offset += 32)
                value[offset] = decoded[entry.value_offset + offset];
        }

        __global__ void encode_restarts(const std::uint32_t *first, const std::uint64_t *position,
                                        const std::uint64_t *raw_offset, std::uint32_t blocks, std::uint32_t interval,
                                        unsigned char *raw)
        {
            const std::uint32_t block = blockIdx.x * blockDim.x + threadIdx.x;
            if (block >= blocks)
                return;
            const std::uint64_t begin = first[block];
            const std::uint64_t end = first[block + 1];
            const auto restarts = static_cast<std::uint32_t>((end - begin + interval - 1) / interval);
            unsigned char *trailer = raw + raw_offset[block] + (position[end] - position[begin]);
            for (std::uint32_t restart = 0; restart < restarts; ++restart)
                store32(trailer + 4 * restart,
                        static_cast<std::uint32_t>(position[begin + restart * interval] - position[begin]));
            store32(trailer + 4 * restarts, restarts);
        }

        __global__ void compress_arguments(unsigned char *raw, const std::uint64_t *raw_offset,
                                           const rocksdb::CompactionBuiltBlock *built, std::uint32_t blocks,
                                           unsigned char *compressed, std::uint64_t stride, const void **sources,
                                           std::size_t *source_bytes, void **destinations)
        {
            const std::uint32_t block = blockIdx.x * blockDim.x + threadIdx.x;
            if (block >= blocks)
                return;
            sources[block] = raw + raw_offset[block];
            source_bytes[block] = built[block].uncompressed_bytes;
            destinations[block] = compressed + block * stride;
        }

        // Keeps a compressed block only if its stored form fits RocksDB's limit.
        __global__ void choose_contents(rocksdb::CompactionBuiltBlock *built, const std::size_t *compressed_bytes,
                                        const nvcompStatus_t *statuses, std::uint32_t blocks, std::uint32_t codec,
                                        std::uint32_t max_compressed_bytes_per_kb, std::uint64_t *contents_bytes)
        {
            const std::uint32_t block = blockIdx.x * blockDim.x + threadIdx.x;
            if (block >= blocks)
                return;
            const std::uint64_t raw = built[block].uncompressed_bytes;
            bool use = false;
            std::uint64_t stored = raw;
            if (codec != 0 && statuses[block] == nvcompSuccess)
            {
                const auto total = varint_length(raw) + compressed_bytes[block];
                use = total <= ((static_cast<std::uint64_t>(max_compressed_bytes_per_kb) * raw) >> 10);
                if (use)
                    stored = total;
            }
            built[block].compressed = use ? codec : 0u;
            built[block].contents_bytes = static_cast<std::uint32_t>(stored);
            contents_bytes[block] = stored;
        }

        // One CUDA block per data block: copy the stored form, then CRC32C of
        // contents plus the compression-type byte (masked, no context modifier).
        __global__ void finalize_contents(rocksdb::CompactionBuiltBlock *built, const unsigned char *raw,
                                          const std::uint64_t *raw_offset, const unsigned char *compressed,
                                          std::uint64_t stride, const std::uint64_t *final_offset,
                                          const std::uint32_t *tables, unsigned char *contents)
        {
            const std::uint32_t block = blockIdx.x;
            const auto info = built[block];
            unsigned char *out = contents + final_offset[block];
            std::uint32_t prefix = 0;
            const unsigned char *source = raw + raw_offset[block];
            std::uint32_t source_bytes = info.uncompressed_bytes;
            if (info.compressed)
            {
                prefix = varint_length(info.uncompressed_bytes);
                source = compressed + block * stride;
                source_bytes = info.contents_bytes - prefix;
                if (threadIdx.x == 0)
                    put_varint(out, info.uncompressed_bytes);
            }
            for (std::uint32_t offset = threadIdx.x; offset < source_bytes; offset += blockDim.x)
                out[prefix + offset] = source[offset];
            __syncthreads();
            if (threadIdx.x != 0)
                return;
            std::uint32_t crc = 0xffffffffu;
            const unsigned char *p = out;
            for (std::uint32_t remaining = info.contents_bytes; remaining > 0; --remaining)
                crc = tables[(crc ^ *p++) & 0xff] ^ (crc >> 8);
            const auto type = static_cast<unsigned char>(info.compressed);
            crc = tables[(crc ^ type) & 0xff] ^ (crc >> 8);
            crc = ~crc;
            built[block].checksum = ((crc >> 15) | (crc << 17)) + 0xa282ead8u;
            built[block].contents_offset = final_offset[block];
        }

        __global__ void block_keys(const std::uint32_t *order, const GpuEntry *entries, const unsigned char *keys,
                                   const std::uint64_t *sequences, const std::uint32_t *first,
                                   const std::uint64_t *key_offset, const std::uint64_t *hash_offset,
                                   std::uint32_t blocks, rocksdb::CompactionBuiltBlock *built, unsigned char *arena)
        {
            const std::uint32_t block = blockIdx.x * blockDim.x + threadIdx.x;
            if (block >= blocks)
                return;
            unsigned char *out = arena + key_offset[block];
            const std::uint64_t ends[2] = {first[block], first[block + 1] - 1};
            for (const auto index : ends)
            {
                const GpuEntry entry = entries[order[index]];
                const unsigned char *key = keys + entry.key_offset;
                const auto user = entry.key_bytes - 8;
                const std::uint64_t trailer = (sequences[index] << 8) | key[user];
                for (std::uint32_t offset = 0; offset < entry.key_bytes; ++offset)
                    *out++ = ikey_byte(key, user, trailer, offset);
            }
            built[block].keys_offset = key_offset[block];
            built[block].hash_offset = hash_offset[block];
        }

        // nvCOMP rejects very large batches; large jobs are processed in sub-batches.
        std::size_t nvcomp_batch_limit()
        {
            static const std::size_t value = [] {
                const char *configured = std::getenv("LSM_NVCOMP_BATCH");
                const auto parsed = configured == nullptr ? 0 : std::strtoull(configured, nullptr, 10);
                return parsed == 0 ? std::size_t{16384} : static_cast<std::size_t>(parsed);
            }();
            return value;
        }

        unsigned int grid(std::uint64_t items, unsigned int threads)
        {
            return static_cast<unsigned int>((items + threads - 1) / threads);
        }

        std::uint32_t decode_fixed32(const unsigned char *p)
        {
            return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                   (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        }

        const unsigned char *decode_varint32(const unsigned char *p, const unsigned char *limit, std::uint32_t *value)
        {
            std::uint32_t result = 0;
            for (std::uint32_t shift = 0; shift <= 28 && p < limit; shift += 7)
            {
                const std::uint32_t byte = *p++;
                if (byte & 128u)
                {
                    result |= (byte & 127u) << shift;
                }
                else
                {
                    *value = result | (byte << shift);
                    return p;
                }
            }
            return nullptr;
        }

        std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        std::vector<std::uint32_t> crc32c_tables()
        {
            std::vector<std::uint32_t> tables(8 * 256);
            for (std::uint32_t index = 0; index < 256; ++index)
            {
                std::uint32_t crc = index;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
                tables[index] = crc;
            }
            for (std::uint32_t slice = 1; slice < 8; ++slice)
                for (std::uint32_t index = 0; index < 256; ++index)
                {
                    const auto previous = tables[(slice - 1) * 256 + index];
                    tables[slice * 256 + index] = (previous >> 8) ^ tables[previous & 0xff];
                }
            return tables;
        }

        class Engine final : public CudaMergeEngine
        {
        public:
            Engine() : pool_(std::make_shared<PinnedPool>())
            {
                cuda_ok(cudaSetDevice(0), "cudaSetDevice");
                cuda_ok(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking), "cudaStreamCreate");
                const auto tables = crc32c_tables();
                cuda_ok(cudaMemcpy(tables_.reserve(tables.size()), tables.data(), tables.size() * sizeof(std::uint32_t),
                                   cudaMemcpyHostToDevice),
                        "copy CRC tables");
                options_ = nvcompBatchedLZ4DecompressDefaultOpts;
                options_.backend = NVCOMP_DECOMPRESS_BACKEND_CUDA;
                nvcomp_ok(nvcompBatchedLZ4DecompressGetRequiredAlignments(options_, &alignments_),
                          "nvCOMP LZ4 alignments");
                compress_options_ = nvcompBatchedLZ4CompressDefaultOpts;
                nvcomp_ok(nvcompBatchedLZ4CompressGetRequiredAlignments(compress_options_, &compress_alignments_),
                          "nvCOMP LZ4 compress alignments");
                zstd_options_ = nvcompBatchedZstdDecompressDefaultOpts;
                zstd_options_.backend = NVCOMP_DECOMPRESS_BACKEND_CUDA;
                nvcomp_ok(nvcompBatchedZstdDecompressGetRequiredAlignments(zstd_options_, &zstd_alignments_),
                          "nvCOMP ZSTD alignments");
                zstd_compress_options_ = nvcompBatchedZstdCompressDefaultOpts;
                nvcomp_ok(nvcompBatchedZstdCompressGetRequiredAlignments(zstd_compress_options_, &zstd_compress_alignments_),
                          "nvCOMP ZSTD compress alignments");
            }

            ~Engine() override
            {
                if (stream_ != nullptr)
                    cudaStreamDestroy(stream_);
            }

            rocksdb::Status merge(const rocksdb::CompactionMergeRequest &request,
                                  std::unique_ptr<rocksdb::CompactionMergedInput> *output,
                                  MergeRunStats *stats) override
            {
                const std::lock_guard guard(mutex_);
                cuda_ok(cudaSetDevice(0), "cudaSetDevice");
                const auto read_started = std::chrono::steady_clock::now();
                static const bool trace = std::getenv("LSM_MERGE_TRACE") != nullptr;
                auto phase_started = read_started;
                std::string phases;
                const auto mark = [&](const char *name) {
                    if (!trace)
                        return;
                    const auto now = std::chrono::steady_clock::now();
                    phases += " ";
                    phases += name;
                    phases += "=";
                    phases += std::to_string(std::chrono::duration<double, std::milli>(now - phase_started).count());
                    phase_started = now;
                };

                // 1. Read blocks into pinned memory and describe them.
                const auto serialized_bytes = merge_serialized_bytes(request);
                std::vector<std::uint64_t> offsets;
                char *host_serialized = host_serialized_.reserve(serialized_bytes);
                auto status = read_merge_blocks(request, host_serialized, &offsets);
                if (!status.ok())
                    return status;
                mark("read");
                const auto device_started = std::chrono::steady_clock::now();
                const auto *bytes = reinterpret_cast<const unsigned char *>(host_serialized);
                std::vector<GpuBlock> blocks;
                blocks.reserve(offsets.size());
                std::vector<std::uint32_t> compressed;
                std::uint64_t decoded_total = 0;
                const std::uint64_t output_alignment =
                    std::max<std::uint64_t>({8, alignments_.output, zstd_alignments_.output});
                const std::uint64_t input_alignment = std::max<std::uint64_t>(alignments_.input, zstd_alignments_.input);
                std::size_t block_index = 0;
                for (const auto &file : request.files)
                {
                    for (const auto &source : file.blocks)
                    {
                        const auto offset = offsets[block_index];
                        const auto *payload = bytes + offset;
                        const auto type = payload[source.size];
                        GpuBlock block{};
                        block.serialized_offset = offset;
                        block.checksum_bytes = source.size + 1;
                        block.expected_checksum = decode_fixed32(payload + source.size + 1) - source.checksum_modifier;
                        block.compression = type;
                        if (type == 0)
                        {
                            block.compressed_offset = offset;
                            block.compressed_bytes = source.size;
                            block.decoded_bytes = source.size;
                        }
                        else if (type == 4 || type == 7)
                        {
                            std::uint32_t decoded_bytes = 0;
                            const auto *data = decode_varint32(payload, payload + source.size, &decoded_bytes);
                            if (data == nullptr || data == payload + source.size)
                                return rocksdb::Status::Corruption("GPU merged-input invalid compressed size prefix");
                            block.compressed_offset = offset + static_cast<std::uint64_t>(data - payload);
                            block.compressed_bytes = static_cast<std::uint32_t>(payload + source.size - data);
                            block.decoded_bytes = decoded_bytes;
                            compressed.push_back(static_cast<std::uint32_t>(block_index));
                        }
                        else
                        {
                            return rocksdb::Status::NotSupported("GPU merged input supports only uncompressed, LZ4 and ZSTD blocks");
                        }
                        if (block.decoded_bytes == 0 || block.decoded_bytes > kMaxBlockBytes)
                            return rocksdb::Status::Corruption("GPU merged-input block size out of range");
                        decoded_total = align_up(decoded_total, output_alignment);
                        block.decoded_offset = decoded_total;
                        decoded_total += block.decoded_bytes;
                        blocks.push_back(block);
                        ++block_index;
                    }
                }
                const auto block_count = static_cast<std::uint32_t>(blocks.size());
                mark("describe");
                if (block_count == 0)
                {
                    *output = std::make_unique<PackedMergedInput>(pool_->take(1), std::vector<MergedEntry>{});
                    return rocksdb::Status::OK();
                }

                // 2. Upload, verify checksums, decompress.
                auto *d_serialized = d_serialized_.reserve(serialized_bytes);
                auto *d_blocks = d_blocks_.reserve(block_count);
                auto *d_decoded = d_decoded_.reserve(decoded_total);
                auto *d_crc_status = d_crc_status_.reserve(block_count);
                auto *h_blocks = h_blocks_.reserve(block_count);
                std::memcpy(h_blocks, blocks.data(), blocks.size() * sizeof(GpuBlock));
                cuda_ok(cudaMemcpyAsync(d_serialized, host_serialized, serialized_bytes, cudaMemcpyHostToDevice, stream_),
                        "upload blocks");
                cuda_ok(cudaMemcpyAsync(d_blocks, h_blocks, block_count * sizeof(GpuBlock), cudaMemcpyHostToDevice, stream_),
                        "upload descriptors");
                verify_blocks<<<grid(block_count, 128), 128, 0, stream_>>>(
                    d_serialized, d_blocks, block_count, d_decoded, tables_.get(), d_crc_status);
                cuda_ok(cudaGetLastError(), "verify_blocks");

                // LZ4 blocks first, then ZSTD; each codec is decoded with its own nvCOMP call.
                const auto lz4_end = std::stable_partition(compressed.begin(), compressed.end(), [&](std::uint32_t index) {
                    return blocks[index].compression == 4;
                });
                const auto lz4_count = static_cast<std::size_t>(lz4_end - compressed.begin());
                const auto compressed_count = compressed.size();
                if (compressed_count > 0)
                {
                    auto *h_sources = h_sources_.reserve(compressed_count);
                    auto *h_destinations = h_destinations_.reserve(compressed_count);
                    auto *h_input_sizes = h_input_sizes_.reserve(compressed_count);
                    auto *h_output_sizes = h_output_sizes_.reserve(compressed_count);
                    auto *h_compressed_index = h_compressed_index_.reserve(compressed_count);
                    std::size_t max_output = 0;
                    std::size_t total_output = 0;
                    bool aligned_sources = true;
                    for (std::size_t index = 0; index < compressed_count; ++index)
                    {
                        const auto &block = blocks[compressed[index]];
                        aligned_sources = aligned_sources && block.compressed_offset % input_alignment == 0;
                        h_destinations[index] = d_decoded + block.decoded_offset;
                        h_input_sizes[index] = block.compressed_bytes;
                        h_output_sizes[index] = block.decoded_bytes;
                        h_compressed_index[index] = compressed[index];
                        max_output = std::max<std::size_t>(max_output, block.decoded_bytes);
                        total_output += block.decoded_bytes;
                    }
                    if (aligned_sources)
                    {
                        for (std::size_t index = 0; index < compressed_count; ++index)
                            h_sources[index] = d_serialized + blocks[compressed[index]].compressed_offset;
                    }
                    else
                    {
                        // nvCOMP requires aligned inputs; copy compressed payloads to an aligned staging area.
                        auto *h_staged_offsets = h_staged_offsets_.reserve(compressed_count);
                        std::uint64_t staged_total = 0;
                        for (std::size_t index = 0; index < compressed_count; ++index)
                        {
                            staged_total = align_up(staged_total, input_alignment);
                            h_staged_offsets[index] = staged_total;
                            staged_total += blocks[compressed[index]].compressed_bytes;
                        }
                        auto *d_staged = d_staged_.reserve(staged_total);
                        auto *d_staged_offsets = d_staged_offsets_.reserve(compressed_count);
                        auto *d_compressed_index = d_compressed_index_.reserve(compressed_count);
                        cuda_ok(cudaMemcpyAsync(d_staged_offsets, h_staged_offsets, compressed_count * sizeof(std::uint64_t),
                                                cudaMemcpyHostToDevice, stream_),
                                "upload staging offsets");
                        cuda_ok(cudaMemcpyAsync(d_compressed_index, h_compressed_index,
                                                compressed_count * sizeof(std::uint32_t), cudaMemcpyHostToDevice, stream_),
                                "upload compressed index");
                        stage_compressed<<<grid(compressed_count, 128), 128, 0, stream_>>>(
                            d_serialized, d_blocks, d_compressed_index, d_staged_offsets,
                            static_cast<std::uint32_t>(compressed_count), d_staged);
                        cuda_ok(cudaGetLastError(), "stage_compressed");
                        for (std::size_t index = 0; index < compressed_count; ++index)
                            h_sources[index] = d_staged + h_staged_offsets[index];
                    }
                    auto *d_sources = d_sources_.reserve(compressed_count);
                    auto *d_destinations = d_destinations_.reserve(compressed_count);
                    auto *d_input_sizes = d_input_sizes_.reserve(compressed_count);
                    auto *d_output_sizes = d_output_sizes_.reserve(compressed_count);
                    auto *d_actual_sizes = d_actual_sizes_.reserve(compressed_count);
                    auto *d_statuses = d_statuses_.reserve(compressed_count);
                    cuda_ok(cudaMemcpyAsync(d_sources, h_sources, compressed_count * sizeof(const void *),
                                            cudaMemcpyHostToDevice, stream_),
                            "upload sources");
                    cuda_ok(cudaMemcpyAsync(d_destinations, h_destinations, compressed_count * sizeof(void *),
                                            cudaMemcpyHostToDevice, stream_),
                            "upload destinations");
                    cuda_ok(cudaMemcpyAsync(d_input_sizes, h_input_sizes, compressed_count * sizeof(std::size_t),
                                            cudaMemcpyHostToDevice, stream_),
                            "upload input sizes");
                    cuda_ok(cudaMemcpyAsync(d_output_sizes, h_output_sizes, compressed_count * sizeof(std::size_t),
                                            cudaMemcpyHostToDevice, stream_),
                            "upload output sizes");
                    const auto batch_limit = nvcomp_batch_limit();
                    for (std::size_t first = 0; first < compressed_count;)
                    {
                        const bool lz4 = first < lz4_count;
                        const auto end = lz4 ? lz4_count : compressed_count;
                        const auto count = std::min(batch_limit, end - first);
                        std::size_t batch_output = 0;
                        for (std::size_t index = first; index < first + count; ++index)
                            batch_output += h_output_sizes[index];
                        std::size_t temp_bytes = 0;
                        if (lz4)
                        {
                            nvcomp_ok(nvcompBatchedLZ4DecompressGetTempSizeAsync(count, max_output, options_, &temp_bytes,
                                                                                 batch_output),
                                      "nvCOMP LZ4 temp size");
                            auto *d_temp = d_nvcomp_temp_.reserve(temp_bytes);
                            nvcomp_ok(nvcompBatchedLZ4DecompressAsync(d_sources + first, d_input_sizes + first,
                                                                      d_output_sizes + first, d_actual_sizes + first, count,
                                                                      d_temp, temp_bytes, d_destinations + first, options_,
                                                                      d_statuses + first, stream_),
                                      "nvCOMP LZ4 decompress");
                        }
                        else
                        {
                            nvcomp_ok(nvcompBatchedZstdDecompressGetTempSizeAsync(count, max_output, zstd_options_,
                                                                                  &temp_bytes, batch_output),
                                      "nvCOMP ZSTD temp size");
                            auto *d_temp = d_nvcomp_temp_.reserve(temp_bytes);
                            nvcomp_ok(nvcompBatchedZstdDecompressAsync(d_sources + first, d_input_sizes + first,
                                                                       d_output_sizes + first, d_actual_sizes + first,
                                                                       count, d_temp, temp_bytes, d_destinations + first,
                                                                       zstd_options_, d_statuses + first, stream_),
                                      "nvCOMP ZSTD decompress");
                        }
                        first += count;
                    }
                    (void)total_output;
                    auto *h_statuses = h_statuses_.reserve(compressed_count);
                    auto *h_actual_sizes = h_actual_sizes_.reserve(compressed_count);
                    cuda_ok(cudaMemcpyAsync(h_statuses, d_statuses, compressed_count * sizeof(nvcompStatus_t),
                                            cudaMemcpyDeviceToHost, stream_),
                            "download nvCOMP statuses");
                    cuda_ok(cudaMemcpyAsync(h_actual_sizes, d_actual_sizes, compressed_count * sizeof(std::size_t),
                                            cudaMemcpyDeviceToHost, stream_),
                            "download nvCOMP sizes");
                }
                auto *h_crc_status = h_crc_status_.reserve(block_count);
                cuda_ok(cudaMemcpyAsync(h_crc_status, d_crc_status, block_count * sizeof(std::uint32_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download CRC statuses");
                cuda_ok(cudaStreamSynchronize(stream_), "synchronize decode");
                mark("upload_crc_lz4");
                for (std::uint32_t index = 0; index < block_count; ++index)
                    if (h_crc_status[index] != 0)
                        return rocksdb::Status::Corruption("GPU merged-input block CRC32C mismatch");
                for (std::size_t index = 0; index < compressed_count; ++index)
                    if (h_statuses_.get()[index] != nvcompSuccess ||
                        h_actual_sizes_.get()[index] != blocks[compressed[index]].decoded_bytes)
                        return rocksdb::Status::Corruption("GPU merged-input LZ4 decoding failed");

                // 3. Restart intervals.
                auto *d_error = d_error_.reserve(1);
                cuda_ok(cudaMemsetAsync(d_error, 0, sizeof(unsigned int), stream_), "clear error");
                auto *d_restarts = d_restarts_.reserve(block_count);
                auto *d_first_interval = d_first_interval_.reserve(block_count);
                count_restarts<<<grid(block_count, 128), 128, 0, stream_>>>(d_decoded, d_blocks, block_count, d_restarts,
                                                                           d_error);
                cuda_ok(cudaGetLastError(), "count_restarts");
                exclusive_sum(d_restarts, d_first_interval, block_count);
                auto *h_scalars = h_scalars_.reserve(4);
                cuda_ok(cudaMemcpyAsync(&h_scalars[0], d_first_interval + block_count - 1, sizeof(std::uint64_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download interval base");
                std::uint32_t last_restarts = 0;
                cuda_ok(cudaMemcpyAsync(h_u32_.reserve(2), d_restarts + block_count - 1, sizeof(std::uint32_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download last restarts");
                cuda_ok(cudaMemcpyAsync(h_u32_.get() + 1, d_error, sizeof(unsigned int), cudaMemcpyDeviceToHost, stream_),
                        "download error");
                cuda_ok(cudaStreamSynchronize(stream_), "synchronize restarts");
                mark("restarts");
                last_restarts = h_u32_.get()[0];
                status = parse_error(h_u32_.get()[1]);
                if (!status.ok())
                    return status;
                const std::uint64_t intervals = h_scalars[0] + last_restarts;
                auto *d_interval_block = d_interval_block_.reserve(intervals);
                expand_intervals<<<grid(block_count, 128), 128, 0, stream_>>>(d_restarts, d_first_interval, block_count,
                                                                             d_interval_block);
                cuda_ok(cudaGetLastError(), "expand_intervals");

                // 4. Parse: count, scan, write.
                KeyBounds bounds{};
                if (request.has_start || request.has_end)
                {
                    const auto bound_bytes = request.start_user_key.size() + request.end_user_key.size();
                    auto *h_bounds = h_bounds_.reserve(bound_bytes);
                    std::memcpy(h_bounds, request.start_user_key.data(), request.start_user_key.size());
                    std::memcpy(h_bounds + request.start_user_key.size(), request.end_user_key.data(),
                                request.end_user_key.size());
                    auto *d_bounds = d_bounds_.reserve(bound_bytes);
                    cuda_ok(cudaMemcpyAsync(d_bounds, h_bounds, bound_bytes, cudaMemcpyHostToDevice, stream_),
                            "upload bounds");
                    bounds = {d_bounds, d_bounds + request.start_user_key.size(),
                              static_cast<std::uint32_t>(request.start_user_key.size()),
                              static_cast<std::uint32_t>(request.end_user_key.size()), request.has_start, request.has_end};
                }
                auto *d_counts = d_counts_.reserve(intervals);
                auto *d_key_bytes = d_key_bytes_.reserve(intervals);
                auto *d_entry_offsets = d_entry_offsets_.reserve(intervals);
                auto *d_key_offsets = d_key_offsets_.reserve(intervals);
                parse_intervals<false><<<grid(intervals, 128), 128, 0, stream_>>>(
                    d_decoded, d_blocks, d_interval_block, d_first_interval, intervals, bounds, d_counts, d_key_bytes,
                    nullptr, nullptr, nullptr, nullptr, d_error);
                cuda_ok(cudaGetLastError(), "parse_intervals count");
                exclusive_sum(d_counts, d_entry_offsets, intervals);
                exclusive_sum(d_key_bytes, d_key_offsets, intervals);
                cuda_ok(cudaMemcpyAsync(&h_scalars[0], d_entry_offsets + intervals - 1, sizeof(std::uint64_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download entry base");
                cuda_ok(cudaMemcpyAsync(&h_scalars[1], d_key_offsets + intervals - 1, sizeof(std::uint64_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download key base");
                cuda_ok(cudaMemcpyAsync(h_u32_.get(), d_counts + intervals - 1, sizeof(std::uint32_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download last count");
                cuda_ok(cudaMemcpyAsync(&h_scalars[2], d_key_bytes + intervals - 1, sizeof(std::uint64_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download last key bytes");
                cuda_ok(cudaMemcpyAsync(h_u32_.get() + 1, d_error, sizeof(unsigned int), cudaMemcpyDeviceToHost, stream_),
                        "download error");
                cuda_ok(cudaStreamSynchronize(stream_), "synchronize count");
                mark("count");
                status = parse_error(h_u32_.get()[1]);
                if (!status.ok())
                    return status;
                const std::uint64_t entry_count = h_scalars[0] + h_u32_.get()[0];
                const std::uint64_t key_total = h_scalars[1] + h_scalars[2];
                if (entry_count >= 0xffffffffULL)
                    return rocksdb::Status::NotSupported("GPU merged input exceeds 4G entries");
                if (entry_count == 0)
                {
                    *output = std::make_unique<PackedMergedInput>(pool_->take(1), std::vector<MergedEntry>{});
                    return rocksdb::Status::OK();
                }
                auto *d_entries = d_entries_.reserve(entry_count);
                auto *d_keys = d_keys_.reserve(key_total);
                parse_intervals<true><<<grid(intervals, 128), 128, 0, stream_>>>(
                    d_decoded, d_blocks, d_interval_block, d_first_interval, intervals, bounds, nullptr, nullptr,
                    d_entry_offsets, d_key_offsets, d_entries, d_keys, d_error);
                cuda_ok(cudaGetLastError(), "parse_intervals write");

                // 5. Sort by internal key and pack.
                auto *d_order = d_order_.reserve(entry_count);
                iota<<<grid(entry_count, 256), 256, 0, stream_>>>(d_order, entry_count);
                cuda_ok(cudaGetLastError(), "iota");
                const InternalKeyLess less{d_entries, d_keys};
                std::size_t sort_temp_bytes = 0;
                cuda_ok(cub::DeviceMergeSort::SortKeys(nullptr, sort_temp_bytes, d_order,
                                                       static_cast<std::int64_t>(entry_count), less, stream_),
                        "merge sort temp size");
                auto *d_sort_temp = d_sort_temp_.reserve(sort_temp_bytes);
                cuda_ok(cub::DeviceMergeSort::SortKeys(d_sort_temp, sort_temp_bytes, d_order,
                                                       static_cast<std::int64_t>(entry_count), less, stream_),
                        "merge sort");
                rocksdb::CompactionMergeDropped dropped;
                std::uint64_t sorted_count = entry_count;
                if (request.drop_hidden)
                {
                    const auto snapshot_count = static_cast<std::uint32_t>(request.snapshots.size());
                    auto *d_snapshots = d_snapshots_.reserve(snapshot_count);
                    if (snapshot_count > 0)
                    {
                        auto *h_snapshots = h_snapshots_.reserve(snapshot_count);
                        std::memcpy(h_snapshots, request.snapshots.data(), snapshot_count * sizeof(std::uint64_t));
                        cuda_ok(cudaMemcpyAsync(d_snapshots, h_snapshots, snapshot_count * sizeof(std::uint64_t),
                                                cudaMemcpyHostToDevice, stream_),
                                "upload snapshots");
                    }
                    auto *d_keep = d_keep_.reserve(entry_count);
                    auto *d_dropped = d_dropped_.reserve(4);
                    auto *d_kept_order = d_kept_order_.reserve(entry_count);
                    auto *d_kept_count = d_kept_count_.reserve(1);
                    cuda_ok(cudaMemsetAsync(d_dropped, 0, 4 * sizeof(unsigned long long), stream_), "clear dropped");
                    mark_hidden<<<grid(entry_count, 256), 256, 0, stream_>>>(
                        d_order, d_entries, d_keys, entry_count, d_snapshots, snapshot_count, d_keep, d_dropped);
                    cuda_ok(cudaGetLastError(), "mark_hidden");
                    std::size_t select_bytes = 0;
                    cuda_ok(cub::DeviceSelect::Flagged(nullptr, select_bytes, d_order, d_keep, d_kept_order,
                                                       d_kept_count, static_cast<std::int64_t>(entry_count), stream_),
                            "select temp size");
                    auto *d_select_temp = d_select_temp_.reserve(select_bytes);
                    cuda_ok(cub::DeviceSelect::Flagged(d_select_temp, select_bytes, d_order, d_keep, d_kept_order,
                                                       d_kept_count, static_cast<std::int64_t>(entry_count), stream_),
                            "select kept");
                    auto *h_dropped = h_dropped_.reserve(5);
                    cuda_ok(cudaMemcpyAsync(h_dropped, d_dropped, 4 * sizeof(unsigned long long), cudaMemcpyDeviceToHost,
                                            stream_),
                            "download dropped");
                    cuda_ok(cudaMemcpyAsync(h_dropped + 4, d_kept_count, sizeof(unsigned long long),
                                            cudaMemcpyDeviceToHost, stream_),
                            "download kept count");
                    cuda_ok(cudaStreamSynchronize(stream_), "synchronize select");
                    mark("parse_sort_gc");
                    dropped = {h_dropped[0], h_dropped[1], h_dropped[2], h_dropped[3]};
                    sorted_count = h_dropped[4];
                    d_order = d_kept_order;
                    if (sorted_count == 0)
                    {
                        *output = std::make_unique<PackedMergedInput>(pool_->take(1), std::vector<MergedEntry>{}, dropped);
                        return rocksdb::Status::OK();
                    }
                }
                if (request.output.enabled)
                {
                    bool built = false;
                    status = build_output(request.output, d_order, d_entries, d_keys, d_decoded, sorted_count, dropped,
                                          output, &built, stats, mark);
                    if (!status.ok())
                        return status;
                    if (built)
                    {
                        if (trace)
                            std::fprintf(stderr, "lsm-merge blocks=%u entries=%llu kept=%llu bytes=%llu built=%llu%s\n",
                                         block_count, static_cast<unsigned long long>(entry_count),
                                         static_cast<unsigned long long>(sorted_count),
                                         static_cast<unsigned long long>(serialized_bytes),
                                         static_cast<unsigned long long>(stats ? stats->built_blocks : 0),
                                         phases.c_str());
                        if (stats)
                        {
                            const auto finished = std::chrono::steady_clock::now();
                            stats->blocks = block_count;
                            stats->input_bytes = serialized_bytes;
                            stats->decoded_bytes = decoded_total;
                            stats->entries = sorted_count;
                            stats->dropped = dropped.entries;
                            stats->read_ns = static_cast<std::uint64_t>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(device_started - read_started).count());
                            stats->device_ns = static_cast<std::uint64_t>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(finished - device_started).count());
                        }
                        return rocksdb::Status::OK();
                    }
                }
                auto *d_sizes = d_sizes_.reserve(sorted_count);
                auto *d_packed_offsets = d_packed_offsets_.reserve(sorted_count);
                sorted_sizes<<<grid(sorted_count, 256), 256, 0, stream_>>>(d_order, d_entries, sorted_count, d_sizes);
                cuda_ok(cudaGetLastError(), "sorted_sizes");
                exclusive_sum(d_sizes, d_packed_offsets, sorted_count);
                cuda_ok(cudaMemcpyAsync(&h_scalars[0], d_packed_offsets + sorted_count - 1, sizeof(std::uint64_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download packed base");
                cuda_ok(cudaMemcpyAsync(&h_scalars[1], d_sizes + sorted_count - 1, sizeof(std::uint64_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download last size");
                cuda_ok(cudaMemcpyAsync(h_u32_.get() + 1, d_error, sizeof(unsigned int), cudaMemcpyDeviceToHost, stream_),
                        "download error");
                cuda_ok(cudaStreamSynchronize(stream_), "synchronize sort");
                mark("sizes");
                status = parse_error(h_u32_.get()[1]);
                if (!status.ok())
                    return status;
                const std::uint64_t packed_bytes = h_scalars[0] + h_scalars[1];
                auto *d_packed = d_packed_.reserve(packed_bytes);
                auto *d_output_entries = d_output_entries_.reserve(sorted_count);
                gather<<<grid(sorted_count * 32, 256), 256, 0, stream_>>>(d_order, d_entries, d_keys, d_decoded,
                                                                        d_packed_offsets, sorted_count, d_packed,
                                                                        d_output_entries);
                cuda_ok(cudaGetLastError(), "gather");
                auto packed = pool_->take(packed_bytes);
                auto entry_buffer = pool_->take(sorted_count * sizeof(MergedEntry));
                auto *h_entries = reinterpret_cast<MergedEntry *>(entry_buffer.get());
                cuda_ok(cudaMemcpyAsync(packed.get(), d_packed, packed_bytes, cudaMemcpyDeviceToHost, stream_),
                        "download packed");
                cuda_ok(cudaMemcpyAsync(h_entries, d_output_entries, sorted_count * sizeof(MergedEntry),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download entries");
                cuda_ok(cudaStreamSynchronize(stream_), "synchronize gather");
                mark("gather_download");
                if (trace)
                    std::fprintf(stderr, "lsm-merge blocks=%u entries=%llu kept=%llu bytes=%llu%s\n", block_count,
                                 static_cast<unsigned long long>(entry_count),
                                 static_cast<unsigned long long>(sorted_count),
                                 static_cast<unsigned long long>(serialized_bytes), phases.c_str());
                const auto finished = std::chrono::steady_clock::now();
                if (stats)
                {
                    stats->blocks = block_count;
                    stats->input_bytes = serialized_bytes;
                    stats->decoded_bytes = decoded_total;
                    stats->entries = sorted_count;
                    stats->dropped = dropped.entries;
                    stats->read_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(device_started - read_started).count());
                    stats->device_ns = static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - device_started).count());
                }
                std::shared_ptr<const MergedEntry> entries(entry_buffer, h_entries);
                *output = std::make_unique<PackedMergedInput>(std::shared_ptr<const char>(packed), std::move(entries),
                                                              sorted_count, dropped);
                return rocksdb::Status::OK();
            }

        private:
            struct BuiltOwner
            {
                std::shared_ptr<char> blocks, contents, keys, hashes;
            };

            // Encodes the kept entries (d_order, internal-key order) into
            // finished data blocks. *built = false when an entry is not a Put
            // or has a user key over 240 bytes; the caller then packs the
            // entry stream as before.
            template <typename Mark>
            rocksdb::Status build_output(const rocksdb::CompactionOutputSpec &request_spec, const std::uint32_t *d_order,
                                         const GpuEntry *d_entries, const unsigned char *d_keys,
                                         const unsigned char *d_decoded, std::uint64_t count,
                                         rocksdb::CompactionMergeDropped dropped,
                                         std::unique_ptr<rocksdb::CompactionMergedInput> *output, bool *built,
                                         MergeRunStats *stats, Mark &mark)
            {
                *built = false;
                BuildSpec spec{};
                spec.block_bytes = request_spec.block_bytes;
                spec.deviation_limit =
                    (static_cast<std::uint64_t>(request_spec.block_bytes) * (100 - request_spec.block_size_deviation) + 99) / 100;
                spec.zero_limit = request_spec.earliest_snapshot;
                spec.interval = std::max<std::uint32_t>(1, request_spec.restart_interval);
                spec.zero_sequences = request_spec.zero_sequences ? 1 : 0;
                spec.hashes = request_spec.filter_hashes ? 1 : 0;
                spec.chunk = rocksdb::CompactionOutputSpec::kBuiltOutputChunkEntries;
                if (count >= 0xffffffffULL)
                    return rocksdb::Status::OK();

                // 0. Drop obsolete deletions exactly as CompactionIterator would.
                {
                    std::string range_bytes;
                    std::vector<std::uint64_t> range_offsets;
                    for (const auto &[start, end] : request_spec.beyond_ranges)
                    {
                        range_offsets.push_back(range_bytes.size());
                        range_offsets.push_back(start.size());
                        range_bytes += start;
                        range_offsets.push_back(range_bytes.size());
                        range_offsets.push_back(end.size());
                        range_bytes += end;
                    }
                    DeletionSpec deletion{};
                    deletion.earliest_snapshot = request_spec.earliest_snapshot_seq;
                    deletion.ranges = static_cast<std::uint32_t>(request_spec.beyond_ranges.size());
                    deletion.bottommost = request_spec.bottommost ? 1 : 0;
                    deletion.check_ranges = request_spec.check_beyond_ranges ? 1 : 0;
                    auto *d_range_bytes = b_range_bytes_.reserve(range_bytes.size());
                    auto *d_range_offsets = b_range_offsets_.reserve(range_offsets.size());
                    if (!range_bytes.empty())
                    {
                        auto *h_range_bytes = h_range_bytes_.reserve(range_bytes.size());
                        auto *h_range_offsets = h_range_offsets_.reserve(range_offsets.size());
                        std::memcpy(h_range_bytes, range_bytes.data(), range_bytes.size());
                        std::memcpy(h_range_offsets, range_offsets.data(), range_offsets.size() * sizeof(std::uint64_t));
                        cuda_ok(cudaMemcpyAsync(d_range_bytes, h_range_bytes, range_bytes.size(), cudaMemcpyHostToDevice,
                                                stream_),
                                "upload beyond ranges");
                        cuda_ok(cudaMemcpyAsync(d_range_offsets, h_range_offsets,
                                                range_offsets.size() * sizeof(std::uint64_t), cudaMemcpyHostToDevice,
                                                stream_),
                                "upload beyond offsets");
                    }
                    deletion.range_bytes = d_range_bytes;
                    deletion.range_offsets = d_range_offsets;
                    auto *d_keep = b_keep_.reserve(count);
                    auto *d_counters = b_deletion_counters_.reserve(3);
                    cuda_ok(cudaMemsetAsync(d_counters, 0, 3 * sizeof(unsigned long long), stream_), "clear deletions");
                    drop_deletions<<<grid(count, 256), 256, 0, stream_>>>(d_order, d_entries, d_keys, count, deletion,
                                                                          d_keep, d_counters);
                    cuda_ok(cudaGetLastError(), "drop_deletions");
                    auto *d_selected = b_kept_order_.reserve(count);
                    std::size_t select_bytes = 0;
                    cuda_ok(cub::DeviceSelect::Flagged(nullptr, select_bytes, d_order, d_keep, d_selected, d_counters + 2,
                                                       static_cast<std::int64_t>(count), stream_),
                            "deletion select temp size");
                    auto *d_select_temp = d_select_temp_.reserve(select_bytes);
                    cuda_ok(cub::DeviceSelect::Flagged(d_select_temp, select_bytes, d_order, d_keep, d_selected,
                                                       d_counters + 2, static_cast<std::int64_t>(count), stream_),
                            "deletion select");
                    auto *h_counters = h_deletion_counters_.reserve(3);
                    cuda_ok(cudaMemcpyAsync(h_counters, d_counters, 3 * sizeof(unsigned long long), cudaMemcpyDeviceToHost,
                                            stream_),
                            "download deletion counters");
                    cuda_ok(cudaStreamSynchronize(stream_), "synchronize deletions");
                    dropped.entries += h_counters[0];
                    dropped.deletions += h_counters[0];
                    dropped.key_bytes += h_counters[1];
                    d_order = d_selected;
                    count = h_counters[2];
                    mark("build_deletions");
                }
                if (count == 0)
                {
                    rocksdb::CompactionBuiltOutput empty;
                    if (stats)
                    {
                        stats->built_blocks = 0;
                        stats->built_bytes = 0;
                    }
                    *output = std::make_unique<BuiltMergedInput>(std::make_shared<BuiltOwner>(), empty, dropped);
                    *built = true;
                    return rocksdb::Status::OK();
                }

                // 1. Entry analysis and block cuts.
                // Separate from d_error_, which the entry-stream path reads as parse errors.
                auto *d_error = b_ineligible_.reserve(1);
                cuda_ok(cudaMemsetAsync(d_error, 0, sizeof(unsigned int), stream_), "clear build eligibility");
                auto *d_sequences = b_sequences_.reserve(count);
                auto *d_shared = b_shared_.reserve(count);
                auto *d_distinct = b_distinct_.reserve(count);
                auto *d_hashes = b_hashes_.reserve(count);
                build_entry_info<<<grid(count, 256), 256, 0, stream_>>>(d_order, d_entries, d_keys, count, spec,
                                                                      d_sequences, d_shared, d_distinct, d_hashes,
                                                                      d_error);
                cuda_ok(cudaGetLastError(), "build_entry_info");
                auto *d_start = b_start_.reserve(count);
                const auto chunks = (count + spec.chunk - 1) / spec.chunk;
                cut_blocks<<<grid(chunks, 64), 64, 0, stream_>>>(d_order, d_entries, d_shared, count, spec, d_start);
                cuda_ok(cudaGetLastError(), "cut_blocks");
                auto *d_block_of = b_block_of_.reserve(count);
                inclusive_sum(d_start, d_block_of, count);
                zero_base<<<grid(count, 256), 256, 0, stream_>>>(d_block_of, count);
                cuda_ok(cudaGetLastError(), "zero_base");
                auto *h_u32 = h_u32_.reserve(2);
                cuda_ok(cudaMemcpyAsync(h_u32, d_error, sizeof(unsigned int), cudaMemcpyDeviceToHost, stream_),
                        "download build error");
                cuda_ok(cudaMemcpyAsync(h_u32 + 1, d_block_of + count - 1, sizeof(std::uint32_t), cudaMemcpyDeviceToHost,
                                        stream_),
                        "download block count");
                cuda_ok(cudaStreamSynchronize(stream_), "synchronize build analysis");
                mark("build_analyze");
                if (h_u32[0] != 0)
                    return rocksdb::Status::OK();
                const std::uint32_t blocks = h_u32[1] + 1;

                // 2. Block layout and statistics.
                auto *d_first = b_first_.reserve(blocks + 1);
                block_firsts<<<grid(count, 256), 256, 0, stream_>>>(d_start, d_block_of, count, d_first);
                cuda_ok(cudaGetLastError(), "block_firsts");
                const std::uint32_t sentinel = static_cast<std::uint32_t>(count);
                auto *h_sentinel = h_sentinel_.reserve(1);
                *h_sentinel = sentinel;
                cuda_ok(cudaMemcpyAsync(d_first + blocks, h_sentinel, sizeof(std::uint32_t), cudaMemcpyHostToDevice, stream_),
                        "upload sentinel");
                auto *d_entry_bytes = b_entry_bytes_.reserve(count + 1);
                auto *d_position = b_position_.reserve(count + 1);
                entry_bytes<<<grid(count + 1, 256), 256, 0, stream_>>>(d_order, d_entries, d_shared, d_block_of, d_first,
                                                                     count, spec.interval, d_entry_bytes);
                cuda_ok(cudaGetLastError(), "entry_bytes");
                exclusive_sum(d_entry_bytes, d_position, count + 1);
                auto *d_built = b_built_.reserve(blocks);
                auto *d_raw_bytes = b_raw_bytes_.reserve(blocks + 1);
                auto *d_key_bytes = b_key_bytes_.reserve(blocks + 1);
                auto *d_hash_counts = b_hash_counts_.reserve(blocks + 1);
                cuda_ok(cudaMemsetAsync(d_raw_bytes + blocks, 0, sizeof(std::uint64_t), stream_), "clear raw tail");
                cuda_ok(cudaMemsetAsync(d_key_bytes + blocks, 0, sizeof(std::uint64_t), stream_), "clear key tail");
                cuda_ok(cudaMemsetAsync(d_hash_counts + blocks, 0, sizeof(std::uint64_t), stream_), "clear hash tail");
                block_summary<<<grid(blocks, 128), 128, 0, stream_>>>(d_order, d_entries, d_keys, d_first, d_position,
                                                                     d_sequences, d_distinct, blocks, spec.interval,
                                                                     d_built, d_raw_bytes, d_key_bytes, d_hash_counts);
                cuda_ok(cudaGetLastError(), "block_summary");
                auto *d_raw_offset = b_raw_offset_.reserve(blocks + 1);
                auto *d_key_offset = b_key_offset_.reserve(blocks + 1);
                auto *d_hash_offset = b_hash_offset_.reserve(blocks + 1);
                exclusive_sum(d_raw_bytes, d_raw_offset, blocks + 1);
                exclusive_sum(d_key_bytes, d_key_offset, blocks + 1);
                exclusive_sum(d_hash_counts, d_hash_offset, blocks + 1);
                auto *d_max_raw = b_max_raw_.reserve(1);
                max_reduce(d_raw_bytes, d_max_raw, blocks);
                auto *h_totals = h_totals_.reserve(4);
                cuda_ok(cudaMemcpyAsync(&h_totals[0], d_raw_offset + blocks, sizeof(std::uint64_t), cudaMemcpyDeviceToHost,
                                        stream_),
                        "download raw total");
                cuda_ok(cudaMemcpyAsync(&h_totals[1], d_key_offset + blocks, sizeof(std::uint64_t), cudaMemcpyDeviceToHost,
                                        stream_),
                        "download key total");
                cuda_ok(cudaMemcpyAsync(&h_totals[2], d_hash_offset + blocks, sizeof(std::uint64_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download hash total");
                cuda_ok(cudaMemcpyAsync(&h_totals[3], d_max_raw, sizeof(std::uint64_t), cudaMemcpyDeviceToHost, stream_),
                        "download max raw");
                cuda_ok(cudaStreamSynchronize(stream_), "synchronize build layout");
                mark("build_layout");
                const auto raw_total = h_totals[0];
                const auto key_total = h_totals[1];
                const auto hash_total = h_totals[2];
                const auto max_raw = h_totals[3];
                if (max_raw > kMaxBlockBytes)
                    return rocksdb::Status::OK();

                // 3. Encode blocks.
                auto *d_raw = b_raw_.reserve(raw_total);
                encode_entries<<<grid(count * 32, 256), 256, 0, stream_>>>(d_order, d_entries, d_keys, d_decoded,
                                                                          d_shared, d_sequences, d_block_of, d_first,
                                                                          d_position, d_raw_offset, count,
                                                                          spec.interval, d_raw);
                cuda_ok(cudaGetLastError(), "encode_entries");
                encode_restarts<<<grid(blocks, 128), 128, 0, stream_>>>(d_first, d_position, d_raw_offset, blocks,
                                                                       spec.interval, d_raw);
                cuda_ok(cudaGetLastError(), "encode_restarts");

                // 4. Compress, choose stored form, copy and checksum.
                auto *d_compressed_bytes = b_compressed_bytes_.reserve(blocks);
                auto *d_statuses = b_statuses_.reserve(blocks);
                std::uint64_t stride = 0;
                unsigned char *d_compressed = nullptr;
                const std::uint32_t codec = request_spec.zstd ? 7u : (request_spec.lz4 ? 4u : 0u);
                if (codec != 0)
                {
                    std::size_t max_compressed = 0;
                    if (codec == 4)
                        nvcomp_ok(nvcompBatchedLZ4CompressGetMaxOutputChunkSize(max_raw, compress_options_, &max_compressed),
                                  "nvCOMP max output");
                    else
                        nvcomp_ok(nvcompBatchedZstdCompressGetMaxOutputChunkSize(max_raw, zstd_compress_options_,
                                                                                &max_compressed),
                                  "nvCOMP ZSTD max output");
                    stride = align_up(max_compressed, std::max<std::uint64_t>(
                                                          {8, compress_alignments_.output, zstd_compress_alignments_.output}));
                    d_compressed = b_compressed_.reserve(stride * blocks);
                    auto *d_sources = b_sources_.reserve(blocks);
                    auto *d_source_bytes = b_source_bytes_.reserve(blocks);
                    auto *d_destinations = b_destinations_.reserve(blocks);
                    compress_arguments<<<grid(blocks, 128), 128, 0, stream_>>>(d_raw, d_raw_offset, d_built, blocks,
                                                                              d_compressed, stride, d_sources,
                                                                              d_source_bytes, d_destinations);
                    cuda_ok(cudaGetLastError(), "compress_arguments");
                    const std::size_t batch_limit = nvcomp_batch_limit();
                    for (std::size_t first = 0; first < blocks; first += batch_limit)
                    {
                        const auto count = std::min<std::size_t>(batch_limit, blocks - first);
                        std::size_t temp_bytes = 0;
                        const auto bound = std::min<std::size_t>(raw_total, static_cast<std::size_t>(max_raw) * count);
                        if (codec == 4)
                        {
                            nvcomp_ok(nvcompBatchedLZ4CompressGetTempSizeAsync(count, max_raw, compress_options_,
                                                                               &temp_bytes, bound),
                                      "nvCOMP compress temp size");
                            auto *d_temp = b_compress_temp_.reserve(temp_bytes);
                            nvcomp_ok(nvcompBatchedLZ4CompressAsync(d_sources + first, d_source_bytes + first, max_raw,
                                                                    count, d_temp, temp_bytes, d_destinations + first,
                                                                    d_compressed_bytes + first, compress_options_,
                                                                    d_statuses + first, stream_),
                                      "nvCOMP LZ4 compress");
                        }
                        else
                        {
                            nvcomp_ok(nvcompBatchedZstdCompressGetTempSizeAsync(count, max_raw, zstd_compress_options_,
                                                                                &temp_bytes, bound),
                                      "nvCOMP ZSTD compress temp size");
                            auto *d_temp = b_compress_temp_.reserve(temp_bytes);
                            nvcomp_ok(nvcompBatchedZstdCompressAsync(d_sources + first, d_source_bytes + first, max_raw,
                                                                     count, d_temp, temp_bytes, d_destinations + first,
                                                                     d_compressed_bytes + first, zstd_compress_options_,
                                                                     d_statuses + first, stream_),
                                      "nvCOMP ZSTD compress");
                        }
                    }
                }
                auto *d_contents_bytes = b_contents_bytes_.reserve(blocks + 1);
                cuda_ok(cudaMemsetAsync(d_contents_bytes + blocks, 0, sizeof(std::uint64_t), stream_), "clear contents tail");
                choose_contents<<<grid(blocks, 128), 128, 0, stream_>>>(d_built, d_compressed_bytes, d_statuses, blocks,
                                                                       codec,
                                                                       request_spec.max_compressed_bytes_per_kb,
                                                                       d_contents_bytes);
                cuda_ok(cudaGetLastError(), "choose_contents");
                auto *d_final_offset = b_final_offset_.reserve(blocks + 1);
                exclusive_sum(d_contents_bytes, d_final_offset, blocks + 1);
                cuda_ok(cudaMemcpyAsync(&h_totals[0], d_final_offset + blocks, sizeof(std::uint64_t),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download contents total");
                cuda_ok(cudaStreamSynchronize(stream_), "synchronize build compress");
                mark("build_encode_compress");
                const auto contents_total = h_totals[0];
                auto *d_contents = b_contents_.reserve(contents_total);
                finalize_contents<<<blocks, 128, 0, stream_>>>(d_built, d_raw, d_raw_offset, d_compressed, stride,
                                                              d_final_offset, tables_.get(), d_contents);
                cuda_ok(cudaGetLastError(), "finalize_contents");

                // 5. Boundary keys and filter hashes.
                auto *d_arena = b_key_arena_.reserve(key_total);
                block_keys<<<grid(blocks, 128), 128, 0, stream_>>>(d_order, d_entries, d_keys, d_sequences, d_first,
                                                                  d_key_offset, d_hash_offset, blocks, d_built, d_arena);
                cuda_ok(cudaGetLastError(), "block_keys");
                auto *d_selected = b_selected_hashes_.reserve(count);
                auto *d_selected_count = d_kept_count_.reserve(1);
                std::size_t select_bytes = 0;
                cuda_ok(cub::DeviceSelect::Flagged(nullptr, select_bytes, d_hashes, d_distinct, d_selected,
                                                   d_selected_count, static_cast<std::int64_t>(count), stream_),
                        "hash select temp size");
                auto *d_select_temp = d_select_temp_.reserve(select_bytes);
                cuda_ok(cub::DeviceSelect::Flagged(d_select_temp, select_bytes, d_hashes, d_distinct, d_selected,
                                                   d_selected_count, static_cast<std::int64_t>(count), stream_),
                        "hash select");

                // 6. Download.
                BuiltOwner owner;
                owner.blocks = pool_->take(blocks * sizeof(rocksdb::CompactionBuiltBlock));
                owner.contents = pool_->take(contents_total);
                owner.keys = pool_->take(key_total);
                owner.hashes = pool_->take(std::max<std::uint64_t>(hash_total, 1) * sizeof(std::uint64_t));
                cuda_ok(cudaMemcpyAsync(owner.blocks.get(), d_built, blocks * sizeof(rocksdb::CompactionBuiltBlock),
                                        cudaMemcpyDeviceToHost, stream_),
                        "download built blocks");
                cuda_ok(cudaMemcpyAsync(owner.contents.get(), d_contents, contents_total, cudaMemcpyDeviceToHost, stream_),
                        "download contents");
                cuda_ok(cudaMemcpyAsync(owner.keys.get(), d_arena, key_total, cudaMemcpyDeviceToHost, stream_),
                        "download block keys");
                if (hash_total > 0)
                    cuda_ok(cudaMemcpyAsync(owner.hashes.get(), d_selected, hash_total * sizeof(std::uint64_t),
                                            cudaMemcpyDeviceToHost, stream_),
                            "download hashes");
                cuda_ok(cudaStreamSynchronize(stream_), "synchronize build download");
                mark("build_download");

                rocksdb::CompactionBuiltOutput result;
                result.blocks = reinterpret_cast<const rocksdb::CompactionBuiltBlock *>(owner.blocks.get());
                result.block_count = blocks;
                result.contents = owner.contents.get();
                result.keys = owner.keys.get();
                result.hashes = reinterpret_cast<const std::uint64_t *>(owner.hashes.get());
                result.entries = count;
                if (stats)
                {
                    stats->built_blocks = blocks;
                    stats->built_bytes = contents_total;
                }
                *output = std::make_unique<BuiltMergedInput>(std::make_shared<BuiltOwner>(std::move(owner)), result,
                                                             dropped);
                *built = true;
                return rocksdb::Status::OK();
            }

            template <typename In, typename Out>
            void inclusive_sum(In *input, Out *output, std::uint64_t count)
            {
                std::size_t bytes = 0;
                cuda_ok(cub::DeviceScan::InclusiveSum(nullptr, bytes, input, output, static_cast<std::int64_t>(count),
                                                      stream_),
                        "inclusive scan temp size");
                auto *temp = d_scan_temp_.reserve(bytes);
                cuda_ok(cub::DeviceScan::InclusiveSum(temp, bytes, input, output, static_cast<std::int64_t>(count),
                                                      stream_),
                        "inclusive scan");
            }

            void max_reduce(const std::uint64_t *input, std::uint64_t *output, std::uint64_t count)
            {
                std::size_t bytes = 0;
                cuda_ok(cub::DeviceReduce::Max(nullptr, bytes, input, output, static_cast<std::int64_t>(count), stream_),
                        "max temp size");
                auto *temp = d_scan_temp_.reserve(bytes);
                cuda_ok(cub::DeviceReduce::Max(temp, bytes, input, output, static_cast<std::int64_t>(count), stream_),
                        "max reduce");
            }

            template <typename In, typename Out>
            void exclusive_sum(In *input, Out *output, std::uint64_t count)
            {
                std::size_t bytes = 0;
                cuda_ok(cub::DeviceScan::ExclusiveSum(nullptr, bytes, input, output, static_cast<std::int64_t>(count),
                                                      stream_),
                        "scan temp size");
                auto *temp = d_scan_temp_.reserve(bytes);
                cuda_ok(cub::DeviceScan::ExclusiveSum(temp, bytes, input, output, static_cast<std::int64_t>(count),
                                                      stream_),
                        "scan");
            }

            static rocksdb::Status parse_error(unsigned int error)
            {
                if (error & kErrorCorrupt)
                    return rocksdb::Status::Corruption("GPU merged-input data block parse failed");
                if (error & kErrorFooter)
                    return rocksdb::Status::NotSupported("GPU merged input requires binary-search data blocks");
                if (error & kErrorEntryType)
                    return rocksdb::Status::NotSupported("GPU merged input supports only Put and Delete entries");
                if (error & kErrorKeyTooLong)
                    return rocksdb::Status::NotSupported("GPU merged input key exceeds 512 bytes");
                return rocksdb::Status::OK();
            }

            std::mutex mutex_;
            cudaStream_t stream_ = nullptr;
            std::shared_ptr<PinnedPool> pool_;
            nvcompBatchedLZ4DecompressOpts_t options_{};
            nvcompAlignmentRequirements_t alignments_{};
            DeviceArray<std::uint32_t> tables_;
            HostArray<char> host_serialized_;
            HostArray<GpuBlock> h_blocks_;
            HostArray<const void *> h_sources_;
            HostArray<void *> h_destinations_;
            HostArray<std::size_t> h_input_sizes_, h_output_sizes_, h_actual_sizes_;
            HostArray<std::uint32_t> h_compressed_index_, h_crc_status_, h_u32_;
            HostArray<std::uint64_t> h_staged_offsets_, h_scalars_;
            HostArray<nvcompStatus_t> h_statuses_;
            HostArray<unsigned char> h_bounds_;
            DeviceArray<unsigned char> d_serialized_, d_decoded_, d_staged_, d_bounds_, d_keys_, d_packed_;
            DeviceArray<GpuBlock> d_blocks_;
            DeviceArray<std::uint32_t> d_crc_status_, d_compressed_index_, d_restarts_, d_interval_block_, d_counts_,
                d_order_;
            DeviceArray<unsigned int> d_error_;
            DeviceArray<std::uint64_t> d_staged_offsets_, d_first_interval_, d_key_bytes_, d_entry_offsets_,
                d_key_offsets_, d_sizes_, d_packed_offsets_;
            DeviceArray<const void *> d_sources_;
            DeviceArray<void *> d_destinations_;
            DeviceArray<std::size_t> d_input_sizes_, d_output_sizes_, d_actual_sizes_;
            DeviceArray<nvcompStatus_t> d_statuses_;
            DeviceArray<unsigned char> d_nvcomp_temp_, d_sort_temp_, d_scan_temp_;
            DeviceArray<GpuEntry> d_entries_;
            DeviceArray<MergedEntry> d_output_entries_;
            DeviceArray<std::uint64_t> d_snapshots_;
            DeviceArray<unsigned char> d_keep_, d_select_temp_;
            DeviceArray<unsigned long long> d_dropped_, d_kept_count_;
            DeviceArray<std::uint32_t> d_kept_order_;
            HostArray<std::uint64_t> h_snapshots_;
            HostArray<unsigned long long> h_dropped_;
            // Stage 3 buffers.
            nvcompBatchedLZ4CompressOpts_t compress_options_{};
            nvcompAlignmentRequirements_t compress_alignments_{};
            nvcompBatchedZstdDecompressOpts_t zstd_options_{};
            nvcompAlignmentRequirements_t zstd_alignments_{};
            nvcompBatchedZstdCompressOpts_t zstd_compress_options_{};
            nvcompAlignmentRequirements_t zstd_compress_alignments_{};
            DeviceArray<std::uint64_t> b_sequences_, b_hashes_, b_entry_bytes_, b_position_, b_raw_bytes_, b_key_bytes_,
                b_hash_counts_, b_raw_offset_, b_key_offset_, b_hash_offset_, b_max_raw_, b_contents_bytes_,
                b_final_offset_, b_selected_hashes_;
            DeviceArray<std::uint32_t> b_shared_, b_start_, b_block_of_, b_first_;
            DeviceArray<unsigned char> b_distinct_, b_raw_, b_compressed_, b_compress_temp_, b_contents_, b_key_arena_;
            DeviceArray<rocksdb::CompactionBuiltBlock> b_built_;
            DeviceArray<const void *> b_sources_;
            DeviceArray<std::size_t> b_source_bytes_, b_compressed_bytes_;
            DeviceArray<void *> b_destinations_;
            DeviceArray<nvcompStatus_t> b_statuses_;
            DeviceArray<unsigned int> b_ineligible_;
            DeviceArray<unsigned char> b_range_bytes_, b_keep_;
            DeviceArray<std::uint64_t> b_range_offsets_;
            DeviceArray<unsigned long long> b_deletion_counters_;
            DeviceArray<std::uint32_t> b_kept_order_;
            HostArray<unsigned char> h_range_bytes_;
            HostArray<std::uint64_t> h_range_offsets_;
            HostArray<unsigned long long> h_deletion_counters_;
            HostArray<std::uint32_t> h_sentinel_;
            HostArray<std::uint64_t> h_totals_;
        };
    }

    std::unique_ptr<CudaMergeEngine> make_cuda_merge_engine() { return std::make_unique<Engine>(); }
}
