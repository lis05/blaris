#!/usr/bin/env python3
"""Calculate average and maximum reference-resolution jumps per byte across datasets.

Measures how many match-to-match hops (jumps) are required to reach a literal run
for each byte position i = 0..n-1 in every dataset, comparing unflattened (f0)
vs. chain-flattened (f1) payloads.
"""

import argparse
import struct
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent.parent
BUILD_DIR = BENCH_DIR / "build"
DATA_DIR = BENCH_DIR / "data"


def extract_bits(data: bytes, base_offset: int, bit_offset: int, bit_count: int) -> int:
    byte_idx = base_offset + ((bit_offset >> 4) * 2)
    raw = int.from_bytes(data[byte_idx : byte_idx + 4], "little")
    return (raw >> (bit_offset & 15)) & ((1 << bit_count) - 1)


def compute_jumps_for_payload(compressed_path: Path, uncompressed_size: int, align_log: int) -> list[int]:
    """Return an array `jumps` of length `uncompressed_size` where jumps[i] is the
    number of reference jumps required to resolve byte at position i to a literal."""
    data = compressed_path.read_bytes()
    metas_offset, controls_offset, literals_offset, distances_offset = struct.unpack_from("<IIII", data, 0)

    # Walk the control stream forward to map every byte i = 0..n-1
    # Since matches only reference strictly earlier positions (i - distance < i),
    # we can compute jumps[i] in O(n) via dynamic programming!
    jumps = [0] * uncompressed_size

    pos = 0
    ctrl_idx = controls_offset
    dist_bit = 0

    while pos < uncompressed_size and ctrl_idx < literals_offset:
        c = data[ctrl_idx]
        ctrl_idx += 1

        if c <= 15:
            # Literal run of length (c + 1) -> 0 jumps needed
            lit_len = c + 1
            pos += lit_len
        else:
            # Match token
            length = (c >> 4) + 1
            distance_bits = (c & 0xF) + 1
            distance = (extract_bits(data, distances_offset, dist_bit, distance_bits) + 1) << align_log
            dist_bit += distance_bits

            for j in range(length):
                if pos + j < uncompressed_size:
                    jumps[pos + j] = 1 + jumps[pos + j - distance]
            pos += length

    if pos != uncompressed_size:
        raise ValueError(f"Decoded size mismatch on {compressed_path}: got {pos}, expected {uncompressed_size}")

    return jumps


