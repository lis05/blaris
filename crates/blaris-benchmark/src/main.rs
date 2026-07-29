use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

use heatshrink::decoder::HeatshrinkDecoder;
use heatshrink::encoder::HeatshrinkEncoder;
use heatshrink::{Finish, Poll};

/// Positions (as % of file length) at which we extract a small window.
const POSITIONS_PCT: &[usize] = &[1, 20, 40, 60, 80, 100];
/// Size of the extracted window, in bytes.
const EXTRACT_LEN: usize = 32;
/// Number of repeated decompressions per position, averaged.
const RUNS: usize = 20;
/// Local I/O staging buffer for sink/poll loops. Not part of either
/// decoder's real memory footprint — just how much we hand to `poll()`
/// per call.
const SCRATCH_SIZE: usize = 256;
/// heatshrink-lib's decoder streaming input buffer size. Kept fixed across
/// all window sizes (matches the library's own default) so that `mem_str`
/// below isolates the effect of the window size specifically.
const DECODER_I: usize = 32;

// Column widths, shared between header and rows so nothing can drift out of alignment.
const W_NAME: usize = 10;
const W_COMP: usize = 8;
const W_SIZE: usize = 8;
const W_RATIO: usize = 7;
const W_POS: usize = 9;
const W_MEM: usize = 9;

fn format_ms(d: Duration) -> String {
    format!("{:.2}ms", d.as_secs_f64() * 1000.0)
}

fn format_us(d: Duration) -> String {
    format!("{:.1}us", d.as_secs_f64() * 1_000_000.0)
}

fn mem_str(bytes: usize) -> String {
    if bytes < 1024 {
        format!("~{}B", bytes)
    } else {
        format!("~{:.1}KiB", bytes as f64 / 1024.0)
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

// ---------- Blaris ----------

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

fn measure_blaris_extract_mean(compressed: &[u8], file_len: usize, pct: usize) -> Duration {
    if file_len == 0 {
        return Duration::ZERO;
    }

    let len = EXTRACT_LEN.min(file_len);
    let raw_offset = (file_len * pct) / 100;
    let offset = raw_offset.min(file_len - len);

    // Warm-up (not measured).
    let out = blaris_decompress_range_vec(compressed, offset, len);
    std::hint::black_box(out);

    let mut total = Duration::ZERO;
    for _ in 0..RUNS {
        let start = Instant::now();
        let out = blaris_decompress_range_vec(compressed, offset, len);
        total += start.elapsed();
        std::hint::black_box(out);
    }

    total / (RUNS as u32)
}

fn blaris_row(raw_data: &[u8], file_len: usize) -> Row {
    let start = Instant::now();
    let compressed = blaris_compress_vec(raw_data);
    let comp_dur = start.elapsed();

    let comp_size = compressed.len();
    let ratio = (comp_size as f64 / file_len.max(1) as f64) * 100.0;

    let extract_means = POSITIONS_PCT
        .iter()
        .map(|&pct| measure_blaris_extract_mean(&compressed, file_len, pct))
        .collect();

    Row {
        name: "Blaris".to_string(),
        comp_dur,
        comp_size,
        ratio,
        extract_means,
        mem_str: mem_str(64), // O(1) state, no window/buffers to speak of
    }
}

// ---------- heatshrink-lib (no_std, no alloc, const-generic) ----------

/// Byte count carried by either `Poll` variant, regardless of which one.
fn poll_bytes(p: Poll) -> (usize, bool) {
    match p {
        Poll::More(n) => (n, false),  // more output remains after this
        Poll::Empty(n) => (n, true),  // drained for now
    }
}

fn heatshrink_compress<const W: usize, const L: usize, const BUF: usize>(input: &[u8]) -> Vec<u8> {
    let mut encoder = HeatshrinkEncoder::<W, L, BUF>::new();
    let mut output = Vec::new();
    let mut scratch = [0u8; SCRATCH_SIZE];

    let mut drain = |encoder: &mut HeatshrinkEncoder<W, L, BUF>, output: &mut Vec<u8>| {
        loop {
            match encoder.poll(&mut scratch) {
                Ok(p) => {
                    let (n, empty) = poll_bytes(p);
                    if n > 0 {
                        output.extend_from_slice(&scratch[..n]);
                    }
                    if empty {
                        break;
                    }
                }
                Err(_) => break,
            }
        }
    };

    let mut in_pos = 0;
    while in_pos < input.len() {
        let consumed = match encoder.sink(&input[in_pos..]) {
            Ok(n) => n,
            Err(_) => break,
        };
        in_pos += consumed;

        drain(&mut encoder, &mut output);

        if consumed == 0 {
            break;
        }
    }

    loop {
        match encoder.finish() {
            Finish::Done => break,
            Finish::More => drain(&mut encoder, &mut output),
        }
    }

    output
}

/// Decompress a `len`-byte window starting at `offset` from `compressed`.
///
/// heatshrink has no structural random access: it must stream-decode from
/// the start and discard bytes before `offset`. This reflects that
/// honestly rather than trying to "seek".
fn heatshrink_decompress_range<const W: usize, const L: usize, const I: usize, const WIN: usize>(
    compressed: &[u8],
    offset: usize,
    len: usize,
) -> Vec<u8> {
    let mut decoder = HeatshrinkDecoder::<W, L, I, WIN>::new();
    let mut target_buf = vec![0u8; len];
    let mut scratch = [0u8; SCRATCH_SIZE];

    let mut decompressed_bytes_so_far: usize = 0;
    let target_end = offset + len;

    // Copies `produced` freshly-decoded bytes (sitting in `scratch[..produced]`)
    // into `target_buf` wherever they overlap [offset, target_end). Returns
    // true once target_end has been reached.
    let mut consume_produced = |produced: usize, scratch: &[u8], count: &mut usize| -> bool {
        if produced > 0 {
            let chunk_start = *count;
            let chunk_end = chunk_start + produced;

            if chunk_end > offset && chunk_start < target_end {
                let overlap_start = chunk_start.max(offset);
                let overlap_end = chunk_end.min(target_end);

                let src_start = overlap_start - chunk_start;
                let src_end = overlap_end - chunk_start;
                let dst_start = overlap_start - offset;
                let dst_end = overlap_end - offset;

                target_buf[dst_start..dst_end].copy_from_slice(&scratch[src_start..src_end]);
            }

            *count += produced;
        }

        *count >= target_end
    };

    let mut in_pos = 0;
    'outer: while in_pos < compressed.len() {
        let consumed = match decoder.sink(&compressed[in_pos..]) {
            Ok(n) => n,
            Err(_) => break,
        };
        in_pos += consumed;

        loop {
            match decoder.poll(&mut scratch) {
                Ok(p) => {
                    let (n, empty) = poll_bytes(p);
                    if consume_produced(n, &scratch, &mut decompressed_bytes_so_far) {
                        break 'outer;
                    }
                    if empty {
                        break;
                    }
                }
                Err(_) => break 'outer,
            }
        }

        if consumed == 0 {
            break;
        }
    }

    // All input has been sunk; flush whatever's still buffered internally.
    if decompressed_bytes_so_far < target_end {
        'flush: loop {
            match decoder.finish() {
                Finish::Done => break 'flush,
                Finish::More => match decoder.poll(&mut scratch) {
                    Ok(p) => {
                        let (n, empty) = poll_bytes(p);
                        if consume_produced(n, &scratch, &mut decompressed_bytes_so_far) {
                            break 'flush;
                        }
                        if empty && n == 0 {
                            break 'flush;
                        }
                    }
                    Err(_) => break 'flush,
                },
            }
        }
    }

    target_buf
}

