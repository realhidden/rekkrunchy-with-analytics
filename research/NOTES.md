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

## Round 3 — 16 more experiments, three more WINS shipped

Harness: `research/gen16.py` + parallel local build/measure (16 jobs at once).
All vs V5 baseline 168166 (sum over cat/ls/gcc/nasm), real `-cx` + roundtrip.

Top results:
- **e12 rel32 absolute (drop zigzag): −1258** — the biggest, and it *removes*
  code. Storing the absolute jump target compresses better than ryg's delta+
  zigzag transform; the C filter/unfilter AND the asm unfilter all get simpler.
- **e8 disp32 SIB-split: −451** — route SIB-based disp32 (modrm low3==4) to its
  own stream #24, separate from base-relative disp32 (#13).
- **e5 push-imm32 (0x68) → #25: −166** on top of e8.
- e1 rel32 byte-plane −300, e7 imm8-test −39, e6 imm8-shift −100: minor / not
  taken (conflict with e12 or marginal).
- NEGATIVE: e2 disp32 SoA +2919, e16 per-opcode modrm-split +2772, e3 rel32
  jmp-vs-jcc +286, e14 mov-reg SoA +458, e10 disp8 partial-merge +135. Byte
  reordering (SoA/byteplane) on the big streams consistently loses — the model
  wants the natural byte order.

SHIPPED e12+e8+e5 (stacked, additive): 169607 → 166286, **−3321 (−1.96%)** vs
the original 20-stream filter. Streams 20→26. asm unfilter 619→626 B (the
zigzag removal paid for most of the disp32/push routing). Validated: filter
fuzz 8057 ASan+UBSan, asm-unfilter differential 3046 cases (incl. jump table),
corpus 9/9 byte-exact.

## Round 4 — call-target / disp8 / abs retests: ALL negative, vein exhausted

Tested vs 166286: wider funcTable for calls (1024/4096/16384 slots, 2-byte
index), abs+jumptab split, rel32-absolute SoA, call-new SoA, disp8 tiny-bucket
merge, imm8 test-al group. Best *valid* result was disp8 tiny-merge −65 B
(0.04%, noise); everything else ≥0 or worse, SoA on the now-absolute target
streams still loses (+921..+2677).

The funcTable idea looked promising but is dead on the merits: instrumenting the
real filter shows the 255-slot table already hits **78–87%**, and the misses are
only 200–665 *distinct* first-use targets (fewer than 255 for gcc/ls) — i.e. the
table is NOT capacity-thrashing, so widening it can't convert misses to hits,
while a 2-byte index would add ~1.5–5 KB. (The f1–f3 builds also FAIL roundtrip
— buggy wrap symmetry — but the analysis kills it regardless.)

Conclusion: after 3 productive rounds (−1.96% total) the stream-splitting vein
is exhausted. Remaining streams are near-entropy (rel8, opcodes) or already
optimally split. Nothing shipped in round 4.

## Round 5 — "craziest ideas": stream PERMUTATION + transforms (16, parallel)

New lever finally tried: **physical stream concatenation order**. The model runs
continuously across the blob, so adjacent streams share context. Permuting the
payload order (header sizes stay index-ordered; decoder bakes the same order)
is reversible. Identity perm = +0 (infra sanity ✓). Findings vs 166286:
- byte transforms: **c7 big-endian imm32 (bswap) −179** (best, trivial); rel8
  zigzag +7, call-target delta +397, rel8/call-idx merge +892, SoA on absolute
  targets +900..+2677 (still lose).
- permutations: reverse +667 (order matters!); semantic clusterings help a bit —
  disp-cluster −139, family −137, rel8-last −127; most others ±small.
- c7 (bswap) + c11 (disp-cluster perm) compose to −311.

SHIPPED: **c7 only (big-endian imm32)**, −179 B → corpus 169607→166107
(**−2.06%** cumulative). The permutation was REJECTED on cost: it saved ~132 B
per payload but the asm unfilter's permuted setup loop + order table costs
~133 B of decoder — a net wash for single-payload self-extraction. bswap alone
is +3 B of decoder for −179 B/payload, clearly worth it.

## Round 6 — 32 experiments (12 transforms + 20 permutations), parallel

Best: **w12 — byte-swap ALL the absolute 4-byte target streams** (rel32 #17,
call-new #18, moffs #15), the same high-byte-clusters insight as imm32-bswap
applied to targets. −618 B vs 166107, roundtrips full corpus. Subsets: rel32
alone −421, abs alone −52, callnew alone +43 (so the win is mostly rel32).
Negatives confirmed: rel32 pc-relative +2190, disp32 little-endian +444 (ryg's
bswap was right), all 20 PERMUTATIONS worse (+140..+667 incl. 12 random) —
index order is locally optimal, permutation conclusively dead. half-word swap,
hi-byte split, target deltas, stream merges all lost.

SHIPPED w12. asm cost +7 B (three lodsd/bswap, one movsd→lodsd/bswap/stosd).

Final tally across all rounds: original 20-stream filter 169607 → **165489**,
**−4118 B (−2.43%)**. ls −9.6%, gcc −12.2%, nasm −11.9% vs plain. asm unfilter
530→636 B. Every 4-byte stream is now stored big-endian (high byte clusters);
the model likes that uniformly. Stream optimization closed out for real.

## Round 7 — rel8/disp8/disp32 normalization + opcode delta: ALL negative

Targeted ideas (zigzag stack displacements, rel8-as-absolute-low-byte, opcode
delta/xor). vs 165489, all worse:
- rel8_abs_low8 +40 (valid). The clean "same win as rel32" bet FAILS here: rel8
  is only 1.6–4.2 KB and already 7.4–7.6 bits/byte (near-random). rel32 won
  because it's high-volume (24 KB) with repeated targets; rel8 short branches go
  to nearby mostly-distinct targets, so abs-low-byte is just as random AND tiny,
  so even a perfect transform saves nothing. rel8 is a dead stream by nature.
- disp8 zigzag (all / ebp / esp / ebp|esp): +93..+256. The model already handles
  the f8/08 small-offset bimodality; zigzag disrupts its learned contexts.
- disp32 zigzag on stack base: +399..+475.
- opcode xor/sub-prev: +21000 (catastrophic) — byte history is exactly what the
  model already uses; delta destroys it. (Also fragile vs other stream-0 writes.)

Confirms the standing rule once more: within-stream value transforms lose;
only field SEPARATION and big-endian SERIALIZATION of high-volume structured
streams help. Nothing shipped.