def main():
    parser = argparse.ArgumentParser(description="Analyze match jump chains for blaris f0 vs f1.")
    parser.add_argument(
        "--align",
        type=int,
        default=0,
        choices=[0, 1, 2],
        help="BLARIS_DISTANCE_ALIGN_LOG (default: 0)",
    )
    parser.add_argument(
        "--offset-size",
        type=int,
        default=2,
        choices=[2, 3, 4],
        help="BLARIS_STREAM_OFFSET_SIZE (default: 2)",
    )
    parser.add_argument(
        "--spread",
        type=int,
        default=8,
        choices=[0, 6, 8, 10, 12, 14],
        help="BLARIS_CHECKPOINTS_SPREAD_LOG (default: 8)",
    )
    args = parser.parse_args()

    dir_f0 = BUILD_DIR / f"blaris_o{args.offset_size}_a{args.align}_s{args.spread}_f0"
    dir_f1 = BUILD_DIR / f"blaris_o{args.offset_size}_a{args.align}_s{args.spread}_f1"

    datasets = sorted(p.name for p in DATA_DIR.iterdir() if p.is_file())

    print(
        f"Configuration base: o{args.offset_size}_a{args.align}_s{args.spread}_f{{0,1}}\n"
    )
    header = (
        f"{'Dataset':<28} | {'Size':>7} | "
        f"{'f0 Ratio':>8} {'f0 Avg':>8} {'f0 Max':>7} | "
        f"{'f1 dRatio':>9} {'f1 Avg':>8} {'f1 Max':>7} | {'Avg Reduc.':>14}"
    )
    print(header)
    print("-" * len(header))

    all_f0_ratios = []
    all_f1_ratios = []
    all_f0_avgs = []
    all_f1_avgs = []
    all_f0_maxs = []
    all_f1_maxs = []
    total_bytes = 0
    total_f0_comp = 0
    total_f1_comp = 0
    weighted_f0_sum = 0
    weighted_f1_sum = 0

    for ds in datasets:
        raw_size = (DATA_DIR / ds).stat().st_size
        p_f0 = dir_f0 / f"dataset_{ds}" / "compressed_payload.bin"
        p_f1 = dir_f1 / f"dataset_{ds}" / "compressed_payload.bin"

        if not p_f0.exists() or not p_f1.exists():
            continue

        comp_f0 = p_f0.stat().st_size
        comp_f1 = p_f1.stat().st_size
        ratio_f0 = comp_f0 / raw_size * 100.0
        ratio_f1 = comp_f1 / raw_size * 100.0
        ratio_diff = ratio_f1 - ratio_f0

        jumps_f0 = compute_jumps_for_payload(p_f0, raw_size, args.align)
        jumps_f1 = compute_jumps_for_payload(p_f1, raw_size, args.align)

        avg_f0 = sum(jumps_f0) / raw_size
        max_f0 = max(jumps_f0)
        avg_f1 = sum(jumps_f1) / raw_size
        max_f1 = max(jumps_f1)

        reduc = ((avg_f0 - avg_f1) / avg_f0 * 100.0) if avg_f0 > 0 else 0.0
        speedup = (avg_f0 / avg_f1) if avg_f1 > 0 else 1.0

        all_f0_ratios.append(ratio_f0)
        all_f1_ratios.append(ratio_f1)
        all_f0_avgs.append(avg_f0)
        all_f1_avgs.append(avg_f1)
        all_f0_maxs.append(max_f0)
        all_f1_maxs.append(max_f1)
        total_bytes += raw_size
        total_f0_comp += comp_f0
        total_f1_comp += comp_f1
        weighted_f0_sum += sum(jumps_f0)
        weighted_f1_sum += sum(jumps_f1)

        print(
            f"{ds:<28} | {raw_size:>7} | "
            f"{ratio_f0:>7.2f}% {avg_f0:>8.2f} {max_f0:>7d} | "
            f"{ratio_diff:>+8.2f}% {avg_f1:>8.2f} {max_f1:>7d} | "
            f"-{reduc:>5.1f}% ({speedup:>4.2f}x)"
        )

    print("-" * len(header))
    mean_r0 = sum(all_f0_ratios) / len(all_f0_ratios)
    mean_r1 = sum(all_f1_ratios) / len(all_f1_ratios)
    mean_f0 = sum(all_f0_avgs) / len(all_f0_avgs)
    mean_f1 = sum(all_f1_avgs) / len(all_f1_avgs)
    mean_reduc = (mean_f0 - mean_f1) / mean_f0 * 100.0
    print(
        f"{'Dataset Mean':<28} | {len(all_f0_avgs):>4} ds | "
        f"{mean_r0:>7.2f}% {mean_f0:>8.2f} {max(all_f0_maxs):>7d} | "
        f"{mean_r1 - mean_r0:>+8.2f}% {mean_f1:>8.2f} {max(all_f1_maxs):>7d} | "
        f"-{mean_reduc:>5.1f}% ({mean_f0 / mean_f1:>4.2f}x)"
    )
    w_r0 = total_f0_comp / total_bytes * 100.0
    w_r1 = total_f1_comp / total_bytes * 100.0
    w_f0 = weighted_f0_sum / total_bytes
    w_f1 = weighted_f1_sum / total_bytes
    w_reduc = (w_f0 - w_f1) / w_f0 * 100.0
    print(
        f"{'Byte-Weighted Overall':<28} | {total_bytes:>7} | "
        f"{w_r0:>7.2f}% {w_f0:>8.2f} {max(all_f0_maxs):>7d} | "
        f"{w_r1 - w_r0:>+8.2f}% {w_f1:>8.2f} {max(all_f1_maxs):>7d} | "
        f"-{w_reduc:>5.1f}% ({w_f0 / w_f1:>4.2f}x)"
    )


if __name__ == "__main__":
    main()
