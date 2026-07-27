# blaris - O(N) random-access LZ decompressor with constant memory usage

blaris is a specialized LZ-based compressor designed for firmware and embedded
systems where memory usage is more important than decompression speed.

Unlike conventional LZ-based compressors, blaris does **not** require a
decompression window or any history buffer. The decompressor reconstructs
requested bytes directly from the compressed stream using only a few dozen
bytes of working memory.

The main goal of blaris is to allow large-window LZ compression on systems
where allocating a decompression buffer is impossible.

The intended use cases are:

- firmware images
- bootloaders
- ROM-resident resources
- embedded web interfaces
- hardware configuration data
- diagnostic messages
- systems with extremely limited RAM

blaris trades decompression throughput for:

- constant memory usage,
- very small decoder size,
- large compression windows,
- direct access to compressed data.

It is not intended as a replacement for general-purpose compressors such as
LZ4, zstd, or DEFLATE.

---

## Decompression

The blaris decompressor satisfies the following constraints:

1. Any byte at position X in the original data can be decoded in O(X) time.
2. The decompressor requires O(1) working memory (currently below ~100 bytes
   on 32-bit platforms).
3. The decompressor implementation fits in less than 1 KiB of code memory
   (currently approximately 700 bytes).
4. Memory usage and algorithmic complexity are independent of the compression
   window size.

Unlike conventional LZ decompression, blaris does not maintain a sliding history buffer.
Instead, previous bytes are reconstructed by recursively following references inside the compressed stream.
This allows compressed data to be accessed on systems where allocating a decompression buffer is impossible.

For example, a firmware image may contain a 40 KiB hardware configuration stored in compressed form.
A conventional compressor may require an additional 16-32 KiB history buffer during decompression, making it unsuitable for a small microcontroller.

With blaris, the firmware only needs to store:

- compressed configuration data
- blaris decompressor code

No decompression window is required.

---

## Decoder size

The decompressor crate is around 700 bytes when compiled for Cortex-M0:

```
$ cargo bloat --release --target thumbv6m-none-eabi -p blaris-size-check
    Finished `release` profile [optimized] target(s) in 0.02s
    Analyzing target/thumbv6m-none-eabi/release/blaris-size-check

 File  .text Size             Crate Name
20.9%  67.6% 460B blaris_decompress blaris_decompress::decompress::decompress
 4.9%  15.9% 108B blaris_decompress blaris_decompress::decompress::read_u32_at_bit_offset
 2.1%   6.8%  46B               std __aeabi_llsl
 1.5%   4.7%  32B         [Unknown] _start
 0.9%   2.9%  20B       blaris_core blaris_core::params::match_from_control
 0.4%   1.2%   8B               std core::panicking::panic_bounds_check
 0.3%   0.9%   6B               std core::panicking::panic_fmt
31.0% 100.0% 680B                   .text section size, the file size is 2.1KiB
```

Current format parameters:

- Match window: 64 KB
- Maximum match length: 15 bytes
- Maximum literal group size: 16 bytes

---

# Benchmarks

Comparison with `heatshrink`:

