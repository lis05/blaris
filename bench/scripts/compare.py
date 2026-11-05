#!/usr/bin/env python3
"""
Script 2: Compare all codecs on a given dataset and slice size, plotting the Pareto frontier.

Usage:
    # Single plot:
    python3 bench/scripts/compare.py <dataset> <slice_idx> [-o OUTPUT] [--results RESULTS_JSON] [--no-show]

    # Multi-graph tabbed & dockable window (--all):
    python3 bench/scripts/compare.py <dataset> --all        # All 9 slice sizes (s=0..8) for <dataset>
    python3 bench/scripts/compare.py --all <slice_idx>      # All datasets at <slice_idx>
    python3 bench/scripts/compare.py --all                  # All datasets (tabs) x all 9 slice sizes (dockable tabs)
"""

import argparse
import json
import sys
from pathlib import Path
import matplotlib.pyplot as plt
from matplotlib.figure import Figure
from matplotlib.lines import Line2D

from common import (
    DEFAULT_RESULTS_JSON,
    VISUAL_DIR,
    CODEC_MARKERS,
    FALLBACK_MARKERS,
    load_records,
    compute_pareto,
    add_ram_colorbar,
    style_axes,
    attach_hover_tooltip,
)


def get_codec_marker(codec: str, idx: int) -> str:
    if codec in CODEC_MARKERS:
        return CODEC_MARKERS[codec]
    return FALLBACK_MARKERS[idx % len(FALLBACK_MARKERS)]


def print_pareto_summary(dataset: str, slice_idx: int, all_records: list[dict], overall_frontier: list[dict]):
    slice_bytes = 1 << slice_idx
    print("=" * 98)
    print(
        f"OVERALL PARETO FRONTIER — Dataset: {dataset} | "
        f"Slice: 2^{slice_idx} = {slice_bytes} B ({len(overall_frontier)} / {len(all_records)} configs)"
    )
    print("=" * 98)
    header = (
        f"{'#':>2}  {'Codec':<12}  {'Configuration':<28}  {'Ratio (%)':>10}  {'Comp (B)':>9}  "
        f"{'Throughput (B/kcyc)':>20}  {'RAM (B)':>8}  {'RAM Tier':<12}"
    )
    print(header)
    print("-" * 98)
    for idx, r in enumerate(overall_frontier, start=1):
        print(
            f"{idx:>2}  {r['decompressor']:<12}  {r['config_name']:<28}  {r['ratio_pct']:>9.2f}%  "
            f"{r['compressed_size']:>9,}  {r['throughput']:>20.4f}  "
            f"{r['total_ram']:>8,}  {r['ram_label']:<12}"
        )
    print("=" * 98)


def populate_compare_axes(fig, ax, all_records: list[dict], dataset: str, slice_idx: int):
    overall_frontier = compute_pareto(all_records)
    fig.patch.set_facecolor("white")
    ax.set_facecolor("white")

    codecs = sorted({r["decompressor"] for r in all_records})
    legend_handles = []
    scatter_groups = []

    # 1. Plot all codecs with distinct marker shapes, colored by RAM tier
    for idx, codec in enumerate(codecs):
        sub = [r for r in all_records if r["decompressor"] == codec]
        marker = get_codec_marker(codec, idx)

        xs = [r["ratio_pct"] for r in sub]
        ys = [r["throughput"] for r in sub]
        colors = [r["ram_color"] for r in sub]

        sc = ax.scatter(
            xs,
            ys,
            c=colors,
            marker=marker,
            s=78,
            alpha=0.88,
            edgecolors="white",
            linewidths=0.6,
            zorder=4,
        )
        scatter_groups.append((sc, sub))

        legend_handles.append(
            Line2D(
                [0],
                [0],
                marker=marker,
                color="none",
                markerfacecolor="#475569",
                markeredgecolor="white",
                markersize=8.5,
                label=f"{codec} ({len(sub)} configs)",
            )
        )

    # 2. Draw Overall Pareto Frontier across all codecs
    px = [r["ratio_pct"] for r in overall_frontier]
    py = [r["throughput"] for r in overall_frontier]

    ax.plot(
        px,
        py,
        color="#7c3aed",
        linestyle="--",
        linewidth=2.0,
        alpha=0.9,
        zorder=5,
    )
    for r in overall_frontier:
        m_shape = get_codec_marker(r["decompressor"], codecs.index(r["decompressor"]))
        ax.scatter(
            [r["ratio_pct"]],
            [r["throughput"]],
            c=[r["ram_color"]],
            marker=m_shape,
            s=115,
            edgecolors="#7c3aed",
            linewidths=1.8,
            zorder=6,
        )

    legend_handles.append(
        Line2D(
            [0],
            [0],
            color="#7c3aed",
            linestyle="--",
            linewidth=2.0,
            marker="o",
            markerfacecolor="white",
            markeredgecolor="#7c3aed",
            markeredgewidth=1.8,
            markersize=7.5,
            label=f"Pareto Frontier ({len(overall_frontier)} optima)",
        )
    )

    # Uncompressed 100% reference line if near range
    all_xs = [r["ratio_pct"] for r in all_records]
    all_ys = [r["throughput"] for r in all_records]
    if max(all_xs) >= 95.0:
        ax.axvline(100.0, color="#9ca3af", linestyle="-.", linewidth=1.1, zorder=2)
        legend_handles.append(
            Line2D([0], [0], color="#9ca3af", linestyle="-.", linewidth=1.1, label="Uncompressed (100%)")
        )

    slice_bytes = 1 << slice_idx
    uncomp_size = all_records[0]["uncompressed_size"]
    best_ratio = min(all_xs)
    best_tp = max(all_ys)

    title = f"Codec Comparison ({', '.join(codecs)}) — Dataset: {dataset} ({uncomp_size:,} B)"
    subtitle = (
        f"Slice Size: 2^{slice_idx} = {slice_bytes} B  |  "
        f"Best Ratio: {best_ratio:.2f}%  |  Max Throughput: {best_tp:.4f} B/kcyc  |  "
        f"Pareto Frontier: {len(overall_frontier)}/{len(all_records)} configs"
    )
    style_axes(ax, title, subtitle)
    add_ram_colorbar(fig, ax)

    ax.legend(
        handles=legend_handles,
        loc="best",
        frameon=True,
        framealpha=0.94,
        facecolor="white",
        edgecolor="#d1d5db",
        fontsize=9.5,
    )
    fig.tight_layout()
    frontier_names = {r["config_name"] for r in overall_frontier}
    return scatter_groups, frontier_names, overall_frontier


