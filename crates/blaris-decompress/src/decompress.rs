use blaris_core::{Checkpoint, Header};

pub fn decompress_with_checkpoint(
    from: &[u8],
    to: &mut [u8],
    offset: usize,
    checkpoint: &mut Checkpoint,
) -> bool {
    if from.len() < Header::SIZE {
        return false;
    }
    let header = Header::load(from);

    let end_pos = (offset + to.len()) as u32;

    for (pos, byte) in (offset as u32..).zip(to.iter_mut()) {
        checkpoint.seek_to_token(pos, &header);
        let mut resolver = *checkpoint;
        *byte = resolver.read_byte(pos, &header);
    }

    if !to.is_empty() {
        checkpoint.seek_to_token(end_pos, &header);
    }

    true
}

pub fn decompress(from: &[u8], to: &mut [u8], offset: usize) -> bool {
    let mut checkpoint = Checkpoint::default();
    decompress_with_checkpoint(from, to, offset, &mut checkpoint)
}

/// C-compatible entry point for decompression.
///
/// # Safety
/// `from_ptr` and `to_ptr` must be non-null and valid for reads/writes up to `from_len` and `to_len` respectively.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn blaris_decompress(
    from_ptr: *const u8,
    from_len: usize,
    to_ptr: *mut u8,
    to_len: usize,
    offset: usize,
) -> bool {
    if from_ptr.is_null() || to_ptr.is_null() {
        return false;
    }

    let from = unsafe { core::slice::from_raw_parts(from_ptr, from_len) };
    let to = unsafe { core::slice::from_raw_parts_mut(to_ptr, to_len) };

    decompress(from, to, offset)
}