```
### File: zgrep (8199 bytes)

Decoder                     Comp ms       Size    Ratio      1B ms     64B ms    4KiB ms      Full ms          Memory
---------------------- ------------ ---------- -------- ---------- ---------- ---------- ------------ ---------------
Blaris                     8.044 ms       3841    46.85%   0.002 ms   0.036 ms   2.489 ms     5.189 ms          <100 B
Heatshrink (W=6)           0.136 ms       6272    76.50%   0.020 ms   0.019 ms   0.065 ms     0.045 ms           ~64 B
Heatshrink (W=7)           0.140 ms       5347    65.22%   0.018 ms   0.018 ms   0.031 ms     0.037 ms          ~128 B
Heatshrink (W=8)           0.158 ms       4764    58.10%   0.017 ms   0.017 ms   0.029 ms     0.034 ms          ~256 B
Heatshrink (W=10)          0.239 ms       4112    50.15%   0.018 ms   0.017 ms   0.028 ms     0.032 ms         ~1024 B
Heatshrink (W=12)          0.518 ms       3890    47.44%   0.016 ms   0.016 ms   0.017 ms     0.030 ms         ~4096 B
Heatshrink (W=14)          0.623 ms       4105    50.07%   0.029 ms   0.029 ms   0.029 ms     0.029 ms        ~16384 B

### File: whereis (31256 bytes)

Decoder                     Comp ms       Size    Ratio      1B ms     64B ms    4KiB ms      Full ms          Memory
---------------------- ------------ ---------- -------- ---------- ---------- ---------- ------------ ---------------
Blaris                   208.252 ms      12477    39.92%   0.007 ms   0.177 ms  13.328 ms   102.227 ms          <100 B
Heatshrink (W=6)           0.326 ms      13912    44.51%   0.081 ms   0.047 ms   0.061 ms     0.124 ms           ~64 B
Heatshrink (W=7)           0.366 ms      13231    42.33%   0.047 ms   0.046 ms   0.058 ms     0.104 ms          ~128 B
Heatshrink (W=8)           0.458 ms      12737    40.75%   0.046 ms   0.044 ms   0.057 ms     0.099 ms          ~256 B
Heatshrink (W=10)          0.978 ms      12556    40.17%   0.047 ms   0.046 ms   0.059 ms     0.101 ms         ~1024 B
Heatshrink (W=12)          2.940 ms      13040    41.72%   0.051 ms   0.050 ms   0.062 ms     0.098 ms         ~4096 B
Heatshrink (W=14)          9.687 ms      13754    44.00%   0.054 ms   0.053 ms   0.054 ms     0.108 ms        ~16384 B

### File: zip (216232 bytes)

Decoder                     Comp ms       Size    Ratio      1B ms     64B ms    4KiB ms      Full ms          Memory
---------------------- ------------ ---------- -------- ---------- ---------- ---------- ------------ ---------------
Blaris                  2390.053 ms     112422    51.99%   0.040 ms   0.385 ms  25.771 ms  1717.393 ms          <100 B
Heatshrink (W=6)           4.437 ms     146042    67.54%   0.541 ms   0.539 ms   0.565 ms     1.280 ms           ~64 B
Heatshrink (W=7)           3.037 ms     135470    62.65%   0.528 ms   0.528 ms   0.547 ms     1.226 ms          ~128 B
Heatshrink (W=8)           4.939 ms     128004    59.20%   0.522 ms   0.519 ms   0.540 ms     1.198 ms          ~256 B
Heatshrink (W=10)          7.492 ms     120554    55.75%   0.531 ms   0.531 ms   0.548 ms     1.204 ms         ~1024 B
Heatshrink (W=12)         16.696 ms     117561    54.37%   0.488 ms   0.487 ms   0.507 ms     1.080 ms         ~4096 B
Heatshrink (W=14)         46.450 ms     116888    54.06%   0.505 ms   0.501 ms   0.576 ms     1.091 ms        ~16384 B

### File: xterm (919544 bytes)

Decoder                     Comp ms       Size    Ratio      1B ms     64B ms    4KiB ms      Full ms          Memory
---------------------- ------------ ---------- -------- ---------- ---------- ---------- ------------ ---------------
Blaris                 19664.206 ms     444113    48.30%   0.203 ms   2.101 ms 119.383 ms 28075.781 ms          <100 B
Heatshrink (W=6)          11.755 ms     542472    58.99%   2.083 ms   2.074 ms   2.111 ms     4.727 ms           ~64 B
Heatshrink (W=7)          12.635 ms     515328    56.04%   2.051 ms   1.998 ms   2.108 ms     4.943 ms          ~128 B
Heatshrink (W=8)          12.735 ms     488320    53.10%   2.020 ms   1.978 ms   1.969 ms     4.408 ms          ~256 B
Heatshrink (W=10)         22.902 ms     465368    50.61%   2.091 ms   2.119 ms   2.044 ms     4.540 ms         ~1024 B
Heatshrink (W=12)         53.014 ms     463419    50.40%   1.833 ms   1.820 ms   1.903 ms     4.259 ms         ~4096 B
Heatshrink (W=14)        155.783 ms     467001    50.79%   1.838 ms   1.840 ms   1.858 ms     4.111 ms        ~16384 B

```

# Compression

The blaris compressor is a split-stream optimal bit-packed LZSS encoder.
Generated tokens are separated into three streams:

1. Controls - One byte describing token type and structure.
2. Literals - Raw bytes emitted by literal tokens.
3. Distances - Bit-packed match distances.

The parser is optimal and based on a modified version of the lzmpo encoder.
The current implementation prioritizes compression ratio over encoding speed and
is intentionally slow.

---

# Design tradeoff

blaris sacrifices decompression throughput in exchange for:

- near-zero RAM usage,
- extremely small decoder size,
- large compression windows,
- random access to compressed data.

It is intended for firmware, bootloaders, ROM-resident resources, and other
systems where RAM is scarce but storage is available.
