use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use embedded_heatshrink::{
    HSDFinishRes, HSDPollRes, HSDSinkRes, HSEFinishRes, HSEPollRes, HSESinkRes, HeatshrinkDecoder,
    HeatshrinkEncoder,
};

const DECOMPRESS_RUNS: usize = 5;
const POSITIONS_PCT: &[usize] = &[1, 10, 20, 30, 40, 50, 60, 70, 80, 90];

#[derive(Debug, Clone)]
enum TargetConfig {
    Blaris,
    Heatshrink { window_bits: u8, lookahead_bits: u8 },
}

fn format_ms(d: Duration) -> String {
    format!("{:.3} ms", d.as_secs_f64() * 1000.0)
}

impl TargetConfig {
    fn name(&self) -> String {
        match self {
            TargetConfig::Blaris => "Blaris".to_string(),
            TargetConfig::Heatshrink { window_bits, .. } => {
                format!("Heatshrink (W={})", window_bits)
            }
        }
    }

    fn estimated_memory_usage(&self) -> String {
        match self {
            TargetConfig::Blaris => "<100 B".to_string(),

            TargetConfig::Heatshrink { window_bits, .. } => {
                let window_size = 1usize << window_bits;

                // Only decoder state/history buffer.
                // Excludes benchmark scratch buffers and output buffers.
                format!("~{} B", window_size)
            }
        }
    }
}

fn blaris_compress_vec(input: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; blaris_compress::compress::compress_bound(input.len())];
    let sz = blaris_compress::compress::compress(input, &mut out);
    out.truncate(sz);
    out
}

fn blaris_decompress_range_vec(compressed: &[u8], offset: usize, len: usize) -> Vec<u8> {
    let mut out = vec![0u8; len];
    blaris_decompress::decompress::decompress(compressed, &mut out, offset);
    out
}

fn blaris_decompress_full_vec(compressed: &[u8], expected_len: usize) -> Vec<u8> {
    blaris_decompress_range_vec(compressed, 0, expected_len)
}

fn heatshrink_compress(input: &[u8], window_bits: u8, lookahead_bits: u8) -> Vec<u8> {
    let mut encoder =
        HeatshrinkEncoder::new(window_bits, lookahead_bits).expect("valid heatshrink params");

    let mut output = Vec::new();
    let mut scratch = vec![0u8; 1usize << window_bits];

    let mut in_pos = 0;

    while in_pos < input.len() {
        let mut sunk = 0;

        if let HSESinkRes::Ok(n) = encoder.sink(&input[in_pos..]) {
            sunk = n;
            in_pos += n;
        }

        loop {
            match encoder.poll(&mut scratch) {
                HSEPollRes::Empty(sz) => {
                    output.extend_from_slice(&scratch[..sz]);

                    if sunk == 0 {
                        break;
                    }

                    break;
                }

                HSEPollRes::More(sz) => {
                    output.extend_from_slice(&scratch[..sz]);
                }

                _ => break,
            }
        }
    }

    loop {
        match encoder.finish() {
            HSEFinishRes::Done => break,
            HSEFinishRes::More => {}

            _ => break,
        }

        loop {
            match encoder.poll(&mut scratch) {
                HSEPollRes::Empty(sz) => {
                    output.extend_from_slice(&scratch[..sz]);
                    break;
                }

                HSEPollRes::More(sz) => {
                    output.extend_from_slice(&scratch[..sz]);
                }

                _ => break,
            }
        }
    }

    output
}

fn heatshrink_decompress_full(compressed: &[u8], window_bits: u8, lookahead_bits: u8) -> Vec<u8> {
    let input_buffer_size = 1u16 << window_bits;

    let mut decoder = HeatshrinkDecoder::new(input_buffer_size, window_bits, lookahead_bits)
        .expect("valid heatshrink params");

    let mut output = Vec::new();
    let mut scratch = vec![0u8; 1usize << window_bits];

    let mut in_pos = 0;

    while in_pos < compressed.len() {
        let mut sunk = 0;

        if let HSDSinkRes::Ok(n) = decoder.sink(&compressed[in_pos..]) {
            sunk = n;
            in_pos += n;
        }

        loop {
            match decoder.poll(&mut scratch) {
                HSDPollRes::Empty(sz) => {
                    output.extend_from_slice(&scratch[..sz]);

                    if sunk == 0 {
                        break;
                    }

                    break;
                }

                HSDPollRes::More(sz) => {
                    output.extend_from_slice(&scratch[..sz]);
                }

                _ => break,
            }
        }
    }

    loop {
        match decoder.finish() {
            HSDFinishRes::Done => break,
            HSDFinishRes::More => {}

            _ => break,
        }

        loop {
            match decoder.poll(&mut scratch) {
                HSDPollRes::Empty(sz) => {
                    output.extend_from_slice(&scratch[..sz]);
                    break;
                }

                HSDPollRes::More(sz) => {
                    output.extend_from_slice(&scratch[..sz]);
                }

                _ => break,
            }
        }
    }

    output
}

