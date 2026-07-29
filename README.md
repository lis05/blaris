# blaris - O(N) random-access LZ decompressor with constant memory usage

blaris is a specialized LZ-based compressor for firmware and embedded systems
where RAM is more valuable than decompression throughput.

Unlike conventional LZ-family compressors, blaris does **not** require a sliding
history buffer during decompression. Instead, requested output bytes are
reconstructed directly from the compressed stream using only a few dozen bytes
of working memory.

This allows large-window LZ compression on systems where allocating a
decompression buffer would otherwise be impossible.

Typical applications include:

* firmware images
* bootloaders
* ROM-resident resources
* embedded web interfaces
* hardware configuration data
* diagnostic messages
* systems with extremely limited RAM

blaris intentionally trades decompression speed for:

* constant working memory (~64 B),
* sub-kilobyte decoder (~700 B),
* large compression windows (64 KiB),
* random-access decompression,
* compression ratios comparable to or slightly better than Heatshrink on the
  tested corpus.

It is **not** intended as a replacement for general-purpose compressors such as
LZ4, zstd, or DEFLATE.

---

# Decompression

The current decompressor has the following properties:

1. Any byte at position **X** can be reconstructed in **O(X)** time.
2. Working memory is **O(1)** (currently approximately **64 bytes** on 32-bit
   targets).
3. The decompressor occupies less than **1 KiB** of executable code
   (currently approximately **694 bytes** on Cortex-M).
4. Memory usage is completely independent of the compression window size.
5. No sliding history buffer is required.

Unlike conventional LZ decompression, blaris does not maintain a history window.
Instead, previous bytes are reconstructed by recursively following references
inside the compressed stream.

This enables a capability that conventional LZ decompressors generally cannot
provide: reconstructing arbitrary regions of the decompressed output without
allocating a decompression buffer proportional to the compression window.

For example, a firmware image may contain a 40 KiB hardware configuration
stored in compressed form.

A conventional LZ decompressor may require an additional 16-32 KiB history
buffer while decoding.

With blaris, the firmware only needs:

* compressed configuration data,
* the blaris decompressor.

No decompression window is allocated regardless of the configured match window.

---

# Decoder size

The decompressor currently occupies approximately **694 bytes** of executable
code when compiled for Cortex-M:

```text
$ cargo bloat --release --target thumbv7em-none-eabihf \
    -p blaris-size-check --bin blaris

 File  .text Size             Crate Name

20.7%  93.4% 648B blaris_decompress blaris_decompress::decompress::decompress
 1.0%   4.6%  32B         [Unknown] _start
 0.3%   1.2%   8B               std core::panicking::panic_bounds_check
 0.2%   0.9%   6B               std core::panicking::panic_fmt

22.2% 100.0% 694B                   .text section size
```

Current format parameters:

* Match window: **64 KiB**
* Maximum match length: **15 bytes**
* Maximum literal group size: **16 bytes**

---

# Benchmarks

The current implementation was compared against Heatshrink using several
real-world files.

Across the tested corpus:

* Blaris consistently produced compressed output equal to or slightly smaller
  than the best Heatshrink configuration.
* Working memory is at most **64 bytes**, independent of the
  compression window. Not measured: calculated based on the implementation.
* The decoder occupied approximately **694 bytes** of executable code.
* Heatshrink required between **96 bytes** and **16 KiB** of RAM depending on
  window size.
* Performance varies based on the data. However, it is worth noting that decoding
  longer chunks of data will be slower with blaris and the same with heatshrink.

## Summary

| Property                     | Blaris                                   | Heatshrink     |
| ---------------------------- | ---------------------------------------- | -------------- |
| Decoder code                 | ~694 B                                   | ~3.8 KiB       |
| Working memory               | ~64 B                                    | ~96 B - 16 KiB |
| Compression window           | 64 KiB                                   | Up to 16 KiB   |
| Compression ratio            | Equal or slightly better (tested corpus) | Good           |

The benchmark reports:

* **Comp** — time required to reconstruct the entire output.
* **@1%, @20%, ...** — time required to reconstruct a 64-byte region located at
  different offsets within the decompressed output. These measurements
  demonstrate random-access latency rather than streaming throughput.

### zgrep (8,199 bytes)

