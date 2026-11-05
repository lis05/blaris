#!/usr/bin/env python3
"""Generate clean, publication-grade raw Pareto plots and a shared legend/colorbar
for zynq7000s.dtb at slice sizes 1 B (s=0), 32 B (s=5), and 256 B (s=8).
"""

from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import matplotlib.ticker as ticker
from matplotlib.lines import Line2D

from common import (
    DEFAULT_RESULTS_JSON,
    RAM_TIERS,
    CODEC_MARKERS,
    FALLBACK_MARKERS,
    load_records,
    compute_pareto,
)

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
FIG_DIR = REPO_ROOT / "paper" / "figures"

# Marker sizes tuned per shape so circles/squares don't visually overpower triangles
MARKER_SIZE_REGULAR = {
    "blaris": 9.5,
    "heatshrink": 12.5,
    "tamp": 9.0,
}
MARKER_SIZE_PARETO = {
    "blaris": 13.5,
    "heatshrink": 16.5,
    "tamp": 12.5,
}


def get_codec_marker(codec: str, idx: int) -> str:
    if codec in CODEC_MARKERS:
        return CODEC_MARKERS[codec]
    return FALLBACK_MARKERS[idx % len(FALLBACK_MARKERS)]


def filter_records(records: list[dict]) -> list[dict]:
    """Filter out redundant o3/o4 (>64 KiB offset widths) and i32 duplicate buffers."""
    filtered = []
    for r in records:
        cfg = r["config_name"]
        if cfg.startswith("blaris_o3_") or cfg.startswith("blaris_o4_"):
            continue
        if cfg.startswith("heatshrink_") and "_i32_" in cfg:
            continue
        filtered.append(r)
    return filtered


def generate_raw_slice_plot(
    dataset: str,
    slice_idx: int,
    panel_label: str,
    out_path: Path,
    show_xlabel: bool = False,
):
    records = filter_records(load_records(DEFAULT_RESULTS_JSON, dataset, slice_idx))
    frontier = compute_pareto(records)
    codecs = sorted({r["decompressor"] for r in records})

    # Maximize plot area to fill the full column height and width
    fig_h = 2.63 if show_xlabel else 2.46
    fig, ax = plt.subplots(figsize=(3.66, fig_h))
    fig.patch.set_facecolor("white")
    ax.set_facecolor("white")

    # 1. Scatter points per codec, colored by RAM tier
    for idx, codec in enumerate(codecs):
        sub = [r for r in records if r["decompressor"] == codec]
        marker = get_codec_marker(codec, idx)
        xs = [r["ratio_pct"] for r in sub]
        ys = [r["throughput"] for r in sub]
        colors = [r["ram_color"] for r in sub]
        ms = MARKER_SIZE_REGULAR.get(codec, 10)

        ax.scatter(
            xs,
            ys,
            c=colors,
            marker=marker,
            s=ms,
            alpha=0.85,
            edgecolors="white",
            linewidths=0.22,
            zorder=4,
        )

    # 2. Pareto frontier curve & highlighted points
    px = [r["ratio_pct"] for r in frontier]
    py = [r["throughput"] for r in frontier]
    ax.plot(
        px,
        py,
        color="#7c3aed",
        linestyle="--",
        linewidth=1.0,
        alpha=0.92,
        zorder=5,
    )
    for r in frontier:
        codec = r["decompressor"]
        m_shape = get_codec_marker(codec, codecs.index(codec))
        ms_pf = MARKER_SIZE_PARETO.get(codec, 14)
        ax.scatter(
            [r["ratio_pct"]],
            [r["throughput"]],
            c=[r["ram_color"]],
            marker=m_shape,
            s=ms_pf,
            edgecolors="#7c3aed",
            linewidths=0.75,
            zorder=6,
        )

    # 3. Uncompressed (100%) reference line
    ax.axvline(100.0, color="#9ca3af", linestyle="-.", linewidth=0.85, zorder=2)

    # 4. Axes styling & fixed bounds for vertical alignment across all 3 slices
    ax.set_xlim(28, 103)
    ax.set_yscale("log")
    ax.xaxis.set_major_formatter(ticker.PercentFormatter(decimals=0))
    ax.xaxis.set_major_locator(ticker.MultipleLocator(10))

    ax.grid(True, which="major", linestyle="-", linewidth=0.45, color="#e5e7eb", zorder=0)
    ax.grid(True, which="minor", linestyle=":", linewidth=0.3, color="#f3f4f6", zorder=0)
    ax.set_axisbelow(True)

    for spine in ["top", "right"]:
        ax.spines[spine].set_visible(False)
    for spine in ["left", "bottom"]:
        ax.spines[spine].set_color("#9ca3af")
        ax.spines[spine].set_linewidth(0.7)

    ax.tick_params(axis="both", which="major", labelsize=6.8, length=2.8, width=0.6, pad=1.5)
    ax.tick_params(axis="both", which="minor", length=1.6, width=0.4)

    slice_bytes = 1 << slice_idx
    ax.set_title(
        f"({panel_label}) Slice Size: {slice_bytes} B",
        fontsize=7.8,
        fontweight="bold",
        pad=3.5,
    )
    ax.set_ylabel("Throughput (B / 1k cyc)", fontsize=7.2, fontweight="bold", labelpad=1.5)
    if show_xlabel:
        ax.set_xlabel("Compression Ratio (%)", fontsize=7.2, fontweight="bold", labelpad=2.0)

    # Tight margins with full clearance for "Compression Ratio (%)" descenders on (c)
    bottom_margin = 0.178 if show_xlabel else 0.095
    top_margin = 0.915 if show_xlabel else 0.91
    fig.subplots_adjust(left=0.115, right=0.992, top=top_margin, bottom=bottom_margin)
    fig.savefig(out_path, format="pdf")
    plt.close(fig)


