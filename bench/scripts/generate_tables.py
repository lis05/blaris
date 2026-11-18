#!/usr/bin/env python3
"""
Generate plain-text per-file benchmark tables for terminal viewing
from bench/results-rv32emu.json based on the blaris preset levels (L1..L9).

Usage:
    python3 bench/scripts/generate_tables.py [path/to/results-rv32emu.json] [--dataset FILE] [--slice BYTES]
"""

import argparse
from generate_readme_tables import (
    DEFAULT_RESULTS_JSON,
    collect_table_data,
)


def format_plain_text(
    results_path: str,
    dataset_filter: str | None = None,
    slice_filter: int | None = None,
) -> str:
    """Format collected benchmark tables as fixed-width plain text for terminal viewing."""
    datasets_data = collect_table_data(results_path)
    if dataset_filter:
        datasets_data = [d for d in datasets_data if d["dataset"] == dataset_filter]

    out = []
    for ds_item in datasets_data:
        ds = ds_item["dataset"]
        uncomp_size = ds_item["uncompressed_size"]
        out.append(f"=== {ds} ({uncomp_size:,} B) ===")

        for sl_item in ds_item["slices"]:
            slice_bytes = sl_item["slice_bytes"]
            if slice_filter is not None and slice_bytes != slice_filter:
                continue

            rows = []
            for g in sl_item["groups"]:
                rows.append((
                    f"{g['level']} ({g['config_name']})",
                    g["ratio_str"],
                    g["cyc_str"],
                    g["vs_str"],
                    g["ram_str"],
                ))
                for b in g["baselines"]:
                    rows.append((
                        f"  {b['config_name']}",
                        b["ratio_str"],
                        b["cyc_str"],
                        b["vs_str"],
                        b["ram_str"],
                    ))
                rows.append(None)

            if rows and rows[-1] is None:
                rows.pop()

            headers = ("Configuration", "Ratio", "Decomp Cycles", "vs blaris", "RAM")
            widths = [
                max([len(headers[i])] + [len(r[i]) for r in rows if r is not None])
                for i in range(5)
            ]

            def fmt_row(cols: tuple[str, ...]) -> str:
                return (
                    f"{cols[0]:<{widths[0]}}  "
                    f"{cols[1]:>{widths[1]}}  "
                    f"{cols[2]:>{widths[2]}}  "
                    f"{cols[3]:>{widths[3]}}  "
                    f"{cols[4]:>{widths[4]}}"
                )

            sep = "-" * (sum(widths) + 8)
            out.append(f"\n--- {ds} — {slice_bytes} B Slices ---")
            out.append(fmt_row(headers))
            out.append(sep)
            for r in rows:
                if r is None:
                    out.append(sep)
                else:
                    out.append(fmt_row(r))

        out.append("")

    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(
        description="Print plain-text benchmark tables based on blaris levels (L1..L9)"
    )
    parser.add_argument(
        "results",
        nargs="?",
        default=str(DEFAULT_RESULTS_JSON),
        help="Path to results JSON (default: bench/results-rv32emu.json)",
    )
    parser.add_argument(
        "--dataset",
        default=None,
        help="Filter output to a single dataset filename (e.g. FreeRTOSConfig.h)",
    )
    parser.add_argument(
        "--slice",
        type=int,
        choices=[4, 32, 256],
        default=None,
        help="Filter output to a single slice size in bytes (4, 32, or 256)",
    )
    args = parser.parse_args()

    print(format_plain_text(args.results, args.dataset, args.slice))


if __name__ == "__main__":
    main()
