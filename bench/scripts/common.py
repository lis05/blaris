import json
import re
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.colors as mcolors
import matplotlib.ticker as ticker
import numpy as np

VISUAL_DIR = Path(__file__).resolve().parent
BENCH_DIR = VISUAL_DIR.parent
DEFAULT_RESULTS_JSON = BENCH_DIR / "results.json"
HARNESS_C = BENCH_DIR / "harness.c"

# Discrete RAM tiers (in bytes):
# Tier 1: < 128 B       (blue)
# Tier 2: < 512 B       (green)
# Tier 3: < 2 KiB       (orange)
# Tier 4: < 4 KiB       (red)
# Tier 5: >= 4 KiB      (black)
RAM_TIERS = [
    {"max": 128, "label": "< 128 B", "color": "#2563eb"},
    {"max": 512, "label": "128–512 B", "color": "#16a34a"},
    {"max": 2048, "label": "512 B–2 KiB", "color": "#f59e0b"},
    {"max": 4096, "label": "2–4 KiB", "color": "#dc2626"},
    {"max": float("inf"), "label": "≥ 4 KiB", "color": "#111827"},
]

CODEC_MARKERS = {
    "blaris": "o",
    "heatshrink": "^",
    "tamp": "s",
}
FALLBACK_MARKERS = ["s", "D", "v", "P", "X", "p"]


def detect_num_iters() -> int:
    """Detect loop iteration count from bench/harness.c (defaults to 1000)."""
    if HARNESS_C.exists():
        text = HARNESS_C.read_text()
        m = re.search(r"for\s*\(\s*int\s+i\s*=\s*0\s*;\s*i\s*<\s*(\d+)\s*;", text)
        if m:
            return int(m.group(1))
    return 1000


def get_total_ram(entry: dict) -> int:
    """Calculate total RAM usage = peak_stack + bss + data (with adapter)."""
    stack = entry.get("peak_stack", 0)
    was = entry.get("with_adapter_size", {})
    return stack + was.get("bss", 0) + was.get("data", 0)


def get_ram_tier_index(ram_bytes: float) -> int:
    """Return tier index 0..4 for a given RAM usage in bytes."""
    for idx, tier in enumerate(RAM_TIERS):
        if ram_bytes < tier["max"]:
            return idx
    return len(RAM_TIERS) - 1


def get_ram_color(ram_bytes: float) -> str:
    """Return hex color string for a given RAM usage in bytes."""
    return RAM_TIERS[get_ram_tier_index(ram_bytes)]["color"]


def get_ram_tier_label(ram_bytes: float) -> str:
    """Return human-readable tier label for a given RAM usage in bytes."""
    return RAM_TIERS[get_ram_tier_index(ram_bytes)]["label"]


def add_ram_colorbar(fig, ax):
    """Attach a clean discrete 5-tier colorbar for RAM usage."""
    colors = [t["color"] for t in RAM_TIERS]
    cmap = mcolors.ListedColormap(colors)
    bounds = [0, 1, 2, 3, 4, 5]
    norm = mcolors.BoundaryNorm(bounds, cmap.N)

    sm = plt.cm.ScalarMappable(cmap=cmap, norm=norm)
    sm.set_array([])

    cbar = fig.colorbar(sm, ax=ax, ticks=[0.5, 1.5, 2.5, 3.5, 4.5], pad=0.02, aspect=28)
    cbar.ax.set_yticklabels([t["label"] for t in RAM_TIERS], fontsize=9, fontweight="medium")
    cbar.set_label("Total RAM (stack + bss + data)", fontsize=10, fontweight="bold", labelpad=10)
    cbar.outline.set_edgecolor("#d1d5db")
    cbar.outline.set_linewidth(0.8)
    return cbar


