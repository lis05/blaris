use blaris_core::{params, Checkpoint, Header};

pub fn compress_bound(len: usize, num_checkpoints: u32) -> usize {
    len * 2 + Header::SIZE + (num_checkpoints as usize) * Checkpoint::SIZE + 16
}

fn strmatch(a: &[u8], b: &[u8], limit: usize) -> usize {
    let n = std::cmp::min(limit, std::cmp::min(a.len(), b.len()));

    for i in 0..n {
        if a[i] != b[i] {
            return i;
        }
    }

    n
}

pub fn write_u32_at_bit_offset(
    buf: &mut [u8],
    global_bit_offset: usize,
    num_bits: usize,
    value: u32,
) {
    assert!(num_bits <= 32, "num_bits cannot exceed 32");

    if num_bits == 0 {
        return;
    }

    let byte_offset = global_bit_offset / 8;
    let bit_offset = global_bit_offset % 8;

    let mask = if num_bits == 32 {
        0xFFFF_FFFF_u64
    } else {
        (1u64 << num_bits) - 1
    };
    let value_masked = (value as u64) & mask;

    let value_shifted = value_masked << bit_offset;
    let window_mask = mask << bit_offset;

    for i in 0..5 {
        let byte_idx = byte_offset + i;
        if byte_idx >= buf.len() {
            break;
        }

        let byte_mask = ((window_mask >> (i * 8)) & 0xFF) as u8;
        let byte_val = ((value_shifted >> (i * 8)) & 0xFF) as u8;

        if byte_mask != 0 {
            buf[byte_idx] = (buf[byte_idx] & !byte_mask) | (byte_val & byte_mask);
        }
    }
}

