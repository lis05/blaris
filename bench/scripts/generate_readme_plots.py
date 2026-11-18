#!/usr/bin/env python3
"""
Generate 21 three-panel (1 B, 32 B, 256 B) horizontal composite plots
comparing heatshrink's own frontier, tamp's own frontier, and all 9 blaris
preset levels (L1..L9) for each dataset in bench/results-rv32emu.json.

Usage:
    python3 bench/scripts/generate_readme_plots.py [--results RESULTS_JSON] [--out-dir DIR]
"""

import argparse
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import matplotlib.ticker as ticker
from matplotlib.lines import Line2D

from common import (
    DEFAULT_RESULTS_JSON,
    RAM_TIERS,
    CODEC_MARKERS,
    load_records,
    compute_pareto,
)
from generate_readme_tables import BLARIS_LEVEL_BY_CFG

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
DEFAULT_PLOTS_DIR = REPO_ROOT / "bench" / "plots"


def generate_dataset_plot(results_path: Path, dataset: str, out_path: Path):
    """Generate a 3-panel horizontal (1 B, 32 B, 256 B) PNG for one dataset."""
    fig = plt.figure(figsize=(11.6, 4.05), dpi=200)
    fig.patch.set_facecolor("white")

    gs = fig.add_gridspec(
        1, 3, left=0.055, right=0.988, bottom=0.14, top=0.73, wspace=0.18
    )

    slices = [(0, "a"), (5, "b"), (8, "c")]

    # Compute consistent x-axis bounds across all 3 slices using only plotted points
    plotted_xs = []
    panel_data = []
    for slice_idx, panel_label in slices:
        all_recs = load_records(results_path, dataset, slice_idx)
        hs_frontier = compute_pareto([r for r in all_recs if r["decompressor"] == "heatshrink"])
        tamp_frontier = compute_pareto([r for r in all_recs if r["decompressor"] == "tamp"])
        blaris_levels = [r for r in all_recs if r["config_name"] in BLARIS_LEVEL_BY_CFG]
        blaris_levels.sort(key=lambda r: int(BLARIS_LEVEL_BY_CFG[r["config_name"]][1:]))

        for r in (*hs_frontier, *tamp_frontier, *blaris_levels):
            plotted_xs.append(r["ratio_pct"])

        panel_data.append((slice_idx, panel_label, hs_frontier, tamp_frontier, blaris_levels))

    min_ratio = min(plotted_xs)
    max_ratio = max(plotted_xs)
    x_min = max(0, int(min_ratio // 5) * 5 - 4)
    x_max = max(103, int(max_ratio // 5) * 5 + 6)
    uncomp_size = panel_data[0][2][0]["uncompressed_size"] if panel_data[0][2] else 0

    for col_idx, (slice_idx, panel_label, hs_pf, tamp_pf, blaris_lvls) in enumerate(panel_data):
        ax = fig.add_subplot(gs[0, col_idx])
        ax.set_facecolor("white")

        # 1. Heatshrink own frontier (triangles + subtle dashed curve)
        if hs_pf:
            ax.plot(
                [r["ratio_pct"] for r in hs_pf],
                [r["throughput"] for r in hs_pf],
                color="#64748b",
                linestyle=":",
                linewidth=1.1,
                alpha=0.85,
                zorder=3,
            )
            ax.scatter(
                [r["ratio_pct"] for r in hs_pf],
                [r["throughput"] for r in hs_pf],
                c=[r["ram_color"] for r in hs_pf],
                marker=CODEC_MARKERS["heatshrink"],
                s=46,
                alpha=0.92,
                edgecolors="#0f172a",
                linewidths=0.45,
                zorder=4,
            )

        # 2. Tamp own frontier (squares + subtle dashed curve)
        if tamp_pf:
            ax.plot(
                [r["ratio_pct"] for r in tamp_pf],
                [r["throughput"] for r in tamp_pf],
                color="#475569",
                linestyle="--",
                linewidth=1.1,
                alpha=0.85,
                zorder=3,
            )
            ax.scatter(
                [r["ratio_pct"] for r in tamp_pf],
                [r["throughput"] for r in tamp_pf],
                c=[r["ram_color"] for r in tamp_pf],
                marker=CODEC_MARKERS["tamp"],
                s=38,
                alpha=0.92,
                edgecolors="#0f172a",
                linewidths=0.45,
                zorder=4,
            )

        # 3. Blaris levels L1..L9 (stars with level digit 1..9 centered inside)
        if blaris_lvls:
            bx = [r["ratio_pct"] for r in blaris_lvls]
            by = [r["throughput"] for r in blaris_lvls]
            ax.plot(
                bx,
                by,
                color="#7c3aed",
                linestyle="-",
                linewidth=1.35,
                alpha=0.75,
                zorder=5,
            )
            ax.scatter(
                bx,
                by,
                c=[r["ram_color"] for r in blaris_lvls],
                marker="*",
                s=290,
                edgecolors="#0f172a",
                linewidths=0.8,
                zorder=6,
            )
            for r in blaris_lvls:
                lvl_num = BLARIS_LEVEL_BY_CFG[r["config_name"]][1:]
                ax.text(
                    r["ratio_pct"],
                    r["throughput"],
                    lvl_num,
                    ha="center",
                    va="center",
                    fontsize=6.3,
                    fontweight="bold",
                    color="white",
                    zorder=7,
                )

        ax.axvline(100.0, color="#9ca3af", linestyle="-.", linewidth=0.9, zorder=2)
        ax.set_xlim(x_min, x_max)
        ax.set_yscale("log")
        ax.xaxis.set_major_formatter(ticker.PercentFormatter(decimals=0))
        ax.xaxis.set_major_locator(ticker.MultipleLocator(10 if (x_max - x_min) <= 95 else 20))

        ax.grid(True, which="major", linestyle="-", linewidth=0.45, color="#e5e7eb", zorder=0)
        ax.grid(True, which="minor", linestyle=":", linewidth=0.3, color="#f3f4f6", zorder=0)
        ax.set_axisbelow(True)

        for spine in ["top", "right"]:
            ax.spines[spine].set_visible(False)
        for spine in ["left", "bottom"]:
            ax.spines[spine].set_color("#9ca3af")
            ax.spines[spine].set_linewidth(0.75)

        ax.tick_params(axis="both", which="major", labelsize=8.0, length=3.0, width=0.65)
        ax.tick_params(axis="both", which="minor", length=1.8, width=0.45)

        slice_bytes = 1 << slice_idx
        ax.set_title(
            f"({panel_label}) Slice Size: {slice_bytes} B",
            fontsize=9.2,
            fontweight="bold",
            pad=5.0,
        )
        if col_idx == 0:
            ax.set_ylabel("Throughput (B / 1k cyc)", fontsize=8.5, fontweight="bold")
        ax.set_xlabel("Compression Ratio (%)", fontsize=8.5, fontweight="bold")

    # Shared figure header + legend + RAM tier colorbar
    fig.suptitle(
        f"{dataset} ({uncomp_size:,} B) — blaris Levels 1–9 vs. heatshrink & tamp Frontiers",
        x=0.055,
        y=0.975,
        ha="left",
        fontsize=10.2,
        fontweight="bold",
        color="#0f172a",
    )

    legend_handles = [
        Line2D(
            [0], [0], color="#7c3aed", linestyle="-", linewidth=1.35,
            marker="*", markerfacecolor="#2563eb", markeredgecolor="#0f172a",
            markeredgewidth=0.7, markersize=11.0,
            label="blaris levels (1–9)",
        ),
        Line2D(
            [0], [0], color="#64748b", linestyle=":", linewidth=1.1,
            marker=CODEC_MARKERS["heatshrink"], markerfacecolor="#475569",
            markeredgecolor="#0f172a", markeredgewidth=0.45, markersize=6.2,
            label="heatshrink frontier",
        ),
        Line2D(
            [0], [0], color="#475569", linestyle="--", linewidth=1.1,
            marker=CODEC_MARKERS["tamp"], markerfacecolor="#475569",
            markeredgecolor="#0f172a", markeredgewidth=0.45, markersize=5.6,
            label="tamp frontier",
        ),
        Line2D(
            [0], [0], color="#9ca3af", linestyle="-.", linewidth=0.9,
            label="Uncompressed (100%)",
        ),
    ]
    fig.legend(
        handles=legend_handles,
        loc="upper left",
        bbox_to_anchor=(0.052, 0.92),
        ncol=2,
        frameon=True,
        framealpha=0.95,
        facecolor="white",
        edgecolor="#d1d5db",
        fontsize=8.0,
        handletextpad=0.5,
        columnspacing=1.2,
        borderpad=0.4,
    )

    cbar_ax = fig.add_axes([0.59, 0.855, 0.395, 0.045])
    colors = [t["color"] for t in RAM_TIERS]
    cmap = mcolors.ListedColormap(colors)
    bounds = [0, 1, 2, 3, 4, 5]
    norm = mcolors.BoundaryNorm(bounds, cmap.N)
    sm = plt.cm.ScalarMappable(cmap=cmap, norm=norm)
    sm.set_array([])
    cbar = fig.colorbar(sm, cax=cbar_ax, orientation="horizontal", ticks=[0.5, 1.5, 2.5, 3.5, 4.5])
    cbar.ax.set_xticklabels([t["label"] for t in RAM_TIERS], fontsize=7.4)
    cbar.ax.tick_params(length=2, width=0.5, pad=2)
    cbar.set_label("Total RAM (stack + bss + data)", fontsize=8.0, fontweight="bold", labelpad=2.0)
    cbar.outline.set_edgecolor("#9ca3af")
    cbar.outline.set_linewidth(0.65)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, format="png", dpi=200)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description="Generate 21 3-panel dataset plots comparing blaris L1-L9 vs heatshrink & tamp frontiers."
    )
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS_JSON)
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_PLOTS_DIR)
    args = parser.parse_args()

    import json
    with open(args.results, "r", encoding="utf-8") as f:
        data = json.load(f)
    datasets = sorted({r["dataset"] for r in data if r.get("status") == "PASS"})

    for ds in datasets:
        out_png = args.out_dir / f"readme_{ds}.png"
        generate_dataset_plot(args.results, ds, out_png)
        print(f"Generated {out_png}")

    print(f"Generated {len(datasets)} plots in {args.out_dir}")


if __name__ == "__main__":
    main()
