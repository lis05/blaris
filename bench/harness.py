import subprocess
from pathlib import Path

from config import (
    CC,
    CFLAGS,
    TARGET_FLAGS,
    BENCH_DIR,
    BUILD_DIR,
)

HARNESS_C = BENCH_DIR / "harness.c"
LINKER_LD = BENCH_DIR / "linker.ld"

def build():
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    harness_o = BUILD_DIR / "harness.o"

    cmd = [
        CC,
        *CFLAGS,
        "-c",
        str(HARNESS_C),
        "-o",
        str(harness_o),
    ]
    subprocess.check_call(cmd)
    print(f"Built {harness_o}")
    return harness_o


def build_with_payload(test_dir):
    test_dir = Path(test_dir).resolve()
    test_dir.mkdir(parents=True, exist_ok=True)

    uncompressed_payload_file = test_dir / "payload.bin"
    uncompressed_size = uncompressed_payload_file.stat().st_size

    compressed_payload_file = test_dir / "compressed_payload.bin"
    asm_file = test_dir / "compressed_payload.s"
    obj_file = test_dir / "compressed_payload.o"

    asm_content = f"""\
    .section .rodata
    .balign 4
    .global uncompressed_payload_size
uncompressed_payload_size:
    .word {uncompressed_size}

    .section .payload, "a", @progbits
    .incbin "{compressed_payload_file}"
"""
    asm_file.write_text(asm_content)

    cmd = [
        CC,
        *TARGET_FLAGS,
        "-c",
        str(asm_file),
        "-o",
        str(obj_file),
    ]
    subprocess.check_call(cmd)
    return obj_file


def link(test_dir):
    test_dir = Path(test_dir).resolve()

    harness_o = BUILD_DIR / "harness.o"
    compressed_payload_o = test_dir / "compressed_payload.o"
    with_adapter_o = test_dir.parent / "with_adapter.o"
    output_elf = test_dir / "benchmark.elf"

    cmd = [
        CC,
        *TARGET_FLAGS,
        "-nostdlib",
        "-fuse-ld=lld",
        "-Wl,--gc-sections",
        "-T",
        str(LINKER_LD),
        str(harness_o),
        str(compressed_payload_o),
        str(with_adapter_o),
        "-o",
        str(output_elf),
    ]
    subprocess.check_call(cmd)
    return output_elf


def run(test_dir):
    test_dir = Path(test_dir).resolve()
    elf = test_dir if test_dir.is_file() else test_dir / "benchmark.elf"

    if not elf.exists():
        build_with_payload(test_dir)
        link(test_dir)

    res = subprocess.run(
        ["rv32emu", str(elf)],
        capture_output=True,
        text=True,
        check=True,
    )

    lines = [int(line.strip()) for line in res.stdout.strip().splitlines() if line.strip()]
    if len(lines) != 19:
        raise ValueError(f"Expected 19 outputs (1 peak stack + 9 (cycles, checksum) pairs), got {len(lines)}: {lines}")

    peak_stack = lines[0]
    cycles = [lines[i] for i in range(1, len(lines), 2)]
    checksums = [lines[i + 1] for i in range(1, len(lines), 2)]
    return peak_stack, cycles, checksums


def fnv1a(hash_val, data):
    for b in data:
        hash_val = ((hash_val ^ b) * 16777619) & 0xFFFFFFFF
    return hash_val


class RNG:
    def __init__(self, seed=2026):
        self.state = seed

    def rand(self):
        self.state = (self.state * 1103515245 + 12345) & 0xFFFFFFFF
        return (self.state >> 16) & 0x7FFF

    def rand_offset(self):
        return (self.rand() << 15) | self.rand()


def verify(test_dir):
    test_dir = Path(test_dir).resolve()
    raw_data = (test_dir / "payload.bin").read_bytes()
    uncomp_size = len(raw_data)

    rng = RNG(2026)
    expected_checksums = []

    for pow2 in range(9):
        slice_size = 1 << pow2
        max_offset = (uncomp_size - slice_size) if uncomp_size >= slice_size else 0
        checksum = 2166136261

        for _ in range(1000):
            offset = (rng.rand_offset() % (max_offset + 1)) if max_offset > 0 else 0
            window = raw_data[offset : offset + slice_size]
            window_chk = fnv1a(2166136261, window)
            checksum = fnv1a(checksum, window_chk.to_bytes(4, "little"))

        expected_checksums.append(checksum)

    return expected_checksums
