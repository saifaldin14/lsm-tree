#include <metal_stdlib>
using namespace metal;

struct BlockDescriptor {
    uint input_offset;
    uint checksum_bytes;
    uint expected_checksum;
    uint compressed_offset;
    uint compressed_bytes;
    uint output_offset;
    uint output_bytes;
    uint compression;
};

kernel void transform_blocks(device const uchar* input [[buffer(0)]],
                             constant BlockDescriptor* blocks [[buffer(1)]],
                             device uchar* output [[buffer(2)]],
                             device uint* results [[buffer(3)]],
                             constant uint& count [[buffer(4)]],
                             uint index [[thread_position_in_grid]]) {
    if (index >= count) return;
    const auto block = blocks[index];
    results[index] = 1;
    uint crc = 0xffffffffu;
    for (uint offset = 0; offset < block.checksum_bytes; ++offset) {
        crc ^= input[block.input_offset + offset];
        for (uint bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0x82f63b78u & (0u - (crc & 1u)));
    }
    crc = ~crc;
    if (((crc >> 15) | (crc << 17)) + 0xa282ead8u != block.expected_checksum) return;
    device const uchar* source = input + block.compressed_offset;
    device uchar* destination = output + block.output_offset;
    if (block.compression == 0) {
        for (uint offset = 0; offset < block.output_bytes; ++offset) destination[offset] = source[offset];
        results[index] = 0;
        return;
    }
    uint read_offset = 0;
    uint write_offset = 0;
    bool matched = false;
    while (read_offset < block.compressed_bytes) {
        const uint token = source[read_offset++];
        uint literals = token >> 4;
        if (literals == 15) {
            uint extension;
            do {
                if (read_offset >= block.compressed_bytes) return;
                extension = source[read_offset++];
                if (literals > block.output_bytes || extension > block.output_bytes - literals) return;
                literals += extension;
            } while (extension == 255);
        }
        if (literals > block.compressed_bytes - read_offset || literals > block.output_bytes - write_offset) return;
        for (uint offset = 0; offset < literals; ++offset) destination[write_offset++] = source[read_offset++];
        if (read_offset == block.compressed_bytes) {
            if (write_offset == block.output_bytes && (!matched || literals >= 5)) results[index] = 0;
            return;
        }
        if (block.compressed_bytes - read_offset < 2 || block.output_bytes - write_offset < 12) return;
        const uint distance = uint(source[read_offset]) | (uint(source[read_offset + 1]) << 8);
        read_offset += 2;
        if (distance == 0 || distance > write_offset) return;
        uint match_bytes = (token & 15) + 4;
        if ((token & 15) == 15) {
            uint extension;
            do {
                if (read_offset >= block.compressed_bytes) return;
                extension = source[read_offset++];
                if (match_bytes > block.output_bytes || extension > block.output_bytes - match_bytes) return;
                match_bytes += extension;
            } while (extension == 255);
        }
        if (match_bytes > block.output_bytes - write_offset - 5) return;
        for (uint offset = 0; offset < match_bytes; ++offset) {
            destination[write_offset] = destination[write_offset - distance];
            ++write_offset;
        }
        matched = true;
    }
}