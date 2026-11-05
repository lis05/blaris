#!/usr/bin/env python3
"""
Compare cycle counts and metrics between ESP32-C3 hardware and rv32emu emulator
for blaris_o2_a0_s8_f1, heatshrink_b10_i16_w8_l4, and tamp_b10_w8_e1.
"""

import json
from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent.parent
ESP32_JSON = BENCH_DIR / "results-esp32.json"
RV32EMU_JSON = BENCH_DIR / "results-rv32emu.json"


def main():
    if not ESP32_JSON.exists():
        print(f"Error: {ESP32_JSON} not found.")
        return
    if not RV32EMU_JSON.exists():
        print(f"Error: {RV32EMU_JSON} not found.")
        return

    with open(ESP32_JSON) as f:
        esp_data = json.load(f)
    with open(RV32EMU_JSON) as f:
        emu_data = json.load(f)

    # Key by (config_name, dataset)
    esp_map = {(r["config_name"], r["dataset"]): r for r in esp_data if r.get("status") == "PASS"}
    emu_map = {(r["config_name"], r["dataset"]): r for r in emu_data if r.get("status") == "PASS"}

    configs = [
        ("blaris_o2_a0_s8_f1", "Blaris"),
        ("heatshrink_b10_i16_w8_l4", "Heatshrink"),
        ("tamp_b10_w8_e1", "Tamp"),
    ]

    for cfg_id, cfg_name in configs:
        print("\n" + "=" * 90)
        print(f"  Configuration: {cfg_name} ({cfg_id})")
        print("=" * 90)
        print(f"{'Dataset':<26} | {'rv32emu Cyc':>14} | {'ESP32 Cyc':>14} | {'Ratio (ESP/Emu)':>15} | {'Extra Cyc (%)':>13}")
        print("-" * 90)

        tot_emu = 0
        tot_esp = 0
        count = 0

        for key in sorted(emu_map.keys()):
            if key[0] != cfg_id:
                continue
            if key not in esp_map:
                continue

            dataset = key[1]
            emu_r = emu_map[key]
            esp_r = esp_map[key]

            emu_cyc = sum(emu_r["cycles"])
            esp_cyc = sum(esp_r["cycles"])

            tot_emu += emu_cyc
            tot_esp += esp_cyc
            count += 1

            overhead_pct = ((esp_cyc - emu_cyc) / emu_cyc) * 100.0 if emu_cyc > 0 else 0.0
            ratio = esp_cyc / emu_cyc if emu_cyc > 0 else 0.0

            print(f"{dataset:<26} | {emu_cyc:>14,} | {esp_cyc:>14,} | {ratio:>14.2f}x | {overhead_pct:>+12.1f}%")

        if count > 0:
            tot_ratio = tot_esp / tot_emu if tot_emu > 0 else 0.0
            tot_overhead = ((tot_esp - tot_emu) / tot_emu) * 100.0 if tot_emu > 0 else 0.0
            print("-" * 90)
            print(f"{'TOTAL (' + str(count) + ' files)':<26} | {tot_emu:>14,} | {tot_esp:>14,} | {tot_ratio:>14.2f}x | {tot_overhead:>+12.1f}%")
        else:
            print(f"No completed ESP32 results found yet for {cfg_id}.")

    # Summary by slice size for completed runs
    print("\n" + "=" * 90)
    print("  OVERHEAD BY SLICE SIZE (1 B to 256 B) - Geometric Mean across completed datasets")
    print("=" * 90)
    print(f"{'Codec':<12} | " + " | ".join(f"2^{s}={1<<s}B" for s in range(9)))
    print("-" * 90)

    for cfg_id, cfg_name in configs:
        common_keys = [k for k in emu_map if k[0] == cfg_id and k in esp_map]
        if not common_keys:
            continue
        slice_ratios = []
        for s in range(9):
            ratios = [esp_map[k]["cycles"][s] / emu_map[k]["cycles"][s] for k in common_keys if emu_map[k]["cycles"][s] > 0]
            avg_ratio = sum(ratios) / len(ratios) if ratios else 0.0
            slice_ratios.append(f"{avg_ratio:5.2f}x")
        print(f"{cfg_name:<12} | " + " | ".join(slice_ratios))


if __name__ == "__main__":
    main()
