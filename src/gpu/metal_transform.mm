#include "lsm_lab/block_transform.hpp"
#include "metal_source.hpp"

#include <algorithm>
#include <stdexcept>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace lsm_lab
{
    namespace
    {
        id<MTLDevice> selected_device()
        {
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (device) return device;
            NSArray<id<MTLDevice>>* devices = MTLCopyAllDevices();
            return devices.firstObject;
        }

        std::string message(NSError* error)
        {
            return error ? std::string(error.localizedDescription.UTF8String) : "unknown Metal error";
        }

        class MetalTransform final : public BatchTransform
        {
        public:
            MetalTransform()
            {
                @autoreleasepool {
                    device_ = selected_device();
                    if (!device_) throw std::runtime_error("no native Metal device available");
                    queue_ = [device_ newCommandQueue];
                    NSError* error = nil;
                    id<MTLLibrary> library = [device_ newLibraryWithSource:[NSString stringWithUTF8String:lsm_metal_source]
                                                                 options:nil error:&error];
                    if (!library) throw std::runtime_error(message(error));
                    id<MTLFunction> function = [library newFunctionWithName:@"transform_blocks"];
                    pipeline_ = [device_ newComputePipelineStateWithFunction:function error:&error];
                    if (!pipeline_ || !queue_) throw std::runtime_error(message(error));
                    device_name_ = std::string(device_.name.UTF8String) + "; registry_id=" + std::to_string(device_.registryID);
                }
            }

            std::string name() const override { return "metal_batched"; }
            std::string device() const override { return device_name_; }

        protected:
            rocksdb::Status execute(const std::string &input, const std::vector<BlockDescriptor> &blocks,
                                     std::vector<std::string> *output) override
            {
                static_assert(sizeof(BlockDescriptor) == 32);
                @autoreleasepool {
                    const auto bytes = blocks.back().output_offset + blocks.back().output_bytes;
                    id<MTLBuffer> source = [device_ newBufferWithBytes:input.data() length:input.size() options:MTLResourceStorageModeShared];
                    id<MTLBuffer> descriptors = [device_ newBufferWithBytes:blocks.data() length:blocks.size() * sizeof(BlockDescriptor)
                                                                  options:MTLResourceStorageModeShared];
                    id<MTLBuffer> decoded = [device_ newBufferWithLength:bytes options:MTLResourceStorageModeShared];
                    id<MTLBuffer> statuses = [device_ newBufferWithLength:blocks.size() * sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
                    if (!source || !descriptors || !decoded || !statuses) return rocksdb::Status::MemoryLimit("Metal batch allocation failed");
                    id<MTLCommandBuffer> command = [queue_ commandBuffer];
                    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
                    if (!command || !encoder) return rocksdb::Status::IOError("Metal command allocation failed");
                    [encoder setComputePipelineState:pipeline_];
                    [encoder setBuffer:source offset:0 atIndex:0];
                    [encoder setBuffer:descriptors offset:0 atIndex:1];
                    [encoder setBuffer:decoded offset:0 atIndex:2];
                    [encoder setBuffer:statuses offset:0 atIndex:3];
                    const auto count = static_cast<std::uint32_t>(blocks.size());
                    [encoder setBytes:&count length:sizeof(count) atIndex:4];
                    [encoder dispatchThreads:MTLSizeMake(count, 1, 1)
                       threadsPerThreadgroup:MTLSizeMake(std::min<NSUInteger>(count, pipeline_.maxTotalThreadsPerThreadgroup), 1, 1)];
                    [encoder endEncoding];
                    [command commit];
                    [command waitUntilCompleted];
                    ++dispatches_;
                    if (command.status != MTLCommandBufferStatusCompleted) return rocksdb::Status::IOError(message(command.error));
                    if (command.GPUEndTime >= command.GPUStartTime)
                        kernel_ns_ += static_cast<std::uint64_t>((command.GPUEndTime - command.GPUStartTime) * 1e9);
                    const auto* status = static_cast<const std::uint32_t*>(statuses.contents);
                    const auto* data = static_cast<const char*>(decoded.contents);
                    for (std::size_t index = 0; index < blocks.size(); ++index)
                    {
                        if (status[index] != 0) return rocksdb::Status::Corruption("Metal CRC32C or LZ4 validation failed");
                        output->emplace_back(data + blocks[index].output_offset, blocks[index].output_bytes);
                    }
                    return rocksdb::Status::OK();
                }
            }

        private:
            id<MTLDevice> device_;
            id<MTLCommandQueue> queue_;
            id<MTLComputePipelineState> pipeline_;
            std::string device_name_;
        };
    }

    bool metal_available()
    {
        @autoreleasepool { return selected_device() != nil; }
    }

    std::shared_ptr<BatchTransform> make_metal_transform() { return std::make_shared<MetalTransform>(); }
}