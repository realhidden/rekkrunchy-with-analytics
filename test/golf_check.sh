#!/bin/sh
# Golf iteration helper: assemble each x86 decoder, print its .text size, and
# run the corpus roundtrip so size wins can't silently break correctness.
# Run inside the linux/386 docker.
set -e
cd "$(dirname "$0")/.."
for a in x86/depack.asm x86/unfilter.asm; do
  [ -f "$a" ] || continue
  nasm -f elf32 -o /tmp/s.o "$a" 2>/dev/null
  printf "%-22s %5d bytes\n" "$a" "$(size -A /tmp/s.o | awk '/\.text/{print $2}')"
done
sh test/corpus_test.sh 2>/dev/null | tail -2
