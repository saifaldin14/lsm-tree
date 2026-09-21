#include "lsm_lab/block_transform.hpp"

#include <algorithm>
#include <stdexcept>
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
            explicit DeviceBuffer(std::size_t count)
            {
                cuda_check(cudaMalloc(reinterpret_cast<void **>(&data), std::max<std::size_t>(1, count) * sizeof(Element)));
            }
            ~DeviceBuffer() { cudaFree(data); }
            DeviceBuffer(const DeviceBuffer &) = delete;
            DeviceBuffer &operator=(const DeviceBuffer &) = delete;
            Element *data = nullptr;
        };

        class Stream
        {
        public:
            Stream() { cuda_check(cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking)); }
            ~Stream() { cudaStreamDestroy(value); }
            cudaStream_t value = nullptr;
        };

        class Event
        {
        public:
            Event() { cuda_check(cudaEventCreate(&value)); }
            ~Event() { cudaEventDestroy(value); }
            cudaEvent_t value = nullptr;
        };

        __global__ void verify_and_copy(const unsigned char *input, const BlockDescriptor *blocks,
                                        unsigned int count, unsigned char *output, unsigned int *statuses)
        {
            const auto index = blockIdx.x * blockDim.x + threadIdx.x;
            if (index >= count)
                return;
            const auto block = blocks[index];
            unsigned int crc = 0xffffffffu;
            for (unsigned int offset = 0; offset < block.checksum_bytes; ++offset)
            {
                crc ^= input[block.input_offset + offset];
                for (unsigned int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
            }
            crc = ~crc;
            statuses[index] = (((crc >> 15) | (crc << 17)) + 0xa282ead8u == block.expected_checksum) ? 0 : 1;
            if (statuses[index] == 0 && block.compression == 0)
            {
                for (unsigned int offset = 0; offset < block.output_bytes; ++offset)
                    output[block.output_offset + offset] = input[block.compressed_offset + offset];
            }
        }

        std::size_t aligned(std::size_t value, std::size_t alignment)
        {
            if (alignment == 0 || alignment > 256)
                throw std::runtime_error("unsupported nvCOMP alignment requirement");
            return ((value + alignment - 1) / alignment) * alignment;
        }

        class CudaTransform final : public BatchTransform
        {
        public:
            CudaTransform()
            {
                cuda_check(cudaSetDevice(0));
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
                device_name_ = std::string(device.name) + "; nvCOMP=5.3.0.16; CUDA=" + std::to_string(runtime);
            }

            std::string name() const override { return "cuda_batched"; }
            std::string device() const override { return device_name_; }

        protected:
            rocksdb::Status execute(const std::string &input, const std::vector<BlockDescriptor> &blocks,
                                    std::vector<std::string> *output) override
            {
                cuda_check(cudaSetDevice(0));
                Stream stream;
                Event begin, end;
                auto decompress = nvcompBatchedLZ4DecompressDefaultOpts;
                decompress.backend = NVCOMP_DECOMPRESS_BACKEND_CUDA;
                nvcompAlignmentRequirements_t requirements{};
                nvcomp_check(nvcompBatchedLZ4DecompressGetRequiredAlignments(decompress, &requirements));
                auto descriptors = blocks;
                std::string compressed;
                std::vector<std::size_t> input_offsets, input_sizes, output_sizes, indices;
                std::size_t output_bytes = 0;
                std::size_t max_output = 0;
                std::size_t compressed_output_bytes = 0;
                for (std::size_t index = 0; index < descriptors.size(); ++index)
                {
                    auto &block = descriptors[index];
                    output_bytes = aligned(output_bytes, requirements.output);
                    block.output_offset = static_cast<std::uint32_t>(output_bytes);
                    output_bytes += block.output_bytes;
                    if (block.compression != 0)
                    {
                        compressed.resize(aligned(compressed.size(), requirements.input), '\0');
                        input_offsets.push_back(compressed.size());
                        compressed.append(input.data() + block.compressed_offset, block.compressed_bytes);
                        input_sizes.push_back(block.compressed_bytes);
                        output_sizes.push_back(block.output_bytes);
                        indices.push_back(index);
                        max_output = std::max<std::size_t>(max_output, block.output_bytes);
                        compressed_output_bytes += block.output_bytes;
                    }
                }
                DeviceBuffer<unsigned char> device_input(input.size()), device_output(output_bytes);
                DeviceBuffer<BlockDescriptor> device_blocks(descriptors.size());
                DeviceBuffer<unsigned int> device_crc_status(blocks.size());
                cuda_check(cudaMemcpyAsync(device_input.data, input.data(), input.size(), cudaMemcpyHostToDevice, stream.value));
                cuda_check(cudaMemcpyAsync(device_blocks.data, descriptors.data(), descriptors.size() * sizeof(BlockDescriptor), cudaMemcpyHostToDevice, stream.value));
                cuda_check(cudaEventRecord(begin.value, stream.value));
                verify_and_copy<<<1, 64, 0, stream.value>>>(device_input.data, device_blocks.data,
                                                            static_cast<unsigned int>(blocks.size()), device_output.data, device_crc_status.data);
                cuda_check(cudaGetLastError());
                cuda_check(cudaEventRecord(end.value, stream.value));
                std::vector<unsigned int> crc_status(blocks.size());
                cuda_check(cudaMemcpyAsync(crc_status.data(), device_crc_status.data, crc_status.size() * sizeof(unsigned int), cudaMemcpyDeviceToHost, stream.value));
                cuda_check(cudaStreamSynchronize(stream.value));
                float milliseconds = 0;
                cuda_check(cudaEventElapsedTime(&milliseconds, begin.value, end.value));
                kernel_ns_ += static_cast<std::uint64_t>(milliseconds * 1e6);
                ++dispatches_;
                if (std::any_of(crc_status.begin(), crc_status.end(), [](auto status)
                                { return status != 0; }))
                    return rocksdb::Status::Corruption("CUDA block CRC32C mismatch");
                if (!indices.empty())
                {
                    std::size_t temporary_bytes = 0;
                    nvcomp_check(nvcompBatchedLZ4DecompressGetTempSizeAsync(indices.size(), max_output, decompress,
                                                                            &temporary_bytes, compressed_output_bytes));
                    if (temporary_bytes > 32ULL << 20 || requirements.temp > 256)
                        return rocksdb::Status::MemoryLimit("nvCOMP workspace exceeds batch budget");
                    DeviceBuffer<unsigned char> device_compressed(compressed.size()), temporary(temporary_bytes);
                    std::vector<const void *> sources;
                    std::vector<void *> destinations;
                    for (std::size_t index = 0; index < indices.size(); ++index)
                    {
                        sources.push_back(device_compressed.data + input_offsets[index]);
                        destinations.push_back(device_output.data + descriptors[indices[index]].output_offset);
                    }
                    DeviceBuffer<const void *> device_sources(sources.size());
                    DeviceBuffer<void *> device_destinations(destinations.size());
                    DeviceBuffer<std::size_t> device_input_sizes(indices.size()), device_output_sizes(indices.size()), device_actual_sizes(indices.size());
                    DeviceBuffer<nvcompStatus_t> device_statuses(indices.size());
                    cuda_check(cudaMemcpyAsync(device_compressed.data, compressed.data(), compressed.size(), cudaMemcpyHostToDevice, stream.value));
                    cuda_check(cudaMemcpyAsync(device_sources.data, sources.data(), sources.size() * sizeof(void *), cudaMemcpyHostToDevice, stream.value));
                    cuda_check(cudaMemcpyAsync(device_destinations.data, destinations.data(), destinations.size() * sizeof(void *), cudaMemcpyHostToDevice, stream.value));
                    cuda_check(cudaMemcpyAsync(device_input_sizes.data, input_sizes.data(), input_sizes.size() * sizeof(std::size_t), cudaMemcpyHostToDevice, stream.value));
                    cuda_check(cudaMemcpyAsync(device_output_sizes.data, output_sizes.data(), output_sizes.size() * sizeof(std::size_t), cudaMemcpyHostToDevice, stream.value));
                    cuda_check(cudaEventRecord(begin.value, stream.value));
                    nvcomp_check(nvcompBatchedLZ4DecompressAsync(device_sources.data, device_input_sizes.data,
                                                                 device_output_sizes.data, device_actual_sizes.data, indices.size(), temporary.data, temporary_bytes,
                                                                 device_destinations.data, decompress, device_statuses.data, stream.value));
                    cuda_check(cudaEventRecord(end.value, stream.value));
                    std::vector<nvcompStatus_t> statuses(indices.size());
                    std::vector<std::size_t> actual_sizes(indices.size());
                    cuda_check(cudaMemcpyAsync(statuses.data(), device_statuses.data, statuses.size() * sizeof(nvcompStatus_t), cudaMemcpyDeviceToHost, stream.value));
                    cuda_check(cudaMemcpyAsync(actual_sizes.data(), device_actual_sizes.data, actual_sizes.size() * sizeof(std::size_t), cudaMemcpyDeviceToHost, stream.value));
                    cuda_check(cudaStreamSynchronize(stream.value));
                    cuda_check(cudaEventElapsedTime(&milliseconds, begin.value, end.value));
                    kernel_ns_ += static_cast<std::uint64_t>(milliseconds * 1e6);
                    ++dispatches_;
                    for (std::size_t index = 0; index < indices.size(); ++index)
                    {
                        if (statuses[index] != nvcompSuccess || actual_sizes[index] != output_sizes[index])
                            return rocksdb::Status::Corruption("nvCOMP LZ4 decoding failed or returned incorrect size");
                    }
                }
                std::string decoded(output_bytes, '\0');
                cuda_check(cudaMemcpyAsync(decoded.data(), device_output.data, output_bytes, cudaMemcpyDeviceToHost, stream.value));
                cuda_check(cudaStreamSynchronize(stream.value));
                for (const auto &block : descriptors)
                    output->emplace_back(decoded.data() + block.output_offset, block.output_bytes);
                return rocksdb::Status::OK();
            }

        private:
            std::string device_name_;
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

    std::shared_ptr<BatchTransform> make_cuda_transform() { return std::make_shared<CudaTransform>(); }
}