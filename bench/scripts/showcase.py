#!/usr/bin/env python3
"""
Script 1: Showcase a single codec's configurations, RAM usage tiers, and Pareto frontier.

Usage:
    python3 bench/scripts/showcase.py <codec> <dataset> <slice_idx> [-o OUTPUT] [--results RESULTS_JSON] [--show]

Example:
    python3 bench/scripts/showcase.py blaris FreeRTOSConfig.h 5
    python3 bench/scripts/showcase.py heatshrink FreeRTOSConfig.h 5
"""

import argparse
from pathlib import Path
import matplotlib.pyplot as plt

from common import (
    DEFAULT_RESULTS_JSON,
    VISUAL_DIR,
    CODEC_MARKERS,
    load_records,
    compute_pareto,
    add_ram_colorbar,
    style_axes,
    attach_hover_tooltip,
)


def print_pareto_table(codec: str, dataset: str, slice_idx: int, frontier: list[dict], total_count: int):
    slice_bytes = 1 << slice_idx
    print("=" * 96)
    print(
        f"PARETO FRONTIER — Codec: {codec} | Dataset: {dataset} | "
        f"Slice: 2^{slice_idx} = {slice_bytes} B ({len(frontier)} / {total_count} configs)"
    )
    print("=" * 96)
    header = (
        f"{'#':>2}  {'Configuration':<28}  {'Ratio (%)':>10}  {'Comp (B)':>9}  "
        f"{'Throughput (B/kcyc)':>20}  {'RAM (B)':>8}  {'RAM Tier':<12}"
    )
    print(header)
    print("-" * 96)
    for idx, r in enumerate(frontier, start=1):
        print(
            f"{idx:>2}  {r['config_name']:<28}  {r['ratio_pct']:>9.2f}%  "
            f"{r['compressed_size']:>9,}  {r['throughput']:>20.4f}  "
            f"{r['total_ram']:>8,}  {r['ram_label']:<12}"
        )
    print("=" * 96)


def main():
    parser = argparse.ArgumentParser(
        description="Showcase a single codec's Ratio vs. Throughput Pareto frontier colored by RAM usage."
    )
    parser.add_argument("codec", help="Decompressor name (e.g. blaris, heatshrink)")
    parser.add_argument("dataset", help="Dataset filename (e.g. FreeRTOSConfig.h, or ALL)")
    parser.add_argument("slice", type=int, choices=range(0, 9), metavar="SLICE_0_TO_8", help="Slice index 0..8 (size = 2^slice bytes)")
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS_JSON, help="Path to results.json")
    parser.add_argument("-o", "--output", type=Path, default=None, help="Output PNG path")
    parser.add_argument("--no-show", action="store_true", help="Do not open interactive window")
    args = parser.parse_args()

    all_records = load_records(args.results, args.dataset, args.slice)
    codec_records = [r for r in all_records if r["decompressor"] == args.codec]
    if not codec_records:
        available_codecs = sorted({r["decompressor"] for r in all_records})
        raise SystemExit(
            f"No records found for codec '{args.codec}'. Available codecs: {', '.join(available_codecs)}"
        )

    frontier = compute_pareto(codec_records)
    print_pareto_table(args.codec, args.dataset, args.slice, frontier, len(codec_records))

    fig, ax = plt.subplots(figsize=(11.5, 7), dpi=180)
    fig.patch.set_facecolor("white")
    ax.set_facecolor("white")

    marker = CODEC_MARKERS.get(args.codec, "o")

    # Scatter all configurations for the codec, colored by discrete RAM tier
    xs = [r["ratio_pct"] for r in codec_records]
    ys = [r["throughput"] for r in codec_records]
    colors = [r["ram_color"] for r in codec_records]

    sc = ax.scatter(
        xs,
        ys,
        c=colors,
        marker=marker,
        s=75,
        alpha=0.88,
        edgecolors="white",
        linewidths=0.6,
        label=f"{args.codec} configurations ({len(codec_records)})",
        zorder=3,
    )

    # Draw Pareto frontier step/line + highlights
    px = [r["ratio_pct"] for r in frontier]
    py = [r["throughput"] for r in frontier]
    p_colors = [r["ram_color"] for r in frontier]

    ax.plot(
        px,
        py,
        color="#7c3aed",
        linestyle="--",
        linewidth=1.8,
        alpha=0.85,
        label=f"Pareto Frontier ({len(frontier)} optima)",
        zorder=4,
    )
    ax.scatter(
        px,
        py,
        c=p_colors,
        marker=marker,
        s=115,
        edgecolors="#7c3aed",
        linewidths=1.8,
        zorder=5,
    )

    # Uncompressed 100% reference line if near range
    if max(xs) >= 95.0:
        ax.axvline(100.0, color="#9ca3af", linestyle=":", linewidth=1.2, label="Uncompressed (100%)", zorder=2)

    slice_bytes = 1 << args.slice
    uncomp_size = codec_records[0]["uncompressed_size"]
    best_ratio = min(xs)
    best_tp = max(ys)

    title = f"Codec Showcase: {args.codec} — Dataset: {args.dataset} ({uncomp_size:,} B)"
    subtitle = (
        f"Slice Size: 2^{args.slice} = {slice_bytes} B  |  "
        f"Best Ratio: {best_ratio:.2f}%  |  Max Throughput: {best_tp:.4f} B/kcyc  |  "
        f"Pareto Frontier: {len(frontier)}/{len(codec_records)} configs"
    )
    style_axes(ax, title, subtitle)
    add_ram_colorbar(fig, ax)

    ax.legend(loc="best", frameon=True, framealpha=0.94, facecolor="white", edgecolor="#d1d5db", fontsize=9.5)
    fig.tight_layout()

    out_path = args.output
    if out_path is None:
        safe_ds = args.dataset.replace("/", "_")
        out_path = VISUAL_DIR / f"showcase_{args.codec}_{safe_ds}_s{args.slice}.png"

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=180)
    print(f"\nSaved plot to: {out_path}")

    if not args.no_show:
        frontier_names = {r["config_name"] for r in frontier}
        attach_hover_tooltip(fig, ax, [(sc, codec_records)], frontier_names)
        plt.show()
    plt.close(fig)


if __name__ == "__main__":
    main()
