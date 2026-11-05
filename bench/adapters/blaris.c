#include "api.h"
#include <blaris.h>

#include "../../src/blaris-decompress.c"

static struct Checkpoint cp;
static struct Decoder dec;

int decompressor_init(const uint32_t *aligned_payload, uint32_t size) {
    (void)size;

    blaris_init(aligned_payload, &dec);
    decompressor_reset();

    return 0;
}

int decompressor_slice(uint32_t offset, uint32_t size, uint8_t *buffer) {
    blaris_decompress(&dec, offset, buffer, size, &cp);

    return 0;
}

int decompressor_reset() {
    cp = (struct Checkpoint){0};

    return 0;
}
