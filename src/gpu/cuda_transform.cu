#include "lsm_lab/block_transform.hpp"
#include "lsm_lab/cuda_merge.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <future>
#include <map>
#include <numeric>
#include <stdexcept>
#include <chrono>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <cuda_runtime.h>
#include <nvcomp.h>
#include <nvcomp/lz4.h>

static_assert(NVCOMP_VER_MAJOR == 5 && NVCOMP_VER_MINOR == 3 && NVCOMP_VER_PATCH == 0 && NVCOMP_VER_BUILD == 16,
              "the CUDA adapter requires the pinned nvCOMP 5.3.0.16 headers");

namespace lsm_lab
{
    namespace
    {
        void cuda_check(cudaError_t status)
        {
            if (status != cudaSuccess)
                throw std::runtime_error(cudaGetErrorString(status));
        }

        void nvcomp_check(nvcompStatus_t status)
        {
            if (status != nvcompSuccess)
                throw std::runtime_error(nvcompGetStatusString(status));
        }

        template <typename Element>
        class DeviceBuffer
        {
        public:
            DeviceBuffer() = default;
            // front_guard bytes are allocated before data. nvCOMP 5.3.0.16 LZ4 decoding of a
            // malformed back-reference can read before the destination buffer; LZ4 offsets
            // are < 64 KiB, so a 64 KiB guard keeps that read in bounds and the status check
            // reports corruption instead of faulting the CUDA context.
            explicit DeviceBuffer(std::size_t front_guard) : guard_(front_guard) {}
            ~DeviceBuffer()
            {
                if (data != nullptr)
                    cudaFree(data - guard_elements());
            }
            DeviceBuffer(const DeviceBuffer &) = delete;
            DeviceBuffer &operator=(const DeviceBuffer &) = delete;

            void reserve(std::size_t count)
            {
                count = std::max<std::size_t>(1, count);
                if (count <= capacity)
                    return;
                Element *replacement = nullptr;
                cuda_check(cudaMalloc(reinterpret_cast<void **>(&replacement),
                                      (count + guard_elements()) * sizeof(Element)));
                cuda_check(cudaMemset(replacement, 0, guard_elements() * sizeof(Element)));
                if (data != nullptr)
                    cuda_check(cudaFree(data - guard_elements()));
                data = replacement + guard_elements();
                capacity = count;
            }

            Element *data = nullptr;
            std::size_t capacity = 0;

        private:
            std::size_t guard_elements() const { return (guard_ + sizeof(Element) - 1) / sizeof(Element); }
            std::size_t guard_ = 0;
        };

        template <typename Element>
        class PinnedBuffer
        {
        public:
            PinnedBuffer() = default;
            ~PinnedBuffer()
            {
                if (data != nullptr)
                    cudaFreeHost(data);
            }
            PinnedBuffer(const PinnedBuffer &) = delete;
            PinnedBuffer &operator=(const PinnedBuffer &) = delete;

            void reserve(std::size_t count)
            {
                count = std::max<std::size_t>(1, count);
                if (count <= capacity)
                    return;
                Element *replacement = nullptr;
                cuda_check(cudaHostAlloc(reinterpret_cast<void **>(&replacement), count * sizeof(Element),
                                         cudaHostAllocPortable));
                if (data != nullptr)
                    cuda_check(cudaFreeHost(data));
                data = replacement;
                capacity = count;
            }

            Element *data = nullptr;
            std::size_t capacity = 0;
        };

        class Stream
        {
        public:
            void create() { cuda_check(cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking)); }
            ~Stream()
            {
                if (value != nullptr)
                    cudaStreamDestroy(value);
            }
            cudaStream_t value = nullptr;
        };

        class Event
        {
        public:
            void create(unsigned int flags) { cuda_check(cudaEventCreateWithFlags(&value, flags)); }
            ~Event()
            {
                if (value != nullptr)
                    cudaEventDestroy(value);
            }
            cudaEvent_t value = nullptr;
        };

        // LSM_CUDA_SYNC selects how host threads wait for the GPU: "auto" (CUDA's
        // default heuristic, which spins on hosts with many logical CPUs), "spin",
        // or "blocking" (sleep on an OS primitive).
        struct SyncMode
        {
            const char *label;
            unsigned int device_flags;
            unsigned int event_flags;
        };

        SyncMode sync_mode_from_environment()
        {
            const char *value = std::getenv("LSM_CUDA_SYNC");
            const std::string mode = value == nullptr ? "" : value;
            if (mode.empty() || mode == "auto")
                return {"auto", cudaDeviceScheduleAuto, cudaEventDefault};
            if (mode == "spin")
                return {"spin", cudaDeviceScheduleSpin, cudaEventDefault};
            if (mode == "blocking")
                return {"blocking", cudaDeviceScheduleBlockingSync, cudaEventBlockingSync};
            throw std::runtime_error("LSM_CUDA_SYNC must be auto, spin, or blocking");
        }

        class FutureOutputSubmission final : public rocksdb::CompactionOutputSubmission
        {
        public:
            using Result = std::pair<
                rocksdb::Status,
                std::vector<rocksdb::CompactionOutputBlock>>;

            explicit FutureOutputSubmission(std::future<Result> result)
                : result_(std::move(result)) {}

            rocksdb::Status Finish(
                std::vector<rocksdb::CompactionOutputBlock> *output) noexcept override
            {
                if (finished_)
                    return rocksdb::Status::InvalidArgument(
                        "compaction output submission already finished");
                finished_ = true;
                output->clear();
                try
                {
                    auto result = result_.get();
                    if (result.first.ok())
                        *output = std::move(result.second);
                    return result.first;
                }
                catch (const std::exception &error)
                {
                    return rocksdb::Status::Aborted(error.what());
                }
            }

        private:
            std::future<Result> result_;
            bool finished_ = false;
        };

