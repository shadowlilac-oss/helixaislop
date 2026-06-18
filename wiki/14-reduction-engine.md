# Reduction Engine

*The heart of Helix: a two-tier rewrite engine in which optimization, constant folding, comptime evaluation, and instruction selection are all the same process — eager oriented normalization at construction time (Tier 1), with a bounded acyclic equivalence overlay used sparingly for the few choices that have no single canonical best form (Tier 2).*

---

## 0. Why this page exists

Every other Helix page describes *what* the graph looks like; this page describes the single mechanism that *changes* it. There is exactly one engine. The same `=>` rules that fold `add 2 3 => 5` also run comptime functions (see [Comptime](15-comptime.md)), drive GVN/LICM/DCE (see [Optimizations](16-optimizations.md)), and — as `lower` rules — perform instruction selection (see [Codegen](17-codegen.md)). Reusing one declarative, verifiable rule DSL across all three jobs is **DC14**, and keeping the node taxonomy small enough that the rule set stays tractable is **DC17**.

The design is a direct response to the prior-art failure modes catalogued in [the synthesis](research/00-synthesis.md):

- **E-graph blowup and NP-complete extraction** (failure mode 5) — we do *not* do full equality saturation (**D7**).
- **Cache thrash from in-place mutating worklist sweeps** (failure mode 7) — we normalize *once*, at construction, append-only (**DC8**, **D6**).
- **Effects can't be floated or rewritten freely** (failure mode 11) — rewrites touch only the pure value strand; the state strand is never reordered by the engine (**DC3**).

The two tiers map cleanly onto the two strand kinds (see [Core Model](11-core-model.md)): Tier 1 normalizes the **pure value strand** that floats freely; neither tier ever rewrites the **state strand**, which is pinned and linear.

---

## 1. Two tiers at a glance

| | **Tier 1 — Eager Oriented Normalization** | **Tier 2 — Bounded Equivalence Overlay** |
|---|---|---|
| Rule operator | `=>` (oriented; one canonical direction) | `~` (equivalence) and `lower ... @cost` |
| When it runs | At **construction time**, inside the `World` factory, every time a node is built | On demand, during the lowering/ISel phase (and a little reassociation) |
| Data structure | The hash-consed DAG itself (mutates nothing; interns the canonical result) | An **acyclic, append-only** overlay of *union nodes* layered over the DAG |
| Fixpoint? | Local fixpoint per construction; **no global worklist sweep** | **No saturation**; greedy bottom-up cost extraction picks one member |
| Cost | Amortized O(1) per node (hash-cons lookup + bounded local rule fires) | Linear scan + DP extraction; exploits tiny e-classes |
| Gives you | Constant folding, algebraic identities, CSE/GVN, comptime β/δ — *for free* | A min-cost tiling among ISel alternatives; a handful of reassociations |
| Prior art | Thorin/MimIR `World`, Cranelift "smart constructors" (Cascades-style) | Cranelift aegraph + ISLE + greedy DP extraction |

The split is deliberate and load-bearing: **almost everything is Tier 1.** Tier 2 is reserved for the genuinely non-deterministic choices — primarily "which machine instruction tiles this subtree" — where there is no single best form to canonicalize toward. Cranelift's measured data is the justification: its acyclic e-graph bought only **~0.1% runtime for ~0.005% compile cost**, and its average e-class held only **1.13 nodes** (failure mode 5; `[05][08]`). Most values have no alternative worth recording, so we keep them out of the overlay entirely.

```
                            node construction request
                                      |
                                      v
                  +-----------------------------------------+
   Tier 1 (eager) |   World factory: mk(op, operands...)    |
                  |   1. canonicalize operand order         |
                  |   2. apply => rules to local fixpoint   |
                  |      (fold, identities, beta/delta)     |
                  |   3. hash-cons  ==>  GVN/CSE for free   |
                  +-----------------------------------------+
                                      |
                       interned canonical value node
                                      |
            ... (parsing, opt, comptime all just call mk) ...
                                      |
                                      v   only at codegen
                  +-----------------------------------------+
   Tier 2 (lazy)  |   lowering: record ~ / lower alts as    |
                  |   union nodes in acyclic overlay        |
                  |   greedy bottom-up DP cost extraction    |
                  +-----------------------------------------+
                                      |
                                      v
                          chosen min-cost target tiling
```

