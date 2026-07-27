use clap::{Parser, Subcommand};
use std::fs::File;
use std::io::{self, Read, Write};
use std::path::PathBuf;
use std::process;
use std::time::Instant;

use blaris_compress::compress::{compress, compress_bound};
use blaris_decompress::decompress::decompress;

#[derive(Parser)]
#[command(name = "blaris")]
#[command(about = "Blaris compression and decompression utility", long_about = None)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Compress a file
    Compress {
        /// Input file path
        #[arg(short, long)]
        input: PathBuf,

        /// Output file path
        #[arg(short, long)]
        output: PathBuf,
    },
    /// Decompress a segment of a file
    Decompress {
        /// Input compressed file path
        #[arg(short, long)]
        input: PathBuf,

        /// Output file path. If omitted, writes to stdout.
        #[arg(short, long)]
        output: Option<PathBuf>,

        /// Offset in the uncompressed data to start decompression from
        #[arg(long, default_value_t = 0)]
        offset: usize,

        /// Length of the data segment to decompress
        #[arg(short, long)]
        length: usize,
    },
    /// Check the entire flow (compress -> decompress -> compare)
    Check {
        /// Input file path to test
        #[arg(short, long)]
        input: PathBuf,
    },
}

fn main() -> io::Result<()> {
    let cli = Cli::parse();

    match cli.command {
        Commands::Compress { input, output } => {
            let mut in_file = File::open(&input)?;
            let mut in_data = Vec::new();
            in_file.read_to_end(&mut in_data)?;

            let mut out_data = vec![0; compress_bound(in_data.len())];

            let compressed_size = compress(&in_data, &mut out_data);
            out_data.truncate(compressed_size);

            let mut out_file = File::create(&output)?;
            out_file.write_all(&out_data)?;
        }
        Commands::Decompress {
            input,
            output,
            offset,
            length,
        } => {
            let mut in_file = File::open(&input)?;
            let mut in_data = Vec::new();
            in_file.read_to_end(&mut in_data)?;

            let mut out_data = vec![0; length];
            let success = decompress(&in_data, &mut out_data, offset);

            if !success {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "Decompression failed. The segment bounds or compressed data may be invalid.",
                ));
            }

            if let Some(out_path) = output {
                let mut out_file = File::create(&out_path)?;
                out_file.write_all(&out_data)?;
            } else {
                let stdout = io::stdout();
                let mut handle = stdout.lock();
                handle.write_all(&out_data)?;
            }
        }
        Commands::Check { input } => {
            let mut in_file = File::open(&input)?;
            let mut original_data = Vec::new();
            in_file.read_to_end(&mut original_data)?;

            let mut compressed_data = vec![0; compress_bound(original_data.len())];

            let compress_start = Instant::now();
            let compressed_size = compress(&original_data, &mut compressed_data);
            let compress_duration = compress_start.elapsed();

            compressed_data.truncate(compressed_size);

            let mut decompressed_data = vec![0; original_data.len()];

            let decompress_start = Instant::now();
            let success = decompress(&compressed_data, &mut decompressed_data, 0);
            let decompress_duration = decompress_start.elapsed();

            if !success {
                eprintln!("Error: Decompression failed during the check operation.");
                process::exit(1);
            }

            if original_data != decompressed_data {
                eprintln!("Error: Decompressed data does not match the original input data.");
                process::exit(1);
            }

            let original_size = original_data.len();
            let ratio = if original_size > 0 {
                (compressed_size as f64 / original_size as f64) * 100.0
            } else {
                100.0
            };
            let saved = if original_size > 0 {
                100.0 - ratio
            } else {
                0.0
            };

            println!("{:<19}{}", "File:", input.display());
            println!("{:<19}{} bytes", "Original size:", original_size);
            println!("{:<19}{} bytes", "Compressed size:", compressed_size);
            println!("{:<19}{:.2}% of original", "Compression ratio:", ratio);
            println!("{:<19}{:.2}%", "Space saved:", saved);
            println!("{:<19}{:?}", "Compress time:", compress_duration);
            println!("{:<19}{:?}", "Decompress time:", decompress_duration);
            println!("{:<19}{}", "Round-trip:", "OK (bytes match)");
        }
    }

    Ok(())
}