| Decoder           | Encode      |  Size | Ratio |    @1% |    @20% |    @40% |    @60% |    @80% |   @100% |  Memory |
| ----------------- | ----------: | ----: | ----: | -----: | ------: | ------: | ------: | ------: | ------: | ------: |
| **Blaris**        |    14.95 ms | 3,841 | 46.8% | 0.4 µs |  9.7 µs | 19.0 µs | 29.6 µs | 24.9 µs | 56.7 µs |   ~64 B |
| Heatshrink (W=12) |     1.12 ms | 3,890 | 47.4% | 2.0 µs | 19.5 µs | 32.9 µs | 47.4 µs | 56.1 µs | 59.4 µs |  ~4 KiB |
| Heatshrink (W=13) |     1.22 ms | 3,977 | 48.5% | 1.7 µs | 17.0 µs | 31.3 µs | 46.9 µs | 57.8 µs | 67.0 µs |  ~8 KiB |
| Heatshrink (W=11) |     0.96 ms | 4,022 | 49.1% | 2.1 µs | 22.4 µs | 38.0 µs | 55.2 µs | 60.7 µs | 74.3 µs |  ~2 KiB |
| Heatshrink (W=14) |     2.39 ms | 4,105 | 50.1% | 2.0 µs | 20.5 µs | 37.4 µs | 53.1 µs | 65.4 µs | 78.4 µs | ~16 KiB |
| Heatshrink (W=10) |     0.51 ms | 4,112 | 50.2% | 2.3 µs | 25.3 µs | 43.4 µs | 57.4 µs | 68.5 µs | 85.0 µs |  ~1 KiB |
| Heatshrink (W=9)  |     0.43 ms | 4,380 | 53.4% | 2.4 µs | 25.6 µs | 43.2 µs | 55.4 µs | 70.5 µs | 92.0 µs |  ~544 B |
| Heatshrink (W=8)  |     0.35 ms | 4,764 | 58.1% | 2.4 µs | 27.3 µs | 42.4 µs | 56.4 µs | 72.6 µs | 94.1 µs |  ~288 B |
| Heatshrink (W=7)  |     0.31 ms | 5,347 | 65.2% | 2.3 µs | 29.2 µs | 44.8 µs | 60.7 µs | 79.7 µs | 104.0 µs |  ~160 B |
| Heatshrink (W=6)  |     0.29 ms | 6,272 | 76.5% | 2.1 µs | 26.0 µs | 46.5 µs | 59.4 µs | 76.7 µs | 100.7 µs |   ~96 B |

### whereis (31,256 bytes)

| Decoder           | Encode      |   Size | Ratio |    @1% |    @20% |    @40% |     @60% |     @80% |    @100% |  Memory |
| ----------------- | ----------: | -----: | ----: | -----: | ------: | ------: | -------: | -------: | -------: | ------: |
| **Blaris**        |   456.75 ms | 12,477 | 39.9% | 5.0 µs | 51.7 µs | 22.6 µs | 387.1 µs | 270.9 µs | 261.0 µs |   ~64 B |
| Heatshrink (W=10) |     2.11 ms | 12,556 | 40.2% | 2.6 µs | 39.3 µs | 88.8 µs | 177.3 µs | 213.8 µs | 260.4 µs |  ~1 KiB |
| Heatshrink (W=9)  |     1.27 ms | 12,558 | 40.2% | 2.6 µs | 38.5 µs | 93.6 µs | 164.4 µs | 225.2 µs | 268.6 µs |  ~544 B |
| Heatshrink (W=8)  |     1.06 ms | 12,737 | 40.8% | 3.2 µs | 42.4 µs | 92.6 µs | 162.6 µs | 221.4 µs | 261.3 µs |  ~288 B |
| Heatshrink (W=11) |     3.93 ms | 12,801 | 41.0% | 3.1 µs | 40.3 µs | 88.4 µs | 167.4 µs | 207.3 µs | 254.4 µs |  ~2 KiB |
| Heatshrink (W=12) |     7.36 ms | 13,040 | 41.7% | 2.3 µs | 33.6 µs | 91.4 µs | 144.1 µs | 175.1 µs | 209.2 µs |  ~4 KiB |
| Heatshrink (W=7)  |     0.76 ms | 13,231 | 42.3% | 2.9 µs | 37.9 µs | 91.9 µs | 188.2 µs | 225.3 µs | 299.1 µs |  ~160 B |
| Heatshrink (W=13) |    10.67 ms | 13,329 | 42.6% | 2.5 µs | 34.6 µs | 76.2 µs | 136.2 µs | 185.1 µs | 228.2 µs |  ~8 KiB |
| Heatshrink (W=14) |    20.71 ms | 13,754 | 44.0% | 2.8 µs | 38.4 µs | 83.4 µs | 163.9 µs | 212.6 µs | 247.8 µs | ~16 KiB |
| Heatshrink (W=6)  |     0.64 ms | 13,912 | 44.5% | 1.7 µs | 33.6 µs | 88.9 µs | 151.5 µs | 224.4 µs | 272.4 µs |   ~96 B |

### xml (102,400 bytes)

