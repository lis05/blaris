import subprocess
import threading
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from config import (
    CC,
    CFLAGS,
    BENCH_DIR,
    BUILD_DIR,
)

TAMP_INCLUDE = BENCH_DIR / "third_party" / "tamp"
TAMP_COMMON = TAMP_INCLUDE / "common.c"
TAMP_DECOMPRESS = TAMP_INCLUDE / "decompressor.c"
ADAPTERS_DIR = BENCH_DIR / "adapters"
TAMP_ADAPTER = ADAPTERS_DIR / "tamp.c"
HOST_DIR = BENCH_DIR / "host"
TAMP_HOST_COMPRESS = HOST_DIR / "tamp.c"


def build(
    block_size,
    window_bits,
    extended,
):
    params = f"b{block_size}_w{window_bits}_e{extended}"
    test_dir = BUILD_DIR / f"tamp_{params}"
    test_dir.mkdir(parents=True, exist_ok=True)

    common_pure = test_dir / "common_pure.o"
    decomp_pure = test_dir / "decomp_pure.o"
    pure_build = test_dir / "pure.o"
    with_adapter_build = test_dir / "with_adapter.o"
    compressor_exe = test_dir / "compressor"

    config = [
        f"-DTAMP_BLOCK_POW2={block_size}",
        f"-DTAMP_WINDOW={window_bits}",
        f"-DTAMP_EXTENDED={extended}",
        "-DTAMP_LAZY_MATCHING=1",
        "-DTAMP_STREAM=0",
    ]

    subprocess.run(
        [
            CC,
            *CFLAGS,
            *config,
            f"-I{TAMP_INCLUDE}",
            "-c",
            str(TAMP_COMMON),
            "-o",
            str(common_pure),
        ],
        check=True,
        text=True,
    )
    subprocess.run(
        [
            CC,
            *CFLAGS,
            *config,
            f"-I{TAMP_INCLUDE}",
            "-c",
            str(TAMP_DECOMPRESS),
            "-o",
            str(decomp_pure),
        ],
        check=True,
        text=True,
    )
    subprocess.run(
        [
            "ld.lld",
            "-r",
            "--gc-sections",
            "-u",
            "tamp_decompressor_init",
            "-u",
            "tamp_decompressor_decompress_cb",
            "-u",
            "tamp_decompressor_read_header",
            str(common_pure),
            str(decomp_pure),
            "-o",
            str(pure_build),
        ],
        check=True,
        text=True,
    )
    common_pure.unlink(missing_ok=True)
    decomp_pure.unlink(missing_ok=True)

    subprocess.run(
        [
            CC,
            *CFLAGS,
            *config,
            f"-I{TAMP_INCLUDE}",
            f"-I{ADAPTERS_DIR}",
            "-c",
            str(TAMP_ADAPTER),
            "-o",
            str(with_adapter_build),
        ],
        check=True,
        text=True,
    )
    subprocess.run(
        [
            "ld.lld",
            "-r",
            "--gc-sections",
            "-u",
            "decompressor_init",
            "-u",
            "decompressor_slice",
            "-u",
            "decompressor_reset",
            str(with_adapter_build),
            "-o",
            str(with_adapter_build),
        ],
        check=True,
        text=True,
    )

    subprocess.run(
        [
            "clang",
            "-O2",
            *config,
            f"-I{TAMP_INCLUDE}",
            f"-I{HOST_DIR}",
            str(TAMP_HOST_COMPRESS),
            "-o",
            str(compressor_exe),
        ],
        check=True,
        text=True,
    )

    return os.path.basename(test_dir), test_dir, pure_build, with_adapter_build, config


def compress(subtest_dir, config=None, compressor_exe=None):
    subtest_dir = Path(subtest_dir)
    subtest_dir.mkdir(parents=True, exist_ok=True)
    if compressor_exe is None:
        compressor_exe = subtest_dir.parent / "compressor"
    if not compressor_exe.exists():
        subprocess.run(
            [
                "clang",
                "-O2",
                *(config or []),
                f"-I{TAMP_INCLUDE}",
                f"-I{HOST_DIR}",
                str(TAMP_HOST_COMPRESS),
                "-o",
                str(compressor_exe),
            ],
            check=True,
            text=True,
        )

    compressed_payload = subtest_dir / "compressed_payload.bin"
    subprocess.run(
        [
            str(compressor_exe),
            str(subtest_dir / "payload.bin"),
            str(compressed_payload),
        ],
        check=True,
        text=True,
    )

    return compressed_payload


def build_all(max_workers=None):
    block_sizes = [4, 6, 8, 10, 12, 14]
    window_options = [8, 10, 12, 14]
    extended_options = [0, 1]

    combinations = []
    for b in block_sizes:
        for w in window_options:
            if w > b and w > 8:
                continue
            for e in extended_options:
                combinations.append((b, w, e))

    total = len(combinations)
    done = 0
    lock = threading.Lock()

    def worker(args):
        nonlocal done
        pretty, test_dir, pure, adapted, config = build(*args)
        with lock:
            done += 1
            print(f"[{done}/{total}] Built {pretty}")
        return pretty, test_dir, pure, adapted, config

    with ThreadPoolExecutor(max_workers=max_workers) as executor:
        return list(executor.map(worker, combinations))