        __device__ unsigned int crc32c_matrix_times(const unsigned int *matrix, unsigned int vector)
        {
            unsigned int result = 0;
            while (vector != 0)
            {
                if ((vector & 1u) != 0)
                    result ^= *matrix;
                vector >>= 1;
                ++matrix;
            }
            return result;
        }

        __device__ void crc32c_matrix_square(unsigned int *square, const unsigned int *matrix)
        {
            for (unsigned int bit = 0; bit < 32; ++bit)
                square[bit] = crc32c_matrix_times(matrix, matrix[bit]);
        }

        __device__ void crc32c_matrix_multiply(unsigned int *product, const unsigned int *left,
                                               const unsigned int *right)
        {
            for (unsigned int column = 0; column < 32; ++column)
                product[column] = crc32c_matrix_times(left, right[column]);
        }

        __device__ void crc32c_shift_operator(unsigned int *result, unsigned int bytes)
        {
            if (bytes == 0)
            {
                for (unsigned int bit = 0; bit < 32; ++bit)
                    result[bit] = 1u << bit;
                return;
            }
            unsigned int odd[32];
            unsigned int even[32];
            unsigned int power[32];
            unsigned int temporary[32];
            odd[0] = 0x82f63b78u;
            unsigned int row = 1;
            for (unsigned int bit = 1; bit < 32; ++bit)
            {
                odd[bit] = row;
                row <<= 1;
            }
            crc32c_matrix_square(even, odd);
            crc32c_matrix_square(odd, even);
            crc32c_matrix_square(power, odd);
            for (unsigned int bit = 0; bit < 32; ++bit)
                result[bit] = 1u << bit;
            do
            {
                if ((bytes & 1u) != 0)
                {
                    crc32c_matrix_multiply(temporary, power, result);
                    for (unsigned int bit = 0; bit < 32; ++bit)
                        result[bit] = temporary[bit];
                }
                bytes >>= 1;
                if (bytes == 0)
                    break;
                crc32c_matrix_square(temporary, power);
                for (unsigned int bit = 0; bit < 32; ++bit)
                    power[bit] = temporary[bit];
            } while (bytes != 0);
        }

