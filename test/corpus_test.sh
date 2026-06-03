#!/bin/sh
# Corpus roundtrip test. Run inside the linux/386 docker (`make docker-test`).
#
# For every corpus/*.bin:
#   1. compress with the C tool, decompress with the C tool        -> verify
#   2. decompress the codec stream with the standalone x86 decoder -> verify
# For every corpus/*_text.bin (real x86 code) also:
#   3. compress with the x86 split-stream filter (-cx), C roundtrip -> verify
#      and report the size win vs plain compression.
set -e
cd "$(dirname "$0")/.."

SRC="src/main.c src/codec.c src/model.c src/tables.c src/x86filter.c"
gcc -m32 -O2 -w -Isrc -o /tmp/rekk $SRC
nasm -f elf32 -o /tmp/depack.o x86/depack.asm
gcc -m32 -O2 -w -o /tmp/td x86/test_depack.c /tmp/depack.o

# also verify the standalone asm unfilter matches the C unfilter (differential)
nasm -f elf32 -o /tmp/uf.o x86/unfilter.asm
gcc -m32 -O2 -w -o /tmp/ud test/unfilter_diff.c src/x86filter.c /tmp/uf.o
/tmp/ud corpus/*_text.bin >/dev/null 2>&1 \
  && echo "asm unfilter: differential OK" \
  || { echo "asm unfilter: FAIL"; exit 1; }

pass=0; fail=0
for f in corpus/*.bin; do
  name=$(basename "$f")
  orig=$(wc -c < "$f")

  # (1) C compress + C decompress
  /tmp/rekk -c "$f" /tmp/c.bin >/dev/null 2>&1
  /tmp/rekk -d /tmp/c.bin /tmp/dc.bin >/dev/null 2>&1
  comp=$(wc -c < /tmp/c.bin)
  if ! cmp -s "$f" /tmp/dc.bin; then
    printf "FAIL %-18s (C codec)\n" "$name"; fail=$((fail+1)); continue
  fi

  # (2) standalone x86 asm decoder on the raw codec stream (strip the 1-byte
  #     container flag so the asm sees just [size][stream]).
  tail -c +2 /tmp/c.bin > /tmp/cs.bin
  if ! /tmp/td /tmp/cs.bin "$f" >/dev/null 2>&1; then
    printf "FAIL %-18s (x86 asm decoder)\n" "$name"; fail=$((fail+1)); continue
  fi

  # (3) x86 split-stream filter on real code sections
  case "$name" in
    *_text.bin)
      /tmp/rekk -cx "$f" /tmp/cx.bin >/dev/null 2>&1
      /tmp/rekk -d  /tmp/cx.bin /tmp/dx.bin >/dev/null 2>&1
      cx=$(wc -c < /tmp/cx.bin)
      if ! cmp -s "$f" /tmp/dx.bin; then
        printf "FAIL %-18s (x86 filter)\n" "$name"; fail=$((fail+1)); continue
      fi
      printf "PASS %-18s %8d -> %8d  (x86 %8d, %+d)\n" "$name" "$orig" "$comp" "$cx" "$((cx-comp))"
      ;;
    *)
      printf "PASS %-18s %8d -> %8d\n" "$name" "$orig" "$comp"
      ;;
  esac
  pass=$((pass+1))
done
echo "----"
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
