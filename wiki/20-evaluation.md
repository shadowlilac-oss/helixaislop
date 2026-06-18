# Helix — Evaluation Plan

*How to actually prove or falsify every Helix claim: metrics, baselines, benchmark suites, concrete experiments, and the specific results that would sink the thesis.*

This page is the empirical contract for Helix. The honest pitch (see [Overview](00-overview.md)) makes four claims, and each must be made *falsifiable* here, not merely argued. We do **not** claim Helix beats LLVM/GCC `-O3` output quality — no prior graph IR has shown that (R1). We claim: (1) a dramatically smaller codebase at parity; (2) optimization parity with the best graph IRs via *structural* GVN/LICM/DCE (D7), not equality saturation; (3) a uniquely unified comptime-and-codegen-in-the-graph; (4) zero IR round-trip at either end (D1). The job of this plan is to attach a number, a baseline, and a falsification condition to each.

---

## 1. The four claims, restated as testable hypotheses

| ID | Claim (honest pitch) | Primary metric | Baseline(s) | Falsified if… |
|----|----------------------|----------------|-------------|---------------|
| **H1** | Optimization parity with best graph IRs / `-O2`; approaches `-O3` on numeric code | Runtime of generated code (geomean speedup vs baseline) | LLVM `-O2`/`-O3`, Cranelift, jlm/RVSDG | Geomean runtime > 1.15× slower than `-O2` on numeric suite |
| **H2** | Tier-1 normalization is cheaper than equality saturation at equal/better quality | Compile time (ms) at equal output quality | egg/egglog-style eqsat overlay; Cranelift aegraph | Tier-1 is both slower *and* lower quality than a saturating baseline |
| **H3** | Dramatically smaller codebase at feature parity | Source LOC (core IR, backend, total) | jlm/RVSDG, MimIR, Cranelift | Helix core+backend LOC ≥ jlm IR-core+bridge for the same surface |
| **H4** | Unified comptime + zero round-trip are real, working differentiators | Comptime expressiveness (case studies); round-trip LOC = 0 | Thorin/MimIR PE; any LLVM-emitting graph IR | Helix needs a secondary IR at either end, or comptime cannot express the case studies |

Each row maps to an experiment in §6. H1 carries risk R1/R3/R6; H2 tests the *restraint* stance D7 (no full equality saturation); H4 carries R2 (comptime termination) and R7 (one-DSL-for-comptime).

---

## 2. Metrics — exact definitions

Vague metrics produce unfalsifiable claims. Each metric below has a measurement procedure.

### 2.1 Output quality (runtime of generated code)
- **Definition:** wall-clock runtime of the compiled binary on a fixed input, geometric mean of per-benchmark ratios `t_helix / t_baseline`. Lower is better; 1.0 = parity.
- **Procedure:** N=30 runs, report median per benchmark, discard 3 warmup runs, pin to one physical core, disable turbo/SMT, `perf stat` for cycles + instructions retired. Report cycles (deterministic) alongside wall-clock.
- **Secondary quality signals (when runtime is noisy):** static instruction count, dynamic instruction count, retired-µops, spill count from the register allocator, code size (bytes). These isolate *where* a regression comes from (ISel vs schedule vs RA — DC15/R3).
- **Honest note:** runtime is the only metric that matters to a user, but it is the noisiest and most backend-maturity-sensitive (R3). Early in development, dynamic instruction count is the leading indicator of *mid-end* quality independent of an immature backend.

### 2.2 Compile time
- **Definition:** front-to-bytes wall time, broken into phases: parse+construct (with eager Tier-1, DC8/DC12), Tier-2 overlay + extraction (DC15), schedule, regalloc, encode.
- **Procedure:** report per-phase ms and total; report normalization-rule-applications and hash-cons hit-rate (Tier-1 is supposed to give GVN/fold with *no worklist sweeps* — measure that it actually does).
- **Key derived metric:** *fixpoint iterations avoided*. Tier-1's pitch (D6) is "no saturation fixpoint, no V8-style cache thrash (failure mode 7)." Instrument: count node revisits and L1-dcache misses per node constructed; compare against a worklist-rewrite control implementation.

### 2.3 Codebase LOC
- **Definition:** non-blank, non-comment source lines (`cloc`/`tokei`), reported in three buckets that match the synthesis matrix so comparisons are apples-to-apples:
  - **IR core** (node taxonomy, World factory, hash-consing, Tier-1 normalizer).
  - **Backend** (lowering rules, extraction, schedule, regalloc, encode).
  - **Round-trip** (any front-end IR or secondary backend IR — *target is exactly 0*, D1).