        __device__ unsigned int crc32c_chunk(const unsigned char *input, unsigned int bytes)
        {
            unsigned int crc = 0xffffffffu;
            for (unsigned int offset = 0; offset < bytes; ++offset)
            {
                crc ^= input[offset];
                for (unsigned int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
            }
            return ~crc;
        }

        __global__ void verify_and_copy(const unsigned char *input, const BlockDescriptor *blocks,
                                        unsigned int count, unsigned char *output, unsigned int *statuses)
        {
            const auto index = blockIdx.x;
            const auto lane = threadIdx.x;
            if (index >= count)
                return;
            const auto block = blocks[index];
            const auto begin = static_cast<unsigned int>(
                (static_cast<unsigned long long>(block.checksum_bytes) * lane) / blockDim.x);
            const auto end = static_cast<unsigned int>(
                (static_cast<unsigned long long>(block.checksum_bytes) * (lane + 1)) / blockDim.x);
            __shared__ unsigned int partial_crc[128];
            __shared__ unsigned int partial_bytes[128];
            __shared__ unsigned int short_shift[32];
            __shared__ unsigned int long_shift[32];
            partial_crc[lane] = crc32c_chunk(input + block.input_offset + begin, end - begin);
            partial_bytes[lane] = end - begin;
            __syncthreads();
            if (lane == 0)
            {
                const auto short_bytes = block.checksum_bytes / blockDim.x;
                crc32c_shift_operator(short_shift, short_bytes);
                crc32c_shift_operator(long_shift, short_bytes + 1);
                unsigned int crc = partial_crc[0];
                for (unsigned int part = 1; part < blockDim.x; ++part)
                {
                    const auto *shift = partial_bytes[part] == short_bytes ? short_shift : long_shift;
                    crc = crc32c_matrix_times(shift, crc) ^ partial_crc[part];
                }
                statuses[index] =
                    (((crc >> 15) | (crc << 17)) + 0xa282ead8u == block.expected_checksum) ? 0 : 1;
            }
            __syncthreads();
            if (statuses[index] == 0 && block.compression == 0)
            {
                for (unsigned int offset = lane; offset < block.output_bytes; offset += blockDim.x)
                    output[block.output_offset + offset] = input[block.compressed_offset + offset];
            }
        }

        __global__ void finalize_compressed_blocks(
            const unsigned char *input, const unsigned char *compressed, const BlockDescriptor *blocks,
            unsigned int count, unsigned int compressed_stride, const std::size_t *compressed_sizes,
            unsigned char *output, std::size_t *output_sizes, unsigned int *checksums)
        {
            const auto index = blockIdx.x;
            const auto lane = threadIdx.x;
            if (index >= count)
                return;
            const auto block = blocks[index];
            const auto compressed_bytes = static_cast<unsigned int>(compressed_sizes[index]);
            const bool use_compressed =
                compressed_bytes + 5 < block.output_bytes - block.output_bytes / 8;
            __shared__ unsigned int contents_bytes;
            __shared__ unsigned int prefix_bytes;
            if (lane == 0)
            {
                prefix_bytes = 0;
                if (use_compressed)
                {
                    auto value = block.output_bytes;
                    do
                    {
                        output[block.output_offset + prefix_bytes++] =
                            static_cast<unsigned char>((value & 127u) | (value > 127u ? 128u : 0u));
                        value >>= 7;
                    } while (value != 0);
                    contents_bytes = prefix_bytes + compressed_bytes;
                }
                else
                {
                    contents_bytes = block.output_bytes;
                }
                output_sizes[index] = contents_bytes;
            }
            __syncthreads();
            const auto *source = use_compressed
                ? compressed + static_cast<std::size_t>(index) * compressed_stride
                : input + block.input_offset;
            const auto source_bytes = use_compressed ? compressed_bytes : block.output_bytes;
            const auto destination_offset = block.output_offset + (use_compressed ? prefix_bytes : 0u);
            for (unsigned int offset = lane; offset < source_bytes; offset += blockDim.x)
                output[destination_offset + offset] = source[offset];
            __syncthreads();
            if (lane == 0)
                output[block.output_offset + contents_bytes] = use_compressed ? 4 : 0;
            __syncthreads();
            const auto begin = static_cast<unsigned int>(
                (static_cast<unsigned long long>(contents_bytes + 1) * lane) / blockDim.x);
            const auto end = static_cast<unsigned int>(
                (static_cast<unsigned long long>(contents_bytes + 1) * (lane + 1)) / blockDim.x);
            __shared__ unsigned int partial_crc[128];
            __shared__ unsigned int partial_bytes[128];
            __shared__ unsigned int short_shift[32];
            __shared__ unsigned int long_shift[32];
            partial_crc[lane] = crc32c_chunk(output + block.output_offset + begin, end - begin);
            partial_bytes[lane] = end - begin;
            __syncthreads();
            if (lane == 0)
            {
                const auto short_bytes = (contents_bytes + 1) / blockDim.x;
                crc32c_shift_operator(short_shift, short_bytes);
                crc32c_shift_operator(long_shift, short_bytes + 1);
                unsigned int crc = partial_crc[0];
                for (unsigned int part = 1; part < blockDim.x; ++part)
                {
                    const auto *shift = partial_bytes[part] == short_bytes ? short_shift : long_shift;
                    crc = crc32c_matrix_times(shift, crc) ^ partial_crc[part];
                }
                checksums[index] = ((crc >> 15) | (crc << 17)) + 0xa282ead8u;
            }
        }

        std::size_t aligned(std::size_t value, std::size_t alignment)
        {
            if (alignment == 0 || alignment > 256)
                throw std::runtime_error("unsupported nvCOMP alignment requirement");
            return ((value + alignment - 1) / alignment) * alignment;
        }

        std::size_t rounded(std::size_t value, std::size_t quantum)
        {
            return ((value + quantum - 1) / quantum) * quantum;
        }

        class CudaTransform final : public BatchTransform
        {
            struct Slot
            {
                Stream stream;
                Event begin;
                Event end;
                Event complete;
                DeviceBuffer<unsigned char> device_input;
                DeviceBuffer<unsigned char> device_output{64ULL << 10};
                DeviceBuffer<unsigned char> device_compressed;
                DeviceBuffer<unsigned char> temporary;
                DeviceBuffer<BlockDescriptor> device_blocks;
                DeviceBuffer<unsigned int> device_crc_status;
                DeviceBuffer<const void *> device_sources;
                DeviceBuffer<void *> device_destinations;
                DeviceBuffer<std::size_t> device_input_sizes;
                DeviceBuffer<std::size_t> device_output_sizes;
                DeviceBuffer<std::size_t> device_actual_sizes;
                DeviceBuffer<nvcompStatus_t> device_statuses;
                PinnedBuffer<unsigned char> host_input;
                PinnedBuffer<unsigned char> host_output;
                PinnedBuffer<BlockDescriptor> host_blocks;
                PinnedBuffer<unsigned int> host_crc_status;
                PinnedBuffer<const void *> host_sources;
                PinnedBuffer<void *> host_destinations;
                PinnedBuffer<std::size_t> host_input_sizes;
                PinnedBuffer<std::size_t> host_output_sizes;
                PinnedBuffer<std::size_t> host_actual_sizes;
                PinnedBuffer<nvcompStatus_t> host_statuses;
                std::vector<BlockDescriptor> descriptors;
                std::vector<std::size_t> input_sizes;
                std::vector<std::size_t> output_sizes;
                std::vector<std::size_t> indices;
                std::vector<std::size_t> input_offsets;
                std::string compressed;
                std::map<std::tuple<std::size_t, std::size_t, std::size_t>, std::size_t> workspace_sizes;
                std::map<std::tuple<std::size_t, std::size_t, std::size_t>, std::size_t>
                    compression_workspace_sizes;
                std::map<std::size_t, std::size_t> compression_max_outputs;
                bool in_use = false;
            };

            class SlotLease
            {
            public:
                explicit SlotLease(CudaTransform *owner) : owner_(owner)
                {
                    std::unique_lock lock(owner_->slots_mutex_);
                    owner_->slots_ready_.wait(lock, [&] {
                        return std::any_of(owner_->slots_.begin(), owner_->slots_.end(),
                                           [](const Slot &slot) { return !slot.in_use; });
                    });
                    for (auto &candidate : owner_->slots_)
                    {
                        if (!candidate.in_use)
                        {
                            candidate.in_use = true;
                            slot_ = &candidate;
                            break;
                        }
                    }
                }
                ~SlotLease()
                {
                    {
                        const std::lock_guard lock(owner_->slots_mutex_);
                        slot_->in_use = false;
                    }
                    owner_->slots_ready_.notify_one();
                }
                Slot &get() { return *slot_; }

            private:
                CudaTransform *owner_;
                Slot *slot_ = nullptr;
            };

        public:
            explicit CudaTransform(bool hybrid) : hybrid_(hybrid)
            {
                const auto sync = sync_mode_from_environment();
                cuda_check(cudaSetDeviceFlags(sync.device_flags));
                cuda_check(cudaSetDevice(0));
                for (auto &slot : slots_)
                {
                    slot.stream.create();
                    slot.begin.create(sync.event_flags);
                    slot.end.create(sync.event_flags);
                    slot.complete.create(sync.event_flags);
                }
                decompress_ = nvcompBatchedLZ4DecompressDefaultOpts;
                decompress_.backend = NVCOMP_DECOMPRESS_BACKEND_CUDA;
                nvcomp_check(nvcompBatchedLZ4DecompressGetRequiredAlignments(decompress_, &requirements_));
                compress_ = nvcompBatchedLZ4CompressDefaultOpts;
                nvcomp_check(nvcompBatchedLZ4CompressGetRequiredAlignments(compress_, &compress_requirements_));
                constexpr std::size_t prepared_blocks = 128;
                constexpr std::size_t prepared_input_bytes = 16ULL << 20;
                constexpr std::size_t prepared_block_bytes = 128ULL << 10;
                std::size_t prepared_compressed_bytes = 0;
                nvcomp_check(nvcompBatchedLZ4CompressGetMaxOutputChunkSize(
                    prepared_block_bytes, compress_, &prepared_compressed_bytes));
                const auto prepared_compressed_stride =
                    aligned(prepared_compressed_bytes, compress_requirements_.output);
                const auto prepared_output_stride =
                    aligned(prepared_compressed_bytes + 6, compress_requirements_.output);
                std::size_t prepared_workspace_bytes = 0;
                nvcomp_check(nvcompBatchedLZ4CompressGetTempSizeAsync(
                    prepared_blocks, prepared_block_bytes, compress_,
                    &prepared_workspace_bytes, prepared_input_bytes));
                for (auto &slot : slots_)
                {
                    slot.host_input.reserve(prepared_input_bytes);
                    slot.host_output.reserve(prepared_output_stride * prepared_blocks);
                    slot.host_blocks.reserve(prepared_blocks);
                    slot.host_sources.reserve(prepared_blocks);
                    slot.host_destinations.reserve(prepared_blocks);
                    slot.host_input_sizes.reserve(prepared_blocks);
                    slot.host_actual_sizes.reserve(prepared_blocks);
                    slot.host_crc_status.reserve(prepared_blocks);
                    slot.host_statuses.reserve(prepared_blocks);
                    slot.device_input.reserve(prepared_input_bytes);
                    slot.device_compressed.reserve(
                        prepared_compressed_stride * prepared_blocks);
                    slot.device_output.reserve(prepared_output_stride * prepared_blocks);
                    slot.device_blocks.reserve(prepared_blocks);
                    slot.device_input_sizes.reserve(prepared_blocks);
                    slot.device_output_sizes.reserve(prepared_blocks);
                    slot.device_actual_sizes.reserve(prepared_blocks);
                    slot.device_crc_status.reserve(prepared_blocks);
                    slot.device_statuses.reserve(prepared_blocks);
                    slot.device_sources.reserve(prepared_blocks);
                    slot.device_destinations.reserve(prepared_blocks);
                    slot.temporary.reserve(prepared_workspace_bytes);
                    slot.descriptors.reserve(prepared_blocks);
                    slot.input_offsets.reserve(prepared_blocks);
                    slot.compression_max_outputs.emplace(
                        prepared_block_bytes, prepared_compressed_bytes);
                    slot.compression_workspace_sizes.emplace(
                        std::make_tuple(
                            prepared_blocks, prepared_block_bytes,
                            prepared_input_bytes),
                        prepared_workspace_bytes);
                }
                cudaDeviceProp device{};
                cuda_check(cudaGetDeviceProperties(&device, 0));
                if (device.major < 7)
                    throw std::runtime_error("nvCOMP requires Volta or newer NVIDIA hardware");
                nvcompProperties_t library{};
                nvcomp_check(nvcompGetProperties(&library));
                if (library.version != NVCOMP_VER)
                    throw std::runtime_error("nvCOMP runtime/header version mismatch");
                int runtime = 0;
                cuda_check(cudaRuntimeGetVersion(&runtime));
                device_name_ = std::string(device.name) + "; nvCOMP=5.3.0.16; CUDA=" + std::to_string(runtime) +
                               "; sync=" + sync.label;
            }
            ~CudaTransform() override { cudaSetDevice(0); }

            static std::size_t merge_stream_limit()
            {
                static const std::size_t limit = [] {
                    const char *configured = std::getenv("LSM_GPU_MERGE_STREAMS");
                    const auto parsed = configured == nullptr ? 0 : std::strtoull(configured, nullptr, 10);
                    return parsed == 0 ? std::size_t{2} : static_cast<std::size_t>(parsed);
                }();
                return limit;
            }

            // With LSM_GPU_PINNED_PREALLOC_MB, every merge engine is created (and pins its host
            // buffers) when the database opens rather than during the first compactions.
            void prepare_merged_input() override { prepare_merge_engines(); }

            void prepare_merge_engines()
            {
                const auto bytes = pinned_prealloc_bytes();
                if (bytes == 0)
                    return;
                const std::lock_guard lock(merge_engine_mutex_);
                while (merge_engines_.size() < merge_stream_limit())
                {
                    merge_engines_.push_back(make_cuda_merge_engine());
                    merge_engines_.back()->preallocate(bytes);
                    merge_engines_idle_.push_back(merge_engines_.back().get());
                }
            }

            std::string name() const override { return hybrid_ ? "cuda_hybrid" : "cuda_batched"; }
            std::string device() const override { return device_name_; }

            rocksdb::Status BuildBloomFilter(const std::uint64_t *hashes, std::size_t count, std::uint32_t len_bytes,
                                             int num_probes, char *data) noexcept override
            {
                const auto started = std::chrono::steady_clock::now();
                const auto status = cuda_build_bloom_filter(hashes, count, len_bytes, num_probes, data);
                if (status.ok())
                {
                    filter_builds_.fetch_add(1, std::memory_order_relaxed);
                    filter_keys_.fetch_add(count, std::memory_order_relaxed);
                    filter_ns_.fetch_add(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                         std::chrono::steady_clock::now() - started)
                                                                         .count()),
                                         std::memory_order_relaxed);
                }
                return status;
            }

