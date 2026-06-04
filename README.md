rekkrunchy
==========

A cross-platform command-line compressor built from the codec of ryg's
`kkrunchy_k7` (a PAQ-style context-mixing model driving a binary range coder).

This is a from-scratch, portable C port of the original Windows/x86 packer. It
keeps only the compression engine — the Windows PE rewriting, PDB/MAP symbol
analytics, and x86 disassembly filter of the original are gone. What remains:

* `rekkrunchy` — a portable C compressor/decompressor (standard library only,
  builds with gcc/clang on any architecture).
* a standalone 32-bit x86 self-decompressor (`x86/depack.asm`) for embedding in
  size-coding intros — the same idea as the original, minus the PE machinery.

The C codec is **byte-exact** with the original assembly: the model port is
validated bit-for-bit against the assembled `model_asm.asm` over hundreds of
real binaries (see `test/`).

Build & use
-----------

```sh
make                            # builds ./rekkrunchy with gcc
./rekkrunchy -c  file file.rk   # compress
./rekkrunchy -cx file file.rk   # compress with the x86 split-stream filter
./rekkrunchy -d  file.rk out    # decompress (auto-detects the filter)
make test                       # quick self-roundtrip
```

The container is `[flag][orig size if x86][codec blob]`; the codec blob is
`[4-byte original size][range-coded stream]`.

x86 split-stream filter (`-cx`)
-------------------------------

`-cx` enables the original kkrunchy x86 preprocessor (`src/x86filter.c`, ported
from `dis.cpp`). It disassembles 32-bit x86 code and splits instruction fields
into 32 streams — opcodes, modrm/sib, displacements (split by base register /
addressing form), immediates (split by opcode class), and jump/call targets
(made absolute) — so each field type compresses against its own statistics, and
repeated call sites become identical. All multi-byte values are stored
big-endian so their high bytes (sign / image-base prefixes) cluster. The
context-mixing model then compresses real x86 code noticeably better:

| input (`.text`) | plain | `-cx` | win |
|-----------------|------:|------:|----:|
| ls   |  37216 |  33388 | −10.3% |
| gcc  |  26077 |  22846 | −12.4% |
| nasm | 115401 | 101193 | −12.3% |

The transform is fully reversible for any input (it falls back to byte escapes
for non-instruction bytes), and validated byte-exact over 864 real `.text`
sections. An optional `va` argument sets the assumed load address (default
`0x401000`); any value roundtrips — it only affects jump-table detection.

### What `-cx` changes vs upstream dispack (for the original authors)

The filter started as a faithful port of ryg's `dis.cpp` / `depack2.asm`
dispack (20 streams). This fork then re-tuned the **stream layout only** — same
idea, same model, no codec change — guided by ~110 measured experiments
(`research/NOTES.md`). The net effect on the corpus (cat/ls/gcc/nasm `.text`,
real `-cx` path, every step roundtrip-verified):

| layout | corpus total | vs upstream dispack |
|--------|-------------:|--------------------:|
| upstream dispack (20 streams) | 169607 B | — |
| this fork (32 streams)        | 164645 B | **−4962 B / −2.93%** |

The improvements, all "separate an operand field by structure the decoder
already knows," are:

* immediates split by opcode class (imm8/imm32: arith vs mov-reg vs mov-r/m vs
  push — different value distributions);
* `disp32` displacements split by base register (esp = stack args, ebp =
  locals, GP = arrays/globals — the single biggest win);
* jump/call targets stored **absolute** instead of zigzag-delta (also removed
  decoder code);
* **all** multi-byte streams stored big-endian (high byte = sign / image-base
  prefix clusters).

What did **not** help (proven dead ends, see `research/NOTES.md`): merging
target streams, splitting spine fields (opcode/modrm), any within-stream value
remap (delta/MTF/SoA/zigzag), permuting stream order, and finer imm32 splits.

**Decoder-size cost.** These gains are paid for once, in the `-cx` unfilter
stub (`x86/unfilter.asm`), not in the codec decoder:

| stub | upstream-equivalent | this fork | Δ |
|------|--------------------:|----------:|---:|
| `unfilter.asm` (reverses `-cx`)        | ~550 B | 775 B | +225 B |
| `depack.asm` (codec, unchanged in capability) | 1588 B | 1583 B | −5 B (golf only) |

So a self-extracting filtered payload carries +225 B of decoder for ≈−3% on
every payload — net positive after roughly the first ~7 KB of compressed
output. The codec decoder itself is byte-exact with the original model; only
the small split-stream picker grew. Sizes are tracked in `x86/SIZES.md`.

x86 self-decompressor
---------------------

`x86/depack.asm` is a standalone `cdecl` function

```c
int rekk_depack(const uint8_t *src, uint8_t *dst);   // returns decoded size
```

that decodes a `rekkrunchy -c` stream. It is derived from the original
`depacker.asm` (see `x86/gen_depack.py`) and shares the model with the C codec,
so the two interoperate exactly. Two intentional constraints inherited from the
original (kept for size) are documented in `originalbugs.md`: the destination
buffer must be zero-initialized, and the payload must be non-empty.

Building and testing it needs a 32-bit x86 toolchain (`nasm`, `gcc -m32`):

```sh
make docker-test           # builds the i386 docker image and runs the corpus
sh  test/corpus_test.sh     # or run directly where a 32-bit toolchain exists
```

Layout
------

```
src/        portable C codec + CLI
  model.c     context-mixing model (port of model_asm.asm)
  codec.c     range encoder/decoder
  tables.c    lookup tables baked from the original model init (generated)
  x86filter.c x86 split-stream preprocessor (port of dis.cpp), used by -cx
  main.c      command-line front-end
x86/        standalone x86 self-decompressor (depack.asm, generated)
test/       differential & corpus validation harness
corpus/     sample inputs (real stripped x86 code + edge cases)
model_asm.asm, depacker.asm   original ryg assembly, kept as generator inputs
```

Credits
-------

Compression engine by Fabian "ryg" Giesen (public domain). `.kkp` analytics
fork and various fixes by BoyC / Conspiracy. Cross-platform C port keeps the
codec and drops everything Windows-specific.
