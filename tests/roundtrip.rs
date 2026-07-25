use blaris_compress::compress::{compress, compress_bound};
use blaris_core::params::Params;
use blaris_decompress::decompress::decompress;

fn roundtrip(data: &[u8], params: &Params) {
    let mut compressed = vec![0u8; compress_bound(data.len())];
    let compressed_len = compress(data, &mut compressed, params);
    compressed.truncate(compressed_len);

    let mut output = vec![0u8; data.len()];
    let ok = decompress(&compressed, &mut output, 0);

    assert!(
        ok,
        "decompress returned false for input {:?} (compressed: {:?})",
        data, compressed
    );
    assert_eq!(
        output, data,
        "roundtrip mismatch for input {:?} (compressed: {:?})",
        data, compressed
    );
}

#[test]
fn empty_input() {
    roundtrip(&[], &Params::default());
}

#[test]
fn single_byte() {
    roundtrip(&[42], &Params::default());
}

#[test]
fn no_repeats_all_literals() {
    roundtrip(b"abcdefghijklmnop", &Params::default());
}

#[test]
fn simple_repeat() {
    roundtrip(b"abcabcabcabc", &Params::default());
}

#[test]
fn long_run_of_same_byte() {
    roundtrip(&[b'x'; 200], &Params::default());
}

#[test]
fn mixed_literals_and_matches() {
    roundtrip(
        b"the quick brown fox jumps over the lazy dog. the quick brown fox is quick.",
        &Params::default(),
    );
}

#[test]
fn near_max_literal_run() {
    // max_literals defaults to 16; push a run just past that boundary
    let data: Vec<u8> = (0..40).map(|i| (i * 37 % 251) as u8).collect();
    roundtrip(&data, &Params::default());
}

#[test]
fn distance_boundaries() {
    // Force matches at varying distances to exercise distance-bit encoding
    let mut data = Vec::new();
    for pattern_len in 1..=8 {
        let pattern: Vec<u8> = (0..pattern_len).map(|i| i as u8).collect();
        for _ in 0..5 {
            data.extend_from_slice(&pattern);
        }
    }
    roundtrip(&data, &Params::default());
}

#[test]
fn repeated_across_full_buffer() {
    let data = b"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA".to_vec();
    roundtrip(&data, &Params::default());
}

#[test]
fn custom_params_small_literals() {
    let params = Params {
        max_literals: 4,
        max_length: 8,
        max_distance_bits: 10,
    };
    roundtrip(b"hello world, hello world, hello again!", &params);
}

#[test]
fn partial_decompress_with_offset() {
    // Decompress only a slice of the original data starting mid-buffer
    let data = b"the quick brown fox jumps over the lazy dog";
    let params = Params::default();
    let mut compressed = vec![0u8; compress_bound(data.len())];
    let compressed_len = compress(data, &mut compressed, &params);
    compressed.truncate(compressed_len);

    let start = 10;
    let want = &data[start..start + 8];
    let mut output = vec![0u8; 8];
    let ok = decompress(&compressed, &mut output, start);

    assert!(ok, "partial decompress failed");
    assert_eq!(&output[..], want);
}

#[test]
fn self_referential_match_distance_less_than_length() {
    // "ab" repeated many times forces a match whose distance (2)
    // is smaller than its length, requiring the match to reference
    // into itself rather than an earlier block.
    let data = b"ababababababababab".to_vec();
    roundtrip(&data, &Params::default());
}