def get_available_datasets(results_path: Path) -> list[str]:
    with open(results_path, "r") as f:
        raw_data = json.load(f)
    passed = [r for r in raw_data if r.get("status") == "PASS"]
    datasets = sorted({r.get("dataset") for r in passed if r.get("dataset")})
    return ["ALL"] + datasets


def launch_multi_window(results_path: Path, dataset_arg: str | None, slice_arg: int | None):
    from PyQt5.QtCore import Qt
    from PyQt5.QtWidgets import (
        QApplication,
        QDockWidget,
        QLabel,
        QMainWindow,
        QTabWidget,
        QVBoxLayout,
        QWidget,
    )
    from matplotlib.backends.backend_qtagg import FigureCanvasQTAgg, NavigationToolbar2QT

    class LazyGraphWidget(QWidget):
        """Renders its Matplotlib figure on first visibility so startup is instant."""

        def __init__(self, dataset: str, slice_idx: int, parent=None):
            super().__init__(parent)
            self.dataset = dataset
            self.slice_idx = slice_idx
            self.rendered = False
            self.layout = QVBoxLayout(self)
            self.layout.setContentsMargins(2, 2, 2, 2)
            self.layout.setSpacing(2)

        def ensure_rendered(self):
            if self.rendered:
                return
            self.rendered = True
            records = load_records(results_path, self.dataset, self.slice_idx)
            fig = Figure(figsize=(11.0, 6.6), dpi=115)
            ax = fig.add_subplot(111)
            scatter_groups, frontier_names, _ = populate_compare_axes(
                fig, ax, records, self.dataset, self.slice_idx
            )
            canvas = FigureCanvasQTAgg(fig)
            toolbar = NavigationToolbar2QT(canvas, self)
            attach_hover_tooltip(fig, ax, scatter_groups, frontier_names)
            self.layout.addWidget(toolbar)
            self.layout.addWidget(canvas, stretch=1)

        def showEvent(self, event):
            super().showEvent(event)
            self.ensure_rendered()

    def build_dock_area(items: list[tuple[str, str, int]], default_idx: int = 0) -> QMainWindow:
        """Create a QMainWindow where each (title, dataset, slice_idx) is a movable/tabifiable QDockWidget."""
        area = QMainWindow()
        area.setDockNestingEnabled(True)
        area.setTabPosition(Qt.AllDockWidgetAreas, QTabWidget.North)
        # Hide central widget so docks occupy 100% of the window and can be split in any direction
        central = QWidget()
        central.hide()
        area.setCentralWidget(central)

        docks = []
        for title, ds, s_idx in items:
            dock = QDockWidget(title, area)
            dock.setAllowedAreas(Qt.AllDockWidgetAreas)
            dock.setFeatures(
                QDockWidget.DockWidgetMovable | QDockWidget.DockWidgetFloatable
            )
            graph_widget = LazyGraphWidget(ds, s_idx, dock)
            dock.setWidget(graph_widget)
            area.addDockWidget(Qt.TopDockWidgetArea, dock)
            if docks:
                area.tabifyDockWidget(docks[0], dock)
            docks.append(dock)

        if docks:
            target = docks[min(default_idx, len(docks) - 1)]
            target.raise_()
            target.widget().ensure_rendered()

        return area

    app = QApplication.instance() or QApplication(sys.argv)
    main_win = QMainWindow()
    main_win.resize(1380, 860)

    if dataset_arg is not None and slice_arg is None:
        # Mode 1: Specific dataset, all 9 slice sizes (s=0..8) as dockable tabs
        main_win.setWindowTitle(
            f"Codec Comparison — Dataset: {dataset_arg} (All Slices s=0..8 | Drag tabs to split view)"
        )
        items = [(f"s={s} ({1 << s} B)", dataset_arg, s) for s in range(9)]
        dock_win = build_dock_area(items, default_idx=5)
        main_win.setCentralWidget(dock_win)

    elif dataset_arg is None and slice_arg is not None:
        # Mode 2: All datasets at a specific slice size as dockable tabs
        datasets = get_available_datasets(results_path)
        main_win.setWindowTitle(
            f"Codec Comparison — All Datasets @ Slice 2^{slice_arg} = {1 << slice_arg} B (Drag tabs to split view)"
        )
        items = [(ds, ds, slice_arg) for ds in datasets]
        dock_win = build_dock_area(items, default_idx=0)
        main_win.setCentralWidget(dock_win)

    else:
        # Mode 3: All datasets (top movable tabs) x all 9 slice sizes (inner dockable/splittable zones)
        datasets = get_available_datasets(results_path)
        main_win.setWindowTitle(
            "Codec Comparison — All Datasets × All Slice Sizes (Drag slice tabs to split side-by-side)"
        )
        ds_tabs = QTabWidget()
        ds_tabs.setMovable(True)
        ds_tabs.setDocumentMode(True)

        for ds in datasets:
            items = [(f"s={s} ({1 << s} B)", ds, s) for s in range(9)]
            dock_win = build_dock_area(items, default_idx=5)
            ds_tabs.addTab(dock_win, ds)

        main_win.setCentralWidget(ds_tabs)

    main_win.show()
    app.exec_()


