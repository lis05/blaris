import argparse
import glob
import json
import os
import shutil
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from config import (
    CC,
    CFLAGS,
    BENCH_DIR,
    BUILD_DIR,
)
import blaris
import heatshrink
import tamp
import harness
import harness_esp32c3fn4

DATA_DIR = BENCH_DIR / "data"
RESULTS_JSON = BENCH_DIR / "results.json"


def extract_size_report(binary):
    res = subprocess.check_output(["llvm-size", "-B", str(binary)], text=True)
    res = list(map(int, res.split("\n")[1].split()[:3]))
    return {"text": res[0], "data": res[1], "bss": res[2]}


def run(max_workers=None, dataset_limit=None, device="rv32emu", module_filter=None, config_limit=None, config_filter=None, dataset_filter=None, exclude_filter=None, skip_completed=False):
    h = harness
    if device == "esp32":
        h = harness_esp32c3fn4
        max_workers = 1

    results_json = BENCH_DIR / f"results-{device}.json"

    if BUILD_DIR.exists():
        shutil.rmtree(BUILD_DIR)
    BUILD_DIR.mkdir(parents=True, exist_ok=True)

    dataset_files = sorted([p for p in DATA_DIR.iterdir() if p.is_file()])
    if dataset_filter:
        dataset_files = [p for p in dataset_files if dataset_filter in p.name]
    if exclude_filter:
        excludes = [p.strip() for p in exclude_filter.split(",")]
        dataset_files = [p for p in dataset_files if not any(e in p.name for e in excludes)]
    if dataset_limit is not None:
        dataset_files = dataset_files[:dataset_limit]

    print(f"Dataset consists of {len(dataset_files)} files:")
    for file in dataset_files:
        print(f"    {file.name}")

    h.build()

    results = []

    def get_existing_map():
        existing_map = {}
        if results_json.exists():
            try:
                with open(results_json) as f:
                    for r in json.load(f):
                        existing_map[(r["config_name"], r["dataset"])] = r
            except Exception:
                pass
        return existing_map

    def save_results(items):
        existing_map = get_existing_map()
        for r in items:
            existing_map[(r["config_name"], r["dataset"])] = r
        with open(results_json, "w") as f:
            json.dump(list(existing_map.values()), f, indent=2)

    def run_module(mod, name):
        if module_filter and name != module_filter:
            return []

        mod_results = []
        mod_configs = mod.build_all(max_workers=max_workers)
        if config_filter:
            patterns = [p.strip() for p in config_filter.split(",")]
            mod_configs = [c for c in mod_configs if any(p in c[0] for p in patterns)]
        if config_limit is not None:
            mod_configs = mod_configs[:config_limit]

        sizes = {}
        for entry in mod_configs:
            pretty = entry[0]
            pure = entry[2]
            with_adapter = entry[3]
            sizes[pretty] = {
                "pure": extract_size_report(pure),
                "with_adapter": extract_size_report(with_adapter),
            }

        tasks = [
            (config_entry, datafile)
            for config_entry in mod_configs
            for datafile in dataset_files
        ]
        total = len(tasks)
        done = 0
        passed = 0
        failed = 0
        lock = threading.Lock()

        def worker(task):
            nonlocal done, passed, failed
            (pretty, test_dir, pure, with_adapter, config), datafile = task
            subtest_name = f"dataset_{datafile.name}"

            if skip_completed:
                existing_entry = get_existing_map().get((pretty, datafile.name))
                if existing_entry and existing_entry.get("status") == "PASS":
                    with lock:
                        done += 1
                        passed += 1
                        mod_results.append(existing_entry)
                        print(f"[{done}/{total}] Skipping {pretty} on {subtest_name} (already PASS)", flush=True)
                    return

            subtest_dir = test_dir / subtest_name
            subtest_dir.mkdir(parents=True, exist_ok=True)

            payload_path = subtest_dir / "payload.bin"
            shutil.copy2(datafile, payload_path)
            uncompressed_payload_size = payload_path.stat().st_size

            compressed_path = mod.compress(subtest_dir, config)
            compressed_payload_size = compressed_path.stat().st_size

            h.build_with_payload(subtest_dir)
            h.link(subtest_dir)

            print(f"[{done+1}/{total}] Running {pretty} on {subtest_name}...", flush=True)
            peak_stack, cycles, checksums = h.run(subtest_dir)
            expected_checksums = h.verify(subtest_dir)
            status = "PASS" if checksums == expected_checksums else "FAIL"

            compression_ratio = (
                compressed_payload_size / uncompressed_payload_size
                if uncompressed_payload_size > 0
                else 0.0
            )

            item = {
                "decompressor": name,
                "config_name": pretty,
                "dataset": datafile.name,
                "uncompressed_size": uncompressed_payload_size,
                "compressed_size": compressed_payload_size,
                "compression_ratio": compression_ratio,
                "peak_stack": peak_stack,
                "cycles": cycles,
                "checksums": checksums,
                "status": status,
                "pure_size": sizes[pretty]["pure"],
                "with_adapter_size": sizes[pretty]["with_adapter"],
            }

            with lock:
                done += 1
                if status == "PASS":
                    passed += 1
                else:
                    failed += 1
                mod_results.append(item)
                save_results([item])

                print(f"[{done}/{total}] {pretty} on {subtest_name} - {status}", flush=True)
                print(
                    f"    Compressed {uncompressed_payload_size} -> {compressed_payload_size} ({100.0 * compression_ratio:.2f}%)"
                )
                print(f"    flash: {sizes[pretty]['with_adapter']['text']}")
                print(f"    stack: {peak_stack}")
                print(f"    mem: {sizes[pretty]['with_adapter']['data'] + sizes[pretty]['with_adapter']['bss']}")

        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            list(executor.map(worker, tasks))

        print(f"\n[{name}] Completed: {done}/{total} (Passed: {passed}, Failed: {failed})")
        return mod_results

    results.extend(run_module(tamp, "tamp"))
    results.extend(run_module(heatshrink, "heatshrink"))
    results.extend(run_module(blaris, "blaris"))

    save_results(results)

    total_runs = len(results)
    total_passed = sum(1 for r in results if r["status"] == "PASS")
    total_failed = sum(1 for r in results if r["status"] != "PASS")

    print("\n" + "=" * 60)
    print(f"Benchmark finished: {total_runs} total runs completed.")
    print(f"Passed: {total_passed}, Failed: {total_failed}")
    print(f"Saved results to {results_json}")
    print("=" * 60)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run Blaris benchmarks")
    parser.add_argument("--limit", type=int, default=None, help="Limit number of dataset files")
    parser.add_argument("--dataset", default=None, help="Filter to specific dataset file (e.g. blink or test.dtb)")
    parser.add_argument("-j", "--jobs", type=int, default=None, help="Max parallel worker threads")
    parser.add_argument("--device", default="rv32emu", choices=["rv32emu", "esp32"], help="Device to run on")
    parser.add_argument("--module", default=None, help="Limit to specific module (e.g. blaris)")
    parser.add_argument("--config", default=None, help="Filter to specific configuration name (e.g. blaris_o2_a0_s6_f0)")
    parser.add_argument("--config-limit", type=int, default=None, help="Limit number of configurations per module")
    parser.add_argument("--exclude", default=None, help="Filter to exclude specific dataset files (e.g. add,fence)")
    parser.add_argument("--skip-completed", action="store_true", help="Skip runs already present and passing in results JSON")
    args = parser.parse_args()
    run(
        max_workers=args.jobs,
        dataset_limit=args.limit,
        device=args.device,
        module_filter=args.module,
        config_limit=args.config_limit,
        config_filter=args.config,
        dataset_filter=args.dataset,
        exclude_filter=args.exclude,
        skip_completed=args.skip_completed,
    )
