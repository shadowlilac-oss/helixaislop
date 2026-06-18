# Helix — Design Rationale

*The bridge from prior-art research to the Helix design: every decision traced to a constraint (DC1–DC17), a recurring failure mode, a differentiator (D1–D7), and its honest risk (R1–R7).*

This page does not introduce Helix from scratch — see [Overview](00-overview.md) and [Core Model](11-core-model.md) for that. Its job is to *justify* the design: for each design constraint distilled in [the synthesis](research/00-synthesis.md), it states the Helix decision and the prior-art finding that forces it; it then walks the twelve recurring failure modes and shows how the Helix form sidesteps each; presents the seven differentiators with their risks inline; lists what we deliberately did *not* build; and closes with the honest thesis.

Helix in one line: **ONE graph, ONE reduction, source to silicon.** A single acyclic hash-consed SSA graph in which optimization, compile-time evaluation, instruction selection, and lowering are all the same rewrite process — parsed directly from source, emitted directly to machine code, with no secondary IR at either end.

---

## 1. Design constraints → Helix decisions

Each row: the constraint from the synthesis, the concrete Helix decision that satisfies it, and the prior-art finding that justifies it. Every constraint is traced to a numbered source note in [the synthesis](research/00-synthesis.md).

| DC | Helix decision | Justifying prior-art finding |
|---|---|---|
| **DC1** — one graph fuses data + control + multi-state | A single graph carries both strands: the **value strand** (pure data deps) and the **state strand** (linear effect skeleton). Control lives in `Cond`/`Loop` region nodes *inside* the same graph, not in a side table. | Click's combined CCP+DCE+GVN fixpoint only works because control and data share one structure `[01]`; RVSDG's one node algebra collapses dead-code/dead-func/unreachable into a single pass `[02]`. |
| **DC2** — single-origin (use→def) edges = enforced SSA | Every value edge is single-origin: **a value IS its defining node.** No phi nodes, no SSA-restoration passes, ever. | RVSDG's "every input is the user of exactly one edge" is precisely what keeps it "always in strict SSA," erasing 14 LLVM SSA-restoration passes `[02]`; in Sea of Nodes "a value *is* a node" `[01]`. |
| **DC3** — pure floats; effects pinned and ordered | The two-strand split *is* the hybrid: pure `Op`s float and are placed only at schedule time; effectful `Op`s consume and produce a state token and are pinned in program order along the state strand. | V8's collapse-to-CFG `[01]`, Cranelift's side-effect skeleton, and Peggy's σ-summaries independently converged on "pure floats, effects pinned" `[05][06]`. |
| **DC4** — explicit, typed, fine-grained, linear state | State is a first-class `state`-typed value, **used exactly once (linearity enforced structurally)**. Multiple independent state tokens (per alias class / region) exist from day one. | RVSDG state edges + multiple disjoint states expose independence a CFG cannot `[02][06]`; MimIR's `%mem.M` token — but with linearity *enforced*, closing MimIR's admitted unenforced hole `[04]`. |
| **DC5** — block parameters, not φ | Region **ports** are block parameters. `Cond` branch regions share input ports and produce identical result ports; `Loop` loop-carried values are ports. There are no phi nodes anywhere in Helix. | MLIR and Cranelift both dropped φ for block/region arguments — cleaner semantics, no "must be first in block," easier rewriting, allocator consumes them directly `[06][08]`. |
| **DC6** — minimal structured control → unique legal schedule | Exactly two region forms: symmetric `Cond` (the γ/gamma) and tail-controlled `Loop` (the θ/theta). `while`/`for` are compositions. A unique legal schedule is always readable off the state strand. | RVSDG's single tail-loop form + symmetric γ make optimizations single-pass and guarantee a legal schedule `[02]`; the VSDG sequentialization disaster is what happens without it `[06]`. |
| **DC7** — acyclic; recursion without graph cycles | The graph is a **DAG**. Loops are `Loop` regions; recursion lives in `Module` recursion groups (the ω/δ). No back-edges, ever. | RVSDG expresses recursion via φ-regions without cycles; Cranelift's aegraph is acyclic by construction; cycles are exactly what defeat optimal extraction `[02][05][06]`. |
| **DC8** — hash-consing + construction-time normalization | Structural nodes (`Const`, `Op`) are interned by the `World` factory; structural equality == pointer equality == automatic GVN/CSE. Smart constructors apply oriented rewrites to local fixpoint *at construction time* (Tier 1). | Thorin's `World` and MimIR fold/CSE/UCE on the fly, "vastly decreasing fixed-point iterations" `[03][04]`; Cranelift's eager single-pass rewrites avoid egg-style rebuild and V8 cache-thrash `[05][08]`. Caveat carried: pointer-equality ⇒ α-equality only for *closed* terms `[04]`. |
| **DC9** — comptime IS graph reduction | Comptime is not a separate interpreter — it is Tier-1 reduction run on the graph, implemented as Normalization-by-Evaluation (eval to a semantic domain, reify back to nodes). | The task requirement, and Thorin/MimIR proving "specialize a `Def`" subsumes inlining, unrolling, folding, and comptime call eval `[03][04][07]`. |
| **DC10** — programmer-visible static/dynamic via filters | Staging is exposed through **filters / annotations** (`@comptime`, `static` params), never opaque heuristics. Default policy: specialize type-level and higher-order args aggressively, defer the final run-time continuation. | Thorin's *documented retreat* from termination heuristics to Schism-style filters `[03][07]`; MimIR's per-function `@e` filters `[04]`. |
| **DC11** — bounded, hermetic, memoized comptime | A **per-call, introspectable, reported** fuel/step budget (learning from Zig's ungettable global quota). Comptime is hermetic/deterministic, so results are cacheable; **memoize by (function, static-args)**, sound only under purity. | The universal termination wall; Zig/C++ budgets; memoization sound only under purity, paired with the state strand (DC3/DC4) `[07]`. |
| **DC12** — parse directly into the graph; no front-end IR | The parser builds Helix nodes directly via on-the-fly SSA construction (Braun et al. 2013, no up-front dominance). Imperative control → `Cond`/`Loop`; mutation/IO → state threading; expressions → interned value strand (GVN during parsing for free). | Thorin's Impala and MimIR's Mim construct the graph directly from the AST via Braun-'13 SSA construction `[03][04]`. |
| **DC13** — emit machine code directly; no secondary backend IR | Lowering rewrites portable `Op`s into TARGET `Op`s (e.g. `x64.lea`) *in the same graph*; greedy cost extraction tiles; schedule reads the state strand; SSA regalloc; streaming encode. No separate backend graph. | Cranelift is the existence proof: postorder lowering → `VCode` → in-place regalloc2 → streaming `MachBuffer`, no fixpoint, no separate graph `[08]`. Every prior graph IR failed this by emitting LLVM IR `[02][03][04]`. |
| **DC14** — one declarative, verifiable rewrite DSL for opt *and* lowering | A single rule DSL (`=>` oriented, `~` equivalence, `lower … @cost`) is shared by peepholes, comptime folds, and instruction selection, and is SMT-verifiable (VeriISLE-style). | Cranelift's ISLE drives both mid-end rewrites and backend ISel, overlap/priority-checked and SMT-verifiable `[05][08]`; Alive found 2.4% of hand-written InstCombine transforms buggy `[06]`. |
| **DC15** — tree-match up the pure strand; allocate on SSA pre-destruction | Tiling matches up the pure value strand, stopping at region-port / state-strand boundaries. Register allocation runs on SSA **before** destruction; region-port transfers become parallel moves at the very end. | Cranelift's lowering pulls only pure producers into a match tree; SSA interference graphs are chordal only before SSA destruction; combined schedule+RA is NP-hard `[08]`. |
| **DC16** — printable, diffable, source-mapped from day one | A canonical [textual format](12-format.md) with **stable semantic names** (avoiding Thorin's non-semantic names), diffable and source-mapped. | "Soup of nodes" / "hard to debug" was a first-order reason for V8's retreat `[01]`; MimIR concedes the same `[04]`; non-semantic names hurt debugging `[03]`. |
| **DC17** — small, minimal node taxonomy | **Exactly six node forms:** `Const`, `Op` (structural/pure/floating) and `Cond`, `Loop`, `Func`, `Module` (nominal/region). Nothing else. | MimIR's "fewer syntactic categories ⇒ fewer patterns," shipping a full extensible typed IR + 21 plugins in ~32k LoC `[04]`; Thorin's "only continuations + primops" kept its PE ~500 LoC `[03]`. |

### The two strands at a glance

```
                 value strand (pure, duplicable, FLOATS — placed at schedule time)
   %x ───┐
   %y ───┤
         ▼
       (add) ──────────► %v1            no state edge ⇒ pure ⇒ floats freely
                            │
                            ▼
   state strand (linear, used-once, PINNED & ordered — the effect skeleton)
   %s0 ──► (store %p,%v1) ──► %s1 ──► (load %q) ──► (%v2,%s2) ──► ...
            effectful Op consumes one state token, produces the next
```

Every edge in Helix is exactly one of these two kinds. The value strand replaces sea-of-nodes' separate data edges; the state strand replaces sea-of-nodes' separate control and memory edges *and* RVSDG's state edges — one mechanism for the proven "pure floats / effects pinned" hybrid, with linearity enforced structurally.

A minimal worked snippet showing both strands and ports (no phi anywhere):

```
module demo {
  ; effectful: state token threaded linearly (state strand)
  func @bump(%p: ptr, %s0: state) -> (i32, state) {
    (%v, %s1) = load %p, %s0          ; effectful Op: consumes %s0, produces %s1
    %v1 = add %v, 1                   ; pure Op on the value strand — floats
    %s2 = store %p, %v1, %s1
    return (%v, %s2)
  }
  ; symmetric conditional (gamma) — results are ports (block params), no phi
  func @absdiff(%a: i32, %b: i32) -> i32 {
    %p = cmp.lt %a, %b
    %r = cond %p -> (i32) {
      case 1: { %t = sub %b, %a  yield %t }
      case 0: { %u = sub %a, %b  yield %u }
    }
    return %r
  }
}
```

---

## 2. The twelve recurring failure modes — and how Helix avoids each

The synthesis catalogues twelve failure modes that prior graph IRs demonstrably hit. Helix's form is shaped to dodge each one.

1. **CFG round-trip cost (construct *and* destruct).** RVSDG/jlm spend ~94.6k LoC on the LLVM↔RVSDG bridge vs ~22.8k for the IR core `[02]`; VSDG calls sequentialization "the major obstacle" `[06]`. **Helix:** parse directly into the graph (DC12) and emit directly to bytes (DC13) — *neither* a construction bridge nor a destruction bridge exists. This is the single biggest codebase-size win (D1).

2. **Effect-ordering ambiguity.** A flat value graph under-specifies order; VSDG could "terminate even if the original program would not," and V8 juggled "3 different effect chains" and got it wrong for months `[01][06]`. **Helix:** order is *data* — the linear state strand (DC4). An effect's predecessors are exactly the state tokens it consumes; there is nothing to "rediscover" and nothing ambiguous to get wrong.

3. **The float collapses to a CFG under pervasive effects.** In V8, "almost all generic JS operations can have arbitrary side effects," so the control chain "always mirrors the equivalent CFG" — graph complexity for little reordering benefit `[01][06]`. **Helix:** the effectful case is *natively* a skeleton (DC3). When everything is effectful, the state strand simply *is* a linear CFG-equivalent — and is as cheap and inspectable as one. We never pay a collapse tax because we never assumed pervasive float. (This is also the cap on our upside — see R6.)

4. **Scheduling / sequentialization cost and oscillation.** V8's scheduler re-duplicated shared pure nodes and could oscillate; VSDG faced "duplicate everything → exponential" vs "compute early → wasted work" `[01][06]`. **Helix:** the state strand *is* the schedule skeleton (DC6). Scheduling = read the order off the strand and list-schedule the floating pure `Op`s into it. A legal schedule always exists and is cheap to read off; there is no fixpoint to oscillate.

5. **E-graph blowup + NP-hard extraction.** Equality saturation often doesn't terminate (Peggy: 84% saturated; Pueblo ILP avg 1,499 ms/method) and optimal DAG extraction is NP-complete `[05][08]`. **Helix:** Tier 1 is eager oriented normalization (`=>`) at construction — no worklist, no saturation. Tier 2 is a *bounded, acyclic, append-only* overlay used sparingly (mainly ISel) with greedy bottom-up cost extraction (BURS-style DP), never full saturation (D7).

6. **Debuggability / "messy soup of nodes."** A first-order reason for V8's retreat `[01]`; MimIR concedes graph IRs need external tools `[04]`. **Helix:** the canonical textual format with stable semantic names is mandatory from day one (DC16), and the state strand gives a readable linear backbone. Mitigation, not cure — see R5.

7. **Cache thrash from in-place mutation / wasted visits.** V8 nodes were "visited 3 times but lowered once," ~3–7× more L1 dcache misses `[01][06]`. **Helix:** construction-time normalization (DC8) plus an *append-only* Tier-2 overlay (no mutating worklist sweeps) — work happens once, at construction or as an append, not via repeated re-visits.

8. **Comptime non-termination & the termination wall.** Thorin abandoned its 2015 heuristic; a `tt` filter can make MimIR's type checker diverge `[03][04][07]`. **Helix:** programmer-visible filters (DC10) instead of opaque heuristics, plus a per-call introspectable fuel budget (DC11). This *bounds* the wall — it does not abolish it (R2).

9. **Conservative single-state memory in practice.** RVSDG/jlm "had the representation" yet fell back to one memory state, "leaving significant parallelization potential unused" `[02]`. **Helix:** the type system carries many fine-grained states from day one (DC4); an alias pass writes its results back *as additional state tokens* (D4). The payoff is gated on that analysis (R4).

10. **Phase-ordering / opt-vs-lowering boundary.** Combined schedule+RA is NP-hard; SSA interference graphs are chordal only before SSA destruction `[08]`. **Helix:** a deliberate boundary (DC15) — lower and tile on the graph, then **allocate on SSA before destruction**, resolving region-port transfers as parallel moves at the very end. We own these NP-hard problems honestly (R3).

11. **Effects can't be floated or rewritten freely.** PEG and Cranelift both walled effects off from the rewrite engine `[05]`. **Helix:** the pure/effect split is first-class, not retrofitted — rewrites apply to the floating value strand; state-strand nodes are pinned. The two-strand model makes the wall structural rather than a special case.

12. **Secondary-IR drift.** Every prior graph system emits to a secondary IR for codegen (RVSDG→LLVM, Thorin→LLVM, MimIR→textual LLVM) `[02][03][04]`. **Helix:** target ISA opcodes are ordinary `Op`s in the same graph (`x64.lea`), lowered in place (DC13). Cranelift's VCode/MachBuffer is the proof it can be done `[08]`.

---

## 3. Differentiators D1–D7 — each with its honest risk

These are the mechanisms by which Helix could plausibly match the best graph IRs in far less code. Each carries its risk register entry inline, because intellectual honesty makes the plan stronger, not weaker.

### D1 — No round-trip at *either* end
Parse straight into the graph (DC12, proven by Impala/Mim) *and* emit straight to bytes Cranelift-style (DC13, proven by VCode/MachBuffer). Both bridges are deleted — the single biggest codebase-size win available. No prior system did *both* in one IR. **Feasibility: high.**
**Risk — R3:** owning emit means owning instruction selection, scheduling, and **register allocation** — combined schedule+RA is NP-hard, optimal ISel/extraction NP-complete, cost is non-local, and SSA chordality evaporates after destruction `[08]`. regalloc2 is >10k LoC of dense Rust for a reason. Output quality will lag LLVM until mature.

### D2 — Hybrid float as the *default*
The state strand + floating pure sub-graph is the native model, not a retrofit, so the effectful common case is already CFG-cheap/inspectable and we never pay V8's collapse tax `[01][05]`. **Feasibility: high** (Cranelift demonstrates it).
**Risk — R6:** the very lesson motivating D2 caps its upside — when most ops are effectful, little floats, so the "optimize better" claim is weakest exactly where real programs spend their effects `[01][06]`.

### D3 — Comptime = the optimizer's reduction rule (NbE + filters)
One reduction engine is simultaneously constant-folder, inliner, loop-unroller, and comptime evaluator — one well-tested path instead of N. NbE's eval→reify handles the static/dynamic split via neutral terms; filters give programmer-visible staging (DC9/DC10). **Feasibility: medium-high.** The win is shipping it *without* MimIR's dependent-type machinery (keep PE, drop the PTS — see §4).
**Risk — R2:** comptime termination/divergence is fundamentally unsolved. The fuel budget (DC11) bounds it but leaks into UX exactly as Zig's quota / C++'s step limits do `[07]`. We cannot promise both maximal specialization and guaranteed termination.

### D4 — Fine-grained typed state from day one
Type memory by alias class / region so non-aliasing effects are *provably* independent in the representation; let an alias pass write its results back as extra state tokens (DC4). This is the lever for optimizing *better*, not merely smaller `[02][06]`. **Feasibility: medium.**
**Risk — R4:** the payoff is gated on an alias analysis precise enough to populate many states without blowing up. RVSDG/jlm *had* the representation and still fell back to one state `[02]`; without the analysis we inherit the very trap we set out to beat.

### D5 — Minimal taxonomy + one rewrite DSL
Combine MimIR's "fewer syntactic categories" with Cranelift's one-DSL-for-opt-and-ISel (DC14/DC17): peepholes, comptime folds, and instruction selection are all rules in one overlap-checked, SMT-verifiable engine. Fewer hand-written passes ⇒ fewer of Alive's 2.4% bugs `[06]`. **Feasibility: medium-high** (ISLE proves opt+ISel).
**Risk — R7:** nobody has shown the *same* rule engine also serving general partial evaluation with filters and fuel. Folding pure constants is easy; staging higher-order/recursive comptime in the same engine is unproven.

### D6 — Acyclic + construction-time normalization
Eager smart constructors (DC7/DC8) give GVN/fold for free with no saturation fixpoint and no repeated mutating sweeps, avoiding both e-graph rebuild/blowup *and* V8's cache thrash `[03][04][05][08]`. **Feasibility: high** — three systems already do this; we compose proven pieces.
**Risk:** shares the closed-terms caveat of DC8 (pointer-equality ⇒ α-equality only for closed terms `[04]`) and the bounded-overlay discipline that keeps Tier 2 from drifting toward EqSat — see R1/D7.

### D7 — Better optimization at lower cost by *not* doing full EqSat
Cranelift's honest ~0.1% aegraph payoff says the real wins are ordinary GVN/LICM/DCE the structure enables `[05][08]`. Helix targets those structural wins (which RVSDG gets as single passes `[02]`) instead of chasing saturation — strictly smaller and, per the data, no worse. **Feasibility: high**, but this is a *restraint* claim, not a magic bullet.
**Risk — R1:** "optimizes better with a smaller codebase" is partly in tension. *Beating* LLVM/GCC -O3 output quality is unproven by *any* prior graph IR; jlm is "competitive, not beating" `[02]` and Cranelift's mid-end bought ~0.1% `[05][08]`. The defensible claim is "matches the best graph IRs / matches -O2–O3 with far less code," not "beats -O3." Claiming strict superiority is the riskiest part of the whole pitch.

```
   D1 ── deletes both IR bridges ───────────────► smallest codebase (risk R3: own the backend)
   D2 ── hybrid float as default ───────────────► no collapse tax (risk R6: little floats if effect-heavy)
   D3 ── comptime = reduction ──────────────────► one engine, N jobs (risk R2: termination wall)
   D4 ── fine-grained state ────────────────────► provable independence (risk R4: needs alias analysis)
   D5 ── minimal taxonomy + one DSL ────────────► fewer pass bugs (risk R7: comptime-in-DSL unproven)
   D6 ── acyclic + eager normalize ─────────────► free GVN, no thrash (caveat: closed-terms only)
   D7 ── NOT full EqSat ────────────────────────► structural wins cheaply (risk R1: "better" ≠ "beats -O3")
```

---

## 4. What we deliberately did NOT do

Restraint is a design feature. Each omission removes a known liability the prior art demonstrated.

- **No full equality saturation (D7).** EqSat doesn't reliably terminate and optimal DAG extraction is NP-complete; Cranelift's acyclic e-graph bought only ~0.1% runtime for measurable compile cost, with average e-classes of just 1.13 nodes `[05][08]`. Helix keeps Tier 2 as a *bounded, acyclic, append-only* overlay with greedy extraction — used sparingly for ISel and a little reassociation, never as a saturation loop.

- **No dependent types / PTS (unlike MimIR).** MimIR unifies terms = types = kinds over a Calculus-of-Constructions PTS — powerful, but its type checker can *diverge* under a bad filter, and the barrier to entry is steep `[04][07]`. Helix keeps types as ordinary `Const` values (DC1) but **shallow, not full dependent types**. We get "types are values" for comptime without the undecidable-checking risk on the critical path. (Dependent-type *hosting* for DSLs is noted as a stretch, deliberately off the critical path.)

- **No CPS-as-core (unlike Thorin).** Thorin makes all control continuations (CPS sea-of-nodes), which is elegant but imposes a real cognitive/debugging cost `[03]`. Helix uses *direct-style* structured regions — symmetric `Cond` and tail-controlled `Loop` (DC6) — which are easier to read, diff, and source-map (DC16), and which still give a unique legal schedule.

- **No CFG round-trip (unlike RVSDG).** RVSDG must *construct* a dataflow graph from a CFG and *destruct* it back to a legal CFG for codegen — the bridge is ~6× the IR core in LoC `[02]`. Helix parses directly in and emits directly out (D1); there is no CFG to recover because the state strand already *is* the legal order (DC6).

- **No secondary backend IR (unlike all four).** RVSDG, Thorin, and MimIR each emit LLVM IR; even LLVM is retreating from a separate SelectionDAG IR toward GlobalISel/MIR `[02][03][04][08]`. Helix lowers portable `Op`s into target `Op`s *in the same graph* and streams bytes out (DC13) — no separate backend graph exists.

- **No phi nodes anywhere.** Region ports (block parameters) carry merged and loop-carried values (DC5), so there is no "phi must be first in block" rule, no SSA-restoration pass, and the allocator consumes ports directly `[06][08]`.

- **No back-edges / graph cycles.** Loops are `Loop` regions and recursion is a `Module` recursion group (DC7); cycles — the thing that defeats optimal extraction `[05]` — never appear.

---

## 5. The honest thesis

Helix is a single acyclic, hash-consed, strict-SSA graph built on two strands — pure values that float and a linearly-threaded state skeleton that pins effects — with exactly six node forms, block parameters instead of phi, and a two-tier reduction engine whose eager Tier-1 normalization *is* the comptime evaluator and whose bounded Tier-2 overlay *is* instruction selection, all expressed in one SMT-verifiable rule DSL, parsed directly from source and emitted directly to machine code with no secondary IR at either end. We do **not** claim to beat LLVM/GCC -O3 output quality — no prior graph IR has been shown to, and owning the entire backend (R3) plus chasing strict optimization superiority (R1) are the load-bearing risks. The defensible, honest pitch is that Helix is **conceptually simpler and dramatically smaller** than Sea-of-Nodes / RVSDG / Thorin / MimIR, **matches the best graph IRs on optimization through structural GVN/LICM/DCE wins** rather than expensive equality saturation, and **uniquely unifies compile-time evaluation and machine-code emission into the graph itself** — with the termination wall (R2), the alias-analysis dependency (R4), the debuggability liability (R5), the effect-heavy float cap (R6), and the unproven comptime-in-one-DSL step (R7) all stated plainly rather than papered over.

---

## See also

- [Overview](00-overview.md) — the elevator pitch and page map
- [Core Model](11-core-model.md) — the two strands, six node forms, and hard invariants in full
- [Textual Format](12-format.md) — the canonical printable/diffable syntax (DC16)
- [Types and Effects](13-types-and-effects.md) — types-as-values and fine-grained linear state (DC4)
- [Reduction Engine](14-reduction-engine.md) — the two tiers and the rule DSL (DC8/DC14)
- [Comptime](15-comptime.md) — NbE, filters, and the fuel budget (DC9–DC11)
- [Optimizations](16-optimizations.md) — the structural GVN/LICM/DCE wins (D7)
- [Codegen](17-codegen.md) — direct lowering, scheduling, regalloc, encode (DC13/DC15)
- [Frontend](18-frontend.md) — direct-to-graph parsing (DC12)
- [Risks and Open Problems](22-risks-and-open-problems.md) — R1–R7 in depth
- [Synthesis](research/00-synthesis.md) — prior-art matrix, failure modes, and the DC/D/R registers
