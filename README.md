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
make                       # builds ./rekkrunchy with gcc
./rekkrunchy -c file file.rk   # compress
./rekkrunchy -d file.rk out    # decompress
make test                  # quick self-roundtrip
```

The compressed format is `[4-byte little-endian original size][range-coded
stream]`.

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