/// Builds one Row for a fixed (W, L, BUF, WIN) combination.
///
/// `mem_str` reflects DECODER_I + WIN (the embedded-relevant footprint —
/// blaris only decompresses on-device; encoding happens on the host and
/// isn't part of the memory budget comparison).
macro_rules! heatshrink_row {
    ($raw_data:expr, $file_len:expr, $w:literal, $l:literal, $buf:literal, $win:literal) => {{
        let start = Instant::now();
        let compressed = heatshrink_compress::<$w, $l, $buf>($raw_data);
        let comp_dur = start.elapsed();

        let comp_size = compressed.len();
        let ratio = (comp_size as f64 / ($file_len as f64).max(1.0)) * 100.0;

        let extract_means: Vec<Duration> = POSITIONS_PCT
            .iter()
            .map(|&pct| {
                if $file_len == 0 {
                    return Duration::ZERO;
                }

                let len = EXTRACT_LEN.min($file_len);
                let raw_offset = ($file_len * pct) / 100;
                let offset = raw_offset.min($file_len - len);

                // Warm-up.
                let out =
                    heatshrink_decompress_range::<$w, $l, DECODER_I, $win>(&compressed, offset, len);
                std::hint::black_box(out);

                let mut total = Duration::ZERO;
                for _ in 0..RUNS {
                    let t0 = Instant::now();
                    let out = heatshrink_decompress_range::<$w, $l, DECODER_I, $win>(
                        &compressed,
                        offset,
                        len,
                    );
                    total += t0.elapsed();
                    std::hint::black_box(out);
                }
                total / (RUNS as u32)
            })
            .collect();

        Row {
            name: format!("HS(W={})", $w),
            comp_dur,
            comp_size,
            ratio,
            extract_means,
            mem_str: mem_str(DECODER_I + $win),
        }
    }};
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

fn benchmark_file(file_path: &Path) {
    let file_name = file_path.file_name().unwrap().to_string_lossy();
    let raw_data = fs::read(file_path).expect("failed to read test file");
    let file_len = raw_data.len();

    println!("\n{file_name}  ({file_len} B)");

    let mut rows = vec![blaris_row(&raw_data, file_len)];

    // W=6..14, L=4 fixed, BUF=2<<W, WIN=1<<W (heatshrink-lib's required
    // relationship between the window bits and its buffer sizes).
    rows.push(heatshrink_row!(&raw_data, file_len, 6, 4, 128, 64));
    rows.push(heatshrink_row!(&raw_data, file_len, 7, 4, 256, 128));
    rows.push(heatshrink_row!(&raw_data, file_len, 8, 4, 512, 256));
    rows.push(heatshrink_row!(&raw_data, file_len, 9, 4, 1024, 512));
    rows.push(heatshrink_row!(&raw_data, file_len, 10, 4, 2048, 1024));
    rows.push(heatshrink_row!(&raw_data, file_len, 11, 4, 4096, 2048));
    rows.push(heatshrink_row!(&raw_data, file_len, 12, 4, 8192, 4096));
    rows.push(heatshrink_row!(&raw_data, file_len, 13, 4, 16384, 8192));
    rows.push(heatshrink_row!(&raw_data, file_len, 14, 4, 32768, 16384));

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
        benchmark_file(file);
    }
}