fn heatshrink_decompress_range(
    compressed: &[u8],
    window_bits: u8,
    lookahead_bits: u8,
    offset: usize,
    len: usize,
) -> Vec<u8> {
    let input_buffer_size = 1u16 << window_bits;

    let mut decoder = HeatshrinkDecoder::new(input_buffer_size, window_bits, lookahead_bits)
        .expect("valid heatshrink params");

    let mut target_buf = vec![0u8; len];

    // Scratch buffer is only benchmark I/O staging.
    // It is intentionally not counted in memory estimation.
    let mut scratch = vec![0u8; 1usize << window_bits];

    let mut in_pos = 0;
    let mut decompressed_bytes_so_far = 0;

    let target_end = offset + len;

    'outer: while in_pos < compressed.len() {
        let mut sunk = 0;

        if let HSDSinkRes::Ok(n) = decoder.sink(&compressed[in_pos..]) {
            sunk = n;
            in_pos += n;
        }

        loop {
            match decoder.poll(&mut scratch) {
                HSDPollRes::Empty(sz) | HSDPollRes::More(sz) => {
                    if sz > 0 {
                        let chunk_start = decompressed_bytes_so_far;
                        let chunk_end = chunk_start + sz;

                        if chunk_end > offset && chunk_start < target_end {
                            let overlap_start = chunk_start.max(offset);
                            let overlap_end = chunk_end.min(target_end);

                            let src_start = overlap_start - chunk_start;
                            let src_end = overlap_end - chunk_start;

                            let dst_start = overlap_start - offset;
                            let dst_end = overlap_end - offset;

                            target_buf[dst_start..dst_end]
                                .copy_from_slice(&scratch[src_start..src_end]);
                        }

                        decompressed_bytes_so_far += sz;

                        if decompressed_bytes_so_far >= target_end {
                            break 'outer;
                        }
                    }

                    if matches!(decoder.poll(&mut []), _) {
                        if sunk == 0 {
                            break;
                        }
                    }

                    break;
                }

                _ => break 'outer,
            }
        }
    }

    target_buf
}

fn compress_data(target: &TargetConfig, input: &[u8]) -> (Vec<u8>, Duration) {
    let start = Instant::now();

    let compressed = match target {
        TargetConfig::Blaris => blaris_compress_vec(input),

        TargetConfig::Heatshrink {
            window_bits,
            lookahead_bits,
        } => heatshrink_compress(input, *window_bits, *lookahead_bits),
    };

    let duration = start.elapsed();

    (compressed, duration)
}

fn decompress_range(
    target: &TargetConfig,
    compressed: &[u8],
    offset: usize,
    len: usize,
) -> Vec<u8> {
    match target {
        TargetConfig::Blaris => blaris_decompress_range_vec(compressed, offset, len),

        TargetConfig::Heatshrink {
            window_bits,
            lookahead_bits,
        } => heatshrink_decompress_range(compressed, *window_bits, *lookahead_bits, offset, len),
    }
}

fn decompress_full(target: &TargetConfig, compressed: &[u8], expected_len: usize) -> Vec<u8> {
    match target {
        TargetConfig::Blaris => blaris_decompress_full_vec(compressed, expected_len),

        TargetConfig::Heatshrink {
            window_bits,
            lookahead_bits,
        } => heatshrink_decompress_full(compressed, *window_bits, *lookahead_bits),
    }
}

fn measure_range_mean(
    target: &TargetConfig,
    compressed: &[u8],
    offset: usize,
    len: usize,
) -> Duration {
    let mut total = Duration::ZERO;

    for _ in 0..DECOMPRESS_RUNS {
        let start = Instant::now();

        let out = decompress_range(target, compressed, offset, len);

        total += start.elapsed();

        std::hint::black_box(out);
    }

    total / (DECOMPRESS_RUNS as u32)
}

fn measure_full_mean(target: &TargetConfig, compressed: &[u8], expected_len: usize) -> Duration {
    let mut total = Duration::ZERO;

    for _ in 0..DECOMPRESS_RUNS {
        let start = Instant::now();

        let out = decompress_full(target, compressed, expected_len);

        total += start.elapsed();

        std::hint::black_box(out);
    }

    total / (DECOMPRESS_RUNS as u32)
}