        protected:
            rocksdb::Status execute_merge(const rocksdb::CompactionMergeRequest &request,
                                          std::unique_ptr<rocksdb::CompactionMergedInput> *output,
                                          MergeRunStats *stats) override
            {
                // A pool of engines, each with its own stream and buffers, lets several
                // background compactions use the GPU at once (LSM_GPU_MERGE_STREAMS, default 2).
                const std::size_t limit = merge_stream_limit();
                CudaMergeEngine *engine = nullptr;
                {
                    std::unique_lock lock(merge_engine_mutex_);
                    merge_engine_ready_.wait(lock, [&] {
                        return !merge_engines_idle_.empty() || merge_engines_.size() < limit;
                    });
                    if (!merge_engines_idle_.empty())
                    {
                        engine = merge_engines_idle_.back();
                        merge_engines_idle_.pop_back();
                    }
                    else
                    {
                        merge_engines_.push_back(make_cuda_merge_engine());
                        engine = merge_engines_.back().get();
                    }
                }
                struct Release
                {
                    CudaTransform *self;
                    CudaMergeEngine *engine;
                    ~Release()
                    {
                        {
                            const std::lock_guard guard(self->merge_engine_mutex_);
                            self->merge_engines_idle_.push_back(engine);
                        }
                        self->merge_engine_ready_.notify_one();
                    }
                } release{this, engine};
                return engine->merge(request, output, stats);
            }

