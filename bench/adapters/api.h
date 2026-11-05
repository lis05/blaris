#pragma once

#include <stdint.h>

/* Setup decompressor with its compressed payload (aligned to 4 because some
 * decompressors require it is such. */
int decompressor_init(const uint32_t *aligned_payload, uint32_t size);

/* Decompress a slice from the original payload at a given offset and of the given
 * size. */
int decompressor_slice(uint32_t offset, uint32_t size, uint8_t *buffer);

/* Reset the decompressor state, if needed. */
int decompressor_reset();
