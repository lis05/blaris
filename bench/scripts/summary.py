#!/usr/bin/env python3
"""
summary.py - Memory and Flash footprint analysis from results.json.

Analyzes bench/results.json and prints:
- Min / Max Flash (.text) usage (both pure and with-adapter)
- Min / Max RAM usage (peak_stack + bss + data)
- Detailed breakdown of Stack, BSS, and Data
- Specific configurations achieving min/max bounds

Usage:
    python3 bench/scripts/summary.py [--results RESULTS_JSON] [--detailed]
"""

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


def format_bytes(n: int) -> str:
    if n >= 1024 * 1024:
        return f"{n / (1024 * 1024):.2f} MiB ({n} B)"
    if n >= 1024:
        return f"{n / 1024:.2f} KiB ({n} B)"
    return f"{n} B"


def main():
    parser = argparse.ArgumentParser(
        description="Analyze Flash and RAM footprints from results.json"
    )
    parser.add_argument(
        "--results",
        type=Path,
        default=Path("results.json"),
        help="Path to results.json (default: results.json)",
    )
    parser.add_argument(
        "--detailed",
        action="store_true",
        help="Show per-configuration breakdown",
    )
    args = parser.parse_args()

    if not args.results.exists():
        print(f"Error: {args.results} not found.", file=sys.stderr)
        sys.exit(1)

    with open(args.results, "r", encoding="utf-8") as f:
        data = json.load(f)

    # Group by decompressor
    by_codec = defaultdict(list)
    for entry in data:
        if entry.get("status") == "PASS":
            by_codec[entry.get("decompressor")].append(entry)

    if not by_codec:
        print("No passing runs found in results.", file=sys.stderr)
        sys.exit(1)

    print("=" * 78)
    print(" DECOMPRESSOR FOOTPRINT SUMMARY (RAM & FLASH)")
    print("=" * 78)

    for codec in sorted(by_codec.keys()):
        runs = by_codec[codec]
        configs = sorted(list(set(r["config_name"] for r in runs)))

        # Unique configurations and their stats
        cfg_stats = {}
        for r in runs:
            cname = r["config_name"]
            if cname not in cfg_stats:
                pure_text = r.get("pure_size", {}).get("text", 0)
                adap_text = r.get("with_adapter_size", {}).get("text", 0)
                bss = r.get("with_adapter_size", {}).get("bss", 0)
                data_sec = r.get("with_adapter_size", {}).get("data", 0)
                cfg_stats[cname] = {
                    "pure_text": pure_text,
                    "adapter_text": adap_text,
                    "bss": bss,
                    "data": data_sec,
                    "stacks": [],
                    "total_rams": [],
                }
            stack = r.get("peak_stack", 0)
            bss = cfg_stats[cname]["bss"]
            data_sec = cfg_stats[cname]["data"]
            cfg_stats[cname]["stacks"].append(stack)
            cfg_stats[cname]["total_rams"].append(stack + bss + data_sec)

        # Aggregate across all runs of this codec
        all_adapter_flash = [r.get("with_adapter_size", {}).get("text", 0) for r in runs]
        all_pure_flash = [r.get("pure_size", {}).get("text", 0) for r in runs]
        all_stack = [r.get("peak_stack", 0) for r in runs]
        all_bss = [r.get("with_adapter_size", {}).get("bss", 0) for r in runs]
        all_data = [r.get("with_adapter_size", {}).get("data", 0) for r in runs]
        all_total_ram = [
            r.get("peak_stack", 0)
            + r.get("with_adapter_size", {}).get("bss", 0)
            + r.get("with_adapter_size", {}).get("data", 0)
            for r in runs
        ]

        min_total_ram = min(all_total_ram)
        max_total_ram = max(all_total_ram)
        min_flash = min(all_adapter_flash)
        max_flash = max(all_adapter_flash)

        # Identify which config gives min/max
        min_ram_cfgs = [c for c, s in cfg_stats.items() if min(s["total_rams"]) == min_total_ram]
        max_ram_cfgs = [c for c, s in cfg_stats.items() if max(s["total_rams"]) == max_total_ram]
        min_flash_cfgs = [c for c, s in cfg_stats.items() if s["adapter_text"] == min_flash]
        max_flash_cfgs = [c for c, s in cfg_stats.items() if s["adapter_text"] == max_flash]

        print(f"\n[{codec.upper()}]  ({len(configs)} configs, {len(runs)} benchmark runs)")
        print("-" * 78)
        print(f"  Flash (Adapter .text) : {min_flash:5d} B  .. {max_flash:5d} B")
        print(f"    - Min Flash config  : {min_flash_cfgs[0]} ({min_flash} B)")
        print(f"    - Max Flash config  : {max_flash_cfgs[0]} ({max_flash} B)")
        print(f"  Flash (Pure .text)    : {min(all_pure_flash):5d} B  .. {max(all_pure_flash):5d} B")
        print()
        print(f"  Total RAM (Stack+BSS) : {min_total_ram:5d} B  .. {max_total_ram:5d} B "
              f"({format_bytes(min_total_ram)} .. {format_bytes(max_total_ram)})")
        print(f"    - Min RAM config    : {min_ram_cfgs[0]} ({min_total_ram} B)")
        print(f"    - Max RAM config    : {max_ram_cfgs[0]} ({max_total_ram} B)")
        print(f"  RAM Breakdown:")
        print(f"    - Peak Stack        : {min(all_stack):5d} B  .. {max(all_stack):5d} B")
        print(f"    - BSS               : {min(all_bss):5d} B  .. {max(all_bss):5d} B")
        print(f"    - Data              : {min(all_data):5d} B  .. {max(all_data):5d} B")

        if args.detailed:
            print("\n  Per-Configuration Details:")
            print(f"  {'Config Name':<32} {'Flash':>8} {'Stack':>8} {'BSS':>8} {'Total RAM':>11}")
            print("  " + "-" * 70)
            for cname in sorted(cfg_stats.keys()):
                cs = cfg_stats[cname]
                print(
                    f"  {cname:<32} "
                    f"{cs['adapter_text']:>7}B "
                    f"{max(cs['stacks']):>7}B "
                    f"{cs['bss']:>7}B "
                    f"{max(cs['total_rams']):>10}B"
                )

    print("\n" + "=" * 78)


if __name__ == "__main__":
    main()
