#![no_std]

pub mod params;

// Points at the beginning of a token.
#[repr(C)]
#[derive(Clone, Copy, Default)]
#[cfg_attr(test, derive(Debug, PartialEq, Eq))]
pub struct Checkpoint {
    // All of these are indices inside of their respective streams.
    pub control: u32,
    pub literal: u32,
    pub distance_bit: u32,
    // Position is "which byte is this"
    pub position: u32,
}

#[derive(Clone, Copy)]
#[cfg_attr(test, derive(Debug, PartialEq, Eq))]
pub struct Header<'a> {
    pub controls_offset: u32, // all offsets from the start of the payload
    pub literals_offset: u32,
    pub distances_offset: u32,
    pub checkpoints_offset: u32, // note: checkpoints in the payload must be aligned to 4.
    pub num_checkpoints: u32,
    pub controls: &'a [u8],
    pub literals: &'a [u8],
    pub distances: &'a [u8],
    pub checkpoints: &'a [Checkpoint],
}

impl Header<'_> {
    pub const SIZE: usize = 20;

    pub fn store(&self, buf: &mut [u8]) {
        let ptr = buf.as_mut_ptr();
        unsafe {
            core::ptr::write_unaligned(ptr as *mut u32, self.controls_offset.to_le());
            core::ptr::write_unaligned(ptr.add(4) as *mut u32, self.literals_offset.to_le());
            core::ptr::write_unaligned(ptr.add(8) as *mut u32, self.distances_offset.to_le());
            core::ptr::write_unaligned(ptr.add(12) as *mut u32, self.checkpoints_offset.to_le());
            core::ptr::write_unaligned(ptr.add(16) as *mut u32, self.num_checkpoints.to_le());
        }
    }

    pub fn load<'a>(payload: &'a [u8]) -> Header<'a> {
        let ptr = payload.as_ptr();
        let controls_offset = unsafe { core::ptr::read_unaligned(ptr as *const u32).to_le() };
        let literals_offset = unsafe { core::ptr::read_unaligned(ptr.add(4) as *const u32).to_le() };
        let distances_offset = unsafe { core::ptr::read_unaligned(ptr.add(8) as *const u32).to_le() };
        let checkpoints_offset = unsafe { core::ptr::read_unaligned(ptr.add(12) as *const u32).to_le() };
        let num_checkpoints = unsafe { core::ptr::read_unaligned(ptr.add(16) as *const u32).to_le() };

        let controls = unsafe {
            core::slice::from_raw_parts(
                ptr.add(controls_offset as usize),
                (literals_offset - controls_offset) as usize,
            )
        };
        let literals = unsafe {
            core::slice::from_raw_parts(
                ptr.add(literals_offset as usize),
                (distances_offset - literals_offset) as usize,
            )
        };

        let distances = unsafe {
            core::slice::from_raw_parts(
                ptr.add(distances_offset as usize),
                (checkpoints_offset.saturating_sub(distances_offset)) as usize,
            )
        };
        let checkpoints = if checkpoints_offset == 0 {
            &[][..]
        } else {
            let cp_ptr = unsafe { ptr.add(checkpoints_offset as usize) as *const Checkpoint };
            debug_assert!((cp_ptr as usize).is_multiple_of(core::mem::align_of::<Checkpoint>()));
            unsafe { core::slice::from_raw_parts(cp_ptr, num_checkpoints as usize) }
        };

        Header {
            controls_offset,
            literals_offset,
            distances_offset,
            checkpoints_offset,
            num_checkpoints,
            controls,
            literals,
            distances,
            checkpoints,
        }
    }
}

pub fn read_u32_at_bit_offset(buf: &[u8], global_bit_offset: usize, num_bits: usize) -> u32 {
    if num_bits == 0 {
        return 0;
    }

    let byte_offset = global_bit_offset >> 3;
    let bit_offset = global_bit_offset & 7;

    let ptr = unsafe { buf.as_ptr().add(byte_offset) };
    let val = unsafe {
        (*ptr as u32) | ((*ptr.add(1) as u32) << 8) | ((*ptr.add(2) as u32) << 16)
    };

    (val >> bit_offset) & ((1u32 << num_bits) - 1)
}

impl Checkpoint {
    pub const SIZE: usize = 16;