            rocksdb::Status submit_output(
                std::vector<std::string> input,
                std::unique_ptr<rocksdb::CompactionOutputSubmission> *submission) override
            {
                auto result = std::async(
                    std::launch::async,
                    [this, input = std::move(input)]() mutable {
                        std::vector<rocksdb::Slice> slices;
                        slices.reserve(input.size());
                        for (const auto &block : input)
                            slices.emplace_back(block);
                        std::vector<rocksdb::CompactionOutputBlock> output;
                        auto status = execute_output(slices, &output);
                        if (status.ok() && output.size() == input.size())
                            for (std::size_t index = 0; index < input.size(); ++index)
                                output[index].uncompressed = std::move(input[index]);
                        return FutureOutputSubmission::Result{
                            std::move(status), std::move(output)};
                    });
                *submission =
                    std::make_unique<FutureOutputSubmission>(std::move(result));
                return rocksdb::Status::OK();
            }

            rocksdb::Status execute(const std::string &input, const std::vector<BlockDescriptor> &blocks,
                                    std::vector<std::string> *output) override
            {
                cuda_check(cudaSetDevice(0));
                SlotLease lease(this);
                auto &slot = lease.get();
                const auto decoded_bytes = std::accumulate(
                    blocks.begin(), blocks.end(), std::size_t{0},
                    [](std::size_t total, const BlockDescriptor &block) { return total + block.output_bytes; });
                const auto average_decoded_bytes = decoded_bytes / blocks.size();
                if (hybrid_ && (blocks.size() < 32 || decoded_bytes < (2ULL << 20) ||
                                average_decoded_bytes < (32ULL << 10)))
                {
                    ++cpu_fallback_batches_;
                    cpu_fallback_blocks_ += blocks.size();
                    cpu_fallback_bytes_ += decoded_bytes;
                    return execute_cpu_batch(input, blocks, output);
                }
                slot.descriptors = blocks;
                slot.input_sizes.clear();
                slot.output_sizes.clear();
                slot.indices.clear();
                std::size_t output_bytes = 0;
                std::size_t max_output = 0;
                std::size_t compressed_output_bytes = 0;
                for (std::size_t index = 0; index < slot.descriptors.size(); ++index)
                {
                    auto &block = slot.descriptors[index];
                    output_bytes = aligned(output_bytes, requirements_.output);
                    block.output_offset = static_cast<std::uint32_t>(output_bytes);
                    output_bytes += block.output_bytes;
                    if (block.compression != 0)
                    {
                        slot.input_sizes.push_back(block.compressed_bytes);
                        slot.output_sizes.push_back(block.output_bytes);
                        slot.indices.push_back(index);
                        max_output = std::max<std::size_t>(max_output, block.output_bytes);
                        compressed_output_bytes += block.output_bytes;
                    }
                }
                slot.device_input.reserve(input.size());
                slot.device_output.reserve(output_bytes);
                slot.device_blocks.reserve(slot.descriptors.size());
                slot.device_crc_status.reserve(blocks.size());
                slot.host_input.reserve(input.size());
                slot.host_blocks.reserve(slot.descriptors.size());
                std::memcpy(slot.host_input.data, input.data(), input.size());
                std::memcpy(slot.host_blocks.data, slot.descriptors.data(),
                            slot.descriptors.size() * sizeof(BlockDescriptor));
                cuda_check(cudaMemcpyAsync(slot.device_input.data, slot.host_input.data, input.size(),
                                           cudaMemcpyHostToDevice, slot.stream.value));
                cuda_check(cudaMemcpyAsync(slot.device_blocks.data, slot.host_blocks.data,
                                           slot.descriptors.size() * sizeof(BlockDescriptor),
                                           cudaMemcpyHostToDevice, slot.stream.value));
                cuda_check(cudaEventRecord(slot.begin.value, slot.stream.value));
                constexpr unsigned int threads = 128;
                const auto grids = static_cast<unsigned int>(blocks.size());
                verify_and_copy<<<grids, threads, 0, slot.stream.value>>>(slot.device_input.data, slot.device_blocks.data,
                                                                     static_cast<unsigned int>(blocks.size()),
                                                                     slot.device_output.data, slot.device_crc_status.data);
                cuda_check(cudaGetLastError());
                slot.host_crc_status.reserve(blocks.size());
                ++dispatches_;

                if (!slot.indices.empty())
                {
                    const auto workspace_key = std::make_tuple(slot.indices.size(), max_output, compressed_output_bytes);
                    auto cached_workspace = slot.workspace_sizes.find(workspace_key);
                    if (cached_workspace == slot.workspace_sizes.end())
                    {
                        if (slot.workspace_sizes.size() >= 64)
                            slot.workspace_sizes.clear();
                        std::size_t queried_bytes = 0;
                        nvcomp_check(nvcompBatchedLZ4DecompressGetTempSizeAsync(
                            slot.indices.size(), max_output, decompress_, &queried_bytes, compressed_output_bytes));
                        cached_workspace = slot.workspace_sizes.emplace(workspace_key, queried_bytes).first;
                    }
                    const auto temporary_bytes = cached_workspace->second;
                    if (temporary_bytes > 32ULL << 20 || requirements_.temp > 256)
                        return rocksdb::Status::MemoryLimit("nvCOMP workspace exceeds batch budget");
                    slot.temporary.reserve(temporary_bytes);
                    const auto direct_sources = std::all_of(
                        slot.indices.begin(), slot.indices.end(), [&](auto index)
                        { return slot.descriptors[index].compressed_offset % requirements_.input == 0; });
                    slot.compressed.clear();
                    slot.input_offsets.clear();
                    if (!direct_sources)
                    {
                        for (const auto index : slot.indices)
                        {
                            const auto &block = slot.descriptors[index];
                            slot.compressed.resize(aligned(slot.compressed.size(), requirements_.input), '\0');
                            slot.input_offsets.push_back(slot.compressed.size());
                            slot.compressed.append(input.data() + block.compressed_offset, block.compressed_bytes);
                        }
                        slot.device_compressed.reserve(slot.compressed.size());
                        cuda_check(cudaMemcpyAsync(slot.device_compressed.data, slot.compressed.data(),
                                                   slot.compressed.size(), cudaMemcpyHostToDevice, slot.stream.value));
                    }
                    slot.host_sources.reserve(slot.indices.size());
                    slot.host_destinations.reserve(slot.indices.size());
                    slot.host_input_sizes.reserve(slot.indices.size());
                    slot.host_output_sizes.reserve(slot.indices.size());
                    slot.host_actual_sizes.reserve(slot.indices.size());
                    slot.host_statuses.reserve(slot.indices.size());
                    for (std::size_t index = 0; index < slot.indices.size(); ++index)
                    {
                        slot.host_sources.data[index] =
                            direct_sources
                                ? static_cast<const void *>(slot.device_input.data +
                                      slot.descriptors[slot.indices[index]].compressed_offset)
                                : static_cast<const void *>(slot.device_compressed.data + slot.input_offsets[index]);
                        slot.host_destinations.data[index] =
                            slot.device_output.data + slot.descriptors[slot.indices[index]].output_offset;
                        slot.host_input_sizes.data[index] = slot.input_sizes[index];
                        slot.host_output_sizes.data[index] = slot.output_sizes[index];
                    }
                    slot.device_sources.reserve(slot.indices.size());
                    slot.device_destinations.reserve(slot.indices.size());
                    slot.device_input_sizes.reserve(slot.indices.size());
                    slot.device_output_sizes.reserve(slot.indices.size());
                    slot.device_actual_sizes.reserve(slot.indices.size());
                    slot.device_statuses.reserve(slot.indices.size());
                    cuda_check(cudaMemcpyAsync(slot.device_sources.data, slot.host_sources.data,
                                               slot.indices.size() * sizeof(void *),
                                               cudaMemcpyHostToDevice, slot.stream.value));
                    cuda_check(cudaMemcpyAsync(slot.device_destinations.data, slot.host_destinations.data,
                                               slot.indices.size() * sizeof(void *),
                                               cudaMemcpyHostToDevice, slot.stream.value));
                    cuda_check(cudaMemcpyAsync(slot.device_input_sizes.data, slot.host_input_sizes.data,
                                               slot.indices.size() * sizeof(std::size_t),
                                               cudaMemcpyHostToDevice, slot.stream.value));
                    cuda_check(cudaMemcpyAsync(slot.device_output_sizes.data, slot.host_output_sizes.data,
                                               slot.indices.size() * sizeof(std::size_t),
                                               cudaMemcpyHostToDevice, slot.stream.value));
                    nvcomp_check(nvcompBatchedLZ4DecompressAsync(
                        slot.device_sources.data, slot.device_input_sizes.data,
                        slot.device_output_sizes.data, slot.device_actual_sizes.data,
                        slot.indices.size(), slot.temporary.data, temporary_bytes,
                        slot.device_destinations.data, decompress_, slot.device_statuses.data, slot.stream.value));
                    ++dispatches_;
                }
                cuda_check(cudaEventRecord(slot.end.value, slot.stream.value));
                cuda_check(cudaMemcpyAsync(slot.host_crc_status.data, slot.device_crc_status.data,
                                           blocks.size() * sizeof(unsigned int),
                                           cudaMemcpyDeviceToHost, slot.stream.value));
                if (!slot.indices.empty())
                {
                    cuda_check(cudaMemcpyAsync(slot.host_statuses.data, slot.device_statuses.data,
                                               slot.indices.size() * sizeof(nvcompStatus_t),
                                               cudaMemcpyDeviceToHost, slot.stream.value));
                    cuda_check(cudaMemcpyAsync(slot.host_actual_sizes.data, slot.device_actual_sizes.data,
                                               slot.indices.size() * sizeof(std::size_t),
                                               cudaMemcpyDeviceToHost, slot.stream.value));
                }
                slot.host_output.reserve(output_bytes);
                cuda_check(cudaMemcpyAsync(slot.host_output.data, slot.device_output.data, output_bytes,
                                           cudaMemcpyDeviceToHost, slot.stream.value));
                cuda_check(cudaEventRecord(slot.complete.value, slot.stream.value));
                cuda_check(cudaEventSynchronize(slot.complete.value));
                float milliseconds = 0;
                cuda_check(cudaEventElapsedTime(&milliseconds, slot.begin.value, slot.end.value));
                kernel_ns_ += static_cast<std::uint64_t>(milliseconds * 1e6);
                for (std::size_t index = 0; index < blocks.size(); ++index)
                {
                    if (slot.host_crc_status.data[index] != 0)
                        return rocksdb::Status::Corruption("CUDA block CRC32C mismatch");
                }
                for (std::size_t index = 0; index < slot.indices.size(); ++index)
                {
                    if (slot.host_statuses.data[index] != nvcompSuccess ||
                        slot.host_actual_sizes.data[index] != slot.output_sizes[index])
                        return rocksdb::Status::Corruption("nvCOMP LZ4 decoding failed or returned incorrect size");
                }
                for (const auto &block : slot.descriptors)
                    output->emplace_back(reinterpret_cast<const char *>(slot.host_output.data + block.output_offset),
                                         block.output_bytes);
                return rocksdb::Status::OK();
            }

