#!/usr/bin/env python3
"""Self-contained RV32IMC performance benchmark & comparator for blaris."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

PERF_DIR = Path(__file__).resolve().parent
TOP_DIR = PERF_DIR.parent
BUILD_DIR = PERF_DIR / "build"
LIB_DIR = PERF_DIR / "lib"
INCLUDE_DIR = TOP_DIR / "include"
SRC_DIR = TOP_DIR / "src"
DATA_DIR = TOP_DIR / "bench" / "data"

HARNESS_C = PERF_DIR / "harness.c"
ADAPTER_C = PERF_DIR / "adapter.c"
COMPRESSOR_C = PERF_DIR / "compressor.c"
LINKER_LD = PERF_DIR / "linker.ld"

RV_CC = "clang"
HOST_CC = "gcc"
TARGET_FLAGS = [
    "--target=riscv32-unknown-elf",
    "-march=rv32imc",
    "-mabi=ilp32",
]
RV_BASE_CFLAGS = [
    *TARGET_FLAGS,
    "-ffreestanding",
    "-fno-builtin",
    "-ffunction-sections",
    "-fdata-sections",
    "-mllvm",
    "-disable-loop-idiom-all",
    f"-I{LIB_DIR}",
]

OPTS = ["-Os", "-O2", "-O3"]
LEVELS = list(range(1, 10))
SLICE_SIZES = [4, 32, 256]
FULL_ITERS = 3
RA_ITERS = 1000


def fnv1a(hash_val: int, data: bytes) -> int:
    for b in data:
        hash_val = ((hash_val ^ b) * 16777619) & 0xFFFFFFFF
    return hash_val


class RNG:
    def __init__(self, seed: int = 2026):
        self.state = seed

    def rand(self) -> int:
        self.state = (self.state * 1103515245 + 12345) & 0xFFFFFFFF
        return (self.state >> 16) & 0x7FFF

    def rand_offset(self) -> int:
        return (self.rand() << 15) | self.rand()


def compute_expected_checksums(dataset_path: Path):
    raw_data = dataset_path.read_bytes()
    uncomp_size = len(raw_data)

    pass_chk = fnv1a(2166136261, raw_data)
    full_chk = 2166136261
    for _ in range(FULL_ITERS):
        full_chk = fnv1a(full_chk, pass_chk.to_bytes(4, "little"))

    rng = RNG(2026)
    ra_chks = {}
    for slice_size in SLICE_SIZES:
        max_offset = (uncomp_size - slice_size) if uncomp_size >= slice_size else 0
        chk = 2166136261
        for _ in range(RA_ITERS):
            offset = (rng.rand_offset() % (max_offset + 1)) if max_offset > 0 else 0
            window_chk = fnv1a(2166136261, raw_data[offset : offset + slice_size])
            chk = fnv1a(chk, window_chk.to_bytes(4, "little"))
        ra_chks[slice_size] = chk

    return uncomp_size, full_chk, ra_chks


def extract_size_report(obj_path: Path) -> dict:
    out = subprocess.check_output(["llvm-size", "-B", str(obj_path)], text=True)
    parts = list(map(int, out.splitlines()[1].split()[:3]))
    return {"text": parts[0], "data": parts[1], "bss": parts[2]}


def get_git_rev() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], cwd=TOP_DIR, text=True
        ).strip()
    except Exception:
        return "unknown"


def build_harness() -> Path:
    harness_o = BUILD_DIR / "harness.o"
    subprocess.check_call(
        [RV_CC, *RV_BASE_CFLAGS, "-Os", "-c", str(HARNESS_C), "-o", str(harness_o)]
    )
    return harness_o


def build_compressor(level: int, offset_size: int) -> Path:
    comp_bin = BUILD_DIR / f"compressor_L{level}"
    subprocess.check_call(
        [
            HOST_CC,
            "-O3",
            f"-I{INCLUDE_DIR}",
            f"-DBLARIS_LEVEL={level}",
            f"-DBLARIS_STREAM_OFFSET_SIZE={offset_size}",
            str(COMPRESSOR_C),
            str(SRC_DIR / "blaris-compress.c"),
            "-o",
            str(comp_bin),
        ]
    )
    return comp_bin


def compress_and_pack(level: int, comp_bin: Path, dataset_path: Path, raw_size: int):
    ds_dir = BUILD_DIR / "payloads" / f"L{level}" / dataset_path.name
    ds_dir.mkdir(parents=True, exist_ok=True)
    comp_file = ds_dir / "compressed.bin"
    subprocess.check_call([str(comp_bin), str(dataset_path), str(comp_file)])
    comp_size = comp_file.stat().st_size

    asm_file = ds_dir / "payload.s"
    obj_file = ds_dir / "payload.o"
    asm_file.write_text(
        f"""\
    .section .rodata
    .balign 4
    .global uncompressed_payload_size