| Decoder           | Encode      |   Size | Ratio |    @1% |     @20% |     @40% |     @60% |     @80% |    @100% |  Memory |
| ----------------- | ----------: | -----: | ----: | -----: | -------: | -------: | -------: | -------: | -------: | ------: |
| **Blaris**        |   369.68 ms | 19,414 | 19.0% | 3.5 µs |  83.3 µs | 208.5 µs | 306.9 µs | 465.9 µs | 498.0 µs |   ~64 B |
| Heatshrink (W=14) |     5.88 ms | 19,457 | 19.0% | 8.4 µs |  83.0 µs | 164.3 µs | 231.2 µs | 295.5 µs | 370.4 µs | ~16 KiB |
| Heatshrink (W=13) |     3.73 ms | 19,591 | 19.1% | 7.5 µs |  78.8 µs | 135.8 µs | 199.4 µs | 289.0 µs | 348.6 µs |  ~8 KiB |
| Heatshrink (W=12) |     3.03 ms | 19,819 | 19.4% | 8.8 µs |  75.2 µs | 133.4 µs | 213.9 µs | 280.5 µs | 374.4 µs |  ~4 KiB |
| Heatshrink (W=11) |     2.52 ms | 20,416 | 19.9% | 9.6 µs | 159.2 µs | 184.5 µs | 262.6 µs | 393.2 µs | 447.5 µs |  ~2 KiB |
| Heatshrink (W=10) |     3.33 ms | 35,528 | 34.7% | 9.7 µs | 148.0 µs | 317.1 µs | 504.5 µs | 690.9 µs | 845.8 µs |  ~1 KiB |
| Heatshrink (W=9)  |     2.58 ms | 43,782 | 42.8% | 10.1 µs | 180.7 µs | 365.9 µs | 575.7 µs | 790.4 µs | 998.3 µs |  ~544 B |
| Heatshrink (W=8)  |     2.32 ms | 51,640 | 50.4% | 9.1 µs | 203.3 µs | 401.5 µs | 629.1 µs | 875.6 µs | 1146.0 µs |  ~288 B |
| Heatshrink (W=7)  |     2.01 ms | 56,099 | 54.8% | 8.5 µs | 201.4 µs | 422.3 µs | 650.7 µs | 890.1 µs | 1124.4 µs |  ~160 B |
| Heatshrink (W=6)  |     1.91 ms | 71,337 | 69.7% | 7.5 µs | 209.9 µs | 429.5 µs | 690.9 µs | 932.0 µs | 1173.6 µs |   ~96 B |

### zip (216,232 bytes)

| Decoder           | Encode       |    Size | Ratio |     @1% |     @20% |     @40% |     @60% |      @80% |     @100% |  Memory |
| ----------------- | -----------: | ------: | ----: | ------: | -------: | -------: | -------: | --------: | --------: | ------: |
| **Blaris**        |  2701.60 ms  | 112,422 | 52.0% | 10.1 µs |  70.9 µs | 207.6 µs | 180.6 µs |  173.9 µs |  495.1 µs |   ~64 B |
| Heatshrink (W=13) |    26.50 ms  | 116,880 | 54.1% |  5.6 µs | 152.4 µs | 468.7 µs | 792.9 µs | 1043.0 µs | 1328.9 µs |  ~8 KiB |
| Heatshrink (W=14) |    48.87 ms  | 116,888 | 54.1% |  6.1 µs | 146.8 µs | 490.3 µs | 787.5 µs | 1043.5 µs | 1261.6 µs | ~16 KiB |
| Heatshrink (W=12) |    16.70 ms  | 117,561 | 54.4% |  5.6 µs | 133.1 µs | 442.3 µs | 746.3 µs | 1003.3 µs | 1202.5 µs |  ~4 KiB |
| Heatshrink (W=11) |    10.42 ms  | 119,918 | 55.5% |  6.8 µs | 176.4 µs | 499.3 µs | 853.7 µs | 1124.2 µs | 1375.8 µs |  ~2 KiB |
| Heatshrink (W=10) |     6.11 ms  | 120,554 | 55.8% | 10.3 µs | 165.0 µs | 533.1 µs | 875.9 µs | 1272.0 µs | 1489.6 µs |  ~1 KiB |
| Heatshrink (W=9)  |     4.49 ms  | 123,371 | 57.1% |  6.0 µs | 181.1 µs | 514.0 µs | 898.2 µs | 1343.1 µs | 1505.3 µs |  ~544 B |
| Heatshrink (W=8)  |     3.44 ms  | 128,004 | 59.2% |  6.6 µs | 186.3 µs | 517.5 µs | 906.6 µs | 1247.8 µs | 1585.7 µs |  ~288 B |
| Heatshrink (W=7)  |     3.25 ms  | 135,470 | 62.7% |  6.3 µs | 180.3 µs | 546.9 µs | 940.6 µs | 1286.6 µs | 1578.9 µs |  ~160 B |
| Heatshrink (W=6)  |     2.99 ms  | 146,042 | 67.5% |  5.8 µs | 146.9 µs | 512.7 µs | 836.2 µs | 1153.7 µs | 1507.1 µs |   ~96 B |

---

# Compression

The blaris compressor is a split-stream optimal bit-packed LZSS encoder.

Compressed data is divided into three streams:

1. **Controls** — token types and metadata.
2. **Literals** — raw literal bytes.
3. **Distances** — bit-packed match offsets.

The encoder uses optimal parsing to maximize compression ratio and
intentionally prioritizes compression efficiency over encoding speed.
