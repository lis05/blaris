# blaris - O(N) random-access LZ decompressor with constant memory usage

blaris is a specialized data compressor designed for firmware and embedded
systems where memory usage is more important than decompression speed.

Unlike conventional LZ-based compressors, blaris does not require a
decompression window or any history buffer. The decompressor reconstructs
requested bytes directly from the compressed stream using only a few dozen
bytes of working memory.

## Decompression

The blaris decompressor satisfies the following constraints:

1. Any byte at position X in the original data can be decoded in O(X) time.
2. The decompressor requires O(1) working memory (currently below ~100 bytes
   on 32-bit platforms).
3. The decompressor implementation fits in less than 1 KiB of code memory
   (currently approximately 700 bytes).
4. Memory usage and algorithmic complexity are independent of the compression
   window size.

This allows compressed data to be accessed on systems where allocating a
decompression buffer is impossible.

For example, a firmware image may contain a 40 KiB hardware configuration
stored in compressed form. A conventional compressor may require an additional
16-32 KiB history buffer during decompression, making it unsuitable for a
small microcontroller.

With blaris, the firmware only needs to store compressed configuration + decompressor code.

For a hypothetical 20 KiB compressed configuration, the total overhead can be
around 21 KiB including the decompressor.

Accessing a small section of the configuration is slower than normal
decompression, requiring potentially hundreds of thousands of cycles, but
requires almost no memory.

## Compression

The blaris compressor is a split-stream optimal bit-packed LZSS encoder.

Generated tokens are separated into three streams:

1. Controls
   - One byte describing token type and structure.

2. Literals
   - Raw bytes emitted by literal tokens.

3. Distances
   - Bit-packed match distances.

The parser is optimal and based on a modified version of the lzmpo encoder.
The current implementation prioritizes compression ratio over encoding speed
and is intentionally slow.

## Design tradeoff

blaris sacrifices decompression throughput in exchange for:

- near-zero RAM usage,
- extremely small decoder size,
- large compression windows,
- random access to compressed data.

It is intended for firmware, bootloaders, ROM-resident resources, and other
systems where RAM is scarce but storage is available.
