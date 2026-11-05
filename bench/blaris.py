import subprocess
import itertools
import threading
import os
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from config import (
    CC,
    CFLAGS,
    BENCH_DIR,
    TOP_DIR,
    BUILD_DIR,
)

BLARIS_INCLUDE = TOP_DIR / "include"
BLARIS_DECOMPRESS = TOP_DIR / "src" / "blaris-decompress.c"
ADAPTERS_DIR = BENCH_DIR / "adapters"
BLARIS_ADAPTER = ADAPTERS_DIR / "blaris.c"
HOST_DIR = BENCH_DIR / "host"
BLARIS_HOST_COMPRESS = HOST_DIR / "blaris.c"


def build(
    stream_offset_size,
    distance_align_log,
    checkpoints_spread_log,
    flatten_chains,
):
    params = f"o{stream_offset_size}_a{distance_align_log}_s{checkpoints_spread_log}_f{flatten_chains}"
    test_dir = BUILD_DIR / f"blaris_{params}"
    test_dir.mkdir(parents=True, exist_ok=True)

    pure_build = test_dir / "pure.o"
    with_adapter_build = test_dir / "with_adapter.o"
    compressor_exe = test_dir / "compressor"

    config = [
        f"-DBLARIS_STREAM_OFFSET_SIZE={stream_offset_size}",
        f"-DBLARIS_DISTANCE_ALIGN_LOG={distance_align_log}",
        f"-DBLARIS_CHECKPOINTS_SPREAD_LOG={checkpoints_spread_log}",
        f"-DBLARIS_FLATTEN_CHAINS={flatten_chains}",
    ]

    subprocess.run(
        [
            CC,
            *CFLAGS,
            *config,
            f"-I{BLARIS_INCLUDE}",
            "-c",
            BLARIS_DECOMPRESS,
            "-o",
            pure_build,
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
            "blaris_init",
            "-u",
            "blaris_decompress",
            str(pure_build),
            "-o",
            str(pure_build),
        ],
        check=True,
        text=True,
    )

    subprocess.run(
        [
            CC,
            *CFLAGS,
            *config,
            f"-I{BLARIS_INCLUDE}",
            f"-I{ADAPTERS_DIR}",
            "-c",
            BLARIS_ADAPTER,
            "-o",
            with_adapter_build,
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
            f"-I{BLARIS_INCLUDE}",
            f"-I{HOST_DIR}",
            BLARIS_HOST_COMPRESS,
            "-o",
            compressor_exe,
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
                f"-I{BLARIS_INCLUDE}",
                f"-I{HOST_DIR}",
                BLARIS_HOST_COMPRESS,
                "-o",
                compressor_exe,
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
    stream_offset_size = [2, 3, 4]
    distance_align_log = [0, 1, 2]
    checkpoints_spread_log = [0, 6, 8, 10, 12, 14]
    flatten_chains = [0, 1]

    combinations = list(
        itertools.product(
            stream_offset_size,
            distance_align_log,
            checkpoints_spread_log,
            flatten_chains,
        )
    )
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