uncompressed_payload_size:
    .word {raw_size}

    .section .payload, "a", @progbits
    .incbin "{comp_file}"
"""
    )
    subprocess.check_call([RV_CC, *TARGET_FLAGS, "-c", str(asm_file), "-o", str(obj_file)])
    return comp_size, obj_file


def build_decoder(opt: str, level: int, offset_size: int):
    out_dir = BUILD_DIR / "decoders" / f"{opt.lstrip('-')}_L{level}"
    out_dir.mkdir(parents=True, exist_ok=True)

    common = [
        RV_CC,
        *RV_BASE_CFLAGS,
        opt,
        f"-I{INCLUDE_DIR}",
        f"-DBLARIS_LEVEL={level}",
        f"-DBLARIS_STREAM_OFFSET_SIZE={offset_size}",
    ]
    pure_o = out_dir / "pure.o"
    adapter_o = out_dir / "adapter.o"
    subprocess.check_call([*common, "-c", str(SRC_DIR / "blaris-decompress.c"), "-o", str(pure_o)])
    subprocess.check_call([*common, "-c", str(ADAPTER_C), "-o", str(adapter_o)])
    pure_sizes = extract_size_report(pure_o)
    adapter_sizes = extract_size_report(adapter_o)
    return pure_sizes, adapter_sizes, adapter_o


def link_and_run(
    opt: str,
    level: int,
    dataset_name: str,
    harness_o: Path,
    payload_o: Path,
    adapter_o: Path,
    expected_full_chk: int,
    expected_ra_chks: dict,
) -> dict:
    run_dir = BUILD_DIR / "runs" / f"{opt.lstrip('-')}_L{level}"
    run_dir.mkdir(parents=True, exist_ok=True)
    elf_path = run_dir / f"{dataset_name}.elf"

    subprocess.check_call(
        [
            RV_CC,
            *TARGET_FLAGS,
            "-nostdlib",
            "-fuse-ld=lld",
            "-Wl,--gc-sections",
            "-T",
            str(LINKER_LD),
            str(harness_o),
            str(payload_o),
            str(adapter_o),
            "-o",
            str(elf_path),
        ]
    )

    res = subprocess.run(["rv32emu", str(elf_path)], capture_output=True, text=True, check=True)
    lines = [int(line.strip()) for line in res.stdout.splitlines() if line.strip()]
    expected_lines = 3 + 2 * len(SLICE_SIZES)
    if len(lines) != expected_lines:
        raise RuntimeError(
            f"Unexpected rv32emu output for {opt} L{level} {dataset_name}: got {len(lines)} lines"
        )

    peak_stack = lines[0]
    full_cycles_sum = lines[1]
    full_chk = lines[2]
    if full_chk != expected_full_chk:
        raise RuntimeError(
            f"Full-file checksum mismatch for {opt} L{level} {dataset_name}: "
            f"got {full_chk:#x}, expected {expected_full_chk:#x}"
        )

    ra_cycles = {}
    for idx, s in enumerate(SLICE_SIZES):
        cyc = lines[3 + 2 * idx]
        chk = lines[4 + 2 * idx]
        if chk != expected_ra_chks[s]:
            raise RuntimeError(
                f"Random-access ({s} B) checksum mismatch for {opt} L{level} {dataset_name}: "
                f"got {chk:#x}, expected {expected_ra_chks[s]:#x}"
            )
        ra_cycles[str(s)] = cyc

    return {
        "peak_stack": peak_stack,
        "full_cycles_sum": full_cycles_sum,
        "ra_cycles": ra_cycles,
    }


def run_benchmark(offset_size: int, max_workers: int) -> dict:
    t0 = time.monotonic()
    datasets = sorted([p for p in DATA_DIR.iterdir() if p.is_file()])
    if not datasets:
        raise RuntimeError(f"No dataset files found in {DATA_DIR}")

    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        # 1. Build harness, host compressors, and compute expected checksums in parallel
        harness_fut = pool.submit(build_harness)
        comp_futs = {lvl: pool.submit(build_compressor, lvl, offset_size) for lvl in LEVELS}
        chk_futs = {ds.name: pool.submit(compute_expected_checksums, ds) for ds in datasets}

        harness_o = harness_fut.result()
        comp_bins = {lvl: fut.result() for lvl, fut in comp_futs.items()}
        expected = {name: fut.result() for name, fut in chk_futs.items()}

        # 2. Compress & pack payloads per (level, dataset) and build RV32IMC decoders per (opt, level)
        payload_futs = {
            (lvl, ds.name): pool.submit(
                compress_and_pack, lvl, comp_bins[lvl], ds, expected[ds.name][0]
            )
            for lvl in LEVELS
            for ds in datasets
        }
        decoder_futs = {
            (opt, lvl): pool.submit(build_decoder, opt, lvl, offset_size)
            for opt in OPTS
            for lvl in LEVELS
        }

        payloads = {k: fut.result() for k, fut in payload_futs.items()}
        decoders = {k: fut.result() for k, fut in decoder_futs.items()}

        # 3. Link & run rv32emu for all (opt, level, dataset)
        run_futs = {}
        for opt in OPTS:
            for lvl in LEVELS:
                _, _, adapter_o = decoders[(opt, lvl)]
                for ds in datasets:
                    _, payload_o = payloads[(lvl, ds.name)]
                    _, exp_full, exp_ra = expected[ds.name]
                    run_futs[(opt, lvl, ds.name)] = pool.submit(
                        link_and_run,
                        opt,
                        lvl,
                        ds.name,
                        harness_o,
                        payload_o,
                        adapter_o,
                        exp_full,
                        exp_ra,
                    )

        raw_runs = {k: fut.result() for k, fut in run_futs.items()}

    elapsed = time.monotonic() - t0
    raw_total = sum(expected[ds.name][0] for ds in datasets)

    summary = {}
    files = {}
    for opt in OPTS:
        summary[opt] = {}
        files[opt] = {}
        for lvl in LEVELS:
            lvl_key = f"L{lvl}"
            pure_sizes, adapter_sizes, _ = decoders[(opt, lvl)]
            comp_total = sum(payloads[(lvl, ds.name)][0] for ds in datasets)
            max_stack = max(raw_runs[(opt, lvl, ds.name)]["peak_stack"] for ds in datasets)
            static_ram = adapter_sizes["data"] + adapter_sizes["bss"]
            full_sum_total = sum(
                raw_runs[(opt, lvl, ds.name)]["full_cycles_sum"] for ds in datasets
            )
            ra_avg = {}
            for s in SLICE_SIZES:
                sk = str(s)
                ra_total = sum(raw_runs[(opt, lvl, ds.name)]["ra_cycles"][sk] for ds in datasets)
                ra_avg[sk] = ra_total / (RA_ITERS * len(datasets))

            summary[opt][lvl_key] = {
                "raw_bytes": raw_total,
                "comp_bytes": comp_total,
                "ratio_pct": 100.0 * comp_total / raw_total,
                "flash_bytes": pure_sizes["text"],
                "data_bytes": adapter_sizes["data"],
                "bss_bytes": adapter_sizes["bss"],
                "static_ram_bytes": static_ram,
                "stack_bytes": max_stack,
                "ram_bytes": max_stack + static_ram,
                "full_cycles_sum": full_sum_total,
                "full_cycles": full_sum_total / FULL_ITERS,
                "ra_cycles": ra_avg,
            }

            files[opt][lvl_key] = {
                ds.name: {
                    "raw_bytes": expected[ds.name][0],
                    "comp_bytes": payloads[(lvl, ds.name)][0],
                    **raw_runs[(opt, lvl, ds.name)],
                }
                for ds in datasets
            }

    return {
        "meta": {
            "git_rev": get_git_rev(),
            "timestamp": datetime.now(timezone.utc).isoformat(),
            "offset_size": offset_size,
            "opts": OPTS,
            "levels": [f"L{lvl}" for lvl in LEVELS],
            "datasets": [ds.name for ds in datasets],
            "raw_bytes": raw_total,
            "full_iters": FULL_ITERS,
            "ra_iters": RA_ITERS,
            "slice_sizes": SLICE_SIZES,
            "elapsed_sec": round(elapsed, 2),
        },
        "summary": summary,
        "files": files,
    }


def format_table(headers: list[str], rows: list[list[str]], markdown: bool = False) -> str:
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            if len(cell) > widths[i]:
                widths[i] = len(cell)

    def fmt_row(cells: list[str]) -> str:
        formatted = [
            cells[0].ljust(widths[0]),
            *[cells[i].rjust(widths[i]) for i in range(1, len(cells))],
        ]
        if markdown:
            return "| " + " | ".join(formatted) + " |"
        return " | ".join(formatted)

    lines = [fmt_row(headers)]
    if markdown:
        sep = ["-" * widths[0], *["-" * max(1, widths[i] - 1) + ":" for i in range(1, len(widths))]]
        lines.append("| " + " | ".join(sep) + " |")
    else:
        lines.append("-+-".join("-" * w for w in widths))
    for row in rows:
        lines.append(fmt_row(row))
    return "\n".join(lines)


def print_results(data: dict, markdown: bool = False):
    meta = data["meta"]
    headers = [
        "Level",
        "Ratio (%)",
        "Flash (B)",
        "RAM (B)",
        "Full (cyc)",
        "RA-4 (cyc)",
        "RA-32 (cyc)",
        "RA-256 (cyc)",
    ]
    print(
        f"Blaris RV32IMC Perf (git {meta['git_rev']}, "
        f"offset_size={meta['offset_size']}, "
        f"{len(meta['datasets'])} files, {meta['raw_bytes']:,} B raw, "
        f"{meta['elapsed_sec']:.2f}s)"
    )
    for opt in meta["opts"]:
        print()
        if markdown:
            print(f"### `{opt}`")
        else:
            print(f"=== {opt} ===")
        rows = []
        for lvl in meta["levels"]:
            s = data["summary"][opt][lvl]
            rows.append(
                [
                    lvl,
                    f"{s['ratio_pct']:.2f}%",
                    f"{s['flash_bytes']}",
                    f"{s['ram_bytes']}",
                    f"{s['full_cycles']:,.0f}",
                    f"{s['ra_cycles']['4']:,.1f}",
                    f"{s['ra_cycles']['32']:,.1f}",
                    f"{s['ra_cycles']['256']:,.1f}",
                ]
            )
        print(format_table(headers, rows, markdown=markdown))


def fmt_pct_delta(base: float, new: float) -> str:
    if base == 0:
        return "n/a"
    pct = 100.0 * (new - base) / base
    return f"{pct:+.2f}%"


def fmt_int_delta(base: int, new: int) -> str:
    diff = new - base
    if diff == 0:
        return "0 B"
    pct = 100.0 * diff / base if base else 0.0
    return f"{diff:+d} B ({pct:+.1f}%)"


def print_comparison(base_data: dict, new_data: dict, base_label: str, new_label: str, markdown: bool = False):
    headers = ["Level", "Ratio", "Flash", "RAM", "Full", "RA-4", "RA-32", "RA-256"]
    print(
        f"Comparison: {base_label} ({base_data['meta']['git_rev']}) -> "
        f"{new_label} ({new_data['meta']['git_rev']})"
    )
    opts = [o for o in base_data["meta"]["opts"] if o in new_data["summary"]]
    levels = [l for l in base_data["meta"]["levels"] if l in new_data["meta"]["levels"]]

    for opt in opts:
        print()
        if markdown:
            print(f"### `{opt}`")
        else:
            print(f"=== {opt} ===")
        rows = []
        for lvl in levels:
            b = base_data["summary"][opt][lvl]
            n = new_data["summary"][opt][lvl]
            ratio_diff = n["ratio_pct"] - b["ratio_pct"]
            rows.append(
                [
                    lvl,
                    f"{ratio_diff:+.2f}%",
                    fmt_int_delta(b["flash_bytes"], n["flash_bytes"]),
                    fmt_int_delta(b["ram_bytes"], n["ram_bytes"]),
                    fmt_pct_delta(b["full_cycles"], n["full_cycles"]),
                    fmt_pct_delta(b["ra_cycles"]["4"], n["ra_cycles"]["4"]),
                    fmt_pct_delta(b["ra_cycles"]["32"], n["ra_cycles"]["32"]),
                    fmt_pct_delta(b["ra_cycles"]["256"], n["ra_cycles"]["256"]),
                ]
            )
        print(format_table(headers, rows, markdown=markdown))


def main() -> int:
    parser = argparse.ArgumentParser(description="Blaris RV32IMC performance benchmark")
    parser.add_argument(
        "--offset-size",
        type=int,
        default=2,
        choices=[2, 3, 4],
        help="BLARIS_STREAM_OFFSET_SIZE (default: 2)",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=os.cpu_count() or 4,
        help="Parallel worker threads (default: CPU count)",
    )
    parser.add_argument("--save", type=Path, help="Save benchmark results to JSON file")
    parser.add_argument(
        "--compare",
        nargs=2,
        metavar=("BASE_JSON", "NEW_JSON"),
        type=Path,
        help="Compare two saved JSON files and print relative changes",
    )
    parser.add_argument(
        "--markdown",
        action="store_true",
        help="Format tables as GitHub Flavored Markdown",
    )
    args = parser.parse_args()

    if args.compare:
        base_path, new_path = args.compare
        base_data = json.loads(base_path.read_text())
        new_data = json.loads(new_path.read_text())
        print_comparison(base_data, new_data, base_path.name, new_path.name, markdown=args.markdown)
        return 0

    data = run_benchmark(offset_size=args.offset_size, max_workers=args.jobs)
    print_results(data, markdown=args.markdown)

    if args.save:
        args.save.parent.mkdir(parents=True, exist_ok=True)
        args.save.write_text(json.dumps(data, indent=2) + "\n")
        print(f"\nSaved results to {args.save}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
