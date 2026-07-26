use blaris_core::params::Params;

const ALIGN_POSITION: u8 = 1 << 0;
const ALIGN_LITERALS: u8 = 1 << 1;
const ALIGN_DISTANCES: u8 = 1 << 2;
const ALIGN_ALL: u8 = ALIGN_POSITION | ALIGN_LITERALS | ALIGN_DISTANCES;

struct State {
    pub current_control_offset: usize,
    pub current_literal_offset: usize,
    pub current_distance_offset_bits: usize,
    pub controls_len: usize,
    pub current_position: usize,
    pub alignment_flags: u8,
}

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

#[inline]
fn decompress_byte(from: &[u8], mut offset: usize, params: &Params, state: &mut State) -> u8 {
    debug_assert_eq!(state.alignment_flags, ALIGN_ALL);

    let save_current_control_offset: usize;
    let save_current_literal_offset: usize;
    let save_current_distances_offset_bits: usize;
    let save_current_position: usize;

    loop {
        debug_assert!(state.current_control_offset < state.controls_len);

        let c = from[state.current_control_offset];

        let literals_count = params.literals_from_control(c);
        if literals_count != 0 {
            if state.current_position <= offset && offset < state.current_position + literals_count
            {
                let offset_within_block = offset - state.current_position;
                return from[state.current_literal_offset + offset_within_block];
            } else {
                if offset >= state.current_position {
                    state.current_control_offset += 1;
                    state.current_position += literals_count;
                    state.current_literal_offset += literals_count;
                } else {
                    debug_assert!(state.current_control_offset > 0);

                    save_current_control_offset = state.current_control_offset;
                    save_current_literal_offset = state.current_literal_offset;
                    save_current_distances_offset_bits = state.current_distance_offset_bits;
                    save_current_position = state.current_position;

                    state.current_control_offset -= 1;
                    state.alignment_flags = 0;
                    break;
                }
            }
        } else {
            let (length, distance_bits) = params.match_from_control(c);
            debug_assert!(length > 0);

            if state.current_position <= offset && offset < state.current_position + length {
                debug_assert!(state.current_control_offset > 0);

                let distance =
                    read_u32_at_bit_offset(from, state.current_distance_offset_bits, distance_bits)
                        + 1;

                debug_assert!((distance as usize) <= offset);

                offset -= distance as usize;

                if offset >= state.current_position {
                    continue;
                }
                save_current_control_offset = state.current_control_offset;
                save_current_literal_offset = state.current_literal_offset;
                save_current_distances_offset_bits = state.current_distance_offset_bits;
                save_current_position = state.current_position;

                state.current_control_offset -= 1;
                state.alignment_flags = 0;
                break;
            } else if offset >= state.current_position {
                state.current_control_offset += 1;
                state.current_position += length;
                state.current_distance_offset_bits += distance_bits;
            } else {
                debug_assert!(state.current_control_offset > 0);

                save_current_control_offset = state.current_control_offset;
                save_current_literal_offset = state.current_literal_offset;
                save_current_distances_offset_bits = state.current_distance_offset_bits;
                save_current_position = state.current_position;

                state.current_control_offset -= 1;
                state.alignment_flags = 0;
                break;
            }
        }
    }

    loop {
        debug_assert!(state.current_control_offset < state.controls_len);

        let c = from[state.current_control_offset];

        let literals_count = params.literals_from_control(c);
        if literals_count != 0 {
            if (state.alignment_flags & ALIGN_LITERALS) == 0 {
                state.current_literal_offset -= literals_count;
                state.alignment_flags |= ALIGN_LITERALS;
            }

            if (state.alignment_flags & ALIGN_POSITION) == 0 {
                state.current_position -= literals_count;
                state.alignment_flags |= ALIGN_POSITION;
            }

            if state.current_position <= offset && offset < state.current_position + literals_count
            {
                let offset_within_block = offset - state.current_position;
                let res = from[state.current_literal_offset + offset_within_block];

                *state = State {
                    current_control_offset: save_current_control_offset,
                    current_literal_offset: save_current_literal_offset,
                    current_distance_offset_bits: save_current_distances_offset_bits,
                    controls_len: state.controls_len,
                    current_position: save_current_position,
                    alignment_flags: ALIGN_ALL,
                };
                return res;
            } else {
                debug_assert!(offset < state.current_position);
                debug_assert!(state.current_control_offset > 0);

                state.current_control_offset -= 1;
                state.alignment_flags = 0;
            }
        } else {
            let (length, distance_bits) = params.match_from_control(c);
            debug_assert!(length > 0);

            if (state.alignment_flags & ALIGN_DISTANCES) == 0 {
                state.current_distance_offset_bits -= distance_bits;
                state.alignment_flags |= ALIGN_DISTANCES;
            }

            if (state.alignment_flags & ALIGN_POSITION) == 0 {
                state.current_position -= length;
                state.alignment_flags |= ALIGN_POSITION;
            }

            if state.current_position <= offset && offset < state.current_position + length {
                debug_assert!(state.current_control_offset > 0);

                let distance =
                    read_u32_at_bit_offset(from, state.current_distance_offset_bits, distance_bits)
                        + 1;

                debug_assert!((distance as usize) <= offset);

                offset -= distance as usize;

                if offset >= state.current_position {
                    continue;
                }

                state.current_control_offset -= 1;
                state.alignment_flags = 0;
            } else {
                debug_assert!(offset < state.current_position);
                debug_assert!(state.current_control_offset > 0);

                state.current_control_offset -= 1;
                state.alignment_flags = 0;
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

    let mut state = State {
        current_control_offset: 0,
        current_literal_offset: literals_offset,
        current_distance_offset_bits: distances_offset * 8,
        controls_len: literals_offset,
        current_position: 0,
        alignment_flags: ALIGN_ALL,
    };

    for i in 0..to.len() {
        to[i] = decompress_byte(from, offset + i, &params, &mut state);
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
