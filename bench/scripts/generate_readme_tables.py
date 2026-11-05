#!/usr/bin/env python3
"""
Generate the detailed per-file Markdown benchmark tables for README.md
from bench/results.json, reporting average total cycles per slice.

Usage:
    python3 generate_readme_tables.py [path/to/results.json]
"""

import argparse
import json
from collections import defaultdict

# Slice indices in the 9-element `cycles` array: s in 0..8 -> slice_size = 2^s B
SLICE_INDICES = [
    (2, 4),    # s=2 -> 4 B
    (5, 32),   # s=5 -> 32 B
    (8, 256),  # s=8 -> 256 B
]

NUM_READS = 1000  # 1,000 pseudorandom offsets per slice size


def total_ram(entry: dict) -> int:
    """Compute Total RAM = peak_stack + with_adapter_size.bss + with_adapter_size.data."""
    adapter = entry["with_adapter_size"]
    return entry["peak_stack"] + adapter.get("bss", 0) + adapter.get("data", 0)


def cycles_per_slice(entry: dict, slice_idx: int) -> float:
    """Compute average total decompression cycles per slice read."""
    return entry["cycles"][slice_idx] / NUM_READS


def format_multiplier(mult: float) -> str:
    """Format speedup/slowdown multiplier to match README style (e.g., x4.98, x12.3, x125, x1,780)."""
    if mult < 9.995:
        return f"x{mult:.2f}"
    elif mult < 99.95:
        return f"x{mult:.1f}"
    else:
        return f"x{round(mult):,}"


def is_candidate_config(entry: dict) -> bool:
    """
    Filter to the canonical 97 configurations used in plots/tables:
    - blaris: o2 stream offsets (omitting o3/o4 duplicates)
    - heatshrink: i16 index width (omitting i32 duplicates)
    - tamp: all configurations
    """
    if entry.get("status") != "PASS":
        return False
    dec = entry["decompressor"]
    cfg = entry["config_name"]
    if dec == "blaris":
        return cfg.startswith("blaris_o2_")
    elif dec == "heatshrink":
        return "_i16_" in cfg
    elif dec == "tamp":
        return True
    return False


def compute_blaris_pareto(blaris_entries: list, slice_idx: int) -> list:
    """
    Return the Pareto-optimal blaris configurations (minimizing both
    compression_ratio and cycles_per_slice), sorted by ascending ratio.
    """
    sorted_entries = sorted(
        blaris_entries,
        key=lambda e: (
            e["compression_ratio"],
            cycles_per_slice(e, slice_idx),
            e["config_name"],
        ),
    )

    pareto = []
    best_cyc = float("inf")
    for e in sorted_entries:
        cyc = cycles_per_slice(e, slice_idx)
        if cyc < best_cyc:
            pareto.append(e)
            best_cyc = cyc

    return pareto


def find_matching_baseline(
    baseline_entries: list,
    target_ratio: float,
    slice_idx: int,
) -> tuple[dict, bool]:
    """
    Find the most lightweight (lowest RAM) baseline configuration with
    compression_ratio <= target_ratio, tie-breaking by lowest decompression cycles.
    If none reaches target_ratio, fall back to the best-ratio configuration (marked with *).
    """
    qualifying = [
        e for e in baseline_entries
        if e["compression_ratio"] <= target_ratio + 1e-12
    ]

    if qualifying:
        best = min(
            qualifying,
            key=lambda e: (
                total_ram(e),
                cycles_per_slice(e, slice_idx),
                e["compression_ratio"],
            ),
        )
        return best, False
    else:
        # Fallback: best compression ratio, tie-breaking by lowest cycles and RAM
        best = min(
            baseline_entries,
            key=lambda e: (
                e["compression_ratio"],
                cycles_per_slice(e, slice_idx),
                total_ram(e),
            ),
        )
        return best, True


def generate_markdown(results_path: str) -> str:
    with open(results_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    # Group entries by dataset and codec
    by_dataset = defaultdict(lambda: defaultdict(list))
    dataset_sizes = {}

    for entry in data:
        if not is_candidate_config(entry):
            continue
        ds = entry["dataset"]
        dec = entry["decompressor"]
        by_dataset[ds][dec].append(entry)
        dataset_sizes[ds] = entry["uncompressed_size"]

    lines = [
        "## Benchmarking",
        "",
        "Detailed per-file comparisons across all 21 corpus datasets at slice sizes of **4 B**, **32 B**, and **256 B**.",
        "For each slice size, every Pareto-optimal `blaris` configuration is grouped with the most lightweight (lowest RAM usage) `heatshrink` and `tamp` configuration that achieves the same or better compression ratio (`ratio <= blaris_ratio`, tie-breaking by lowest decompression cycles; if none reaches that ratio, the best-ratio configuration for that codec is shown as a fallback and marked with `*`).",
    ]

    for ds in sorted(by_dataset.keys()):
        uncomp_size = dataset_sizes[ds]
        lines.append("")
        lines.append("<details>")
        lines.append(f"<summary><strong><code>{ds}</code></strong> ({uncomp_size:,} B)</summary>")

        blaris_list = by_dataset[ds]["blaris"]
        hs_list = by_dataset[ds]["heatshrink"]
        tamp_list = by_dataset[ds]["tamp"]

        for slice_idx, slice_bytes in SLICE_INDICES:
            lines.append("")
            lines.append(f"#### `{ds}` — {slice_bytes} B Slices")
            lines.append("")
            lines.append("| Configuration | Ratio | Decompression Cycles | vs. `blaris` | RAM Usage |")
            lines.append("| :--- | ---: | ---: | ---: | ---: |")

            pareto_blaris = compute_blaris_pareto(blaris_list, slice_idx)

            for i, b_entry in enumerate(pareto_blaris):
                if i > 0:
                    lines.append("| | | | | |")

                b_cfg = b_entry["config_name"]
                b_ratio = b_entry["compression_ratio"]
                b_cyc = cycles_per_slice(b_entry, slice_idx)
                b_ram = total_ram(b_entry)

                lines.append(
                    f"| **`{b_cfg}`** | **{b_ratio * 100:.2f}%** | "
                    f"**{round(b_cyc):,} cyc** | **-** | **{b_ram:,} B** |"
                )

                for codec_entries in (hs_list, tamp_list):
                    if not codec_entries:
                        continue
                    m_entry, is_fallback = find_matching_baseline(
                        codec_entries, b_ratio, slice_idx
                    )
                    m_cfg = m_entry["config_name"]
                    m_ratio_str = f"{m_entry['compression_ratio'] * 100:.2f}%" + ("*" if is_fallback else "")
                    m_cyc = cycles_per_slice(m_entry, slice_idx)
                    m_ram = total_ram(m_entry)
                    mult_str = format_multiplier(m_cyc / b_cyc)

                    lines.append(
                        f"| `{m_cfg}` | {m_ratio_str} | "
                        f"{round(m_cyc):,} cyc | {mult_str} | {m_ram:,} B |"
                    )

        lines.append("")
        lines.append("</details>")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Generate README benchmark tables from results.json")
    parser.add_argument(
        "results",
        nargs="?",
        default="bench/results.json",
        help="Path to results.json (default: bench/results.json)",
    )
    parser.add_argument(
        "-o", "--output",
        help="Optional output file path (prints to stdout if omitted)",
    )
    args = parser.parse_args()

    md = generate_markdown(args.results)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(md + "\n")
    else:
        print(md)


if __name__ == "__main__":
    main()
