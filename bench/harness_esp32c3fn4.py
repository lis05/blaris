import glob
import subprocess
import sys
import time
from pathlib import Path
import serial

from config import (
    CC,
    CFLAGS,
    TARGET_FLAGS,
    BENCH_DIR,
    BUILD_DIR,
)

HARNESS_C = BENCH_DIR / "harness-esp32c3fn4.c"
LINKER_LD = BENCH_DIR / "esp32c3fn4.ld"
GATHER_PY = BENCH_DIR / "gather-esp32c3fn4.py"

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


def find_serial_port() -> str:
    ports = glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")
    if not ports:
        raise RuntimeError("No ESP32-C3 serial port found (/dev/ttyACM* or /dev/ttyUSB*)")
    return sorted(ports)[0]


def wait_for_serial_port(port: str, timeout: float = 5.0) -> serial.Serial:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            ser = serial.Serial(port, baudrate=115200, timeout=1.0)
            ser.dtr = False
            ser.rts = False
            ser.reset_input_buffer()
            return ser
        except (serial.SerialException, FileNotFoundError):
            time.sleep(0.1)
    raise TimeoutError(f"Serial port {port} did not reappear after reset.")


def run(test_dir, port=None):
    test_dir = Path(test_dir).resolve()
    harness_elf = test_dir / "benchmark.elf"

    cmd = [
        sys.executable,
        str(GATHER_PY),
        "--elf",
        str(harness_elf),
        "--memory-ld",
        str(LINKER_LD),
        "--out-dir",
        str(test_dir),
    ]
    gather_res = subprocess.run(cmd, capture_output=True, text=True, check=True)
    flash_args = gather_res.stdout.strip().split()

    if port is None:
        port = find_serial_port()

    esptool_cmd = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32c3",
        "-p",
        port,
        "-b",
        "921600",
        "--after",
        "hard-reset",
        "write-flash",
        *flash_args,
    ]
    subprocess.run(esptool_cmd, check=True, stdout=subprocess.DEVNULL)

    ser = wait_for_serial_port(port)
    # Wait for device to emit READY
    ready_deadline = time.time() + 10.0
    while time.time() < ready_deadline:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if "READY" in line:
            break
        if "Guru Meditation" in line:
            rest = ser.read_all().decode("utf-8", errors="replace")
            ser.close()
            raise RuntimeError(f"Panic on boot:\n{line}\n{rest}")
    else:
        ser.close()
        raise TimeoutError("Device did not signal READY within 10 seconds")

    ser.write(b"GO\n")
    ser.flush()

    output_lines = []
    start_time = time.time()
    while time.time() - start_time < 3600.0:
        line = ser.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if "READY" in line:
            continue
        if line == "DONE":
            break
        output_lines.append(line)
    ser.close()

    if any("Guru Meditation" in l or "panic" in l for l in output_lines):
        print("\n".join(output_lines))
        raise RuntimeError("Device panicked:\n" + "\n".join(output_lines))

    lines = [int(line.strip()) for line in output_lines if line.strip()]
    if len(lines) != 19:
        print("\n".join(output_lines))
        raise ValueError(f"Expected 19 outputs (1 peak stack + 9 (cycles, checksum) pairs), got {len(lines)}: {output_lines}")

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