def generate_shared_legend_and_colorbar(out_path: Path):
    fig = plt.figure(figsize=(3.66, 1.08))
    fig.patch.set_facecolor("white")

    # Top area: shared marker/line legend
    legend_handles = [
        Line2D(
            [0], [0],
            marker=CODEC_MARKERS["blaris"],
            color="none",
            markerfacecolor="#475569",
            markeredgecolor="white",
            markersize=4.5,
            label="blaris (36)",
        ),
        Line2D(
            [0], [0],
            marker=CODEC_MARKERS["heatshrink"],
            color="none",
            markerfacecolor="#475569",
            markeredgecolor="white",
            markersize=5.0,
            label="heatshrink (37)",
        ),
        Line2D(
            [0], [0],
            marker=CODEC_MARKERS["tamp"],
            color="none",
            markerfacecolor="#475569",
            markeredgecolor="white",
            markersize=4.4,
            label="tamp (24)",
        ),
        Line2D(
            [0], [0],
            color="#7c3aed",
            linestyle="--",
            linewidth=1.0,
            marker="o",
            markerfacecolor="white",
            markeredgecolor="#7c3aed",
            markeredgewidth=0.75,
            markersize=4.0,
            label="Pareto Frontier",
        ),
        Line2D(
            [0], [0],
            color="#9ca3af",
            linestyle="-.",
            linewidth=0.85,
            label="Uncompressed (100%)",
        ),
    ]

    fig.legend(
        handles=legend_handles,
        loc="upper center",
        bbox_to_anchor=(0.52, 0.99),
        ncol=3,
        frameon=True,
        framealpha=0.95,
        facecolor="white",
        edgecolor="#d1d5db",
        fontsize=6.6,
        handletextpad=0.4,
        columnspacing=0.9,
        borderpad=0.35,
    )

    # Bottom area: horizontal 5-tier discrete RAM colorbar well separated from legend above and edge below
    cbar_ax = fig.add_axes([0.08, 0.29, 0.88, 0.13])
    colors = [t["color"] for t in RAM_TIERS]
    cmap = mcolors.ListedColormap(colors)
    bounds = [0, 1, 2, 3, 4, 5]
    norm = mcolors.BoundaryNorm(bounds, cmap.N)
    sm = plt.cm.ScalarMappable(cmap=cmap, norm=norm)
    sm.set_array([])

    cbar = fig.colorbar(
        sm,
        cax=cbar_ax,
        orientation="horizontal",
        ticks=[0.5, 1.5, 2.5, 3.5, 4.5],
    )
    cbar.ax.set_xticklabels([t["label"] for t in RAM_TIERS], fontsize=6.4)
    cbar.ax.tick_params(length=2, width=0.5, pad=2)
    cbar.set_label("Total RAM (stack + bss + data)", fontsize=6.8, fontweight="bold", labelpad=2.0)
    cbar.outline.set_edgecolor("#9ca3af")
    cbar.outline.set_linewidth(0.6)

    fig.savefig(out_path, format="pdf")
    plt.close(fig)


