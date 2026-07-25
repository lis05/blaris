use blaris_core::params::Params;

pub fn compress_bound(len: usize) -> usize {
    return len * 2 + 16;
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

pub fn compress(from: &[u8], mut to: &mut [u8], params: &Params) -> usize {
    assert!(to.len() >= compress_bound(from.len()));

    let max_literals: usize = params.max_literals.into();
    let max_length: usize = params.max_length.into();
    let max_distance: usize = (1_usize << params.max_distance_bits) - 1 + 1;

    let bits_of = |val: usize| if val == 0 { 1 } else { val.ilog2() + 1 };

    let cost_of_literals = |count| 8 + count * 8;
    let cost_of_match = |distance: usize| 8 + bits_of(distance - 1) + 1;

    let n = from.len();

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

        for distance in 1..std::cmp::min(i, max_distance) + 1 {
            let ii = i - distance;
            let match_len = strmatch(&from[ii..], &from[i..], max_length);
            assert!(match_len <= max_length);

            if sheet[match_len] == u32::MAX
                || cost_of_match(sheet[match_len] as usize) > cost_of_match(distance)
            {
                sheet[match_len] = distance.try_into().unwrap();
            }

            if sheet[max_length] != u32::MAX {
                break;
            }
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
                || dp_cost[i + length] > dp_cost[i] + cost_of_match(sheet[length] as usize) as u32
            {
                dp_cost[i + length] = dp_cost[i] + cost_of_match(sheet[length] as usize) as u32;
                dp_from[i + length] = i;
                dp_distance[i + length] = sheet[length];
            }
        }

        for count in 1..max_literals + 1 {
            if i + count > n {
                break;
            }

            if dp_cost[i + count] == u32::MAX
                || dp_cost[i + count] > dp_cost[i] + cost_of_literals(count) as u32
            {
                dp_cost[i + count] = dp_cost[i] + cost_of_literals(count) as u32;
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

            controls.push(params.control_from_literals(i - cur_from).unwrap());
            for ii in 0..(i - cur_from) {
                literals.push(from[i - ii - 1]);
            }
        } else {
            assert!(cur_distance <= max_distance as u32);
            assert!(i - cur_from <= max_length);

            controls.push(
                params
                    .control_from_match(
                        i - cur_from,
                        bits_of((cur_distance - 1) as usize).try_into().unwrap(),
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

    // Write parameters first
    assert!(params.write_to(to));
    let (_, after) = to.split_at_mut(Params::LENGTH);
    to = after;

    let (before, after) = to.split_at_mut(4);
    before.copy_from_slice(&(controls.len() as u32).to_le_bytes());
    to = after;

    let (before, after) = to.split_at_mut(4);
    before.copy_from_slice(&((controls.len() + literals.len()) as u32).to_le_bytes());
    to = after;

    let (before, after) = to.split_at_mut(controls.len());
    before.copy_from_slice(&controls);
    to = after;

    let (before, after) = to.split_at_mut(literals.len());
    before.copy_from_slice(&literals);
    to = after;

    let mut bits_written: usize = 0;
    for distance in distances {
        let bits = bits_of((distance - 1) as usize);
        write_u32_at_bit_offset(to, bits_written, bits as usize, distance - 1);
        bits_written += bits as usize;
    }

    return Params::LENGTH + 8 + controls.len() + literals.len() + (bits_written + 7) / 8;
}
