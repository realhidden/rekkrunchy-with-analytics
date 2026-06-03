# x86 decoder sizes

`.text` byte sizes of the standalone x86 decoders (measured with
`size -A` / `objdump -h` on the `nasm -f elf32` object). Track golfing progress
here.

| component | what it does | baseline | current |
|-----------|--------------|---------:|--------:|
| `depack.asm` (`rekk_depack`) | context-mixing range decoder (codec self-decompressor) | **1588** | 1584 |
| `unfilter.asm` (`rekk_unfilter`) | x86 split-stream unfilter (reverses `-cx`) | **550** | 549 |

A fully self-extracting filtered x86 payload runs both: 1584 + 549 = 2133 bytes
of decoder `.text`.

## Golf log

- `depack.asm` 1588→1584: `lodsd` folds the size-header read + pointer bump in
  the entry prologue.
- `unfilter.asm` 550→549: `.done` uses `lea eax,[edi-4]; sub eax,[offset]`
  instead of a three-instruction subtract; init builds the `~0` jump-table
  sentinel with `dec`/`neg` sharing the constant.

Diminishing returns past this: ~95% of `depack.asm` is ryg's already-golfed
PAQ model math, and the `unfilter.asm` body is dense `xchg esi,[ebp+disp8]` +
`movs` stream plumbing (already 3 B/op, optimal). Further single-byte folds
trade real correctness risk for little gain, so they were left.

Notes:
- The codec decoder is intrinsically large: it carries the full PAQ-style
  context-mixing model (state tables, mixer, APM). Unlike an LZ decoder it
  cannot shrink to the ~150 B range — golfing targets the framing/glue code and
  redundant sequences, not the model math.
- Baselines recorded before size-optimization. See git history for the diff.

Measure with: `make -s decoder-size` (needs the i386 docker image).
