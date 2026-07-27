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

## 8KB FILE

```
######## 8KB FILE ##################################################
=== Ratio + full compress/decompress time ===
config              orig(B)    comp(B)   ratio% compress(ms) decompress(ms)
heatshrink-w5          8199       6984    85.18 11.461530000  7.698114000
heatshrink-w7          8199       5347    65.21  8.556099000  8.255805000
heatshrink-w9          8199       4380    53.42  9.058155000  8.170303000
heatshrink-w11         8199       4022    49.05  9.194812000  8.274630000
heatshrink-w13         8199       3977    48.50  9.594294000  8.749513000
heatshrink-w14         8199       4105    50.06  9.409366000  7.813141000
blaris                 8199       3841    46.84 101.876146000 22.720760000
```

---

## 100KB FILE

```
######## 100KB FILE ##################################################
=== Ratio + full compress/decompress time ===
config              orig(B)    comp(B)   ratio% compress(ms) decompress(ms)
heatshrink-w5        101392      69347    68.39 51.930376000 11.220196000
heatshrink-w7        101392      59094    58.28 13.392155000  9.983828000
heatshrink-w9        101392      54494    53.74 14.787272000 10.272732000
heatshrink-w11       101392      52432    51.71 23.251559000  9.648837000
heatshrink-w13       101392      51735    51.02 41.887347000  9.845698000
heatshrink-w14       101392      52329    51.61 68.189059000 10.456428000
blaris               101392      50014    49.32 3653.949563000 748.796778000
```

---

## 900KB FILE

```
######## 900KB FILE ##################################################
=== Ratio + full compress/decompress time ===
config              orig(B)    comp(B)   ratio% compress(ms) decompress(ms)
heatshrink-w5        919544     586966    63.83 52.819390000 36.713185000
heatshrink-w7        919544     515328    56.04 52.930148000 29.625089000
heatshrink-w9        919544     473006    51.43 63.914211000 30.631785000
heatshrink-w11       919544     466305    50.71 109.307988000 28.764529000
heatshrink-w13       919544     464187    50.48 242.627247000 29.269319000
heatshrink-w14       919544     467001    50.78 420.678948000 28.186560000
blaris               919544     444113    48.29 31551.668247000 34797.788427000
```

---

# Random extraction benchmark

One of the design goals of blaris is direct extraction of arbitrary output
ranges without decoding the entire file.

The current CLI benchmark has an important limitation:
**The CLI first reads the entire compressed file into memory before performing
the extraction.**

Because of this, extraction measurements near the beginning of the file can be
affected by input loading time.
For example, extracting data at 1% offset may appear slower because the CLI
still has to read the complete compressed input first.

On embedded systems, where compressed data is already stored in ROM or flash,
this overhead does not exist.

```
=== Chunk extraction time (128 bytes) at various offsets ===
```

```
######## 8KB FILE ##################################################

offset%    offset(B)    hs-w5(ms)        hs-w7(ms)        hs-w9(ms)        hs-w11(ms)       hs-w13(ms)       hs-w14(ms)       blaris(ms)  
1%         81           14.540327000     13.820191000     13.830030000     14.817589000     13.468198000     14.222739000     8.389456000 
25%        2049         14.113253000     13.675469000     14.618184000     14.164259000     13.580469000     14.458873000     7.947634000 
50%        4099         15.789569000     13.756972000     14.744561000     13.809259000     14.333007000     13.888269000     8.678570000 
75%        6149         15.439219000     14.834691000     13.054600000     15.390798000     15.946965000     14.550707000     8.607105000 
99%        8071         14.184257000     13.895804000     14.670793000     13.634421000     13.375895000     14.031700000     8.461271000 
```

```
######## 100KB FILE ##################################################

offset%    offset(B)    hs-w5(ms)        hs-w7(ms)        hs-w9(ms)        hs-w11(ms)       hs-w13(ms)       hs-w14(ms)       blaris(ms)  
1%         1013         11.026921000     9.576261000      9.958991000      10.677243000     11.980297000     10.429127000     5.016816000 
25%        25348        10.489019000     9.706987000      10.403558000     10.001291000     9.399959000      10.610287000     5.792787000 
50%        50696        10.930180000     12.973487000     10.595539000     10.793693000     8.889687000      10.635785000     7.073268000 
75%        76044        10.081061000     10.724612000     9.706706000      12.222112000     11.811118000     9.882327000     5.305981000 
99%        100378       12.836499000     11.656768000     10.386416000     11.109421000     9.533570000      10.640287000     9.342891000 
```

```
######## 900KB FILE ##################################################

offset%    offset(B)    hs-w5(ms)        hs-w7(ms)        hs-w9(ms)        hs-w11(ms)       hs-w13(ms)       hs-w14(ms)       blaris(ms)  
1%         9195         20.943273000     20.693963000     21.655544000     20.389721000     21.369174000     19.129587000     6.519515000 
25%        229886       25.552150000     22.317710000     20.364153000     20.587823000     17.645143000     20.315360000     8.406338000 
50%        459772       22.641210000     21.154410000     19.978566000     21.998530000     20.365014000     19.989768000     16.732004000
75%        689658       22.589031000     21.771322000     21.157115000     18.913492000     22.080825000     19.272426000     8.578622000
99%        910348       22.497950000     21.135225000     19.334493000     21.342104000     21.219174000     19.398093000     12.281003000
```

---

# Compression

The blaris compressor is a split-stream optimal bit-packed LZSS encoder.
Generated tokens are separated into three streams:

1. Controls - One byte describing token type and structure.
2. Literals - Raw bytes emitted by literal tokens.
3. Distances - Bit-packed match distances.

The parser is optimal and based on a modified version of the lzmpo encoder.
The current implementation prioritizes compression ratio over encoding speed and
is intentionally slow.
It uses multiple threads, but the current implementation is not optimized.

---

# Design tradeoff

blaris sacrifices decompression throughput in exchange for:

- near-zero RAM usage,
- extremely small decoder size,
- large compression windows,
- random access to compressed data.

It is intended for firmware, bootloaders, ROM-resident resources, and other
systems where RAM is scarce but storage is available.