            rocksdb::Status execute_output(
                const std::vector<rocksdb::Slice> &input,
                std::vector<rocksdb::CompactionOutputBlock> *output) override
            {
                const auto total_bytes = std::accumulate(
                    input.begin(), input.end(), std::size_t{0},
                    [](std::size_t total, const rocksdb::Slice &block) { return total + block.size(); });
                if (std::getenv("LSM_FORCE_CPU_OUTPUT") != nullptr)
                    return execute_cpu_output(input, output);
                try
                {
                    return execute_gpu_output(input, output, total_bytes);
                }
                catch (const std::exception &)
                {
                    // e.g. the GPU is out of memory because other processes share it: compress on
                    // the CPU instead of failing the compaction.
                    ++cpu_fallback_batches_;
                    cpu_fallback_blocks_ += input.size();
                    cpu_fallback_bytes_ += total_bytes;
                    output->clear();
                    return execute_cpu_output(input, output);
                }
            }

            rocksdb::Status execute_gpu_output(const std::vector<rocksdb::Slice> &input,
                                               std::vector<rocksdb::CompactionOutputBlock> *output,
                                               std::size_t total_bytes)
            {
                cuda_check(cudaSetDevice(0));
                ++output_gpu_batches_;
                SlotLease lease(this);
                auto &slot = lease.get();
                std::size_t max_input = 0;
                for (const auto &block : input)
                    max_input = std::max(max_input, block.size());
                const auto workspace_max_input = rounded(max_input, 4ULL << 10);
                const auto workspace_total_bytes = rounded(total_bytes, 1ULL << 20);
                auto cached_max_output =
                    slot.compression_max_outputs.find(workspace_max_input);
                if (cached_max_output == slot.compression_max_outputs.end())
                {
                    if (slot.compression_max_outputs.size() >= 64)
                        slot.compression_max_outputs.clear();
                    std::size_t queried_bytes = 0;
                    nvcomp_check(nvcompBatchedLZ4CompressGetMaxOutputChunkSize(
                        workspace_max_input, compress_, &queried_bytes));
                    cached_max_output =
                        slot.compression_max_outputs.emplace(
                            workspace_max_input, queried_bytes).first;
                }
                const auto max_compressed = cached_max_output->second;
                const auto compressed_stride = aligned(max_compressed, compress_requirements_.output);
                const auto output_stride = aligned(max_compressed + 6, compress_requirements_.output);

                slot.descriptors.clear();
                slot.input_offsets.clear();
                slot.host_input.reserve(total_bytes + input.size() * compress_requirements_.input);
                std::size_t packed_bytes = 0;
                for (std::size_t index = 0; index < input.size(); ++index)
                {
                    packed_bytes = aligned(packed_bytes, compress_requirements_.input);
                    slot.input_offsets.push_back(packed_bytes);
                    std::memcpy(slot.host_input.data + packed_bytes, input[index].data(), input[index].size());
                    slot.descriptors.push_back({
                        static_cast<std::uint32_t>(packed_bytes), 0, 0, 0, 0,
                        static_cast<std::uint32_t>(index * output_stride),
                        static_cast<std::uint32_t>(input[index].size()), 4});
                    packed_bytes += input[index].size();
                }

                slot.device_input.reserve(packed_bytes);
                slot.device_compressed.reserve(compressed_stride * input.size());
                slot.device_output.reserve(output_stride * input.size());
                slot.device_blocks.reserve(input.size());
                slot.device_input_sizes.reserve(input.size());
                slot.device_output_sizes.reserve(input.size());
                slot.device_actual_sizes.reserve(input.size());
                slot.device_crc_status.reserve(input.size());
                slot.device_statuses.reserve(input.size());
                slot.device_sources.reserve(input.size());
                slot.device_destinations.reserve(input.size());
                slot.host_blocks.reserve(input.size());
                slot.host_sources.reserve(input.size());
                slot.host_destinations.reserve(input.size());
                slot.host_input_sizes.reserve(input.size());
                slot.host_actual_sizes.reserve(input.size());
                slot.host_crc_status.reserve(input.size());
                slot.host_statuses.reserve(input.size());
                slot.host_output.reserve(output_stride * input.size());
                std::memcpy(slot.host_blocks.data, slot.descriptors.data(),
                            input.size() * sizeof(BlockDescriptor));
                for (std::size_t index = 0; index < input.size(); ++index)
                {
                    slot.host_sources.data[index] = slot.device_input.data + slot.input_offsets[index];
                    slot.host_destinations.data[index] =
                        slot.device_compressed.data + index * compressed_stride;
                    slot.host_input_sizes.data[index] = input[index].size();
                }
                cuda_check(cudaMemcpyAsync(slot.device_input.data, slot.host_input.data, packed_bytes,
                                           cudaMemcpyHostToDevice, slot.stream.value));
                cuda_check(cudaMemcpyAsync(slot.device_blocks.data, slot.host_blocks.data,
                                           input.size() * sizeof(BlockDescriptor),
                                           cudaMemcpyHostToDevice, slot.stream.value));
                cuda_check(cudaMemcpyAsync(slot.device_sources.data, slot.host_sources.data,
                                           input.size() * sizeof(void *),
                                           cudaMemcpyHostToDevice, slot.stream.value));
                cuda_check(cudaMemcpyAsync(slot.device_destinations.data, slot.host_destinations.data,
                                           input.size() * sizeof(void *),
                                           cudaMemcpyHostToDevice, slot.stream.value));
                cuda_check(cudaMemcpyAsync(slot.device_input_sizes.data, slot.host_input_sizes.data,
                                           input.size() * sizeof(std::size_t),
                                           cudaMemcpyHostToDevice, slot.stream.value));
                const auto workspace_key = std::make_tuple(
                    input.size(), workspace_max_input, workspace_total_bytes);
                auto cached_workspace = slot.compression_workspace_sizes.find(workspace_key);
                if (cached_workspace == slot.compression_workspace_sizes.end())
                {
                    if (slot.compression_workspace_sizes.size() >= 64)
                        slot.compression_workspace_sizes.clear();
                    std::size_t queried_bytes = 0;
                    nvcomp_check(nvcompBatchedLZ4CompressGetTempSizeAsync(
                        input.size(), workspace_max_input, compress_, &queried_bytes,
                        workspace_total_bytes));
                    cached_workspace =
                        slot.compression_workspace_sizes.emplace(workspace_key, queried_bytes).first;
                }
                const auto temporary_bytes = cached_workspace->second;
                if (temporary_bytes > 512ULL << 20)
                {
                    // Uploads above are already queued from this slot's pinned buffers.
                    cuda_check(cudaStreamSynchronize(slot.stream.value));
                    ++cpu_fallback_batches_;
                    cpu_fallback_blocks_ += input.size();
                    cpu_fallback_bytes_ += total_bytes;
                    return execute_cpu_output(input, output);
                }
                slot.temporary.reserve(temporary_bytes);
                cuda_check(cudaEventRecord(slot.begin.value, slot.stream.value));
                nvcomp_check(nvcompBatchedLZ4CompressAsync(
                    slot.device_sources.data, slot.device_input_sizes.data, max_input, input.size(),
                    slot.temporary.data, temporary_bytes, slot.device_destinations.data,
                    slot.device_actual_sizes.data, compress_, slot.device_statuses.data, slot.stream.value));
                ++dispatches_;
                constexpr unsigned int threads = 128;
                finalize_compressed_blocks<<<static_cast<unsigned int>(input.size()), threads, 0, slot.stream.value>>>(
                    slot.device_input.data, slot.device_compressed.data, slot.device_blocks.data,
                    static_cast<unsigned int>(input.size()), static_cast<unsigned int>(compressed_stride),
                    slot.device_actual_sizes.data, slot.device_output.data, slot.device_output_sizes.data,
                    slot.device_crc_status.data);
                cuda_check(cudaGetLastError());
                ++dispatches_;
                cuda_check(cudaEventRecord(slot.end.value, slot.stream.value));
                cuda_check(cudaMemcpyAsync(slot.host_statuses.data, slot.device_statuses.data,
                                           input.size() * sizeof(nvcompStatus_t),
                                           cudaMemcpyDeviceToHost, slot.stream.value));
                cuda_check(cudaMemcpyAsync(slot.host_actual_sizes.data, slot.device_output_sizes.data,
                                           input.size() * sizeof(std::size_t),
                                           cudaMemcpyDeviceToHost, slot.stream.value));
                cuda_check(cudaMemcpyAsync(slot.host_crc_status.data, slot.device_crc_status.data,
                                           input.size() * sizeof(unsigned int),
                                           cudaMemcpyDeviceToHost, slot.stream.value));
                cuda_check(cudaMemcpyAsync(slot.host_output.data, slot.device_output.data,
                                           output_stride * input.size(),
                                           cudaMemcpyDeviceToHost, slot.stream.value));
                cuda_check(cudaEventRecord(slot.complete.value, slot.stream.value));
                cuda_check(cudaEventSynchronize(slot.complete.value));
                float milliseconds = 0;
                cuda_check(cudaEventElapsedTime(&milliseconds, slot.begin.value, slot.end.value));
                kernel_ns_ += static_cast<std::uint64_t>(milliseconds * 1e6);
                output->clear();
                output->reserve(input.size());
                for (std::size_t index = 0; index < input.size(); ++index)
                {
                    if (slot.host_statuses.data[index] != nvcompSuccess)
                        return rocksdb::Status::Corruption("nvCOMP LZ4 output compression failed");
                    const auto bytes = slot.host_actual_sizes.data[index];
                    const bool compressed = bytes < input[index].size();
                    output->push_back({
                        std::string(reinterpret_cast<const char *>(
                                        slot.host_output.data + index * output_stride), bytes),
                        slot.host_crc_status.data[index], compressed});
                }
                return rocksdb::Status::OK();
            }

