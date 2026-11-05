#!/usr/bin/env python3
"""
gather.py - Gather IFLASH and DFLASH payloads from an ELF executable for flashing.
Parses memory.ld for region origins and sizes, extracts corresponding segments,
prepends the Direct Boot header to IFLASH if needed, and writes out the binaries.
Prints the flash arguments (offset + filepath pairs) to stdout for esptool.
"""

import argparse
from pathlib import Path
import re
import struct
import sys
from typing import Dict, List, Tuple


DIRECT_BOOT_HEADER = b"\x1d\x04\xdb\xae\x1d\x04\xdb\xae"
IBUS_BASE = 0x42000000
DBUS_BASE = 0x3C000000


def parse_elf_entry(elf_path: Path) -> int:
    """Returns the entry point address from a 32-bit ELF header."""
    with open(elf_path, "rb") as f:
        hdr = f.read(52)
        if len(hdr) < 52 or hdr[:4] != b"\x7fELF":
            raise ValueError(f"'{elf_path}' is not a valid 32-bit ELF file")
        return struct.unpack("<I", hdr[24:28])[0]


def encode_jal(rd: int, imm: int) -> int:
    """Encodes a RISC-V JAL instruction (J-type)."""
    if imm % 2 != 0:
        raise ValueError(f"JAL immediate must be 2-byte aligned, got {imm}")
    if not (-1048576 <= imm < 1048576):
        raise ValueError(f"JAL immediate out of range: {imm}")
    imm_u = imm & 0x1FFFFF
    imm_20 = (imm_u >> 20) & 1
    imm_10_1 = (imm_u >> 1) & 0x3FF
    imm_11 = (imm_u >> 11) & 1
    imm_19_12 = (imm_u >> 12) & 0xFF
    return (
        (imm_20 << 31)
        | (imm_10_1 << 21)
        | (imm_11 << 20)
        | (imm_19_12 << 12)
        | (rd << 7)
        | 0x6F
    )


def parse_size_expr(expr: str) -> int:
    expr = expr.strip()
    if "-" in expr:
        parts = expr.split("-", 1)
        return parse_size_expr(parts[0]) - parse_size_expr(parts[1])
    if "+" in expr:
        parts = expr.split("+", 1)
        return parse_size_expr(parts[0]) + parse_size_expr(parts[1])

    s = expr.strip()
    multiplier = 1
    for suffix, mult in [
        ("GiB", 1024 * 1024 * 1024),
        ("G", 1024 * 1024 * 1024),
        ("GB", 1024 * 1024 * 1024),
        ("MiB", 1024 * 1024),
        ("M", 1024 * 1024),
        ("MB", 1024 * 1024),
        ("KiB", 1024),
        ("K", 1024),
        ("KB", 1024),
    ]:
        if s.endswith(suffix):
            multiplier = mult
            s = s[: -len(suffix)].strip()
            break
    if s.startswith("0x") or s.startswith("0X"):
        return int(s, 16) * multiplier
    return int(s) * multiplier


def parse_memory_ld(path: Path) -> Dict[str, Tuple[int, int]]:
    """Returns a dict mapping region name (lowercase) to (origin, length)."""
    text = path.read_text()
    regions = {}
    pattern = re.compile(
        r"([a-zA-Z0-9_]+)\s*\([^)]*\)\s*:\s*(?:org|ORIGIN)\s*=\s*([^,\n]+),\s*(?:len|LENGTH)\s*=\s*([^\n;]+)",
        re.IGNORECASE,
    )
    for match in pattern.finditer(text):
        name = match.group(1).lower()
        org_str = match.group(2).strip()
        len_str = match.group(3).strip()
        org = parse_size_expr(org_str)
        length = parse_size_expr(len_str)
        regions[name] = (org, length)
    return regions


def parse_elf_load_segments(elf_path: Path) -> List[Tuple[int, int, bytes]]:
    """Returns a list of (vaddr, paddr, data) for all PT_LOAD segments with filesz > 0."""
    with open(elf_path, "rb") as f:
        hdr = f.read(52)
        if len(hdr) < 52 or hdr[:4] != b"\x7fELF":
            raise ValueError(f"'{elf_path}' is not a valid 32-bit ELF file")
        phoff = struct.unpack("<I", hdr[28:32])[0]
        phentsize = struct.unpack("<H", hdr[42:44])[0]
        phnum = struct.unpack("<H", hdr[44:46])[0]

        segments = []
        f.seek(phoff)
        for _ in range(phnum):
            ph = f.read(phentsize)
            (
                p_type,
                p_offset,
                p_vaddr,
                p_paddr,
                p_filesz,
                _p_memsz,
                _p_flags,
                _p_align,
            ) = struct.unpack("<8I", ph[:32])
            if p_type == 1 and p_filesz > 0:  # PT_LOAD
                cur_pos = f.tell()
                f.seek(p_offset)
                data = f.read(p_filesz)
                f.seek(cur_pos)
                segments.append((p_vaddr, p_paddr, data))
        return segments