fn benchmark_file(file_path: &Path, targets: &[TargetConfig]) {
    let file_name = file_path.file_name().unwrap().to_string_lossy();

    let raw_data = fs::read(file_path).expect("failed to read test file");

    let file_len = raw_data.len();

    println!("\n### File: {} ({} bytes)\n", file_name, file_len);

    println!(
        "{:<22} {:>12} {:>10} {:>8} {:>10} {:>10} {:>10} {:>12} {:>15}",
        "Decoder", "Comp ms", "Size", "Ratio", "1B ms", "64B ms", "4KiB ms", "Full ms", "Memory"
    );

    println!(
        "{:-<22} {:->12} {:->10} {:->8} {:->10} {:->10} {:->10} {:->12} {:->15}",
        "", "", "", "", "", "", "", "", ""
    );

    for target in targets {
        let target_name = target.name();

        let (compressed, comp_dur) = compress_data(target, &raw_data);

        let comp_size = compressed.len();

        let ratio = (comp_size as f64 / file_len as f64) * 100.0;

        let mut byte_durations = Vec::new();

        for &pct in POSITIONS_PCT {
            let offset = (file_len * pct) / 100;

            let len = 1.min(file_len.saturating_sub(offset));

            if len > 0 {
                byte_durations.push(measure_range_mean(target, &compressed, offset, len));
            }
        }

        let mean_1b = if !byte_durations.is_empty() {
            byte_durations.iter().sum::<Duration>() / (byte_durations.len() as u32)
        } else {
            Duration::ZERO
        };

        let mut chunk_64_durations = Vec::new();

        for &pct in POSITIONS_PCT {
            let offset = (file_len * pct) / 100;

            let len = 64.min(file_len.saturating_sub(offset));

            if len > 0 {
                chunk_64_durations.push(measure_range_mean(target, &compressed, offset, len));
            }
        }

        let mean_64b = if !chunk_64_durations.is_empty() {
            chunk_64_durations.iter().sum::<Duration>() / (chunk_64_durations.len() as u32)
        } else {
            Duration::ZERO
        };

        let mut chunk_4k_durations = Vec::new();

        for &pct in POSITIONS_PCT {
            let offset = (file_len * pct) / 100;

            let len = 4096.min(file_len.saturating_sub(offset));

            if len > 0 {
                chunk_4k_durations.push(measure_range_mean(target, &compressed, offset, len));
            }
        }

        let mean_4kb = if !chunk_4k_durations.is_empty() {
            chunk_4k_durations.iter().sum::<Duration>() / (chunk_4k_durations.len() as u32)
        } else {
            Duration::ZERO
        };

        let full_dur = measure_full_mean(target, &compressed, file_len);

        let est_mem = target.estimated_memory_usage();

        println!(
            "{:<22} {:>12} {:>10} {:>8.2}% {:>10} {:>10} {:>10} {:>12} {:>15}",
            target_name,
            format_ms(comp_dur),
            comp_size,
            ratio,
            format_ms(mean_1b),
            format_ms(mean_64b),
            format_ms(mean_4kb),
            format_ms(full_dur),
            est_mem
        );
    }
}

fn get_test_data_dir() -> PathBuf {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));

    manifest_dir.join("test_data")
}

fn main() {
    let test_dir = get_test_data_dir();

    if !test_dir.exists() {
        eprintln!("Test directory not found at {:?}", test_dir);

        std::process::exit(1);
    }

    let targets = vec![
        TargetConfig::Blaris,
        TargetConfig::Heatshrink {
            window_bits: 6,
            lookahead_bits: 4,
        },
        TargetConfig::Heatshrink {
            window_bits: 7,
            lookahead_bits: 4,
        },
        TargetConfig::Heatshrink {
            window_bits: 8,
            lookahead_bits: 4,
        },
        TargetConfig::Heatshrink {
            window_bits: 10,
            lookahead_bits: 4,
        },
        TargetConfig::Heatshrink {
            window_bits: 12,
            lookahead_bits: 4,
        },
        TargetConfig::Heatshrink {
            window_bits: 14,
            lookahead_bits: 4,
        },
    ];

    let mut files: Vec<PathBuf> = fs::read_dir(&test_dir)
        .expect("unable to read test directory")
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|path| path.is_file())
        .collect();

    files.sort();

    if files.is_empty() {
        println!("No test files found in {:?}", test_dir);

        return;
    }

    for file in &files {
        benchmark_file(file, &targets);
    }
}