- **Why three buckets:** the synthesis shows jlm's cost is the *round-trip* (~94.6k LoC bridge vs ~22.8k IR core), not the IR (failure mode 1). The headline number must separate "IR is small" from "we deleted the bridge."

### 2.4 Comptime expressiveness
- **Definition:** a qualitative + quantitative scorecard over a fixed set of comptime idioms (§5). Quantitative: does it compile, does it terminate within the reported fuel budget, what is the residual-graph node count vs a hand-specialized baseline. Qualitative: did it require programmer annotations beyond the default policy (DC10)?
- **Honest note:** comptime is Turing-complete; no metric can show "always specializes and always terminates" (R2). We measure *what fraction of the idiom set succeeds within budget*, and we report fuel consumed (DC11) — leaking-into-UX is a known cost, not a hidden one.

### 2.5 Optimization parity (per-transform)
- **Definition:** for a fixed catalog of expected transforms (constant fold, GVN/CSE, LICM, DCE, dead-func/unreachable elim, strength reduction, load/store forwarding), a binary "did Helix perform it" + the resulting dynamic-instruction delta.
- **Procedure:** diffable IR dumps (DC16) before/after; assert the structural change. This is how we show GVN/LICM/DCE are *emergent from the form* (D7) rather than bespoke passes — see [Optimizations](16-optimizations.md).

---

## 3. Baselines

| Baseline | Version pinned | Used for | What it proves/disproves |
|----------|----------------|----------|--------------------------|
| **LLVM `-O2`** | clang/opt, fixed release | H1 output quality, compile time | The realistic parity target; "matches `-O2`" is the defensible claim |
| **LLVM `-O3`** | same | H1 stretch | Numeric: approach it; effect-heavy: expect a gap (R6) |
| **GCC `-O3`** | fixed release | H1 cross-check | Guards against over-fitting to LLVM's choices |
| **Cranelift** | fixed release | H1 (fast-compiler peer), H2, H3 | The existence proof for direct codegen (DC13) and aegraph (D6/D7); the *right* peer for a fast non-LLVM backend |
| **jlm / RVSDG** | published prototype | H1 (graph-IR opt), H3 | The "less code at parity" comparison for the IR core + the round-trip cost (failure mode 1) |
| **MimIR / Thorin-2** | POPL'25 artifact | H3, H4 | Smallest typed graph IR (~32k LoC; core ~9.4k) — the LOC bar to beat; comptime/PE comparison |

Notes on fairness:
- **Same source programs, same target ISA (x86-64 first), same input data.** Where a baseline emits LLVM IR (jlm, Thorin, MimIR), the *output-quality* number is really "their mid-end + LLVM backend" — so for H1 vs those systems, also report Helix's *mid-end-only* signal (dynamic instruction count pre-RA) to separate mid-end quality from backend maturity (R3).
- **Cranelift is the load-bearing peer for H1/H2/H3** because it is the only baseline that, like Helix, owns direct codegen and an acyclic eager-rewrite mid-end. If Helix cannot match Cranelift, the thesis is in trouble independent of LLVM.

---

## 4. Benchmark suites

Three suites, chosen to separate the *numeric* case (where the hybrid float pays, D2) from the *effect-heavy* case (where it provably pays less, R6).

```
Suite A — MICRO (mid-end isolation)
  small kernels, mostly pure, dominated by one transform each:
    saxpy, dotprod, matmul-naive, mandelbrot, crc32, fib(comptime),
    strength-reduce loop, redundant-load chain, dead-branch ladder
  purpose: per-transform parity (§2.5); cheap to dump+diff (DC16)

Suite B — REAL-SMALL (end-to-end)
  ~10 small-but-complete programs with mixed pure/effectful code:
    json-parse, regex-match, sha256, a bytecode interpreter loop,
    a small ray tracer, an LZ-style compressor, a hashmap stress
  purpose: H1 geomean runtime; exercises schedule + regalloc + encode

Suite C — COMPTIME-HEAVY
  programs whose point is compile-time specialization:
    typed printf, comptime-sized vector / SIMD width selection,
    generic container monomorphization, a comptime-unrolled FFT,
    a comptime regex -> DFA compiler, a comptime config -> dispatch table
  purpose: H4 comptime expressiveness + the unified-engine claim (D3)
```

