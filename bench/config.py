from pathlib import Path

BENCH_DIR = Path(__file__).resolve().parent
TOP_DIR = BENCH_DIR.parent
BUILD_DIR = BENCH_DIR / "build"
LIB_DIR = BENCH_DIR / "lib"

CC = "clang"
CFLAGS = [
    "--target=riscv32-unknown-elf",
    "-march=rv32imc",
    "-mabi=ilp32",
    "-ffreestanding",
    "-fno-builtin",
    "-ffunction-sections",
    "-fdata-sections",
    "-mllvm",
    "-disable-loop-idiom-all",
    "-Os",
    f"-I{LIB_DIR}",
]

TARGET_FLAGS = [
    "--target=riscv32-unknown-elf",
    "-march=rv32imc",
    "-mabi=ilp32",
]