    pub fn store(&self, buf: &mut [u8]) {
        let ptr = buf.as_mut_ptr();
        unsafe {
            core::ptr::write_unaligned(ptr as *mut u32, self.control.to_le());
            core::ptr::write_unaligned(ptr.add(4) as *mut u32, self.literal.to_le());
            core::ptr::write_unaligned(ptr.add(8) as *mut u32, self.distance_bit.to_le());
            core::ptr::write_unaligned(ptr.add(12) as *mut u32, self.position.to_le());
        }
    }

    pub fn prev(&mut self, header: &Header<'_>) {
        self.control -= 1;
        let c = unsafe { *header.controls.get_unchecked(self.control as usize) };
        let num_literals = crate::params::literals_from_control(c);

        if num_literals != 0 {
            // this token is literal(s)
            self.literal -= num_literals as u32;
            self.position -= num_literals as u32;
        } else {
            // this token is a match
            let (length, distance_bits) = crate::params::match_from_control(c);
            self.distance_bit -= distance_bits as u32;
            self.position -= length as u32;
        }
    }

    pub fn contains_weakly(&self, pos: u32, header: &Header<'_>) -> bool {
        if (self.control as usize) >= header.controls.len() {
            return false;
        }
        let c = unsafe { *header.controls.get_unchecked(self.control as usize) };
        let num_literals = crate::params::literals_from_control(c);

        if num_literals != 0 {
            // this token is literal(s)
            self.position <= pos && pos < self.position + (num_literals as u32)
        } else {
            // this token is a match
            let (length, _) = crate::params::match_from_control(c);
            self.position <= pos && pos < self.position + (length as u32)
        }
    }

    #[inline(never)]
    pub fn seek_to_token(&mut self, pos: u32, header: &Header<'_>) {
        if self.contains_weakly(pos, header) {
            return;
        }

        let mut best_checkpoint = *self;
        let mut best_dist = self.position.abs_diff(pos);

        let cps = header.checkpoints;
        let n = cps.len();
        // TODO: best_dist > fraction of total length
        if n > 0 && best_dist > 64 {
            let mut left = 0;
            let mut right = n;
            while left < right {
                let mid = (left + right) / 2;
                if unsafe { cps.get_unchecked(mid).position } <= pos {
                    left = mid + 1;
                } else {
                    right = mid;
                }
            }

            if left < n {
                let cp = unsafe { *cps.get_unchecked(left) };
                let distance = cp.position.abs_diff(pos);
                if distance < best_dist {
                    best_dist = distance;
                    best_checkpoint = cp;
                }
            }

            if left > 0 {
                let cp = unsafe { *cps.get_unchecked(left - 1) };
                let distance = cp.position.abs_diff(pos);
                if distance < best_dist {
                    best_checkpoint = cp;
                }
            }
        }

        // start from best checkpoint
        *self = best_checkpoint;

        // advance until token containing `pos` is found
        while pos >= self.position && (self.control as usize) < header.controls.len() {
            let c = unsafe { *header.controls.get_unchecked(self.control as usize) };
            if (c as usize) < crate::params::MAX_LITERALS {
                let len = (c as u32) + 1;
                if pos < self.position + len {
                    break;
                }
                self.literal += len;
                self.position += len;
            } else {
                let (length, distance_bits) = crate::params::match_from_control(c);
                let len = length as u32;
                if pos < self.position + len {
                    break;
                }
                self.distance_bit += distance_bits as u32;
                self.position += len;
            }
            self.control += 1;
        }

        while pos < self.position && self.control > 0 {
            self.prev(header);
        }
    }

    pub fn seek(&mut self, mut pos: u32, header: &Header<'_>) -> u32 {
        loop {
            self.seek_to_token(pos, header);

            let c = unsafe { *header.controls.get_unchecked(self.control as usize) };
            if (c as usize) < crate::params::MAX_LITERALS {
                return pos;
            }

            let (_, distance_bits) = crate::params::match_from_control(c);
            let distance = read_u32_at_bit_offset(
                header.distances,
                self.distance_bit as usize,
                distance_bits,
            ) + 1;

            pos -= distance;
        }
    }

