pub const MAX_LITERALS: usize = 16;
pub const MAX_LENGTH: usize = 15;
pub const MAX_DISTANCE_BITS: usize = 16;

/// log2(MAX_DISTANCE_BITS); used to replace `* / %` with shifts and masks.
const DISTANCE_SHIFT: u32 = MAX_DISTANCE_BITS.trailing_zeros();
const DISTANCE_MASK: usize = MAX_DISTANCE_BITS - 1;

// The shift/mask encoding below requires a power-of-two MAX_DISTANCE_BITS,
// and every match control must fit in a u8.
const _: () = assert!(MAX_DISTANCE_BITS.is_power_of_two());
const _: () = assert!(MAX_LITERALS + (MAX_LENGTH << DISTANCE_SHIFT) <= 256);

pub fn control_from_literals(count: usize) -> Option<u8> {
    if count == 0 || count > MAX_LITERALS {
        return None;
    }

    Some((count - 1) as u8)
}

pub fn control_from_match(length: usize, distance_bits: usize) -> Option<u8> {
    if length == 0 || length > MAX_LENGTH {
        return None;
    }
    if distance_bits == 0 || distance_bits > MAX_DISTANCE_BITS {
        return None;
    }

    let c = MAX_LITERALS + ((length - 1) << DISTANCE_SHIFT) + (distance_bits - 1);

    if c > 255 {
        return None;
    }
    Some(c as u8)
}

#[inline(always)]
pub fn literals_from_control(c: u8) -> usize {
    if (c as usize) < MAX_LITERALS {
        (c as usize) + 1
    } else {
        0
    }
}

#[inline(always)]
pub fn match_from_control(c: u8) -> (usize, usize) {
    let c = c as usize;
    if c >= MAX_LITERALS {
        let diff = c - MAX_LITERALS;
        ((diff >> DISTANCE_SHIFT) + 1, (diff & DISTANCE_MASK) + 1)
    } else {
        (0, 0)
    }
}
