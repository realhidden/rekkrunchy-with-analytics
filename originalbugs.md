# Bugs / oddities found in the original kkrunchy model while porting to C

Issues in the *original* rygs kkrunchy model/asm (model_asm.asm, depacker.asm,
rangecoder.cpp), found while writing the byte-exact C port. The C port
reproduces the original behavior bit-for-bit (validated by a differential
harness against the assembled original), so it inherits these by design.

## 1. `emms` placement vs the model's MMX use (cosmetic)

`rangecoder.cpp` issues `__asm emms` around the progress callback and at the
end, but the per-bit `modelASM` uses MMX (`pmaddwd`, `pmulhw`, …) and returns
without `emms`. Mixing MMX and any x87 float code without an intervening `emms`
is unsafe; the encoder happens to do its `-log()` float math only after an
`emms`, so it is fine in practice, but the model itself leaves the FPU tag word
in MMX state on every call. Not reproduced in C (no MMX), no behavioral effect.

## 2. Self-decompressor assumes a zero-initialized output buffer

The decoder's zero-page fast path (every 8 KB of all-zero output) just advances
the destination cursor by 8192 *without writing zeros* — it relies on the
output region already being zero. In the original kkrunchy this always holds
(the depacked image lands in a freshly mapped, zero-filled PE section). The
standalone `rekk_depack` keeps this contract: callers must pass a zeroed output
buffer (the C `Decompress()` does its own `memset`, so the CLI is unaffected).

## 3. Self-decompressor assumes a non-empty payload

The decode loop is do-while: it always decodes at least one byte before testing
the remaining count, so a zero-length payload would run away. The compressor
never needs to pack empty input in the self-extracting use case, so the stub
omits the guard by design (keeps it smaller). The portable C `Decompress()`
handles size 0 correctly.

<!-- No correctness bugs in the model logic were found: the C port matches the
     assembled original bit-for-bit over 762 system binaries plus the corpus.
     Earlier suspicion that the match search desyncs encoder vs decoder by
     reading the cursor byte was investigated and dismissed — the standalone
     `cmpsb` before `repe cmpsb` has its ZF discarded, so it has no effect. -->