def load_records(results_path: Path, dataset: str, slice_idx: int) -> list[dict]:
    """
    Load records from results.json for a specific dataset (or 'ALL') and slice_idx (0..8).
    Computes enriched fields:
      - ratio_pct: compression ratio in % (lower is better)
      - throughput: decompression throughput in Bytes / 1k cycles (higher is better)
      - total_ram: stack + bss + data in bytes
      - flash_text: text size in bytes
    """
    if slice_idx < 0 or slice_idx > 8:
        raise ValueError(f"slice index must be between 0 and 8 (got {slice_idx})")

    with open(results_path, "r") as f:
        raw_data = json.load(f)

    num_iters = detect_num_iters()
    slice_bytes = 1 << slice_idx
    total_decoded_bytes = num_iters * slice_bytes

    passed = [r for r in raw_data if r.get("status") == "PASS"]
    if not passed:
        raise ValueError(f"No passing benchmark records found in {results_path}")

    if dataset.upper() == "ALL":
        by_cfg = {}
        for r in passed:
            by_cfg.setdefault(r["config_name"], []).append(r)

        records = []
        for cfg_name, runs in by_cfg.items():
            first = runs[0]
            ratios_pct = [r["compression_ratio"] * 100.0 for r in runs]
            throughputs = [
                (total_decoded_bytes / r["cycles"][slice_idx]) * 1000.0
                for r in runs
                if r["cycles"][slice_idx] > 0
            ]
            rams = [get_total_ram(r) for r in runs]

            mean_ratio_pct = float(np.mean(ratios_pct))
            geomean_tp = float(np.exp(np.mean(np.log(throughputs))))
            max_ram = int(max(rams))

            records.append(
                {
                    "decompressor": first["decompressor"],
                    "config_name": cfg_name,
                    "dataset": f"ALL ({len(runs)} files)",
                    "uncompressed_size": sum(r["uncompressed_size"] for r in runs),
                    "compressed_size": sum(r["compressed_size"] for r in runs),
                    "compression_ratio": mean_ratio_pct / 100.0,
                    "ratio_pct": mean_ratio_pct,
                    "cycles": int(np.mean([r["cycles"][slice_idx] for r in runs])),
                    "throughput": geomean_tp,
                    "total_ram": max_ram,
                    "peak_stack": max(r.get("peak_stack", 0) for r in runs),
                    "bss": first.get("with_adapter_size", {}).get("bss", 0),
                    "data": first.get("with_adapter_size", {}).get("data", 0),
                    "flash_text": first.get("with_adapter_size", {}).get("text", 0),
                    "ram_tier": get_ram_tier_index(max_ram),
                    "ram_color": get_ram_color(max_ram),
                    "ram_label": get_ram_tier_label(max_ram),
                }
            )
        return records

    filtered = [r for r in passed if r.get("dataset") == dataset]
    if not filtered:
        available = sorted({r.get("dataset") for r in passed})
        raise ValueError(
            f"Dataset '{dataset}' not found in {results_path}. "
            f"Available datasets: {', '.join(available)} (or 'ALL')"
        )

    records = []
    for r in filtered:
        cyc = r["cycles"][slice_idx]
        tp = (total_decoded_bytes / cyc) * 1000.0 if cyc > 0 else 0.0
        tot_ram = get_total_ram(r)
        was = r.get("with_adapter_size", {})
        records.append(
            {
                "decompressor": r["decompressor"],
                "config_name": r["config_name"],
                "dataset": r["dataset"],
                "uncompressed_size": r["uncompressed_size"],
                "compressed_size": r["compressed_size"],
                "compression_ratio": r["compression_ratio"],
                "ratio_pct": r["compression_ratio"] * 100.0,
                "cycles": cyc,
                "throughput": tp,
                "total_ram": tot_ram,
                "peak_stack": r.get("peak_stack", 0),
                "bss": was.get("bss", 0),
                "data": was.get("data", 0),
                "flash_text": was.get("text", 0),
                "ram_tier": get_ram_tier_index(tot_ram),
                "ram_color": get_ram_color(tot_ram),
                "ram_label": get_ram_tier_label(tot_ram),
            }
        )
    return records


def compute_pareto(records: list[dict]) -> list[dict]:
    """
    Compute Pareto-optimal records minimizing ratio_pct and maximizing throughput.
    Returns frontier sorted by ascending ratio_pct.
    """
    sorted_recs = sorted(records, key=lambda r: (r["ratio_pct"], -r["throughput"]))
    frontier = []
    max_tp = -1.0
    for r in sorted_recs:
        if r["throughput"] > max_tp:
            frontier.append(r)
            max_tp = r["throughput"]
    return frontier


