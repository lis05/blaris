use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use embedded_heatshrink::{
    HSDFinishRes, HSDPollRes, HSDSinkRes, HSEFinishRes, HSEPollRes, HSESinkRes, HeatshrinkDecoder,
    HeatshrinkEncoder,
};

/// Positions (as % of file length) at which we extract a small window.
const POSITIONS_PCT: &[usize] = &[1, 20, 40, 60, 80, 100];
/// Size of the extracted window, in bytes.
const EXTRACT_LEN: usize = 32;
/// Number of repeated decompressions per position, averaged.
const RUNS: usize = 20;

// Column widths, shared between header and rows so nothing can drift out of alignment.
const W_NAME: usize = 10;
const W_COMP: usize = 8;
const W_SIZE: usize = 8;
const W_RATIO: usize = 7;
const W_POS: usize = 9;
const W_MEM: usize = 9;

#[derive(Debug, Clone)]
enum TargetConfig {
    Blaris,
    Heatshrink { window_bits: u8, lookahead_bits: u8 },
}

fn format_ms(d: Duration) -> String {
    format!("{:.2}ms", d.as_secs_f64() * 1000.0)
}

fn format_us(d: Duration) -> String {
    format!("{:.1}us", d.as_secs_f64() * 1_000_000.0)
}

impl TargetConfig {
    fn name(&self) -> String {
        match self {
            TargetConfig::Blaris => "Blaris".to_string(),
            TargetConfig::Heatshrink { window_bits, .. } => format!("HS(W={})", window_bits),
        }
    }

    /// Estimated real memory footprint.
    ///
    /// Heatshrink's decoder needs both an input buffer *and* a window/history
    /// buffer, each sized `1 << window_bits`. So real usage is ~2x the window
    /// size, not just the window size itself. To target a given memory
    /// budget `X`, configure the window so that `window_size == X / 2`.
    fn estimated_memory_bytes(&self) -> usize {
        match self {
            TargetConfig::Blaris => 64, // O(1), just a handful of state vars
            TargetConfig::Heatshrink { window_bits, .. } => {
                let window_size = 1usize << window_bits;
                window_size * 2
            }
        }
    }

    fn estimated_memory_str(&self) -> String {
        let bytes = self.estimated_memory_bytes();
        if bytes < 1024 {
            format!("~{}B", bytes)
        } else {
            format!("~{:.1}KiB", bytes as f64 / 1024.0)
        }
    }
}