Suite split is deliberate: A and C are where Helix should look *good* (pure float + unified comptime); B is where the honest gaps live (effect-heavy, backend-maturity). Reporting them separately is what keeps the pitch honest rather than cherry-picked.

---

## 5. Comptime case studies (H4 detail)

Each is a fixed program plus a success rubric. Comptime is Tier-1 reduction run on the graph (NbE: eval to a semantic domain, reify dynamic values back as neutral `Op` nodes — see [Comptime](15-comptime.md)). The studies probe the *unified-engine* claim (D3): the same reduction that folds also specializes.

1. **Typed `printf`.** A `@comptime` format-string parse produces a residual graph with no string at runtime. *Success:* residual graph contains zero string ops; fuel reported; no annotation beyond marking the format `static`.
2. **Comptime FFT unroll.** `fn fft(static %n: i32, %x: ptr, %s0: state)` fully unrolls for a static size. *Success:* loop region eliminated; residual is a straight-line state-threaded schedule; node count within 1.2× of hand-unrolled.
3. **Generic monomorphization.** A container generic over a `static` type value (types are `Const` values, DC1/taxonomy form 1). *Success:* one specialized `Func` per type, memoized by (function, static-args) (DC11) — verify the memo cache prevents re-specialization.
4. **Comptime regex → DFA.** Higher-order/recursive comptime. *Success:* terminates within budget *or* reports fuel exhaustion with a localizable site (DC11) — **a clean fuel-exhaustion report is a passing result**, an unbounded hang is a failure (R2).

```
; typed printf, sketch in canonical textual format
func @logline(static %fmt: str, %x: i32, %s0: state) -> state {
  ; @comptime reduction (Tier-1, NbE) consumes %fmt entirely;
  ; dynamic %x survives as a neutral term -> reified Op
  %s1 = call @print_i32 %x, %s0      ; residual: no format string at runtime
  return %s1
}
```

Rubric column per study: {compiles? · terminates-in-budget? · residual-node-count vs hand-spec · annotations-needed}. H4 is falsified if the default staging policy (DC10: specialize type-level/higher-order args, defer the final run-time continuation) cannot drive these without ad-hoc per-program hacks — that would mean comptime is not really *the same engine* (R7).

---

## 6. The experiments

### E1 — Optimization parity vs `-O2`/`-O3` (numeric and effect-heavy)
- **Setup:** Suite A (per-transform) + Suite B (geomean). Targets x86-64. Report runtime, cycles, dynamic instruction count.
- **Procedure:** (a) §2.5 per-transform binary checklist on Suite A with diffed IR dumps; (b) geomean runtime ratio on Suite B, split into a *numeric subset* and an *effect-heavy subset*.
- **Expected (honest):** numeric subset — match `-O2`, approach `-O3`; effect-heavy subset — match-to-slightly-behind `-O2`, clearly behind `-O3` (R6: little floats when most ops are effectful; R3: immature backend). Against jlm/RVSDG, expect mid-end parity (RVSDG gets CSE/LICM/DCE as single passes too, failure-mode analysis).
- **Falsifies H1 if:** numeric geomean > 1.15× slower than `-O2`, *or* a transform in the §2.5 catalog simply does not occur (would mean a structural win D7 is not actually emergent).

### E2 — Tier-1 cost vs an equality-saturation baseline
- **Setup:** implement a *control*: the same rewrite rules run as (a) Tier-1 eager smart constructors (the shipping path) vs (b) an egg/egglog-style saturating overlay over the same acyclic graph.
- **Procedure:** on Suite A+B, measure compile time, rules applied, fixpoint iterations, and output quality (dynamic instruction count) for both.
- **Expected (honest):** Tier-1 is much faster at equal or near-equal quality, mirroring Cranelift's finding that the radical e-graph bought ~0.1% runtime for real compile cost, and that saturation often does not terminate (failure mode 5). This is a *restraint* result (D7), not a magic-bullet result.
- **Falsifies H2 if:** the saturating baseline produces materially better code (say > 2% dynamic-instruction reduction) at tolerable compile cost — that would undercut the "don't saturate" stance (D7) and argue Tier-2 should be larger.

