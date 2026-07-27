use blaris_core::params;
use rayon::prelude::*;

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

fn bits_of(val: usize) -> u32 {
    if val == 0 {
        1
    } else {
        val.ilog2() + 1
    }
}

fn cost_of_match(distance: usize) -> u32 {
    8 + bits_of(distance - 1) + 1
}

// Below the "worth it" threshold, plain thread/task dispatch overhead outweighs
// the win, so small ranges (mostly early positions where i is small) stay
// sequential.
const MIN_DISTANCES_PER_THREAD: usize = 4096;

/// Scans a contiguous half-open range of distances [dist_lo, dist_hi) at position
/// `i`, returning a local sheet: for each match length, the cheapest distance found
/// in *this range* giving a match of exactly that length. Not yet propagated to
/// shorter lengths — that happens once, after all blocks are merged.
fn scan_distance_range(
    from: &[u8],
    i: usize,
    dist_lo: usize,
    dist_hi: usize,
    max_length: usize,
) -> Vec<u32> {
    let mut local_sheet = vec![u32::MAX; max_length + 1];

    for distance in dist_lo..dist_hi {
        let ii = i - distance;
        let match_len = strmatch(&from[ii..], &from[i..], max_length);
        assert!(match_len <= max_length);

        if local_sheet[match_len] == u32::MAX
            || cost_of_match(local_sheet[match_len] as usize) > cost_of_match(distance)
        {
            local_sheet[match_len] = distance.try_into().unwrap();
        }

        // Early-exit within this block only; other blocks still run.
        if local_sheet[max_length] != u32::MAX {
            break;
        }
    }

    local_sheet
}

/// Merges `incoming` into `base`, keeping whichever distance is cheaper at each
/// length. On a tie, `base` wins — callers must merge blocks in ascending-distance
/// order for this to match the original sequential tie-breaking (prefer the
/// smaller/nearer distance).
fn merge_sheet_into(base: &mut [u32], incoming: &[u32]) {
    for len in 0..base.len() {
        if incoming[len] != u32::MAX
            && (base[len] == u32::MAX
                || cost_of_match(base[len] as usize) > cost_of_match(incoming[len] as usize))
        {
            base[len] = incoming[len];
        }
    }
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

pub fn compress(from: &[u8], mut to: &mut [u8]) -> usize {
    assert!(to.len() >= compress_bound(from.len()));

    let max_literals: usize = params::MAX_LITERALS;
    let max_length: usize = params::MAX_LENGTH;
    let max_distance: usize = 1_usize << params::MAX_DISTANCE_BITS;

    let cost_of_literals = |count| 8 + count * 8;

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

        let max_dist_for_i = std::cmp::min(i, max_distance);

        if max_dist_for_i >= MIN_DISTANCES_PER_THREAD * 2 {
            // Parallel path: split [1, max_dist_for_i] into contiguous blocks,
            // scan each independently, then merge in ascending order.
            let num_threads = rayon::current_num_threads().max(1);
            let chunk_size = (max_dist_for_i / num_threads).max(MIN_DISTANCES_PER_THREAD);

            let mut bounds = Vec::new();
            let mut lo = 1;
            while lo <= max_dist_for_i {
                let hi = std::cmp::min(lo + chunk_size, max_dist_for_i + 1);
                bounds.push((lo, hi));
                lo = hi;
            }

            // Vec's into_par_iter is an indexed parallel iterator, so collect()
            // preserves `bounds`' ascending order regardless of which worker
            // thread handled which chunk — required for correct tie-breaking.
            let partials: Vec<Vec<u32>> = bounds
                .into_par_iter()
                .map(|(lo, hi)| scan_distance_range(from, i, lo, hi, max_length))
                .collect();

            for partial in &partials {
                merge_sheet_into(&mut sheet, partial);
            }
        } else {
            // Sequential path for small ranges (mostly small i).
            for distance in 1..max_dist_for_i + 1 {
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

    return 8 + controls.len() + literals.len() + (bits_written + 7) / 8;
}