/// One fully-measured row of the results table.
struct Row {
    name: String,
    comp_dur: Duration,
    comp_size: usize,
    ratio: f64,
    extract_means: Vec<Duration>, // one per POSITIONS_PCT entry, same order
    mem_str: String,
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

fn heatshrink_compress(input: &[u8], window_bits: u8, lookahead_bits: u8) -> Vec<u8> {
    let mut encoder =
        HeatshrinkEncoder::new(window_bits, lookahead_bits).expect("valid heatshrink params");

    let mut output = Vec::new();
    let mut scratch = vec![0u8; 1usize << window_bits];

    let mut in_pos = 0;

    while in_pos < input.len() {
        if let HSESinkRes::Ok(n) = encoder.sink(&input[in_pos..]) {
            in_pos += n;
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

/// Decompress a `len`-byte window starting at `offset` from `compressed`.
///
/// Heatshrink has no structural random access: it must stream-decode from
/// the start and discard bytes before `offset`. This function reflects that
/// honestly rather than trying to "seek".
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

                    if sunk == 0 {
                        break;
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

    (compressed, start.elapsed())
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

/// Mean extraction time of `EXTRACT_LEN` bytes at position `pct`%, over `RUNS` runs.
fn measure_extract_mean(
    target: &TargetConfig,
    compressed: &[u8],
    file_len: usize,
    pct: usize,
) -> Duration {
    if file_len == 0 {
        return Duration::ZERO;
    }

    let len = EXTRACT_LEN.min(file_len);
    let raw_offset = (file_len * pct) / 100;
    let offset = raw_offset.min(file_len - len);

    // Warm-up (not measured).
    let out = decompress_range(target, compressed, offset, len);
    std::hint::black_box(out);

    let mut total = Duration::ZERO;
    for _ in 0..RUNS {
        let start = Instant::now();
        let out = decompress_range(target, compressed, offset, len);
        total += start.elapsed();
        std::hint::black_box(out);
    }

    total / (RUNS as u32)
}

fn print_header() {
    print!(
        "{:<w_name$} {:>w_comp$} {:>w_size$} {:>w_ratio$} ",
        "decoder",
        "comp",
        "size",
        "ratio",
        w_name = W_NAME,
        w_comp = W_COMP,
        w_size = W_SIZE,
        w_ratio = W_RATIO,
    );
    for pct in POSITIONS_PCT {
        print!("{:>w_pos$} ", format!("@{pct}%"), w_pos = W_POS);
    }
    println!("{:>w_mem$}", "mem", w_mem = W_MEM);

    let total_width = W_NAME
        + 1
        + W_COMP
        + 1
        + W_SIZE
        + 1
        + W_RATIO
        + 1
        + (W_POS + 1) * POSITIONS_PCT.len()
        + W_MEM;
    println!("{}", "-".repeat(total_width));
}

fn print_row(row: &Row) {
    print!(
        "{:<w_name$} {:>w_comp$} {:>w_size$} {:>w_ratio$} ",
        row.name,
        format_ms(row.comp_dur),
        row.comp_size,
        format!("{:.1}%", row.ratio),
        w_name = W_NAME,
        w_comp = W_COMP,
        w_size = W_SIZE,
        w_ratio = W_RATIO,
    );

    for d in &row.extract_means {
        print!("{:>w_pos$} ", format_us(*d), w_pos = W_POS);
    }

    println!("{:>w_mem$}", row.mem_str, w_mem = W_MEM);
}

fn benchmark_file(file_path: &Path, targets: &[TargetConfig]) {
    let file_name = file_path.file_name().unwrap().to_string_lossy();
    let raw_data = fs::read(file_path).expect("failed to read test file");
    let file_len = raw_data.len();

    println!("\n{file_name}  ({file_len} B)");

    let mut rows: Vec<Row> = targets
        .iter()
        .map(|target| {
            let (compressed, comp_dur) = compress_data(target, &raw_data);
            let comp_size = compressed.len();
            let ratio = (comp_size as f64 / file_len.max(1) as f64) * 100.0;

            let extract_means = POSITIONS_PCT
                .iter()
                .map(|&pct| measure_extract_mean(target, &compressed, file_len, pct))
                .collect();

            Row {
                name: target.name(),
                comp_dur,
                comp_size,
                ratio,
                extract_means,
                mem_str: target.estimated_memory_str(),
            }
        })
        .collect();

    rows.sort_by(|a, b| a.ratio.partial_cmp(&b.ratio).unwrap());

    print_header();
    for row in &rows {
        print_row(row);
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

    // Heatshrink window_bits chosen so that estimated_memory_bytes()
    // (== 2x window) lands on round-ish targets, from 128B up to 32KiB.
    let targets = vec![
        TargetConfig::Blaris,
        TargetConfig::Heatshrink {
            window_bits: 6,
            lookahead_bits: 4,
        }, // ~128B
        TargetConfig::Heatshrink {
            window_bits: 7,
            lookahead_bits: 4,
        }, // ~256B
        TargetConfig::Heatshrink {
            window_bits: 8,
            lookahead_bits: 4,
        }, // ~512B
        TargetConfig::Heatshrink {
            window_bits: 9,
            lookahead_bits: 4,
        }, // ~1KiB
        TargetConfig::Heatshrink {
            window_bits: 10,
            lookahead_bits: 4,
        }, // ~2KiB
        TargetConfig::Heatshrink {
            window_bits: 11,
            lookahead_bits: 4,
        }, // ~4KiB
        TargetConfig::Heatshrink {
            window_bits: 12,
            lookahead_bits: 4,
        }, // ~8KiB
        TargetConfig::Heatshrink {
            window_bits: 13,
            lookahead_bits: 4,
        }, // ~16KiB
        TargetConfig::Heatshrink {
            window_bits: 14,
            lookahead_bits: 4,
        }, // ~32KiB
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
