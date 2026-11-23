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

LZ0_INCLUDE = BENCH_DIR / "third_party" / "lz0"
LZ0_DECOMPRESS = LZ0_INCLUDE / "lz0dec_min.c"
LZ0_COMPRESS_SRCS = [
    LZ0_INCLUDE / "lz0_cost.c",
    LZ0_INCLUDE / "lz0_find.c",
    LZ0_INCLUDE / "lz0_parse.c",
    LZ0_INCLUDE / "lz0_emit.c",
    LZ0_INCLUDE / "lz0_decompress.c",
    LZ0_INCLUDE / "lz0_compress.c",
    LZ0_INCLUDE / "lz0_ultra.c",
    LZ0_INCLUDE / "lz0_simd.c",
    LZ0_INCLUDE / "lz0_simd_x86_64.S",
    LZ0_INCLUDE / "lz0_simd_arm64.S",
    LZ0_INCLUDE / "lz0_simd_riscv64.S",
]
ADAPTERS_DIR = BENCH_DIR / "adapters"
LZ0_ADAPTER = ADAPTERS_DIR / "lz0.c"
HOST_DIR = BENCH_DIR / "host"
LZ0_HOST_COMPRESS = HOST_DIR / "lz0.c"


def build(block_size, level=9):
    params = f"b{block_size}"
    test_dir = BUILD_DIR / f"lz0_{params}"
    test_dir.mkdir(parents=True, exist_ok=True)

    pure_build = test_dir / "pure.o"
    with_adapter_build = test_dir / "with_adapter.o"
    compressor_exe = test_dir / "compressor"

    config = [
        f"-DLZ0_BLOCK_POW2={block_size}",
        f"-DLZ0_LEVEL={level}",
    ]

    subprocess.run(
        [
            CC,
            *CFLAGS,
            *config,
            f"-I{LZ0_INCLUDE}",
            "-c",
            str(LZ0_DECOMPRESS),
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
            "lz0dec_min",
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
            f"-I{LZ0_INCLUDE}",
            f"-I{ADAPTERS_DIR}",
            "-c",
            str(LZ0_ADAPTER),
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
            f"-I{LZ0_INCLUDE}",
            f"-I{HOST_DIR}",
            str(LZ0_HOST_COMPRESS),
            *[str(s) for s in LZ0_COMPRESS_SRCS],
            "-lm",
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
                f"-I{LZ0_INCLUDE}",
                f"-I{HOST_DIR}",
                str(LZ0_HOST_COMPRESS),
                *[str(s) for s in LZ0_COMPRESS_SRCS],
                "-lm",
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
    block_sizes = list(range(4, 15))
    combinations = [(b,) for b in block_sizes]

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
