OUTPUT_ARCH("riscv")
ENTRY(_start)

/* https://documentation.espressif.com/esp32-c3_technical_reference_manual_en.pdf */
MEMORY {
    /* External: 4M */
    IFLASH    (rx) : org = 0x42000010, len = 64K - 0x10
    DFLASH    (r)  : org = 0x3C010000, len = 3M

    /* Internal SRAM1: 384K */
    RAM       (rw) : org = 0x3FC80000, len = 384K
}

SECTIONS {
    .text : {
        KEEP(*(.text._start))
        *(.text*)
    } > IFLASH

    .rodata : {
        *(.rodata*)
        *(.srodata*)

        . = ALIGN(4);
        compressed_payload = .;
        KEEP(*(.payload*))
        compressed_payload_size = . - compressed_payload;
    } > DFLASH

    .data : {
        . = ALIGN(4);
        _sdata = .;
        *(.data*)
        *(.sdata*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > DFLASH
    _sidata = LOADADDR(.data);

    .bss : {
        . = ALIGN(4);
        _sbss = .;
        *(.bss*)
        *(.sbss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM

    . = ALIGN(16);
    .stack (NOLOAD) : {
        _stack_bottom = .;
        . += 32K;
        _stack_top = .;
    } > RAM

    _end = .;
}
