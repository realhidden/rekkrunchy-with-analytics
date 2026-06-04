# x86 decoder sizes

`.text` byte sizes of the standalone x86 decoders (measured with
`size -A` / `objdump -h` on the `nasm -f elf32` object). Track golfing progress
here.

| component | what it does | baseline | current |
|-----------|--------------|---------:|--------:|
| `depack.asm` (`rekk_depack`) | context-mixing range decoder (codec self-decompressor) | **1588** | 1583 |
| `unfilter.asm` (`rekk_unfilter`) | x86 split-stream unfilter (reverses `-cx`) | **550** | 626 |

A fully self-extracting filtered x86 payload runs both: 1583 + 626 = 2209 bytes
of decoder `.text`. The unfilter grew 530→626 across two stream-layout rounds
(per-opcode immediate + disp32/push split, 20→26 streams); that ~96 B of extra
decoder buys ~2% off every compressed x86 payload. Note round-3 also *removed*
the rel32 zigzag (storing absolute targets compresses better), which shrank the
decoder and offset most of the new routing cost.

## Golf log

- `depack.asm` 1588→1584: `lodsd` folds the size-header read + pointer bump in
  the entry prologue (via `gen_depack.py`).
- `depack.asm` 1584→1583: zero-page test `xor eax,eax; cmp eax,[bitcounter]` →
  `cmp dword [bitcounter],0` (bitcounter is bumped as a word, hi half always 0).
- `unfilter.asm` 550→549: `.done` uses `lea eax,[edi-4]; sub eax,[offset]`
  instead of a three-instruction subtract; init builds the `~0` jump-table
  sentinel with `dec`/`neg` sharing the constant.
- `unfilter.asm` 549→534: route the five copy-and-continue paths (fAD/fBR/fBI/
  fDI/fWI) through a central `.tomain` trampoline so each reaches `.main` with a
  2-byte `jmp short` instead of a 5-byte near jump.
- `unfilter.asm` 534→530: the trampoline shrank offsets enough that the two
  `jz/jne near` branches to `.nomdrm`/`.noaddr` now fit as 2-byte short jumps.

Why not more: a clean-room ablation (`research/cm.c`, see `research/NOTES.md`)
proved no simpler model fits a 5% ratio budget — the size in `depack.asm` is the
PAQ machinery that delivers the ratio. So the model math is left bit-exact and
only framing/instruction-selection was golfed. The `unfilter.asm` body is now
dense `xchg esi,[ebp+disp8]` + `movs` plumbing (3 B/op); deeper folds trade
correctness risk for little gain.

All golf steps verified: `depack.asm` against the corpus byte-exact (it still
decodes the C compressor's output), `unfilter.asm` against the C `X86Unfilter`
over 4 real `.text` files + 3046 fuzz cases incl. a crafted jump table.

Notes:
- The codec decoder is intrinsically large: it carries the full PAQ-style
  context-mixing model (state tables, mixer, APM). Unlike an LZ decoder it
  cannot shrink to the ~150 B range — golfing targets the framing/glue code and
  redundant sequences, not the model math.
- Baselines recorded before size-optimization. See git history for the diff.

Measure with: `make -s decoder-size` (needs the i386 docker image).