def main():
    parser = argparse.ArgumentParser(description="Gather IFLASH and DFLASH payloads from an ELF executable.")
    parser.add_argument(
        "--elf",
        type=Path,
    )
    parser.add_argument(
        "--memory-ld",
        type=Path,
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
    )
    args = parser.parse_args()

    elf_path = args.elf
    if not elf_path or not elf_path.exists():
        sys.stderr.write(f"Error: ELF file not found: {elf_path}\n")
        sys.exit(1)

    memory_ld = args.memory_ld
    if not memory_ld.exists():
        sys.stderr.write(f"Error: memory.ld not found: {memory_ld}\n")
        sys.exit(1)

    mem_regions = parse_memory_ld(memory_ld)
    if "iflash" not in mem_regions or "dflash" not in mem_regions:
        sys.stderr.write("Error: memory.ld must define both IFLASH and DFLASH regions\n")
        sys.exit(1)

    iflash_org, iflash_len = mem_regions["iflash"]
    dflash_org, dflash_len = mem_regions["dflash"]

    segments = parse_elf_load_segments(elf_path)
    segments.sort(key=lambda s: s[1])
    entry_addr = parse_elf_entry(elf_path)
    sys.stderr.write(f"[gather] ELF entry point: 0x{entry_addr:08x}\n")

    # 1. Gather IFLASH
    # Physical offset in flash: 0x0
    # In ESP32-C3 Direct Boot:
    #   - Flash offset 0x0..0x7: 8-byte Direct Boot header (0xaedb041d 0xaedb041d)
    #   - Flash offset 0x8 (0x42000008): ROM jump target -> JAL trampoline branching to ELF entry.
    iflash_flash_offset = 0x0
    iflash_buf = bytearray()
    iflash_buf.extend(DIRECT_BOOT_HEADER)

    jump_offset = entry_addr - (IBUS_BASE + 8)
    sys.stderr.write(
        f"[gather] Emitting Direct Boot trampoline at 0x{IBUS_BASE + 8:08x}: "
        f"jal x0, 0x{jump_offset:x} (target 0x{entry_addr:08x})\n"
    )
    jal_inst = encode_jal(0, jump_offset)
    iflash_buf.extend(struct.pack("<I", jal_inst))

    for _vaddr, paddr, data in segments:
        if iflash_org <= paddr < iflash_org + iflash_len:
            # Physical offset relative to 0x42000000
            file_offset = paddr - IBUS_BASE
            if file_offset < len(iflash_buf):
                raise ValueError(
                    f"Segment at 0x{paddr:08x} overlaps with Direct Boot header/trampoline "
                    f"(len={len(iflash_buf)} bytes)"
                )
            req_len = file_offset + len(data)
            if len(iflash_buf) < req_len:
                iflash_buf.extend(b"\x00" * (req_len - len(iflash_buf)))
            iflash_buf[file_offset : file_offset + len(data)] = data

    # 2. Gather DFLASH
    dflash_flash_offset = dflash_org - DBUS_BASE
    dflash_buf = bytearray()
    for _vaddr, paddr, data in segments:
        if dflash_org <= paddr < dflash_org + dflash_len:
            file_offset = paddr - dflash_org
            req_len = file_offset + len(data)
            if len(dflash_buf) < req_len:
                dflash_buf.extend(b"\x00" * (req_len - len(dflash_buf)))
            dflash_buf[file_offset : file_offset + len(data)] = data

    # Write output files
    args.out_dir.mkdir(parents=True, exist_ok=True)
    iflash_file = args.out_dir / "iflash.bin"
    dflash_file = args.out_dir / "dflash.bin"

    iflash_file.write_bytes(iflash_buf)
    dflash_file.write_bytes(dflash_buf)

    sys.stderr.write(
        f"[gather] IFLASH: 0x{iflash_flash_offset:x} -> {iflash_file} ({len(iflash_buf)} bytes)\n"
    )
    sys.stderr.write(
        f"[gather] DFLASH: 0x{dflash_flash_offset:x} -> {dflash_file} ({len(dflash_buf)} bytes)\n"
    )

    # Print arguments for esptool to stdout:
    # 0x0 artifacts/iflash.bin 0x10000 artifacts/dflash.bin
    print(f"0x{iflash_flash_offset:x} {iflash_file} 0x{dflash_flash_offset:x} {dflash_file}")


if __name__ == "__main__":
    main()
