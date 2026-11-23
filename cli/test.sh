#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

CC_BIN="${CC:-cc}"

echo "==> Building hybrid CLI ($CC_BIN)"
make -C cli clean >/dev/null
make -C cli CC="$CC_BIN" CFLAGS="-O3 -DNDEBUG -Wall -Wextra -Werror" >/dev/null

FILES=(bench/data/*)
TOTAL_FILES=${#FILES[@]}
TOTAL_RUNS=0
TOTAL_SLICES=0

echo "==> Testing $TOTAL_FILES files x 9 levels (full + slices)"
for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue
    fsize=$(wc -c < "$f")

    for L in 1 2 3 4 5 6 7 8 9; do
        ./cli/blaris -"$L" "$f" "$TMP_DIR/out.bl" >/dev/null

        # Full-file decompression
        ./cli/blaris -d -"$L" "$TMP_DIR/out.bl" "$TMP_DIR/full.bin" >/dev/null
        cmp -s "$f" "$TMP_DIR/full.bin" || {
            echo "FAIL: full mismatch for $f level $L" >&2
            exit 1
        }
        TOTAL_RUNS=$((TOTAL_RUNS + 1))

        # Slice decompression
        if [ "$fsize" -gt 0 ]; then
            s1_off=0
            s1_len=1

            s2_off=$((fsize / 3))
            s2_len=$((fsize - s2_off))
            [ "$s2_len" -gt 16 ] && s2_len=16

            s3_off=$((fsize / 2))
            s3_len=$((fsize - s3_off))
            [ "$s3_len" -gt 64 ] && s3_len=64

            s4_len=256
            [ "$s4_len" -gt "$fsize" ] && s4_len="$fsize"
            s4_off=$((fsize - s4_len))

            for spec in "$s1_off:$s1_len" "$s2_off:$s2_len" "$s3_off:$s3_len" "$s4_off:$s4_len"; do
                off="${spec%%:*}"
                cnt="${spec##*:}"
                [ "$cnt" -gt 0 ] || continue
                ./cli/blaris -d -"$L" --offset "$off" --size "$cnt" "$TMP_DIR/out.bl" "$TMP_DIR/slice.bin" >/dev/null
                dd if="$f" of="$TMP_DIR/ref.bin" bs=1 skip="$off" count="$cnt" status=none
                cmp -s "$TMP_DIR/ref.bin" "$TMP_DIR/slice.bin" || {
                    echo "FAIL: slice mismatch for $f level $L (offset=$off size=$cnt)" >&2
                    exit 1
                }
                TOTAL_SLICES=$((TOTAL_SLICES + 1))
            done
        fi
    done
done

echo "==> Testing C-array output (--sym ci_test)"
SAMPLE="${FILES[0]}"
./cli/blaris -6 -c --sym ci_test "$SAMPLE" "$TMP_DIR/payload.c" >/dev/null
cat << 'EOF' > "$TMP_DIR/harness.c"
#define BLARIS_LEVEL 6
#include "blaris.h"
#include "blaris-decompress.c"
#include "payload.c"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) return 1;
    if (((uintptr_t)ci_test % 4) != 0) {
        fprintf(stderr, "unaligned payload\n");
        return 1;
    }
    uint8_t *out = malloc(ci_test_origlen > 0 ? ci_test_origlen : 1);
    if (!out) return 1;
    struct Decoder dec;
    struct Checkpoint cp = {0};
    blaris_init((const uint32_t *)ci_test, &dec);
    blaris_decompress(&dec, 0, out, (uint32_t)ci_test_origlen, &cp);
    FILE *f = fopen(argv[1], "wb");
    if (!f) { free(out); return 1; }
    fwrite(out, 1, ci_test_origlen, f);
    fclose(f);
    free(out);
    return 0;
}
EOF
"$CC_BIN" -std=c11 -O2 -Wall -Wextra -pedantic -Iinclude -Isrc -I"$TMP_DIR" "$TMP_DIR/harness.c" -o "$TMP_DIR/harness"
"$TMP_DIR/harness" "$TMP_DIR/c_out.bin"
cmp -s "$SAMPLE" "$TMP_DIR/c_out.bin" || {
    echo "FAIL: C-array round-trip mismatch for $SAMPLE" >&2
    exit 1
}

echo "==> Testing STATIC=1 build"
make -C cli clean >/dev/null
make -C cli CC="$CC_BIN" STATIC=1 CFLAGS="-O3 -DNDEBUG -Wall -Wextra -Werror -DBLARIS_LEVEL=3" >/dev/null
./cli/blaris "$SAMPLE" "$TMP_DIR/static.bl" >/dev/null
./cli/blaris -d "$TMP_DIR/static.bl" "$TMP_DIR/static.bin" >/dev/null
cmp -s "$SAMPLE" "$TMP_DIR/static.bin" || {
    echo "FAIL: static build round-trip mismatch for $SAMPLE" >&2
    exit 1
}

echo "==> Rebuilding default hybrid CLI"
make -C cli clean >/dev/null
make -C cli CC="$CC_BIN" >/dev/null

echo "PASS: $TOTAL_RUNS full-file runs, $TOTAL_SLICES slice runs, C-array, and STATIC=1 all passed."