### E3 — Codebase size comparison
- **Setup:** `tokei`/`cloc` on Helix and each peer, bucketed per §2.3.
- **Procedure:** report IR core, backend, round-trip separately; normalize by *feature surface* (note which peers lack exceptions, intrinsics, full ISel — e.g., RVSDG prototype lacks exceptions/intrinsics, so a raw LOC compare flatters Helix and must be footnoted).
- **Expected:** Helix's clearest, lowest-risk win (D1). Round-trip bucket = 0 by construction (parse direct in, emit direct out). Headline: "core+backend at parity with Cranelift/MimIR scale; **and** we deleted the jlm-style ~6× bridge entirely."
- **Falsifies H3 if:** Helix core+backend LOC for a *matched* feature surface is not meaningfully below jlm(core+bridge) and not in the same small league as MimIR — i.e., the minimal-taxonomy (DC17) + one-rule-DSL (DC14) bet did not pay off.

### E4 — Comptime case studies (§5)
- **Setup:** Suite C + the four rubric studies.
- **Procedure:** run each, fill the rubric, report fuel.
- **Expected:** studies 1–3 succeed within budget under the default policy; study 4 either succeeds or reports a clean, localizable fuel exhaustion (both are passes — R2 honesty).
- **Falsifies H4 if:** any study needs a *secondary interpreter* (would break "comptime IS graph reduction," DC9), or the engine diverges without a budget cutoff (R2), or expressiveness requires abandoning the minimal taxonomy.

### E5 — Ablation of fine-grained state (DC4 / R4)
- **Setup:** two configurations of Helix: **fine-grained** (multiple independent state tokens per alias class/region, DC4) vs **single-state** (one global state token, the RVSDG/jlm fallback that "leaves significant parallelization potential unused," failure mode 9).
- **Procedure:** on Suite B's effect-heavy subset, measure scheduling freedom (count of pure ops that can float past an effect), reordering opportunities realized, and runtime delta.
- **Expected:** fine-grained state widens the float and improves effect-heavy results — *but only if the alias analysis populates states precisely* (R4). If the analysis is weak, fine-grained collapses toward single-state and the ablation shows ~no gain — which is itself the honest finding the synthesis predicts.
- **Why this ablation matters:** D4 is the lever for optimizing *better* (not just smaller). E5 is the experiment that says whether that lever is real or aspirational (R4). It also directly probes R6: how much of the effect-heavy gap is "nothing floats" vs "alias analysis too coarse."

---

## 7. Reporting format

A single results table per experiment, plus diffable artifacts. Example skeleton for E1:

| Benchmark | Subset | t_helix/t_O2 | t_helix/t_O3 | dyn-insns vs O2 | spills | notes |
|-----------|--------|--------------|--------------|-----------------|--------|-------|
| matmul-naive | numeric | 1.02 | 1.11 | +1.5% | 0 | parity O2, behind O3 (R3 sched) |
| json-parse | effect | 1.18 | 1.34 | +9% | 4 | R6: little floats; R3 RA |
| dotprod | numeric | 0.99 | 1.04 | −0.5% | 0 | — |

All IR dumps are committed in the canonical textual format (DC16) so a reviewer can re-derive *why* a number moved:

```
; before LICM-emergent placement (Tier-1 GVN already applied at construction)
func @sum(%n: i32, %p: ptr, %s0: state) -> i32 {
  %r = loop (%acc = 0, %i = 0) : i32 {
    %c   = cmp.lt %i, %n
    break unless %c -> %acc
    %base = add %p, 0            ; loop-invariant pure Op -> floats out at schedule time
    %acc1 = add %acc, %i
    %i1   = add %i, 1
    continue (%acc1, %i1)
  }
  return %r
}
```

The corresponding rule-DSL artifacts (the rules that fired) are reported alongside, since the same DSL drives folds, peepholes, and lowering (DC14):

```
rule add-zero : (add ?x (const _ 0)) => ?x                 ; Tier-1 fold, fired 3x
rule fold-add : (add (const ?t ?a) (const ?t ?b)) => (const ?t {a + b})
lower lea     : (add ?b (mul ?i (const i64 ?s))) => (x64.lea ?b ?i ?s) @cost 1 if member(?s, {1,2,4,8})
```

---

## 8. Threats to validity