pub fn compress(from: &[u8], to: &mut [u8], num_checkpoints: u32) -> usize {
    assert!(to.len() >= compress_bound(from.len(), num_checkpoints));

    let max_literals: usize = params::MAX_LITERALS;
    let max_length: usize = params::MAX_LENGTH;
    let max_distance: usize = 1usize << params::MAX_DISTANCE_BITS;

    let bits_of = |val: usize| if val == 0 { 1 } else { val.ilog2() + 1 };

    let cost_of_literals = |count: usize| 8u32 + count as u32 * 8;
    let cost_of_match = |distance: usize| 8u32 + bits_of(distance - 1) + 1;

    let n = from.len();

    let mut chain = std::vec![usize::MAX; n + 1];
    let mut table = std::vec![usize::MAX; 256];

    for i in 0..n {
        let prev = table[from[i] as usize];
        if prev != usize::MAX {
            chain[i] = prev;
        }
        table[from[i] as usize] = i;
    }

    let mut dp_cost = std::vec![u32::MAX; n + 1];
    let mut dp_from = std::vec![usize::MAX; n + 1];
    let mut dp_distance = std::vec![u32::MAX; n + 1]; // 0 means literals

    let mut sheet = std::vec![u32::MAX; max_length + 1];

    dp_cost[0] = 0;
    dp_from[0] = 0;
    dp_distance[0] = 0;

    for i in 0..n + 1 {
        assert!(dp_cost[i] != u32::MAX);
        sheet.fill(u32::MAX);

        let mut pos = chain[i];
        while pos != usize::MAX {
            let distance = i - pos;
            if distance > max_distance {
                break;
            }
            let match_len = strmatch(&from[pos..], &from[i..], max_length);
            assert!(match_len <= max_length);

            if sheet[match_len] == u32::MAX
                || cost_of_match(sheet[match_len] as usize) > cost_of_match(distance)
            {
                sheet[match_len] = distance.try_into().unwrap();
            }

            if sheet[max_length] != u32::MAX {
                break;
            }

            pos = chain[pos];
        }

        for length in (1..max_length).rev() {
            if sheet[length + 1] != u32::MAX
                && (sheet[length] == u32::MAX
                    || cost_of_match(sheet[length] as usize)
                        > cost_of_match(sheet[length + 1] as usize))
            {
                sheet[length] = sheet[length + 1];
            }
        }

        for length in 1..max_length + 1 {
            if i + length > n || sheet[length] == u32::MAX {
                break;
            }

            if dp_cost[i + length] == u32::MAX
                || dp_cost[i + length] > dp_cost[i] + cost_of_match(sheet[length] as usize)
            {
                dp_cost[i + length] = dp_cost[i] + cost_of_match(sheet[length] as usize);
                dp_from[i + length] = i;
                dp_distance[i + length] = sheet[length];
            }
        }

        for count in 1..max_literals + 1 {
            if i + count > n {
                break;
            }

            if dp_cost[i + count] == u32::MAX
                || dp_cost[i + count] > dp_cost[i] + cost_of_literals(count)
            {
                dp_cost[i + count] = dp_cost[i] + cost_of_literals(count);
                dp_from[i + count] = i;
                dp_distance[i + count] = 0;
            }
        }
    }

    let mut controls = std::vec::Vec::<u8>::new();
    let mut literals = std::vec::Vec::<u8>::new();
    let mut distances = std::vec::Vec::<u32>::new();

    let mut i = n;
    while i != 0 {
        let cur_cost = dp_cost[i];
        let cur_from = dp_from[i];
        let cur_distance = dp_distance[i];

        assert!(cur_cost != u32::MAX);
        assert!(cur_from != usize::MAX);

        if cur_distance == 0 {
            assert!(i - cur_from <= max_literals);

            controls.push(params::control_from_literals(i - cur_from).unwrap());
            for ii in 0..(i - cur_from) {
                literals.push(from[i - ii - 1]);
            }
        } else {
            assert!(cur_distance <= max_distance as u32);
            assert!(i - cur_from <= max_length);

            controls.push(
                params::control_from_match(
                    i - cur_from,
                    bits_of((cur_distance - 1) as usize) as usize,
                )
                .unwrap(),
            );
            distances.push(cur_distance);
        }
        i = cur_from;
    }

    controls.reverse();
    literals.reverse();
    distances.reverse();

    // 1. Calculate evenly-spread checkpoints
    let mut checkpoints = std::vec::Vec::with_capacity(num_checkpoints as usize);
    if num_checkpoints > 0 {
        let step = if n > 0 {
            (n as f64) / (num_checkpoints as f64)
        } else {
            0.0
        };
        let mut cur = Checkpoint::default();
        let mut next_target = 0usize;

        for &c in &controls {
            while next_target < num_checkpoints as usize
                && cur.position >= (next_target as f64 * step) as u32
            {
                checkpoints.push(cur);
                next_target += 1;
            }

            let num_literals = params::literals_from_control(c);
            if num_literals != 0 {
                cur.literal += num_literals as u32;
                cur.position += num_literals as u32;
            } else {
                let (length, distance_bits) = params::match_from_control(c);
                cur.distance_bit += distance_bits as u32;
                cur.position += length as u32;
            }
            cur.control += 1;
        }

        while next_target < num_checkpoints as usize {
            checkpoints.push(cur);
            next_target += 1;
        }
    }

    // 2. Compute stream offsets and layout
    let controls_offset = Header::SIZE as u32;
    let literals_offset = controls_offset + controls.len() as u32;
    let distances_offset = literals_offset + literals.len() as u32;

    let mut bits_written: usize = 0;
    for &distance in &distances {
        bits_written += bits_of((distance - 1) as usize) as usize;
    }
    let distances_bytes = bits_written.div_ceil(8);
    let distances_end = distances_offset + distances_bytes as u32;

    // Checkpoints must be aligned to 4 bytes
    let padding = (4 - (distances_end % 4)) % 4;
    let checkpoints_offset = distances_end + padding;

    // 3. Create header and store it
    let header = Header {
        controls_offset,
        literals_offset,
        distances_offset,
        checkpoints_offset,
        num_checkpoints: checkpoints.len() as u32,
        controls: &controls,
        literals: &literals,
        distances: &[],
        checkpoints: &checkpoints,
    };
    header.store(&mut to[..Header::SIZE]);

    // 4. Write controls & literals
    to[controls_offset as usize..literals_offset as usize].copy_from_slice(&controls);
    to[literals_offset as usize..distances_offset as usize].copy_from_slice(&literals);

    // 5. Write bit-packed distances
    let distances_slice = &mut to[distances_offset as usize..distances_end as usize];
    distances_slice.fill(0);
    let mut bit_offset: usize = 0;
    for distance in distances {
        let bits = bits_of((distance - 1) as usize) as usize;
        write_u32_at_bit_offset(distances_slice, bit_offset, bits, distance - 1);
        bit_offset += bits;
    }

    // Zero padding between distances and checkpoints
    if padding > 0 {
        to[distances_end as usize..checkpoints_offset as usize].fill(0);
    }

    // 6. Write checkpoints
    let mut cp_dest = checkpoints_offset as usize;
    for cp in &checkpoints {
        cp.store(&mut to[cp_dest..cp_dest + Checkpoint::SIZE]);
        cp_dest += Checkpoint::SIZE;
    }

    checkpoints_offset as usize + checkpoints.len() * Checkpoint::SIZE
}
