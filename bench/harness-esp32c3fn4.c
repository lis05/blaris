#include <stdint.h>
#include "esp32c3fn4.c"

extern uint8_t _sdata[];
extern uint8_t _edata[];
extern uint8_t _sidata[];
extern uint8_t _sbss[];
extern uint8_t _ebss[];
extern uint8_t _stack_bottom[];
extern uint8_t _stack_top[];

extern const uint32_t compressed_payload[];
extern const uint8_t compressed_payload_size[];
extern const uint32_t uncompressed_payload_size;

extern int decompressor_init(const uint32_t *aligned_payload, uint32_t size);
extern int decompressor_slice(uint32_t offset, uint32_t size, uint8_t *buffer);
extern int decompressor_reset(void);

static inline uint32_t rdcycle32(void) {
    uint32_t val;
    __asm__ volatile("csrr %0, 0x7E2" : "=r"(val));
    return val;
}

static void sys_write(const char *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        serial_write_byte((uint8_t)buf[i]);
    }
}

static void print_u32(uint32_t val) {
    char buf[12];
    int i = 10;
    buf[11] = '\n';
    if (val == 0) {
        buf[i--] = '0';
    } else {
        while (val > 0) {
            buf[i--] = '0' + (val % 10);
            val /= 10;
        }
    }
    sys_write(&buf[i + 1], 11 - i);
}

static uint64_t divmod10(uint64_t val, uint32_t *rem) {
    uint32_t hi = (uint32_t)(val >> 32);
    uint32_t lo = (uint32_t)val;

    uint32_t q_hi = hi / 10;
    uint32_t r_hi = hi % 10;

    uint32_t q_mid = r_hi * 429496729U;
    uint32_t rem_mid = r_hi * 6U;

    uint32_t sum_lo = lo + rem_mid;
    uint32_t carry = (sum_lo < lo) ? 1 : 0;

    uint32_t q_lo = sum_lo / 10;
    uint32_t r = sum_lo % 10;

    if (carry) {
        q_mid += 429496729U;
        r += 6;
        if (r >= 10) {
            q_lo += 1;
            r -= 10;
        }
    }

    *rem = r;
    return ((uint64_t)q_hi << 32) + (uint64_t)q_mid + (uint64_t)q_lo;
}

static void print_u64(uint64_t val) {
    char buf[24];
    int i = 22;
    buf[23] = '\n';
    if (val == 0) {
        buf[i--] = '0';
    } else {
        while (val > 0) {
            uint32_t rem;
            val = divmod10(val, &rem);
            buf[i--] = '0' + rem;
        }
    }
    sys_write(&buf[i + 1], 23 - i);
}

static uint32_t get_peak_stack_usage(uintptr_t base_sp) {
    uint8_t *ptr = _stack_bottom;
    while (ptr < (uint8_t *)base_sp && *ptr == 0xA5) {
        ptr++;
    }
    return (uint32_t)(base_sp - (uintptr_t)ptr);
}

static uint32_t rand_state = 1;

static void srand(uint32_t seed) {
    rand_state = seed;
}

static int rand(void) {
    rand_state = rand_state * 1103515245U + 12345U;
    return (int)((rand_state >> 16) & 0x7FFF);
}

static inline uint32_t rand_offset(void) {
    return ((uint32_t)rand() << 15) | (uint32_t)rand();
}

static uint32_t fnv1a(uint32_t hash, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        hash = (hash ^ data[i]) * 16777619U;
    }
    return hash;
}

#define MAX_POW2 8
static uint8_t slice_buffer[1 << MAX_POW2];
static uint64_t results_cycles[MAX_POW2 + 1];
static uint32_t results_checksum[MAX_POW2 + 1];

void benchmark_entry(void) {
    disable_watchdogs();

    // Enable hardware performance counter for CPU cycles
    __asm__ volatile(
        "csrw 0x7E0, 1\n" // mpcer = 1 (count CPU cycles)
        "csrw 0x7E1, 1\n" // mpcmr = 1 (enable counter)
    );

    // Handshake: emit READY until host sends a byte
    while (!(USB_SERIAL_JTAG_EP1_CONF_REG & USB_SERIAL_JTAG_SERIAL_OUT_EP_DATA_AVAIL)) {
        sys_write("READY\n", 6);
        for (volatile int d = 0; d < 400000; d++);
    }
    serial_read_byte();

    uintptr_t base_sp;
    __asm__ volatile("mv %0, sp" : "=r"(base_sp));
    uint32_t comp_size = (uint32_t)(uintptr_t)compressed_payload_size;
    uint32_t uncomp_size = uncompressed_payload_size;
    decompressor_init(compressed_payload, comp_size);

    srand(2026);

    for (int pow2 = 0; pow2 <= MAX_POW2; pow2++) {
        uint32_t slice_size = 1 << pow2;
        uint32_t max_offset = (uncomp_size >= slice_size) ? (uncomp_size - slice_size) : 0;
        uint64_t cycles = 0;
        uint32_t checksum = 2166136261U;

        for (int i = 0; i < 1000; i++) {
            uint32_t offset = max_offset > 0 ? (rand_offset() % (max_offset + 1)) : 0;

            decompressor_reset();
            uint32_t start = rdcycle32();
            decompressor_slice(offset, slice_size, slice_buffer);
            uint32_t end = rdcycle32();
            cycles += (uint64_t)(end - start);

            uint32_t window_chk = fnv1a(2166136261U, slice_buffer, slice_size);
            checksum = fnv1a(checksum, (const uint8_t *)&window_chk, sizeof(window_chk));
        }

        results_cycles[pow2] = cycles;
        results_checksum[pow2] = checksum;
    }

    uint32_t peak = get_peak_stack_usage(base_sp);
    print_u32(peak);

    for (int pow2 = 0; pow2 <= MAX_POW2; pow2++) {
        print_u64(results_cycles[pow2]);
        print_u64((uint64_t)results_checksum[pow2]);
    }

    sys_write("DONE\n", 5);
}

__attribute__((naked, section(".text._start")))
void _start(void) {
    __asm__ volatile(
        "la sp, _stack_top\n"

        // 1. Copy .data from Flash (_sidata) to RAM (_sdata)
        "la a0, _sdata\n"
        "la a1, _edata\n"
        "la a2, _sidata\n"
        "1:\n"
        "bgeu a0, a1, 2f\n"
        "lw a3, 0(a2)\n"
        "sw a3, 0(a0)\n"
        "addi a0, a0, 4\n"
        "addi a2, a2, 4\n"
        "j 1b\n"
        "2:\n"

        // 2. Zero-fill .bss
        "la a0, _sbss\n"
        "la a1, _ebss\n"
        "3:\n"
        "bgeu a0, a1, 4f\n"
        "sw zero, 0(a0)\n"
        "addi a0, a0, 4\n"
        "j 3b\n"
        "4:\n"

        // 3. Paint stack with 0xA5
        "la a0, _stack_bottom\n"
        "la a1, _stack_top\n"
        "li a2, 0xA5\n"
        "5:\n"
        "bgeu a0, a1, 6f\n"
        "sb a2, 0(a0)\n"
        "addi a0, a0, 1\n"
        "j 5b\n"
        "6:\n"

        // 4. Jump to benchmark entry
        "tail benchmark_entry\n"
    );
}