| # | Threat | Mitigation |
|---|--------|------------|
| T1 | **Backend immaturity confounds H1** (R3). A bad number may be ISel/schedule/RA, not the IR. | Always report dynamic-instruction count (mid-end signal) beside runtime; report spills separately; compare mid-end-only vs Cranelift/LLVM. |
| T2 | **LOC compare flatters Helix** (peers have more features). | Bucket per §2.3; footnote missing features (RVSDG: no exceptions/intrinsics); normalize by feature surface. |
| T3 | **Cherry-picked benchmarks.** | Fixed, pre-registered suites (§4); numeric and effect-heavy reported separately; never report a single blended geomean alone. |
| T4 | **"Same source" isn't really same** (different frontends emit different code). | One Helix frontend, parse-direct (DC12); for peers, use their own frontend but compare mid-end signals where possible. |
| T5 | **Comptime fuel hides divergence as "success."** | Report fuel consumed per case; a fuel-exhaustion is reported as such, never silently dropped (R2/DC11). |
| T6 | **Hash-cons pointer-equality fast path is only valid for closed terms.** | Verify GVN correctness with an open-term test set; assert the fast path is gated on closedness (DC8 caveat). |
| T7 | **Measurement noise** swamps small deltas. | N=30, median, pinned core, cycles + wall-clock, report variance; treat <2% runtime deltas as noise. |

---

## 9. What would falsify the whole thesis

The honest pitch survives a *lot* of bad numbers (we never claimed to beat `-O3`). It does **not** survive these:

1. **Helix needs a secondary IR at either end.** If parse-direct (DC12) or emit-direct (DC13) proves infeasible and a front-end IR or backend graph creeps back in, D1 — the single biggest and lowest-risk claim — is dead. *This is the thesis-defining falsifier.*
2. **No codebase win at parity (H3 fails E3).** If matching the feature surface requires LOC in jlm's league once the round-trip is counted, the "dramatically smaller" claim collapses and the minimal-taxonomy/one-DSL bet (DC14/DC17) failed.
3. **Tier-1 is dominated (H2 fails E2).** If a bounded saturating overlay is both faster *and* better, the "structural wins without saturation" stance (D7) is wrong and Tier-2 should be the main engine — a different IR.
4. **Comptime is not the same engine (H4 fails E4).** If the case studies need a separate interpreter, DC9 is false and the unified-comptime differentiator (D3) evaporates.
5. **Mid-end parity fails on numeric code (H1 core).** If, controlling for the immature backend (dynamic-instruction count vs LLVM/Cranelift mid-end), Helix *still* misses the §2.5 structural transforms, then "matches the best graph IRs on optimization via the form" (D7) is false — and that is the optimization half of the pitch.

Note what is **not** on this list: lagging `-O3` on effect-heavy code (R6), or lagging LLVM on raw runtime while the backend matures (R3). Those are *predicted*, disclosed, and survivable. Confusing "predicted gap" with "falsification" is the trap; this section draws that line explicitly.

---

## 10. Phasing the evaluation (ties to the implementation plan)

The experiments come online in the same order capabilities land (see [Implementation Plan](19-implementation-plan.md)):

```
Phase 1 (IR core + Tier-1)      -> E3 (LOC), E2 (Tier-1 vs eqsat), Suite A per-transform (E1 mid-end)
Phase 2 (comptime / NbE)        -> E4 + Suite C
Phase 3 (lowering + extraction) -> E1 dynamic-insn vs LLVM/Cranelift mid-end
Phase 4 (schedule + regalloc + encode) -> E1 runtime (R3 begins to resolve), E5 fine-grained state
```

This ordering means the **lowest-risk, highest-confidence wins (E3 LOC, E2 Tier-1 cost, E4 comptime) are demonstrable early**, before the high-risk backend-maturity numbers (E1 runtime, R3) are even meaningful. That is the honest sequencing: prove D1/D5/D6/H3/H4 first; let H1 runtime mature.

---

## See also

- [Overview](00-overview.md) — the honest pitch and claim summary this plan tests
- [Design Rationale](10-design-rationale.md) — DC/D/R provenance for every claim here
- [Optimizations](16-optimizations.md) — the structural GVN/LICM/DCE wins E1 verifies (D7)
- [Reduction Engine](14-reduction-engine.md) — Tier-1 vs Tier-2; the subject of E2
- [Comptime](15-comptime.md) — NbE + filters + fuel; the subject of E4 (R2)
- [Codegen](17-codegen.md) — lowering/schedule/regalloc/encode; the source of R3 in E1/E5
- [Types and Effects](13-types-and-effects.md) — fine-grained state tokens; the subject of E5 (DC4/R4)
- [Risks and Open Problems](22-risks-and-open-problems.md) — R1..R7 in full
- [Synthesis](research/00-synthesis.md) — prior-art matrix and the DC/D/R register
```