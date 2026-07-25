pub struct Params {
    /// Max number of literals per single control (at least 1)
    pub max_literals: u8,

    /// Max length of a match (at least 1)
    pub max_length: u8,

    /// Max bits per distance (at least 1)
    pub max_distance_bits: u8,
}

impl Default for Params {
    fn default() -> Self {
        Self {
            max_literals: 16,
            max_length: 15,
            max_distance_bits: 16,
        }
    }
}

impl Params {
    pub const LENGTH: usize = 3;

    pub fn are_valid(&self) -> bool {
        self.max_literals != 0
            && self.max_length != 0
            && self.max_distance_bits != 0
            && self.max_length <= 30
            && self.max_distance_bits <= 30
            && self.max_literals as usize
                + self.max_length as usize * self.max_distance_bits as usize
                <= 256
    }

    pub fn read_from(&mut self, buf: &[u8]) -> bool {
        if buf.len() < Self::LENGTH {
            return false;
        }

        self.max_literals = buf[0];
        self.max_length = buf[1];
        self.max_distance_bits = buf[2];

        true
    }

    pub fn write_to(&self, buf: &mut [u8]) -> bool {
        if buf.len() < Self::LENGTH {
            return false;
        }

        buf[0] = self.max_literals;
        buf[1] = self.max_length;
        buf[2] = self.max_distance_bits;

        true
    }

    pub fn control_from_literals(&self, count: usize) -> Option<u8> {
        if count == 0 || count > self.max_literals.into() {
            return None;
        }

        Some((count - 1) as u8)
    }

    pub fn control_from_match(&self, length: usize, distance_bits: usize) -> Option<u8> {
        if length == 0 || length > self.max_length.into() {
            return None;
        }
        if distance_bits == 0 || distance_bits > self.max_distance_bits.into() {
            return None;
        }

        let c = (self.max_literals as usize)
            + (length - 1) * (self.max_distance_bits as usize)
            + (distance_bits - 1);

        if c > 255 {
            return None;
        }
        Some(c as u8)
    }

    pub fn literals_from_control(&self, c: u8) -> usize {
        if c < self.max_literals {
            (c + 1).into()
        } else {
            0
        }
    }

    pub fn match_from_control(&self, c: u8) -> (usize, usize) {
        let max_literals = self.max_literals as usize;
        let max_length = self.max_length as usize;
        let max_distance_bits = self.max_distance_bits as usize;
        let c = c as usize;

        if max_literals <= c && c < max_literals + max_length * max_distance_bits {
            (
                (c - max_literals) / max_distance_bits + 1,
                (c - max_literals) % max_distance_bits + 1,
            )
        } else {
            (0, 0)
        }
    }
}