def generate_readme_composite_plot(dataset: str, out_path: Path):
    """Generate a 3-panel (1 B, 32 B, 256 B) composite PNG with shared legend and colorbar for README."""
    fig = plt.figure(figsize=(11.2, 3.85), dpi=220)
    fig.patch.set_facecolor("white")

    gs = fig.add_gridspec(
        1, 3, left=0.055, right=0.988, bottom=0.14, top=0.73, wspace=0.18
    )

    slices = [(0, "a"), (5, "b"), (8, "c")]
    first_records = filter_records(load_records(DEFAULT_RESULTS_JSON, dataset, 0))
    min_ratio = min(r["ratio_pct"] for r in first_records)
    x_min = max(0, int(min_ratio // 5) * 5 - 3)

    for col_idx, (slice_idx, panel_label) in enumerate(slices):
        ax = fig.add_subplot(gs[0, col_idx])
        ax.set_facecolor("white")
        records = filter_records(load_records(DEFAULT_RESULTS_JSON, dataset, slice_idx))
        frontier = compute_pareto(records)
        codecs = sorted({r["decompressor"] for r in records})

        for idx, codec in enumerate(codecs):
            sub = [r for r in records if r["decompressor"] == codec]
            marker = get_codec_marker(codec, idx)
            xs = [r["ratio_pct"] for r in sub]
            ys = [r["throughput"] for r in sub]
            colors = [r["ram_color"] for r in sub]
            ms = MARKER_SIZE_REGULAR.get(codec, 10) * 1.35

            ax.scatter(
                xs, ys, c=colors, marker=marker, s=ms,
                alpha=0.85, edgecolors="white", linewidths=0.25, zorder=4,
            )

        px = [r["ratio_pct"] for r in frontier]
        py = [r["throughput"] for r in frontier]
        ax.plot(px, py, color="#7c3aed", linestyle="--", linewidth=1.1, alpha=0.92, zorder=5)
        for r in frontier:
            codec = r["decompressor"]
            m_shape = get_codec_marker(codec, codecs.index(codec))
            ms_pf = MARKER_SIZE_PARETO.get(codec, 14) * 1.35
            ax.scatter(
                [r["ratio_pct"]], [r["throughput"]], c=[r["ram_color"]],
                marker=m_shape, s=ms_pf, edgecolors="#7c3aed", linewidths=0.85, zorder=6,
            )

        ax.axvline(100.0, color="#9ca3af", linestyle="-.", linewidth=0.9, zorder=2)
        ax.set_xlim(x_min, 103)
        ax.set_yscale("log")
        ax.xaxis.set_major_formatter(ticker.PercentFormatter(decimals=0))
        ax.xaxis.set_major_locator(ticker.MultipleLocator(10))

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
        ax.set_title(f"({panel_label}) Slice Size: {slice_bytes} B", fontsize=9.2, fontweight="bold", pad=5.0)
        if col_idx == 0:
            ax.set_ylabel("Throughput (B / 1k cyc)", fontsize=8.5, fontweight="bold")
        ax.set_xlabel("Compression Ratio (%)", fontsize=8.5, fontweight="bold")

    # Shared top legend (left) and discrete RAM colorbar (right)
    legend_handles = [
        Line2D([0], [0], marker=CODEC_MARKERS["blaris"], color="none",
               markerfacecolor="#475569", markeredgecolor="white", markersize=6.0, label="blaris (36)"),
        Line2D([0], [0], marker=CODEC_MARKERS["heatshrink"], color="none",
               markerfacecolor="#475569", markeredgecolor="white", markersize=6.5, label="heatshrink (37)"),
        Line2D([0], [0], marker=CODEC_MARKERS["tamp"], color="none",
               markerfacecolor="#475569", markeredgecolor="white", markersize=5.8, label="tamp (24)"),
        Line2D([0], [0], color="#7c3aed", linestyle="--", linewidth=1.1, marker="o",
               markerfacecolor="white", markeredgecolor="#7c3aed", markeredgewidth=0.85,
               markersize=5.2, label="Pareto Frontier"),
        Line2D([0], [0], color="#9ca3af", linestyle="-.", linewidth=0.9, label="Uncompressed (100%)"),
    ]
    fig.legend(
        handles=legend_handles, loc="upper left", bbox_to_anchor=(0.055, 0.96),
        ncol=5, frameon=True, framealpha=0.95, facecolor="white", edgecolor="#d1d5db",
        fontsize=8.2, handletextpad=0.4, columnspacing=1.1, borderpad=0.45,
    )

    cbar_ax = fig.add_axes([0.62, 0.885, 0.365, 0.045])
    colors = [t["color"] for t in RAM_TIERS]
    cmap = mcolors.ListedColormap(colors)
    bounds = [0, 1, 2, 3, 4, 5]
    norm = mcolors.BoundaryNorm(bounds, cmap.N)
    sm = plt.cm.ScalarMappable(cmap=cmap, norm=norm)
    sm.set_array([])
    cbar = fig.colorbar(sm, cax=cbar_ax, orientation="horizontal", ticks=[0.5, 1.5, 2.5, 3.5, 4.5])
    cbar.ax.set_xticklabels([t["label"] for t in RAM_TIERS], fontsize=7.5)
    cbar.ax.tick_params(length=2, width=0.5, pad=2)
    cbar.set_label("Total RAM (stack + bss + data)", fontsize=8.0, fontweight="bold", labelpad=2.0)
    cbar.outline.set_edgecolor("#9ca3af")
    cbar.outline.set_linewidth(0.65)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, format="png", dpi=220)
    plt.close(fig)


def main():
    FIG_DIR.mkdir(parents=True, exist_ok=True)
    dataset = "zynq7000s.dtb"
    generate_raw_slice_plot(dataset, 0, "a", FIG_DIR / "zynq7000s_s0.pdf", show_xlabel=False)
    generate_raw_slice_plot(dataset, 5, "b", FIG_DIR / "zynq7000s_s5.pdf", show_xlabel=False)
    generate_raw_slice_plot(dataset, 8, "c", FIG_DIR / "zynq7000s_s8.pdf", show_xlabel=True)
    generate_shared_legend_and_colorbar(FIG_DIR / "zynq7000s_legend_cbar.pdf")
    print(f"Generated 4 PDF figures in {FIG_DIR}")

    plots_dir = REPO_ROOT / "bench" / "plots"
    for ds in ["zynq7000s.dtb", "FreeRTOSConfig.h", "cp.html"]:
        out_png = plots_dir / f"readme_{ds}.png"
        generate_readme_composite_plot(ds, out_png)
        print(f"Generated README plot: {out_png}")


if __name__ == "__main__":
    main()