def style_axes(ax, title: str, subtitle: str = ""):
    """Apply clean, consistent styling and log-scale formatting to axes."""
    ax.set_yscale("log")
    ax.yaxis.set_major_formatter(
        ticker.FuncFormatter(
            lambda y, _: f"{y:.4g}" if y < 1 else f"{y:g}"
        )
    )
    ax.xaxis.set_major_formatter(ticker.PercentFormatter(decimals=0))

    ax.grid(True, which="major", linestyle="-", linewidth=0.6, color="#e5e7eb", zorder=0)
    ax.grid(True, which="minor", linestyle=":", linewidth=0.4, color="#f3f4f6", zorder=0)
    ax.set_axisbelow(True)

    for spine in ["top", "right"]:
        ax.spines[spine].set_visible(False)
    for spine in ["left", "bottom"]:
        ax.spines[spine].set_color("#9ca3af")

    ax.set_xlabel(
        "Compression Ratio (%) [Compressed / Uncompressed]  ← Lower is Better",
        fontsize=10.5,
        fontweight="bold",
        labelpad=8,
    )
    ax.set_ylabel(
        "Decompression Throughput (B / 1k cycles, log)  ↑ Higher is Better",
        fontsize=10.5,
        fontweight="bold",
        labelpad=8,
    )

    if subtitle:
        ax.set_title(f"{title}\n{subtitle}", fontsize=10.5, fontweight="bold", pad=10)
    else:
        ax.set_title(title, fontsize=11.5, fontweight="bold", pad=10)


def attach_hover_tooltip(fig, ax, scatter_groups: list[tuple], frontier_names: set[str]):
    """
    Attach an interactive mouse-hover tooltip to the figure for inspecting points.
    scatter_groups is a list of (PathCollection, list[dict]) pairs.
    """
    annot = ax.annotate(
        "",
        xy=(0, 0),
        xytext=(14, 14),
        textcoords="offset points",
        fontsize=8.5,
        fontweight="bold",
        color="#0f172a",
        bbox=dict(boxstyle="round,pad=0.35", fc="#ffffffee", ec="#475569", lw=1.2),
        zorder=50,
    )
    annot.set_visible(False)

    def format_record(r: dict) -> str:
        is_opt = " ★ PARETO OPTIMAL" if r["config_name"] in frontier_names else ""
        return (
            f"{r['config_name']} ({r['decompressor']}){is_opt}\n"
            f"Ratio      : {r['ratio_pct']:.2f}% ({r['compressed_size']:,} / {r['uncompressed_size']:,} B)\n"
            f"Throughput : {r['throughput']:.4f} B/kcyc ({r['cycles']:,} cyc)\n"
            f"Total RAM  : {r['total_ram']:,} B [{r['ram_label']}] "
            f"(stk={r['peak_stack']}B, bss={r['bss']}B, data={r['data']}B)\n"
            f"Flash      : {r['flash_text']:,} B"
        )

    def on_move(event):
        if event.inaxes != ax:
            if annot.get_visible():
                annot.set_visible(False)
                fig.canvas.draw_idle()
            return

        for sc, recs in scatter_groups:
            cont, ind = sc.contains(event)
            if cont and len(ind["ind"]) > 0:
                idx = ind["ind"][0]
                rec = recs[idx]
                pos = sc.get_offsets()[idx]
                annot.xy = pos
                annot.set_text(format_record(rec))

                # Flip tooltip direction if near top or right edge of axes
                bbox = ax.get_window_extent()
                rel_x = (event.x - bbox.x0) / max(bbox.width, 1.0)
                rel_y = (event.y - bbox.y0) / max(bbox.height, 1.0)
                ox = -14 if rel_x > 0.65 else 14
                oy = -14 if rel_y > 0.72 else 14
                annot.set_ha("right" if rel_x > 0.65 else "left")
                annot.set_va("top" if rel_y > 0.72 else "bottom")
                annot.xyann = (ox, oy)

                annot.set_visible(True)
                fig.canvas.draw_idle()
                return

        if annot.get_visible():
            annot.set_visible(False)
            fig.canvas.draw_idle()

    fig.canvas.mpl_connect("motion_notify_event", on_move)
