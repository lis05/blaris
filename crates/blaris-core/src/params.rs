pub const MAX_LITERALS: usize = 16;
pub const MAX_LENGTH: usize = 15;
pub const MAX_DISTANCE_BITS: usize = 16;

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

    let c = MAX_LITERALS + (length - 1) * MAX_DISTANCE_BITS + (distance_bits - 1);

    if c > 255 {
        return None;
    }
    Some(c as u8)
}

pub fn literals_from_control(c: u8) -> usize {
    if (c as usize) < MAX_LITERALS {
        (c as usize) + 1
    } else {
        0
    }
}

pub fn match_from_control(c: u8) -> (usize, usize) {
    let c = c as usize;

    if MAX_LITERALS <= c && c < MAX_LITERALS + MAX_LENGTH * MAX_DISTANCE_BITS {
        (
            (c - MAX_LITERALS) / MAX_DISTANCE_BITS + 1,
            (c - MAX_LITERALS) % MAX_DISTANCE_BITS + 1,
        )
    } else {
        (0, 0)
    }
}