def main():
    parser = argparse.ArgumentParser(
        description="Compare all codecs on a given dataset and slice size with Pareto frontier."
    )
    parser.add_argument(
        "pos1",
        nargs="?",
        default=None,
        help="Dataset filename (e.g. FreeRTOSConfig.h, or ALL), or slice index when used with --all",
    )
    parser.add_argument(
        "pos2",
        nargs="?",
        default=None,
        help="Slice index 0..8 (size = 2^slice bytes)",
    )
    parser.add_argument(
        "--all",
        action="store_true",
        help="Open multi-graph window with movable tabs / dockable split zones for all slices and/or datasets",
    )
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS_JSON, help="Path to results.json")
    parser.add_argument("-o", "--output", type=Path, default=None, help="Output PNG path")
    parser.add_argument("--no-show", action="store_true", help="Do not open interactive window")
    args = parser.parse_args()

    # Resolve positional arguments (dataset and slice_idx)
    dataset_arg = None
    slice_arg = None

    if args.pos1 is not None and args.pos2 is not None:
        dataset_arg = args.pos1
        try:
            slice_arg = int(args.pos2)
        except ValueError:
            parser.error(f"Slice index must be an integer 0..8, got '{args.pos2}'")
    elif args.pos1 is not None and args.pos2 is None:
        if args.pos1.isdigit() and 0 <= int(args.pos1) <= 8:
            slice_arg = int(args.pos1)
        else:
            dataset_arg = args.pos1

    if slice_arg is not None and not (0 <= slice_arg <= 8):
        parser.error(f"Slice index must be between 0 and 8 (got {slice_arg})")

    if args.all:
        launch_multi_window(args.results, dataset_arg, slice_arg)
        return

    if dataset_arg is None or slice_arg is None:
        parser.error("Provide <dataset> <slice_0_to_8>, or pass --all for multi-graph tabbed mode.")

    all_records = load_records(args.results, dataset_arg, slice_arg)
    fig, ax = plt.subplots(figsize=(11.5, 7), dpi=180)
    scatter_groups, frontier_names, overall_frontier = populate_compare_axes(
        fig, ax, all_records, dataset_arg, slice_arg
    )
    print_pareto_summary(dataset_arg, slice_arg, all_records, overall_frontier)

    out_path = args.output
    if out_path is None:
        safe_ds = dataset_arg.replace("/", "_")
        out_path = VISUAL_DIR / f"compare_{safe_ds}_s{slice_arg}.png"

    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=180)
    print(f"\nSaved plot to: {out_path}")

    if not args.no_show:
        attach_hover_tooltip(fig, ax, scatter_groups, frontier_names)
        plt.show()
    plt.close(fig)


if __name__ == "__main__":
    main()
