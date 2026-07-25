use blaris_core::params::Params;

pub fn read_u32_at_bit_offset(buf: &[u8], global_bit_offset: usize, num_bits: usize) -> u32 {
    if num_bits == 0 {
        return 0;
    }

    let byte_offset = global_bit_offset / 8;
    let bit_offset = global_bit_offset % 8;

    let mut accumulator: u64 = 0;
    for i in 0..5 {
        if let Some(&byte) = buf.get(byte_offset + i) {
            accumulator |= (byte as u64) << (i * 8);
        }
    }

    let shifted = accumulator >> bit_offset;
    let mask = (1u64 << num_bits) - 1;

    (shifted & mask) as u32
}

fn decompress_byte(
    from: &[u8],
    mut offset: usize,
    params: &Params,
    mut current_control_offset: usize,
    mut current_literal_offset: usize,
    mut current_distance_offset_bits: usize,
    controls_len: usize,
) -> Option<u8> {
    assert!(current_control_offset == 0);

    let mut current_position: usize = 0;
    let mut position_aligned = true;
    let mut literals_aligned = true;
    let mut distances_aligned = true;

    loop {
        if current_control_offset >= controls_len {
            return None;
        }

        let c = from[current_control_offset];

        let literals_count = params.literals_from_control(c);
        if literals_count != 0 {
            if !literals_aligned {
                current_literal_offset -= literals_count;
                literals_aligned = true;
            }

            if !position_aligned {
                current_position -= literals_count;
                position_aligned = true;
            }

            if current_position <= offset && offset < current_position + literals_count {
                let offset_within_block = offset - current_position;
                return from
                    .get(current_literal_offset + offset_within_block)
                    .copied();
            } else {
                if offset >= current_position {
                    current_control_offset += 1;
                    current_position += literals_count;
                    current_literal_offset += literals_count;
                } else {
                    if current_control_offset == 0 {
                        return None;
                    }
                    current_control_offset -= 1;
                    position_aligned = false;
                    literals_aligned = false;
                    distances_aligned = false;
                }
            }
        } else {
            let (length, distance_bits) = params.match_from_control(c);
            if length == 0 {
                return None;
            }

            if !distances_aligned {
                current_distance_offset_bits -= distance_bits;
                distances_aligned = true;
            }

            if !position_aligned {
                current_position -= length;
                position_aligned = true;
            }

            if current_position <= offset && offset < current_position + length {
                if current_control_offset == 0 {
                    return None;
                }

                let distance =
                    read_u32_at_bit_offset(from, current_distance_offset_bits, distance_bits) + 1;

                if (distance as usize) > offset {
                    return None;
                }

                offset -= distance as usize;

                if offset >= current_position {
                    continue;
                }

                current_control_offset -= 1;
                position_aligned = false;
                literals_aligned = false;
                distances_aligned = false;
            } else if offset >= current_position {
                current_control_offset += 1;
                current_position += length;
                current_distance_offset_bits += distance_bits;
            } else {
                if current_control_offset == 0 {
                    return None;
                }
                current_control_offset -= 1;
                position_aligned = false;
                literals_aligned = false;
                distances_aligned = false;
            }
        }
    }
}

pub fn decompress(mut from: &[u8], to: &mut [u8], offset: usize) -> bool {
    let mut params = Params::default();

    if !params.read_from(from) || !params.are_valid() {
        return false;
    }

    from = &from[Params::LENGTH..];

    if from.len() < 4 {
        return false;
    }
    let literals_offset = u32::from_le_bytes(from[..4].try_into().unwrap());
    from = &from[4..];

    if from.len() < 4 {
        return false;
    }
    let distances_offset = u32::from_le_bytes(from[..4].try_into().unwrap());
    from = &from[4..];

    let literals_offset = literals_offset as usize;
    let distances_offset = distances_offset as usize;

    for i in 0..to.len() {
        to[i] = match decompress_byte(
            from,
            offset + i,
            &params,
            0,
            literals_offset,
            distances_offset * 8,
            literals_offset,
        ) {
            Some(x) => x,
            None => return false,
        }
    }

    true
}

/// C-compatible entry point for decompression.
///
/// # Safety:
/// `from_ptr` and `to_ptr` must be non-null and valid for reads/writes up to `from_len` and `to_len` respectively.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn blaris_decompress(
    from_ptr: *const u8,
    from_len: usize,
    to_ptr: *mut u8,
    to_len: usize,
    offset: usize,
) -> bool {
    if from_ptr.is_null() || to_ptr.is_null() {
        return false;
    }

    // SAFETY: from_ptr and from_len form a valid slice due to the Safety of the entire function.
    let from = unsafe { core::slice::from_raw_parts(from_ptr, from_len) };
    // SAFETY: to_ptr and to_len form a valid slice due to the Safety of the entire function.
    let to = unsafe { core::slice::from_raw_parts_mut(to_ptr, to_len) };

    decompress(from, to, offset)
}