---

## 2. Tier 1 — eager oriented normalization ("smart constructors")

### 2.1 The World factory is the only way to make a node

There is no public node constructor. Every structural node (Const, Op) is created by calling the **World** factory — `mk(opcode, operands…, state?)`. This is **DC8**: hash-consing plus construction-time normalization, exactly as Thorin's and MimIR's `World` fold/CSE/normalize on the fly, "vastly decreas[ing] iterations in the fixed-point loops" `[03][04]`. The factory does four things, in order, on every call:

1. **Canonicalize operand order.** Commutative opcodes sort their operands by node id, so `add %x, %y` and `add %y, %x` produce the same key. (Genuinely ambiguous reassociation is *not* done here — it goes to Tier 2; see §4.)
2. **Apply `=>` rules to a local fixpoint.** Match the prospective node against the oriented rule set; if a rule fires, it rewrites to a (usually smaller) term, which itself re-enters `mk`. This recursion bottoms out quickly (§2.4).
3. **Hash-cons the result.** Look the canonical node up in the intern table. If it exists, return the existing pointer; otherwise insert. **Structural equality == pointer equality == automatic GVN/CSE** (**DC8**). No separate value-numbering pass exists in Helix because there is nothing left to number.
4. **Return the interned node.** The caller never sees a non-normal node.

Because *parsing itself* calls `mk` (frontend builds graph nodes directly, **DC12**; see [Frontend](18-frontend.md)), folding and GVN happen *during parsing*. By the time a function is parsed, it is already in normal form.

```
; source:  let a = x + 0;  let b = x + 0;  return a * 1
; what mk() returns, step by step — no pass ever runs:
%x ...                         ; (param)
mk(add, %x, const(i32,0))   => %x        ; rule add-zero fires, returns %x
mk(add, %x, const(i32,0))   => %x        ; identical -> hash-cons -> SAME pointer (GVN)
mk(mul, %x, const(i32,1))   => %x        ; rule mul-one fires
return %x                                ; a, b, and a*1 all collapsed to %x at build time
```

This is the mechanism behind **D6**: eager smart constructors give GVN/fold for free with *no saturation fixpoint and no repeated mutating sweeps*. Contrast the alternatives in §3.

### 2.2 The rule DSL (canonical syntax)

Tier 1 rules use the oriented `=>` operator. The full DSL — shared verbatim with peepholes, comptime folds, and (via `lower`) instruction selection — is **DC14**:

```
rule add-zero : (add ?x (const _ 0)) => ?x
rule fold-add : (add (const ?t ?a) (const ?t ?b)) => (const ?t {a + b})   ; {..} = comptime host computation
rule mul-pow2 : (mul ?x (const i32 2)) => (shl ?x (const i32 1))
rule mul-one  : (mul ?x (const _ 1)) => ?x
```

Legend (identical across all Helix pages):

| Token | Meaning | Tier |
|---|---|---|
| `=>` | **oriented** rewrite — LHS reduces to RHS, one canonical direction | Tier 1, eager |
| `~`  | **equivalence** — both sides recorded as alternatives, no preferred direction | Tier 2, overlay only, *sparingly* |
| `lower … @cost n` | lowering/ISel rule producing a target Op with machine cost `n` | Tier 2 |
| `{ .. }` | **host fold** — a comptime computation run on the host (this is where Tier 1 *is* the comptime evaluator) | Tier 1 |
| `?x` | pattern variable (binds a subterm) | both |
| `?t` | pattern variable used as a *type* (types are ordinary Const values) | both |
| `if guard` | side condition gating the rule | both |

### 2.3 Pattern matching against the interned graph

Because the graph is hash-consed and acyclic (**DC7**, **DC8**), matching is cheap and structural:

- **Top-down, shallow.** A rule LHS like `(add (const ?t ?a) (const ?t ?b))` is matched against the *node being constructed* and its immediate operands. Operands are already-interned, already-normal nodes, so a match is a sequence of opcode tag checks and pointer reads — no recursive normalization needed (the operands were normalized when *they* were built).
- **Two-occurrence variables = pointer equality.** A rule that mentions `?x` twice (e.g. `(sub ?x ?x) => (const ?t 0)`) checks that both positions are the *same interned pointer*. This is sound only for **closed terms** — the documented caveat that pointer-equality ⇒ α-equality only for closed terms `[04]`. Helix tracks a `closed` bit per node; the pointer-equality fast path is taken only when set, otherwise structural comparison is used.
- **Guards** (`if member(?s, {1,2,4,8})`, `if a != 0`) are host predicates evaluated after the structural match binds the pattern variables.
- **Host folds** `{a + b}` run the host computation over the *already-known* constant values bound to `?a`, `?b`. This is the seam where Tier 1 reduction and comptime are literally the same code (§6).

Rules are compiled to a **decision tree / matching automaton** keyed on the root opcode (ISLE-style), so the per-construction cost is proportional to the number of rules that *could* apply to that opcode, not the whole rule set.

### 2.4 Confluence, local termination, and how we handle the gaps

Helix does **not** claim global confluence of the full rule set — that would be undecidable to guarantee and false in general (associativity/distributivity famously break it). Instead the discipline is:

