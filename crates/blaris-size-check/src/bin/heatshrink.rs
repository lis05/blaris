#![no_std]
#![no_main]

use core::panic::PanicInfo;

use heatshrink::decoder::HeatshrinkDecoder;
use heatshrink::{Finish, Poll};

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}

// W=8, L=4, I=32 matches the original heatshrink C library's defaults, and
// is the same window size used for the "HS(W=8)" (~512B) row in the
// blaris-benchmark table, so this is directly comparable to those numbers.
//
// WIN must equal 1 << W (a workaround for stable Rust not yet supporting
// const-generic arithmetic in array sizes) — see heatshrink-lib's own
// DefaultDecoder alias, which uses these exact values.
type Decoder = HeatshrinkDecoder<8, 4, 32, 256>;

#[unsafe(no_mangle)]
pub extern "C" fn _start() -> ! {
    let compressed: [u8; 16] = [0; 16];
    let mut out: [u8; 8] = [0; 8];

    let mut decoder = Decoder::new();

    let mut in_pos = 0;
    let mut out_pos = 0;

    while in_pos < compressed.len() && out_pos < out.len() {
        match decoder.sink(&compressed[in_pos..]) {
            Ok(consumed) => {
                in_pos += consumed;
                if consumed == 0 {
                    break;
                }
            }
            Err(_) => break,
        }

        loop {
            match decoder.poll(&mut out[out_pos..]) {
                Ok(Poll::More(n)) => {
                    out_pos += n;
                    if out_pos >= out.len() {
                        break;
                    }
                }
                Ok(Poll::Empty(n)) => {
                    out_pos += n;
                    break;
                }
                Err(_) => break,
            }
        }
    }

    // Flush any remaining buffered output.
    loop {
        let len = out.len();
        match decoder.finish() {
            Finish::Done => break,
            Finish::More => {
                match decoder.poll(&mut out[out_pos.min(len)..]) {
                    Ok(p) => {
                        let n = match p {
                            Poll::More(n) | Poll::Empty(n) => n,
                        };
                        out_pos += n;
                        if n == 0 {
                            break;
                        }
                    }
                    Err(_) => break,
                }
            }
        }
    }

    loop {}
}
