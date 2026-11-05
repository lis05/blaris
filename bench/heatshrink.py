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
    BUILD_DIR,
)

HEATSHRINK_INCLUDE = BENCH_DIR / "third_party" / "heatshrink"
HEATSHRINK_DECOMPRESS = BENCH_DIR / "third_party" / "heatshrink" / "heatshrink_decoder.c"
ADAPTERS_DIR = BENCH_DIR / "adapters"
HEATSHRINK_ADAPTER = ADAPTERS_DIR / "heatshrink.c"
HOST_DIR = BENCH_DIR / "host"
HEATSHRINK_HOST_COMPRESS = HOST_DIR / "heatshrink.c"


def build(
    block_size,
    input_buffer_size,
    window_bits,
    lookahead_bits,
):
    params = f"b{block_size}_i{input_buffer_size}_w{window_bits}_l{lookahead_bits}"
    test_dir = BUILD_DIR / f"heatshrink_{params}"
    test_dir.mkdir(parents=True, exist_ok=True)

    pure_build = test_dir / "pure.o"
    with_adapter_build = test_dir / "with_adapter.o"
    compressor_exe = test_dir / "compressor"

    config = [
        f"-DHEATSHRINK_BLOCK_POW2={block_size}",
        f"-DHEATSHRINK_STATIC_INPUT_BUFFER_SIZE={input_buffer_size}",
        f"-DHEATSHRINK_STATIC_WINDOW_BITS={window_bits}",
        f"-DHEATSHRINK_STATIC_LOOKAHEAD_BITS={lookahead_bits}",
    ]

    subprocess.run(
        [
            CC,
            *CFLAGS,
            *config,
            f"-I{HEATSHRINK_INCLUDE}",
            "-c",
            str(HEATSHRINK_DECOMPRESS),
            "-o",
            str(pure_build),
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
            "heatshrink_decoder_reset",
            "-u",
            "heatshrink_decoder_sink",
            "-u",
            "heatshrink_decoder_poll",
            "-u",
            "heatshrink_decoder_finish",
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
            f"-I{HEATSHRINK_INCLUDE}",
            f"-I{ADAPTERS_DIR}",
            "-c",
            str(HEATSHRINK_ADAPTER),
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
            f"-I{HEATSHRINK_INCLUDE}",
            f"-I{HOST_DIR}",
            str(HEATSHRINK_HOST_COMPRESS),
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
                f"-I{HEATSHRINK_INCLUDE}",
                f"-I{HOST_DIR}",
                str(HEATSHRINK_HOST_COMPRESS),
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
    window_options = [4, 6, 8, 10, 12, 14]
    input_buffer_sizes = [16, 32]
    lookahead_options = [4, 6, 8]

    combinations = []
    for b in block_sizes:
        for w in window_options:
            if w > b:
                continue
            valid_ls = [l for l in lookahead_options if l <= w - 1]
            if not valid_ls and w - 1 >= 3:
                valid_ls = [w - 1]
            for i in input_buffer_sizes:
                for l in valid_ls:
                    combinations.append((b, i, w, l))

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