- **Orientation toward a cost-monotone normal form.** Every `=>` rule must reduce a *termination measure* (a lexicographic combination of node count, then a fixed opcode rank). `add-zero`, `mul-one`, `fold-add` all strictly shrink the term. `mul-pow2` (`mul → shl`) does not change node count, so opcode rank breaks the tie (`shl` ranks below `mul`). This makes **local rewriting terminating**: the per-`mk` fixpoint in step 2 cannot loop, because each fire strictly decreases the measure and the measure is well-founded.
- **Critical-pair / overlap checking offline.** The rule set is checked at build time for overlapping LHSs that rewrite the same redex to *different* normal forms. Overlaps are not forbidden outright but must be **priority-ordered** (ISLE's rule: *no two overlapping rules may share a priority* `[05][08]`). The priority check is a static lint over the compiled decision tree; an unranked overlap is a compile error in the rule set, not a silent nondeterminism.
- **Non-confluent / no-canonical-winner cases are pushed to Tier 2.** When two forms are genuinely incomparable — `(add ?x ?y) ~ (add ?y ?x)` for reassociation, or two equally-good instruction tilings — there is *no* oriented rule. We record them as `~` equivalences in the overlay and let cost extraction decide (§4). This is the principled escape hatch: Tier 1 stays confluent-by-orientation precisely *because* the hard cases are quarantined into Tier 2.

> **Honest limitation.** "Locally terminating and offline-overlap-checked" is weaker than "globally confluent." Two different *construction orders* of the same source could in principle intern different (but equivalent) normal forms if a rule author violates the measure discipline. The mitigation is the offline checker plus the SMT verification below; the residual risk is a flavor of **R7** — nobody has shown a single rule engine serving folding, ISel, *and* general comptime staging without seams.

### 2.5 SMT-verifiable rules (VeriISLE-style)

Each `=>` and `lower` rule is independently lowered to an SMT query asserting **LHS ≡ RHS** over the operand bit-vectors / value semantics, in the style of VeriISLE — "the first formal verification effort for the instruction-lowering phase of an efficiency-focused production compiler" `[05][08]`. This matters because hand-written peephole sets are buggy in practice: Alive found **2.4%** (8/334) of LLVM InstCombine transforms unsound `[06]`. Verifying rules individually (rather than the whole optimizer) is tractable and is one of the concrete ways Helix turns "fewer hand-written passes" into "fewer bugs" (**D5**). Host folds `{..}` are verified against the host's own semantics of the operator; guarded rules carry the guard into the SMT precondition.

---

## 3. Why not full equality saturation, egg rebuild, or sea-of-nodes `idealize()`

Helix deliberately avoids three well-documented mechanisms. This is **D7** — a *restraint* claim, not a magic-bullet claim.

### 3.1 vs. full equality saturation (egg / egglog / Peggy)

Equality saturation builds an e-graph holding *every* program reachable under the rules, then extracts the cheapest. The documented problems (failure mode 5, `[05]`):

- **Blowup / non-termination.** Peggy fully saturated only **84%** of methods; distributivity/associativity explode the graph. Saturation over loops need not terminate at all.
- **Extraction is NP-complete.** DAG-aware extraction (counting shared subexpressions once — the *right* model for a compiler) is NP-hard; Peggy's Pueblo ILP averaged **1,499 ms/method**, 1% hit a 1-minute timeout.
- **The payoff is small in a real compiler.** Cranelift declined general EqSat because it needs a "fuel"-bounded strategy driver "not [suitable] for a fast compiler," and its acyclic variant bought **~0.1%** `[05][08]`.

Helix's answer: get the *structural* wins (GVN/LICM/DCE) that RVSDG gets as single passes `[02]` directly from the form (acyclicity + hash-consing + the strand split), and reserve a *bounded, acyclic, greedy* overlay only for ISel. We never assert the e-graph holds all programs; the overlay holds a handful of alternatives for a handful of nodes.

### 3.2 vs. egg's read/write/`rebuild` cycle

egg's signature move is splitting each iteration into a read phase (collect all matches) and a write phase (union/merge), restoring congruence once per iteration via `rebuild()` with a deduplicated worklist — an asymptotic **21–88×** win *for saturation* `[05]`. We borrow the *idea* of deferring invariant maintenance (it informs the append-only overlay in §4) but **we never enter the saturation loop at all**, so there is no congruence to rebuild on the hot path: hash-consing maintains congruence *eagerly and incrementally* at every `mk`. egg's rebuild is the right tool when you *do* saturate; Helix's bet is that you usually shouldn't.

### 3.3 vs. sea-of-nodes `idealize()` worklist

C2/TurboFan-style sea-of-nodes runs per-node `idealize()`/peephole to a *global* fixpoint over a mutable graph with an IGVN worklist. The documented cost (failure mode 7, `[01][06]`): nodes "visited 3 times but only lowered once," "changed only once every 20 visits," and **~3× (up to 7×) more L1 dcache misses** from the in-place mutation churn. Helix's construction-time normalization touches each node's neighborhood *once*, when it is built, and the result is immutable and interned — there is no revisiting and no mutation-driven cache thrash (**DC8**, **D6**). The cost model is "normalize once, append-only" rather than "mutate to fixpoint."

| Mechanism | Fixpoint shape | Mutation | Failure it incurs (avoided by Helix) |
|---|---|---|---|
| EqSat (egg/Peggy) | global saturation | append (union-find) | blowup, NP-hard extraction `[05]` |
| sea-of-nodes `idealize()` | global worklist | **in-place** | cache thrash, oscillation `[01]` |
| **Helix Tier 1** | **local, per-construction** | **none (interned)** | — |

---

## 4. Tier 2 — the bounded equivalence overlay

Some choices have no canonical winner. The clearest is **instruction selection**: `(add ?b (mul ?i (const i64 ?s)))` could become an `x64.lea` (one instruction, if `?s ∈ {1,2,4,8}`) or a separate `mul` + `add`; which is cheaper depends on surrounding pressure and the target. There is no oriented direction, so these go into Tier 2.

### 4.1 What gets recorded, and how

When the lowering phase encounters a node with alternatives, it records them as **union nodes** in an overlay that is:

- **Acyclic** (**DC7**) — alternatives only ever point *down* into existing value-strand subterms; never introduces a cycle. Acyclicity is what keeps extraction from degenerating (cycles are exactly what defeat optimal extraction `[05]`).
- **Append-only** — recording an alternative never mutates or deletes the original node, so the read/write-phase soundness property egg formalizes holds trivially: matching always sees a consistent snapshot.
- **Sparse** — only nodes with a real alternative get a union node. Following Cranelift's measured **1.13** average e-class size `[05]`, the vast majority of nodes have exactly one representative and never enter the overlay.

```
; Tier-2 alternatives recorded for one address computation:
;   union node u over the value-strand subtree  %a = base + i*8
%u = union {
       (add %base (mul %i (const i64 8)))      @cost 2    ; portable Ops
       (x64.lea %base %i 8)                     @cost 1    ; from `lower lea`
     }
```

The `~` operator records a symmetric equivalence (used *sparingly* — a little algebraic reassociation only); the `lower … @cost` operator records a directed lowering alternative with a machine cost. Both land in the same overlay.

```
lower lea : (add ?b (mul ?i (const i64 ?s))) => (x64.lea ?b ?i ?s) @cost 1  if member(?s, {1,2,4,8})
comm-add  : (add ?x ?y) ~ (add ?y ?x)                                       ; Tier 2 only, used sparingly
```

### 4.2 Greedy bottom-up cost extraction (BURS-style DP)

Selection is a **greedy bottom-up dynamic program**, not an ILP. For each union node, compute the min-cost member assuming children are already costed (a *local* cost model); memoize; propagate up. This is the BURS / tree-matching tiling that walks *up the pure value strand*, stopping at region-port and state-strand boundaries (**DC15**; see [Codegen](17-codegen.md) for the full lowering pipeline).

- **Why greedy is acceptable:** optimal DAG extraction is NP-complete `[05][08]`; greedy with a tree cost model can double-count shared subterms, but Cranelift's data (1.13 avg e-class) shows the practical gap is tiny because there is almost nothing to choose. We accept suboptimality for speed and predictability (**D7**).
- **Termination is trivial:** the overlay is a DAG and we never add equivalences during extraction — it is one bottom-up pass.

> **Honest limitation (R3).** Owning ISel via greedy extraction means owning an NP-hard problem with a deliberately weak (local, tree-cost) approximation. Output quality will lag a mature LLVM backend until the cost model and tile library are tuned; this is the load-bearing backend risk. We are not claiming to beat LLVM `-O3` *output quality* — only to match the best *graph* IRs while emitting directly, with no secondary IR.

---

## 5. Worked traces

### 5.1 Tier-1 normalization during construction

Source fragment `((x * 2) + 0) >> 1`, with `x : i32` a runtime-unknown (dynamic) value:

```
; mk() calls, in evaluation order; each line shows input => interned result
mk(mul, %x, const(i32,2))                ; rule mul-pow2
   => (shl %x (const i32 1))             ; canonical: shl ranks below mul
mk(add, (shl %x (const i32 1)), const(i32,0))
   => (shl %x (const i32 1))             ; rule add-zero: drops the +0
mk(shr, (shl %x (const i32 1)), const(i32,1))
   => (shl %x (const i32 1)) ??          ; NO oriented rule: (shr (shl x 1) 1) != x in general (sign/overflow)
   => (shr (shl %x (const i32 1)) (const i32 1))   ; left as-is; SMT would reject the unsound fold
```

Result: the program is in normal form the instant it is built; the only thing that did *not* fold is the one rewrite that is actually unsound (caught by the no-rule / SMT discipline of §2.5). No worklist, no sweep, no revisit.

A fully-static fragment collapses to a single Const — this is comptime (§6):

```
mk(add, const(i32,40), const(i32,2))
   => (const i32 {40 + 2})              ; host fold {..}
   => (const i32 42)                    ; interned; any other `42 : i32` is the SAME pointer
```

### 5.2 Tier-2 ISel choice resolved by extraction

The address `base + i*8` inside an effectful load (`base : ptr`, `i : i64`, both dynamic):

```
; 1. lowering records alternatives as a union node (append-only overlay)
%u = union {
       (add %base (mul %i (const i64 8)))   @cost 2
       (x64.lea %base %i 8)                  @cost 1     ; from `lower lea`, guard 8 in {1,2,4,8} holds
     }

; 2. greedy bottom-up extraction:
;      cost(%base)=0, cost(%i)=0  (params/leaves)
;      member A = add(...,mul(...)) -> 2
;      member B = x64.lea          -> 1     <= min
;    pick B

; 3. chosen tiling feeds the schedule (order read off the state strand):
(%v, %s1) = load (x64.lea %base %i 8), %s0   ; effectful Op still threads state linearly
```

The state strand (`%s0 -> %s1`) is untouched by either tier; only the pure address subtree was a candidate for rewriting. This is **DC3**/failure mode 11 in action: pure floats and is rewritable; effects are pinned and ordered.

---

## 6. Relationship to comptime (same engine)

Tier 1 **is** the comptime evaluator. There is no separate interpreter (**DC9**). The mechanism is Normalization-by-Evaluation run on the graph (**DC9/DC10/DC11**; full treatment in [Comptime](15-comptime.md)):

- A **host fold** `{a + b}` *is* an evaluation step: when the operands are static Consts, the `{..}` body computes on the host and `mk` reifies the result back as a Const node. Static `if` over a Cond region collapses to the taken branch region; a static-bounded Loop unrolls; a call with static arguments β/δ-reduces — all as `=>` rewrites at construction.
- **Dynamic values are neutral terms.** A value whose operands are not all static does not fold; reification turns it back into an Op node (exactly the `(shr (shl %x 1) 1)` residual in §5.1). This is the clean static/dynamic split NbE provides.
- **Staging is filter-driven, not heuristic** (**DC10**): `fn f(static %n: i32, %x: i32)` tells the engine to specialize on `%n`. Default policy: specialize type-level and higher-order arguments aggressively, defer the final run-time continuation.
- **Bounded and memoized** (**DC11**): the per-`mk` local fixpoint is naturally bounded by the termination measure (§2.4); whole-call comptime carries an introspectable per-call **fuel** budget, and results memoize by `(function, static-args)` — sound only under purity, which the state strand enforces.

> **Honest limitation (R2).** Comptime is Turing-complete; no specializer both always terminates *and* always maximally specializes. Fuel bounds it but leaks into UX (Zig's quota, C++'s step limit). Helix's fuel is *per-call and reported*, which is a mitigation, not a cure. And **R7**: that the *same* rule engine cleanly serves folding, ISel, and general higher-order/recursive comptime staging is the part no prior system has fully demonstrated — folding constants is easy; staging is where the seams could appear.

---

## 7. Invariants this engine preserves

The reduction engine is correct only if it never violates the structural invariants from [Core Model](11-core-model.md):

| Invariant | How the engine respects it |
|---|---|
| **Acyclic (DC7)** | `=>` rewrites produce strictly-smaller (measure-decreasing) terms; the Tier-2 overlay only points down. No rule introduces a back-edge — loops/recursion are Loop/Module regions, never graph cycles. |
| **Strict SSA (DC2)** | Every result of `mk` is a fresh single-origin value (or an existing interned one). Rewrites replace *uses* by repointing edges to the canonical def; SSA is never broken, so no restoration pass exists. |
| **Hash-consed / GVN (DC8)** | Step 3 of `mk` is the only place nodes enter the intern table; CSE is therefore an invariant, not a pass. Pointer-equality fast path gated on the `closed` bit. |
| **Linear state (DC4)** | Neither tier ever rewrites, duplicates, or reorders a state edge. Effectful Ops thread state exactly once; the engine treats the state operand as opaque and pinned (failure mode 11). |
| **Block params, no phi (DC5)** | Cond/Loop result *ports* are the merge mechanism; the engine rewrites the pure values that flow into ports, never synthesizing a phi (there are none in Helix). |

---

## See also

- [Core Model](11-core-model.md) — the six node forms, the two strands, ports, and the structural invariants this engine preserves.
- [Comptime](15-comptime.md) — comptime as Tier-1 reduction via NbE; filters, fuel, memoization.
- [Optimizations](16-optimizations.md) — how GVN/LICM/DCE emerge structurally from Tier-1 + the form.
- [Codegen](17-codegen.md) — Tier-2 lowering, greedy tiling up the value strand, schedule, regalloc, encode.
- [Types and Effects](13-types-and-effects.md) — types-as-values and the state strand the engine must not touch.
- [Format](12-format.md) — the canonical textual syntax used in every trace above.
- [Design Rationale](10-design-rationale.md) and [Risks and Open Problems](22-risks-and-open-problems.md) — DC/D/R provenance, especially D6, D7, R2, R3, R7.
- [Prior-art synthesis](research/00-synthesis.md) — the comparison matrix, failure modes, and constraint IDs cited throughout.