        private:
            bool hybrid_;
            std::string device_name_;
            std::remove_cv_t<decltype(nvcompBatchedLZ4DecompressDefaultOpts)> decompress_{};
            nvcompAlignmentRequirements_t requirements_{};
            std::remove_cv_t<decltype(nvcompBatchedLZ4CompressDefaultOpts)> compress_{};
            nvcompAlignmentRequirements_t compress_requirements_{};
            std::array<Slot, 2> slots_;
            std::mutex slots_mutex_;
            std::condition_variable slots_ready_;
            std::mutex merge_engine_mutex_;
            std::condition_variable merge_engine_ready_;
            std::vector<std::unique_ptr<CudaMergeEngine>> merge_engines_;
            std::vector<CudaMergeEngine *> merge_engines_idle_;
        };
    }

    bool cuda_available()
    {
        int count = 0;
        if (cudaGetDeviceCount(&count) != cudaSuccess || count == 0)
            return false;
        cudaDeviceProp properties{};
        return cudaGetDeviceProperties(&properties, 0) == cudaSuccess && properties.major >= 7;
    }

    std::shared_ptr<BatchTransform> make_cuda_transform() { return std::make_shared<CudaTransform>(false); }
    std::shared_ptr<BatchTransform> make_cuda_hybrid_transform() { return std::make_shared<CudaTransform>(true); }
}