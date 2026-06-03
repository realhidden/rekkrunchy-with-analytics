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
