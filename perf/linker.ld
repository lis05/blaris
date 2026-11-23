OUTPUT_ARCH("riscv")
ENTRY(_start)

MEMORY {
    FLASH (rx)  : ORIGIN = 0x00010000, LENGTH = 32M
    RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 32M
}

SECTIONS {
    .text : {
        *(.text._start)
        *(.text*)
    } > FLASH

    .rodata : {
        *(.rodata*)
        *(.srodata*)

        . = ALIGN(4);
        compressed_payload = .;
        KEEP(*(.payload*))
        compressed_payload_size = . - compressed_payload;
    } > FLASH

    .data : {
        _sdata = .;
        *(.data*)
        *(.sdata*)
        _edata = .;
    } > RAM

    .bss : {
        _sbss = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        _ebss = .;
    } > RAM

    . = ALIGN(16);
    .stack (NOLOAD) : {
        _stack_bottom = .;
        . += 64K;
        _stack_top = .;
    } > RAM

    _end = .;
}
