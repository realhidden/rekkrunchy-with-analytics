# PAQ model reimplementation — research notes

Goal: reimplement the context-mixing model from scratch and try to beat the
existing decoders on size, within a 5% compression-ratio budget.

## Baselines (corpus/*.bin, raw `-c`, no x86 filter)

| metric | value |
|--------|------:|
| total input | 542877 B |
| total compressed | 203190 B |
| ratio | 0.3742 |
| 5% budget ceiling | 213350 B (ratio 0.3929) |

Decoder `.text` sizes: `depack.asm` 1584 B, `unfilter.asm` 549 B.

## depack.asm byte budget (where the 1584 go)

| region | bytes | what |
|--------|------:|------|
| init / table builders | 325 | runTable, stateNext, stateMap, stretch, APM, ctx models |
| hot decode+model loop  | 957 | 2-stage mixer, 11 ctx models, match model, APM |
| helpers + data         | 306 | train, contextHash, squash(+66B tab), decodebit, masks/bitm |

The hot loop dominates. Real size wins require a *simpler model* (Phase B);
a bit-exact rewrite (Phase A) can only golf instruction selection.

## Plan

- Phase A: bit-exact asm decoder rewrite — measure pure-golf ceiling.
- Phase B: leaner clean-room model in C, measure ratio per dropped component,
  pick one within budget, write its asm decoder.
- Phase C: golf unfilter.asm further.

## Phase B result — a leaner model CANNOT meet the 5% budget (decisive)

Built a clean-room parameterized context-mixing codec (`research/cm.c`):
direct order-N byte contexts -> adaptive prob, logistic mix, optional match
model. Verified reversible (roundtrips on all corpus). Ratios vs baseline 0.3742:

| config | ratio | vs baseline |
|--------|------:|------------:|
| 4 direct orders | 0.525 | +40% |
| 6 direct orders | 0.519 | +39% |
| 6 orders, rate>>4 | 0.497 | +33% |
| 6 orders + match model, rate>>4 | **0.483** | **+29%** |
| 7-8 orders + match | 0.483 | +29% (plateau) |

The simplest "leaner" model is already ~29% worse and plateaus there. The
shipping model's extra size buys exactly that gap: bit-history STATE MACHINE
maps (not direct prob), the run model, APM/SSE, and a tuned 2-stage mixer over
11 contexts. Removing any of them blows the 5% budget immediately.

**Conclusion:** within a 5% ratio budget there is no smaller model. Phase B is
ruled out by the budget. The viable size win is Phase A: a bit-exact rewrite of
the asm decoder (same predictions, better instruction selection) + Phase C
(golf the unfilter). Keeping `research/cm.c` as the evidence + a reusable
ablation harness.

## Stream layout study — ryg's 20-stream split is already near-optimal

Tools: `research/streamstats.c` (per-stream raw+isolated-compressed cost),
plus true end-to-end `-cx` A/B tests (the only valid signal — see caveat).

Per-stream cost (nasm/ls): opcodes(#0) ~54% of output; jmp-rel32(#17) ~8.5%;
disp32(#13) ~7%; imm8/imm32 ~6/5%; rest small. Structured address streams
(disp32-nobase #14, abs+jumptab #15) compress to 6-16%.

**CAVEAT that killed every "obvious" idea:** compressing a stream in isolation
is NOT a valid signal. The shipping codec runs one continuous adaptive model
over the whole filtered blob, so per-stream tests mis-price both warmup
overhead and cross-stream context. Everything below is measured in the real
`-cx` path with roundtrip.

Tested, all REJECTED (made real `-cx` worse):
- Merge the 8 per-register disp8 streams (#1-8) into one: isolated test said
  -165..-424 B; real path **+141..+561 B**. Per-register runs are self-similar;
  the model wants them contiguous. ryg right.
- Split modrm out of the opcode stream (#0): **+551..+10193 B**. Opcode→modrm is
  the model's strongest correlation; separating them destroys it. ryg right.
- rel8 (#9) ~85-95%: byte entropy is 7.4-7.6 bits/byte — genuinely near-random
  (small signed PC-relative offsets). Already at the entropy floor; no transform
  helps without modeling instruction semantics.

Verdict: the 20-stream layout is empirically tuned and resists local changes.
No stream add/remove/merge beat it. Real ratio gains would need a fundamentally
different (e.g. instruction-semantic) model, out of scope for the size goal.

## Round 2 — 16 experiments (transforms + routing), one WIN shipped

Harness `research/streamexp.c` (length-preserving reversible transforms, real
`-cx` measurement). All deltas vs baseline 169607 (sum over cat/ls/gcc/nasm).

Transforms (byteplane / dword-delta / byte-delta / MTF on the dword & byte
streams): 15 of 16 hurt; only byteplane-rel32 −276 (too marginal). Reconfirms
the model already models within-stream structure.

Routing (parallel subagents + local):
- disp8 regrouping (esp/ebp vs rest; even/odd; pairs): +249..+716. ryg right.
- 0x0f-suffix → own stream: +1064. 66-prefix split: needs decoder rework.
- push/pop reg (0x50-5f) → side stream w/ placeholder: **+1887**. The register
  is *inside* the opcode byte (the decode spine), so it can't move without a
  per-instruction placeholder, and the model already predicts push/pop well in
  opcode context. Idea sound, result negative.
- **immediates split by opcode class: WIN.** Different instruction kinds emit
  immediates with different distributions; own streams let the model adapt.
  - imm8: #20 group-arith(0x80/0x83), #21 AL-forms(04/0c/24/2c/34/3c), #10 rest
  - imm32: #22 mov-r/m(0xc7), #23 mov-reg(0xb8..bf), #12 rest
  V1(coarse) −1163, V3(imm32 c7/b8 split) −1391, V4(imm8 3-way) −1211,
  **V5 = V4+V3 combined −1441 (−0.85%)**. SHIPPED.

V5 cost: 4 extra streams (20→24, +16 B header/file, already netted) and ~89 B
in the asm unfilter (it now computes the per-opcode immediate stream index).
Validated: filter fuzz 8057 ASan+UBSan, asm-unfilter differential 3046 cases,
corpus 9/9 byte-exact.
