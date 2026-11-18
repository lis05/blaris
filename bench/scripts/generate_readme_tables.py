#!/usr/bin/env python3
"""
Generate the detailed per-file Markdown benchmark tables for README.md
from bench/results-rv32emu.json, reporting average total cycles per slice.

Usage:
    python3 generate_readme_tables.py [path/to/results-rv32emu.json]
"""

import argparse
import json
from collections import defaultdict
from pathlib import Path

DEFAULT_RESULTS_JSON = Path(__file__).resolve().parent.parent / "results-rv32emu.json"

# Slice indices in the 9-element `cycles` array: s in 0..8 -> slice_size = 2^s B
SLICE_INDICES = [
    (2, 4),    # s=2 -> 4 B
    (5, 32),   # s=5 -> 32 B
    (8, 256),  # s=8 -> 256 B
]

NUM_READS = 1000  # 1,000 pseudorandom offsets per slice size


# Ordered from tightest/slowest (L9) to fastest (L1), matching BLARIS_LEVEL=1..9 with o=2, a=0, r=1
BLARIS_LEVEL_CONFIGS = [
    ("L9", "blaris_optimal_o2_a0_s0_f0_r1"),
    ("L8", "blaris_optimal_o2_a0_s10_f0_r1"),
    ("L7", "blaris_greedymaxhops_o2_a0_s10_p1_h32_c0_l8_r1"),
    ("L6", "blaris_greedymaxhops_o2_a0_s8_p1_h32_c0_l16_r1"),
    ("L5", "blaris_greedymaxhops_o2_a0_s8_p4_h8_c0_l16_r1"),
    ("L4", "blaris_greedymaxhops_o2_a0_s8_p16_h8_c128_l16_r1"),
    ("L3", "blaris_greedymaxhops_o2_a0_s8_p16_h8_c384_l64_r1"),
    ("L2", "blaris_greedymaxhops_o2_a0_s6_p16_h8_c128_l64_r1"),
    ("L1", "blaris_greedymaxhops_o2_a0_s6_p16_h8_c384_l64_r1"),
]
BLARIS_LEVEL_BY_CFG = {cfg: lvl for lvl, cfg in BLARIS_LEVEL_CONFIGS}
BLARIS_ORDER = {cfg: idx for idx, (_, cfg) in enumerate(BLARIS_LEVEL_CONFIGS)}


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
    Filter to the selected blaris levels (L1..L9) and canonical baselines:
    - blaris_optimal / blaris_greedymaxhops: configs in BLARIS_LEVEL_CONFIGS
    - heatshrink: i16 index width (omitting i32 duplicates)
    - tamp: all configurations
    """
    if entry.get("status") != "PASS":
        return False
    dec = entry["decompressor"]
    cfg = entry["config_name"]
    if dec in ("blaris_optimal", "blaris_greedymaxhops"):
        return cfg in BLARIS_LEVEL_BY_CFG
    elif dec == "heatshrink":
        return "_i16_" in cfg
    elif dec == "tamp":
        return True
    return False


def ordered_blaris_levels(blaris_entries: list) -> list:
    """Return blaris level configurations ordered from L9 (tightest) to L1 (fastest)."""
    return sorted(
        blaris_entries,
        key=lambda e: BLARIS_ORDER.get(e["config_name"], len(BLARIS_ORDER)),
    )


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


def collect_table_data(results_path: str) -> list[dict]:
    """Load results JSON and build structured comparison groups per dataset and slice size."""
    with open(results_path, "r", encoding="utf-8") as f:
        data = json.load(f)

    by_dataset = defaultdict(lambda: defaultdict(list))
    dataset_sizes = {}

    for entry in data:
        if not is_candidate_config(entry):
            continue
        ds = entry["dataset"]
        dec = entry["decompressor"]
        group = "blaris" if dec in ("blaris_optimal", "blaris_greedymaxhops") else dec
        by_dataset[ds][group].append(entry)
        dataset_sizes[ds] = entry["uncompressed_size"]

    datasets_out = []
    for ds in sorted(by_dataset.keys()):
        blaris_list = ordered_blaris_levels(by_dataset[ds]["blaris"])
        hs_list = by_dataset[ds]["heatshrink"]
        tamp_list = by_dataset[ds]["tamp"]

        slices_out = []
        for slice_idx, slice_bytes in SLICE_INDICES:
            groups = []
            for b_entry in blaris_list:
                b_cfg = b_entry["config_name"]
                b_lvl = BLARIS_LEVEL_BY_CFG[b_cfg]
                b_ratio = b_entry["compression_ratio"]
                b_cyc = cycles_per_slice(b_entry, slice_idx)
                b_ram = total_ram(b_entry)

                baselines = []
                for codec_entries in (hs_list, tamp_list):
                    if not codec_entries:
                        continue
                    m_entry, is_fallback = find_matching_baseline(
                        codec_entries, b_ratio, slice_idx
                    )
                    m_cyc = cycles_per_slice(m_entry, slice_idx)
                    baselines.append(
                        {
                            "config_name": m_entry["config_name"],
                            "ratio_str": f"{m_entry['compression_ratio'] * 100:.2f}%"
                            + ("*" if is_fallback else ""),
                            "cyc_str": f"{round(m_cyc):,} cyc",
                            "vs_str": format_multiplier(m_cyc / b_cyc),
                            "ram_str": f"{total_ram(m_entry):,} B",
                        }
                    )

                groups.append(
                    {
                        "level": b_lvl,
                        "config_name": b_cfg,
                        "ratio_str": f"{b_ratio * 100:.2f}%",
                        "cyc_str": f"{round(b_cyc):,} cyc",
                        "vs_str": "-",
                        "ram_str": f"{b_ram:,} B",
                        "baselines": baselines,
                    }
                )

            slices_out.append({"slice_idx": slice_idx, "slice_bytes": slice_bytes, "groups": groups})

        datasets_out.append(
            {"dataset": ds, "uncompressed_size": dataset_sizes[ds], "slices": slices_out}
        )

    return datasets_out


def generate_markdown(results_path: str) -> str:
    datasets_data = collect_table_data(results_path)
    num_datasets = len(datasets_data)

    lines = [
        "## Benchmarking",
        "",
        f"Detailed per-file comparisons across all {num_datasets} corpus datasets at slice sizes of **4 B**, **32 B**, and **256 B**.",
        "For each slice size, all 9 `blaris` preset levels (`L9` tightest to `L1` fastest) are grouped with the most lightweight (lowest RAM usage) `heatshrink` and `tamp` configuration that achieves the same or better compression ratio (`ratio <= blaris_ratio`, tie-breaking by lowest decompression cycles; if none reaches that ratio, the best-ratio configuration for that codec is shown as a fallback and marked with `*`).",
    ]

    for ds_item in datasets_data:
        ds = ds_item["dataset"]
        uncomp_size = ds_item["uncompressed_size"]
        lines.append("")
        lines.append("<details>")
        lines.append(f"<summary><strong><code>{ds}</code></strong> ({uncomp_size:,} B)</summary>")
        lines.append("")
        lines.append(f"![{ds} — blaris Levels 1–9 vs. heatshrink & tamp Frontiers](bench/plots/readme_{ds}.png)")

        for sl_item in ds_item["slices"]:
            slice_bytes = sl_item["slice_bytes"]
            lines.append("")
            lines.append(f"#### `{ds}` — {slice_bytes} B Slices")
            lines.append("")
            lines.append("| Configuration | Ratio | Decompression Cycles | vs. `blaris` | RAM Usage |")
            lines.append("| :--- | ---: | ---: | ---: | ---: |")

            for i, g in enumerate(sl_item["groups"]):
                if i > 0:
                    lines.append("| | | | | |")

                lines.append(
                    f"| **`{g['level']}` (`{g['config_name']}`)** | **{g['ratio_str']}** | "
                    f"**{g['cyc_str']}** | **{g['vs_str']}** | **{g['ram_str']}** |"
                )

                for b in g["baselines"]:
                    lines.append(
                        f"| `{b['config_name']}` | {b['ratio_str']} | "
                        f"{b['cyc_str']} | {b['vs_str']} | {b['ram_str']} |"
                    )

        lines.append("")
        lines.append("</details>")

    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Generate README benchmark tables from results JSON")
    parser.add_argument(
        "results",
        nargs="?",
        default=str(DEFAULT_RESULTS_JSON),
        help="Path to results JSON (default: bench/results-rv32emu.json)",
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
