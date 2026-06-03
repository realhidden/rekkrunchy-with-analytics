#!/bin/sh
# Corpus roundtrip test. For every corpus/*.bin: compress with the C tool, then
# decompress with BOTH the C tool and the standalone x86 asm decoder, and verify
# byte-exact against the original. Run inside the linux/386 docker (see Makefile
# `make docker-test`). Reports SIZE + PASS/FAIL per file.
set -e
cd "$(dirname "$0")/.."

gcc -m32 -O2 -w -Isrc -o /tmp/rekk src/main.c src/codec.c src/model.c src/tables.c
nasm -f elf32 -o /tmp/depack.o x86/depack.asm
gcc -m32 -O2 -w -o /tmp/td x86/test_depack.c /tmp/depack.o

pass=0; fail=0
for f in corpus/*.bin; do
  /tmp/rekk -c "$f" /tmp/c.bin >/dev/null 2>&1
  /tmp/rekk -d /tmp/c.bin /tmp/dc.bin >/dev/null 2>&1
  orig=$(wc -c < "$f"); comp=$(wc -c < /tmp/c.bin)
  name=$(basename "$f")
  if ! cmp -s "$f" /tmp/dc.bin; then
    printf "FAIL %-18s (C decoder)\n" "$name"; fail=$((fail+1)); continue
  fi
  if /tmp/td /tmp/c.bin "$f" >/dev/null 2>&1; then
    printf "PASS %-18s %8d -> %8d\n" "$name" "$orig" "$comp"; pass=$((pass+1))
  else
    printf "FAIL %-18s (x86 asm decoder)\n" "$name"; fail=$((fail+1))
  fi
done
echo "----"
echo "PASS=$pass FAIL=$fail"
[ "$fail" -eq 0 ]