    pub fn read_byte(&mut self, pos: u32, header: &Header<'_>) -> u8 {
        let resolved_pos = self.seek(pos, header);
        unsafe {
            *header
                .literals
                .get_unchecked((self.literal + (resolved_pos - self.position)) as usize)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_checkpoint_size_and_align() {
        assert_eq!(core::mem::size_of::<Checkpoint>(), 16);
        assert_eq!(core::mem::size_of::<Checkpoint>(), Checkpoint::SIZE);
        assert_eq!(core::mem::align_of::<Checkpoint>(), 4);
    }

    #[test]
    fn test_checkpoint_store() {
        let checkpoint = Checkpoint {
            control: 10,
            literal: 20,
            distance_bit: 30,
            position: 40,
        };

        let mut buf = [0u8; Checkpoint::SIZE];
        checkpoint.store(&mut buf);

        assert_eq!(&buf[0..4], &10u32.to_le_bytes());
        assert_eq!(&buf[4..8], &20u32.to_le_bytes());
        assert_eq!(&buf[8..12], &30u32.to_le_bytes());
        assert_eq!(&buf[12..16], &40u32.to_le_bytes());
    }

    #[test]
    fn test_header_store_load_with_checkpoints() {
        // Construct a payload containing:
        // [Header: 20 bytes]
        // [Controls: 4 bytes] (offset 20)
        // [Literals: 6 bytes] (offset 24)
        // [Distances: 2 bytes] (offset 30)
        // [Padding: 0 bytes] -> offset 32 (aligned to 4)
        // [Checkpoints: 2 checkpoints = 32 bytes] (offset 32, num_checkpoints = 2)

        #[repr(align(4))]
        struct AlignedPayload([u8; 64]);

        let mut storage = AlignedPayload([0u8; 64]);
        let payload = &mut storage.0;

        let controls_offset = 20u32;
        let literals_offset = 24u32;
        let distances_offset = 30u32;
        let checkpoints_offset = 32u32;
        let num_checkpoints = 2u32; // 2 checkpoints

        // Populate controls
        payload[20..24].copy_from_slice(&[1, 2, 3, 4]);
        // Populate literals
        payload[24..30].copy_from_slice(&[10, 20, 30, 40, 50, 60]);
        // Populate distances
        payload[30..32].copy_from_slice(&[0xAA, 0xBB]);

        // Populate 2 checkpoints at offset 32
        let c1 = Checkpoint {
            control: 1,
            literal: 2,
            distance_bit: 3,
            position: 4,
        };
        let c2 = Checkpoint {
            control: 5,
            literal: 6,
            distance_bit: 7,
            position: 8,
        };
        c1.store(&mut payload[32..48]);
        c2.store(&mut payload[48..64]);

        let header = Header {
            controls_offset,
            literals_offset,
            distances_offset,
            checkpoints_offset,
            num_checkpoints,
            controls: &[1, 2, 3, 4],
            literals: &[10, 20, 30, 40, 50, 60],
            distances: &[0xAA, 0xBB],
            checkpoints: &[c1, c2],
        };

        header.store(&mut payload[..Header::SIZE]);

        let loaded = Header::load(payload);
        assert_eq!(loaded.controls_offset, controls_offset);
        assert_eq!(loaded.literals_offset, literals_offset);
        assert_eq!(loaded.distances_offset, distances_offset);
        assert_eq!(loaded.checkpoints_offset, checkpoints_offset);
        assert_eq!(loaded.num_checkpoints, num_checkpoints);
        assert_eq!(loaded.controls, &[1, 2, 3, 4]);
        assert_eq!(loaded.literals, &[10, 20, 30, 40, 50, 60]);
        assert_eq!(loaded.distances, &[0xAA, 0xBB]);
        assert_eq!(loaded.checkpoints.len(), 2);
        assert_eq!(loaded.checkpoints[0], c1);
        assert_eq!(loaded.checkpoints[1], c2);
    }

    #[test]
    #[should_panic]
    fn test_misaligned_checkpoints_panics() {
        // Create an unaligned buffer where checkpoints pointer % 4 != 0
        #[repr(align(4))]
        struct Aligned([u8; 100]);

        let mut buf = Aligned([0u8; 100]);
        // Store header at offset 1, so everything is offset by 1 byte
        let slice = &mut buf.0[1..90]; // start address is odd!

        let header = Header {
            controls_offset: 20,
            literals_offset: 24,
            distances_offset: 28,
            checkpoints_offset: 32, // multiple of 4, but base pointer is odd!
            num_checkpoints: 0,
            controls: &[],
            literals: &[],
            distances: &[],
            checkpoints: &[],
        };
        header.store(&mut slice[..Header::SIZE]);
        Header::load(slice);
    }

    #[test]
    fn test_checkpoint_prev() {
        let lit_control = crate::params::control_from_literals(5).unwrap();
        let match_control = crate::params::control_from_match(7, 10).unwrap();
        let controls = [lit_control, match_control];
        let header = Header {
            controls_offset: 0,
            literals_offset: 0,
            distances_offset: 0,
            checkpoints_offset: 0,
            num_checkpoints: 0,
            controls: &controls,
            literals: &[],
            distances: &[],
            checkpoints: &[],
        };

        let mut checkpoint = Checkpoint {
            control: 2,
            literal: 5,
            distance_bit: 10,
            position: 12,
        };

        // Rewind over match token
        checkpoint.prev(&header);
        assert_eq!(checkpoint.control, 1);
        assert_eq!(checkpoint.literal, 5);
        assert_eq!(checkpoint.distance_bit, 0);
        assert_eq!(checkpoint.position, 5);

        // Rewind over literal token
        checkpoint.prev(&header);
        assert_eq!(checkpoint.control, 0);
        assert_eq!(checkpoint.literal, 0);
        assert_eq!(checkpoint.distance_bit, 0);
        assert_eq!(checkpoint.position, 0);
    }

    #[test]
    fn test_checkpoint_seek_and_read_byte() {
        let lit_control = crate::params::control_from_literals(5).unwrap();
        // match length 3, distance 5 (stored as distance - 1 = 4, which fits in 3 bits)
        let match_control = crate::params::control_from_match(3, 3).unwrap();
        let controls = [lit_control, match_control];
        let literals = b"ABCDE";
        // Distance value (5 - 1 = 4) in binary is 100_2 (3 bits)
        let distances = [0b0000_0100u8];

        let header = Header {
            controls_offset: 0,
            literals_offset: 0,
            distances_offset: 0,
            checkpoints_offset: 0,
            num_checkpoints: 0,
            controls: &controls,
            literals,
            distances: &distances,
            checkpoints: &[],
        };

        let mut checkpoint = Checkpoint::default();

        // Read direct literals
        assert_eq!(checkpoint.read_byte(0, &header), b'A');
        assert_eq!(checkpoint.read_byte(1, &header), b'B');
        assert_eq!(checkpoint.read_byte(2, &header), b'C');
        assert_eq!(checkpoint.read_byte(3, &header), b'D');
        assert_eq!(checkpoint.read_byte(4, &header), b'E');

        // Read matches (5 -> 0 => 'A', 6 -> 1 => 'B', 7 -> 2 => 'C')
        assert_eq!(checkpoint.read_byte(5, &header), b'A');
        assert_eq!(checkpoint.read_byte(6, &header), b'B');
        assert_eq!(checkpoint.read_byte(7, &header), b'C');

        // Random access jump backwards
        assert_eq!(checkpoint.read_byte(1, &header), b'B');
        // Random access jump forwards
        assert_eq!(checkpoint.read_byte(7, &header), b'C');
    }

    #[test]
    fn test_seek_selects_closest_checkpoint_before_or_after() {
        // 10 single-literal tokens: '0' to '9'
        let c = crate::params::control_from_literals(1).unwrap();
        let controls = [c; 10];
        let literals = b"0123456789";

        // Two checkpoints: at position 0 and position 8
        let cp0 = Checkpoint {
            control: 0,
            literal: 0,
            distance_bit: 0,
            position: 0,
        };
        let cp8 = Checkpoint {
            control: 8,
            literal: 8,
            distance_bit: 0,
            position: 8,
        };
        let checkpoints = [cp0, cp8];

        let header = Header {
            controls_offset: 0,
            literals_offset: 0,
            distances_offset: 0,
            checkpoints_offset: 0,
            num_checkpoints: 2,
            controls: &controls,
            literals,
            distances: &[],
            checkpoints: &checkpoints,
        };

        let mut checkpoint = Checkpoint::default();

        // Target position 7:
        // cp0 is at 0 (distance = 7)
        // cp8 is at 8 (distance = 1)
        // cp8 is closer, so it jumps to cp8 and calls prev() once!
        assert_eq!(checkpoint.read_byte(7, &header), b'7');

        // Target position 1:
        // cp0 is at 0 (distance = 1)
        // cp8 is at 8 (distance = 7)
        // cp0 is closer, so it jumps to cp0 and calls next() once!
        assert_eq!(checkpoint.read_byte(1, &header), b'1');
    }
}
