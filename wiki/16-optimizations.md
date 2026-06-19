# Optimizations

_Most Helix optimizations are not passes. They are either **emergent** from the graph form (hash-consing, single-origin edges, the pure/state split) or expressed as a handful of **declarative rules** in the shared DSL. This page catalogues them, shows the mechanism behind each, and is honest about what this buys (a far smaller codebase that **matches** the best graph IRs — not a claim to beat `-O3` output quality, R1)._

> **⚠️ Design vs. built** (see [24-implementation-status](24-implementation-status.md), [25-path-to-production](25-path-to-production.md)). This chapter describes the *intended* optimizer. **Built today:** Tier-1 construction-time folding (~15 algebraic identities, one strength-reduction rule), hash-cons CSE, the GCM scheduler, and function inlining (wired behind `helixc -O`, differential-fuzzed). **Not built:** the declarative / SMT-verified rule DSL and the Tier-2 equality-saturation overlay (every rule is a hand-written C++ smart-constructor case), cross-control-flow GVN/PRE, SCCP/range analysis, LICM/unroll/TRE, and all memory optimizations (they need the unbuilt multi-state model). The "matches the best graph IRs" claim has **not** been benchmarked against LLVM/GCC.

> Prerequisites: [Core Model](11-core-model.md) (the six node forms, the two strands), [Reduction Engine](14-reduction-engine.md) (Tier-1 eager normalization, Tier-2 bounded overlay), and [Comptime](15-comptime.md) (the specialize-a-region primitive). This page assumes that vocabulary.

---

## 1. The thesis of this page

A conventional optimizer is a pipeline of dozens of hand-written, mutually-ordered passes (LLVM's `-O3` is the canonical example; 6/13 of its hottest passes merely re-discover SSA and loops, ~21% of invocations — failure-mode commentary in [synthesis](research/00-synthesis.md)). Helix replaces almost all of that with three things that already exist for *other* reasons:

1. **The form itself.** Strict SSA by single-origin edges (DC2), hash-consing of structural nodes (DC8), the acyclic DAG (DC7), and the pure/state strand split (DC3/DC4) make GVN, CSE, DCE, copy elimination, and code motion *fall out* rather than be implemented.
2. **Tier-1 oriented rules (`=>`).** Constant folding, algebraic identities, peepholes, branch simplification — short declarative rewrites applied eagerly at construction by the `World` factory (DC8/DC14). The *same* engine is the comptime evaluator (DC9).
3. **One region-rewrite primitive.** Inlining, specialization, unrolling, peeling, and tail-recursion elimination are all "specialize a region" — the Thorin-mangling analogue (D-table "one composable rewrite primitive"), shared verbatim with comptime ([Comptime](15-comptime.md)).

Only a thin Tier-2 layer (`~` equivalence overlay) handles the genuinely non-canonical choices: a little reassociation and instruction selection ([Codegen](17-codegen.md)). We deliberately **do not** run equality saturation (D7): Cranelift's data shows the radical e-graph bought ~0.1% runtime for real cost, and optimal DAG extraction is NP-complete (R-notes, failure-mode 5).

```
                    ┌──────────────────────────────────────────────┐
   source ─parse──▶ │  Helix graph (one acyclic SSA DAG)            │ ──emit──▶ bytes
                    │                                               │
                    │  TIER 1 (eager, at construction)             │
                    │   • hash-cons  ⇒ GVN/CSE for free            │
                    │   • => rules   ⇒ fold, identities, peephole  │
                    │   • NbE        ⇒ comptime = same reduction   │
                    │                                               │
                    │  STRUCTURAL (no pass; property of the form)   │
                    │   • unreferenced node  ⇒ DCE                  │
                    │   • unreferenced port  ⇒ dead-region elim     │
                    │   • pure Op floats     ⇒ LICM/sink at sched   │
                    │   • single-origin edge ⇒ SSA never restored   │
                    │                                               │
                    │  REGION PRIMITIVE (shared w/ comptime)        │
                    │   • specialize(region, args) ⇒ inline /       │
                    │     unroll / peel / TRE / spec               │
                    │                                               │
                    │  TIER 2 (bounded overlay, ~ ; sparingly)      │
                    │   • reassociation alternatives                │
                    │   • ISel tiling alternatives  ⇒ greedy DP     │
                    └──────────────────────────────────────────────┘
```

---

## 2. Emergent optimizations (no pass at all)

These cost essentially zero implementation code beyond the data structure that already had to exist.

### 2.1 GVN / CSE — free via hash-consing (DC8)

Structural nodes (`Const`, `Op`) are interned in the `World`. **Structural equality == pointer equality == automatic GVN/CSE.** Constructing a node that is structurally identical to an existing one returns the *same* node; there is no separate value-numbering pass and no worklist.

```
; both adds intern to the SAME node — GVN happened at construction, no pass ran
func @cse(%x: i32, %y: i32) -> i32 {
  %a = add %x, %y        ; first construction interns a node
  %b = add %x, %y        ; structurally equal ⇒ returns the identical node as %a
  %r = mul %a, %b        ; effectively mul %a, %a
  return %r
}
```

This is the eager smart-constructor discipline taken from Thorin's `World` and MimIR (DC8). Caveat carried from the spine: the pointer-equality fast path is valid only for **closed** terms; open terms (those mentioning unbound region ports) compare structurally. CSE across a `Cond`/`Loop` boundary therefore happens only for sub-terms that do not capture that region's ports — exactly the right scope.

### 2.2 Dead Code Elimination — unreferenced nodes (DC7 + DC2)

Because the graph is a DAG of use→def edges and a value *is* its defining node, **a node with no use is dead by definition.** There is no mark-and-sweep pass in the classical sense: reachability from `Module`/`Func` result ports and from the live tail of each state strand *is* liveness. A node that no live root can reach is simply never emitted.

- **Pure dead values** drop out for free: nothing references them.
- **Dead effects** require the state strand: a `store` is live iff its produced state token is (transitively) consumed by a live root. Because state is **linear** (DC4 — used exactly once), tracing the single consumer chain is trivial and exact; no fixpoint.

### 2.3 Dead-region / dead-port elimination (DC1 + DC5)

Region ports are block parameters, never phi nodes (DC5). This makes whole categories of cleanup structural:

- **Unreferenced result port:** a `Cond`/`Loop` result port that no consumer reads is deleted, and with it the per-branch `yield`/`continue` operands that fed only it. The producers of those operands then become unreferenced and die by 2.2.
- **Unreachable branch:** if a `Cond` predicate folds to a constant (Tier-1), the dead `case` region is dropped and the live region is **spliced into the parent** (its ports become direct edges). This is RVSDG-style γ-collapse; in Helix it is just substitution.
- **Dead loop:** a `Loop` whose carried results are all unread and whose body is pure (no live state strand through it) is deleted entirely.

One mechanism — "is this port/node reachable from a live root?" — subsumes LLVM's separate dead-code, dead-arg, and unreachable-block elimination (the RVSDG "one DNE = dead code + dead func + unreachable" collapse, DC1).

### 2.4 Copy / identity elimination — there are no copies (DC2)

In SSA-with-block-parameters there is no move instruction in the IR; a "copy" would be an identity `Op`, and identity `Op`s are removed by a one-line Tier-1 rule. More importantly, value *renaming* never exists: a use points directly at its def. The only real copies in the whole system are the **parallel moves** that resolve region-port transfers at the very end of register allocation ([Codegen](17-codegen.md), DC15) — and those are generated by the allocator, not present in the IR to be eliminated.

```
rule id-add  : (add ?x (const _ 0)) => ?x          ; additive identity
rule id-mul  : (mul ?x (const _ 1)) => ?x          ; multiplicative identity
rule id-or   : (or  ?x (const _ 0)) => ?x
rule id-copy : (copy ?x)            => ?x          ; an explicit identity Op never survives construction
```

---

## 3. Tier-1 rule-expressed optimizations (short declarative rewrites)

All of these are oriented `=>` rules in the one DSL (DC14), applied to fixpoint **locally at construction time** by the `World` (Tier-1, [Reduction Engine](14-reduction-engine.md)). No worklist sweep, no separate pass. They are SMT-verifiable (VeriISLE-style) so we avoid the ~2.4% buggy-transform rate Alive found in hand-written InstCombine (DC14).

### 3.1 Constant folding (DC8/DC9)

Folding is just a rule whose right-hand side runs a comptime host computation `{..}`. Because Tier-1 *is* the comptime evaluator (DC9), there is no separate folder.

```
rule fold-add : (add (const ?t ?a) (const ?t ?b)) => (const ?t {a + b})
rule fold-mul : (mul (const ?t ?a) (const ?t ?b)) => (const ?t {a * b})
rule fold-cmp : (cmp.lt (const ?t ?a) (const ?t ?b)) => (const i1 {a < b})
```

Conditional constant propagation falls out of the interaction with §2.3: a folded predicate collapses the `Cond`, exposing more constants up the chain, which fold further — all at construction, no optimistic fixpoint needed.

### 3.2 Algebraic identities & strength reduction

```
rule add-zero : (add ?x (const _ 0)) => ?x
rule sub-self : (sub ?x ?x)          => (const i32 0)
rule mul-pow2 : (mul ?x (const i32 2)) => (shl ?x (const i32 1))      ; strength reduction
rule div-pow2 : (udiv ?x (const i32 ?k)) => (shr ?x (const i32 {log2 k}))  if is_pow2(?k)
rule and-self : (and ?x ?x)          => ?x
```

Note `mul-pow2`/`div-pow2`: classic strength reduction is a **portable** Tier-1 rule, distinct from *machine* lowering rules (`lower ...`, §6). The split keeps target-independence: strength reduction that is always profitable is `=>`; choices that depend on machine cost are deferred to Tier-2 ISel.

### 3.3 Branch & switch simplification

These are γ-level (`Cond`) rewrites; because `Cond` is symmetric (DC6) they are uniform:

```
rule cond-const  : (cond (const i1 1) ?then ?else) => ?then     ; dead branch removed, region spliced
rule cond-same   : (cond ?p ?r ?r)                 => ?r        ; both arms identical ⇒ predicate is dead
rule cmp-not-not : (xor (xor ?x (const i1 1)) (const i1 1)) => ?x
rule switch-1    : (cond ?p -> () { case k: { yield ?v } })     => ?v   ; single live case collapses
```

`cond-same` is the structural form of "if both arms compute the same value, the branch is irrelevant" — it deletes the predicate's only consumer, and the predicate then dies by DCE (§2.2). This subsumes a chunk of LLVM's SimplifyCFG.

### 3.4 Loop-invariant predicates feeding folds

Loop-body sub-terms that depend only on values defined *outside* the loop are not loop-carried (they reference no `Loop` port). They intern once, fold once, and never re-evaluate — this is the seed of LICM, completed structurally in §4.

---

## 4. Code motion as a consequence of the form (DC3 + GCM intuition)

Helix has **no LICM pass, no sinking pass, no global-code-motion pass.** It has a placement step at scheduling time, and the optimizations are what that placement *is*.

The mechanism (the Sea-of-Nodes Global Code Motion intuition, "stolen" in the synthesis table; DC3):

- A **pure** `Op` (no state edge) is **unpinned** — it FLOATS. It has no fixed position; it is merely a node in the DAG with value-strand operands.
- An **effectful** `Op` (consumes/produces a state token) is **pinned** into the state strand in linear order (DC4). The state strand *is* the side-effect skeleton.
- At scheduling ([Codegen](17-codegen.md) step 3), the order is read off the state strand, and each floating pure `Op` is **list-scheduled** into the latest legal / lowest-loop-depth position that dominates all its uses.

From that single placement rule, two named optimizations emerge:

| Emergent optimization | Why it happens automatically |
|---|---|
| **LICM (hoisting)** | A pure `Op` inside a `Loop` body that references **no loop port** does not depend on the iteration. Its uses all dominate from outside, so scheduling places it *before* the loop. It was never "in" the loop — it merely appeared textually inside it. |
| **Sinking** | A pure `Op` whose only use is on one arm of a `Cond` is scheduled *into* that arm (latest placement), so it is not computed on paths that discard it — partial dead-code motion for free. |

```
; %scale does not reference the loop ports (%acc, %i) ⇒ it is loop-invariant by construction.
; Scheduling will place it BEFORE the loop. No LICM pass examined this.
func @licm(%n: i32, %a: i32, %b: i32) -> i32 {
  %r = loop (%acc = 0, %i = 0) : i32 {
    %c = cmp.lt %i, %n
    break unless %c -> %acc
    %scale = mul %a, %b            ; pure, references no loop port ⇒ floats out
    %acc1  = add %acc, %scale
    %i1    = add %i, 1
    continue (%acc1, %i1)
  }
  return %r
}
```

Honest limitation (R6): the float — and therefore these wins — is largest for pure numeric code and smallest for effect-heavy code, because effectful ops are pinned. When most ops thread state, little floats and LICM/sinking have little to move. This is the same ceiling V8 hit; we adopt the hybrid (DC3) so the effectful case is at least CFG-cheap and inspectable, but we do not pretend the reordering upside is large there.

A second-order win: because LICM is just placement, it composes with §2.1 automatically — a hoisted invariant that is structurally equal to another hoisted invariant is the *same* interned node (GVN), so it is computed once across the whole function, not once per loop.

---

## 5. Inlining, specialization, unrolling, peeling, TRE — one primitive

All of these are the **single region-rewrite primitive** `specialize(region, bindings)` — lift the free variables of a region, substitute the supplied arguments, re-intern. This is the Thorin lambda-mangling analogue (lift + drop + substitute), and it is **the same code path comptime uses** ([Comptime](15-comptime.md), DC9). Implementing one well-tested primitive instead of five bespoke passes is a core "smaller codebase" lever (DC17, D5).

| Transform | What `specialize` does |
|---|---|
| **Inlining** | Specialize a `Func`'s region with the call site's argument edges substituted for its parameter ports; splice the result region into the caller; thread the caller's state token through the callee's state in/out. |
| **Specialization / static-arg** | Specialize a `Func` with *some* parameters bound to known `Const`s (or `static`-marked args, [Comptime](15-comptime.md)), leaving the rest as ports. Memoized by `(function, static-args)` so two call sites with the same static args share one specialized body (DC11). |
| **Unrolling** | Specialize a `Loop` body N times, threading the carried ports between copies; if the trip count is a comptime `Const`, the residual exit `break`s fold away (§3.1) and the loop disappears. |
| **Peeling** | Specialize just the *first* iteration out of the `Loop`, leaving the rest as a (smaller-trip-count) loop. Special case of unrolling by one. |
| **Tail-recursion elimination** | A self-call in tail position is specialized into a `continue` of an enclosing `Loop` region — recursion (expressed in the `Module`, DC7, never as a graph cycle) becomes iteration. |

```
; @sumto specialized on static %n = 4 ⇒ trip count known ⇒ unroll ⇒ fold ⇒ a constant.
; The SAME engine that does this is the comptime evaluator (DC9). No dedicated unroller exists.
func @sumto(static %n: i32) -> i32 {
  %r = loop (%acc = 0, %i = 0) : i32 {
    %c = cmp.lt %i, %n
    break unless %c -> %acc
    %acc1 = add %acc, %i
    %i1   = add %i, 1
    continue (%acc1, %i1)
  }
  return %r
}
; after specialize(@sumto, {n = 4}) + unroll + Tier-1 fold:
;   %r = const i32 6      ; 0+1+2+3
```

Honest limitation (R2): because this primitive is shared with comptime and comptime is Turing-complete, unbounded unrolling/specialization can diverge. It is governed by the **per-call, introspectable fuel budget** (DC11) — the same budget comptime reports. Aggressive specialization is the default for type-level and higher-order arguments (DC10); the final run-time continuation is deferred. Unrolling of *runtime-unknown* trip counts is a heuristic, fuel-bounded choice, not a guarantee.

---

## 6. Tier-2: reassociation and instruction selection (bounded, sparingly)

Some choices have **no single canonical best form**, so they cannot be oriented `=>` rules. These — and only these — go into the Tier-2 acyclic, append-only equivalence overlay (`~`), and are resolved by **greedy bottom-up cost extraction** (BURS-style DP). This is *not* equality saturation (D7); we record alternatives, never saturate, and extract greedily because optimal DAG extraction is NP-complete (R3, failure-mode 5).

### 6.1 Reassociation

```
rule comm-add : (add ?x ?y) ~ (add ?y ?x)          ; equivalence, overlay only — used sparingly
rule assoc-add: (add (add ?x ?y) ?z) ~ (add ?x (add ?y ?z))
```

Reassociation is recorded as `~` alternatives so that a *later* consumer (e.g. a `lea` lowering, or a chance to expose a common sub-expression for GVN) can pick the shape that minimizes cost. We keep this deliberately small: associativity/distributivity are exactly the rules that blow up e-graphs (failure-mode 5), so the overlay is bounded and acyclic by construction.

### 6.2 Instruction selection

ISel is the main Tier-2 client. Portable `Op`s are rewritten into target `Op`s by `lower` rules, recorded as alternatives, then a min-cost tiling is chosen by tree-matching up the pure value strand (stopping at region-port / state-strand boundaries — DC15). Detail lives in [Codegen](17-codegen.md); the relevant point here is that **ISel uses the same rule DSL** as the optimizer (DC14).

```
lower lea : (add ?b (mul ?i (const i64 ?s))) => (x64.lea ?b ?i ?s) @cost 1
            if member(?s, {1,2,4,8})
```

### 6.3 Fine-state-enabled reordering & parallelization (DC4)

Because state is not a single global token but **multiple independent fine-grained tokens** (one per alias class / region, from day one — DC4, closing RVSDG/jlm's single-state regret, R4), two effects on provably-disjoint state strands carry **no ordering edge between them**. This is an optimization the representation *enables* rather than performs:

- Effects on different state tokens may be freely reordered by the scheduler (no dependence to respect).
- They may be flagged for parallelization / independent issue.
- Redundant-load elimination and store-to-load forwarding become local: a `load` on token `s_k` can only be fed or killed by `store`s on the *same* token `s_k`, so the search is confined to one strand instead of a global memory SSA.

```
; %s.heap and %s.io are independent state tokens ⇒ the store and the print
; have NO ordering edge between them; the scheduler may emit them in either order.
func @indep(%p: ptr, %v: i32, %s.heap: state, %s.io: state) -> (state, state) {
  %s.heap1 = store %p, %v, %s.heap        ; threads ONLY the heap token
  %s.io1   = print %v, %s.io              ; threads ONLY the io token
  return (%s.heap1, %s.io1)
}
```

Honest limitation (R4): this payoff is gated entirely on the alias analysis that *populates* fine-grained state precisely. RVSDG/jlm had the representation and still fell back to a single state. If the alias pass is weak, every effect lands on one token and we inherit exactly the conservative trap we set out to beat. The representation is the easy half; the analysis is the open work.

---

## 7. The mapping table (mechanism · rough rule count · what it replaces)

Rough rule counts are order-of-magnitude estimates for a mature core, to substantiate the "dramatically smaller codebase" claim (D5/DC17) — not measured numbers. "Structural" means **zero rules**: the optimization is a property of the data structure.

| Optimization | Mechanism | Rough rule count | Prior-art pass it replaces |
|---|---|---|---|
| GVN / CSE | **Structural** (hash-consing, DC8) | 0 | LLVM GVN/EarlyCSE; SoN IGVN |
| Constant folding | Tier-1 `=>` + host fold `{..}` (DC9) | ~1 per opcode | LLVM ConstantFold / InstSimplify |
| Algebraic identities | Tier-1 `=>` | ~30–80 | LLVM InstCombine (identity subset) |
| Strength reduction (pow2 etc.) | Tier-1 `=>` (portable) | ~10–20 | LLVM InstCombine / -reassociate subset |
| DCE (pure) | **Structural** (unreferenced node, DC2/DC7) | 0 | LLVM ADCE / DCE |
| Dead effect elimination | **Structural** (dead state-token tail, DC4) | 0 | LLVM DSE (dead store elim) |
| Dead-region / unreachable | **Structural** (unreferenced port + γ-collapse, DC1/DC5) | ~3 | LLVM SimplifyCFG (unreachable/dead-block) |
| Copy / identity elimination | **Structural** + 1 Tier-1 rule | ~1 | LLVM copy-prop; SSA has no copies anyway |
| Branch / switch simplification | Tier-1 `=>` on `Cond` (DC6) | ~6–12 | LLVM SimplifyCFG / -simplifycfg |
| Conditional const propagation | Emergent (fold + γ-collapse) | 0 (composition) | LLVM SCCP |
| LICM (hoisting) | **Structural** (pure float + GCM placement, DC3) | 0 | LLVM LICM |
| Sinking | **Structural** (latest placement, DC3) | 0 | LLVM Sink |
| Inlining | `specialize(region)` primitive (DC9) | 1 primitive (shared) | LLVM Inliner |
| Specialization / static-arg | `specialize` + memoize (DC11) | shared | LLVM IPSCCP / FunctionSpecialization |
| Loop unrolling | `specialize(Loop)` + fold | shared | LLVM LoopUnroll |
| Loop peeling | `specialize(Loop, 1)` | shared | LLVM LoopPeel |
| Tail-recursion elimination | `specialize` → `Loop continue` | shared | LLVM TailCallElim |
| Loop unswitching | hoist `Cond` out of `Loop` (§8) | ~2 | LLVM LoopUnswitch |
| Reassociation | Tier-2 `~` overlay + greedy extract (D7) | ~4–8 | LLVM Reassociate (subset) |
| Redundant-load elim / store-fwd | **Structural** (per-token strand walk, DC4) | ~few | LLVM GVN load-elim / MemCpyOpt |
| Effect reordering / parallelization | **Structural** (independent fine state, DC4) | 0 | (no direct LLVM analogue; exposes independence a CFG cannot) |
| Instruction selection | Tier-2 `lower ... @cost` + greedy DP (DC15) | ~one tile-set per ISA | LLVM SelectionDAG / GlobalISel ISel |

Reading the table: the entries that are **Structural (0 rules)** or **shared** with comptime/codegen are where the codebase shrinks. The genuinely rule-bearing optimizers (folding, identities, branch simplification) are short declarative tables in the one verifiable DSL, not imperative passes.

---

## 8. Loop unswitching (a worked compositional example)

Unswitching is not a bespoke pass either; it is hoisting a loop-invariant `Cond` out of a `Loop` — a region-restructuring built from `specialize` (§5) plus γ/θ commutation. If the `Cond`'s predicate references no loop port, the loop runs the *same* arm every iteration, so we can lift the `Cond` above the loop and specialize one `Loop` per arm:

```
; before — invariant predicate %flag tested every iteration
func @unswitch(%n: i32, %flag: i1, %x: i32) -> i32 {
  %r = loop (%acc = 0, %i = 0) : i32 {
    %c = cmp.lt %i, %n
    break unless %c -> %acc
    %d = cond %flag -> (i32) {        ; %flag is NOT a loop port ⇒ invariant
      case 1: { %a = add %acc, %x  yield %a }
      case 0: { %b = sub %acc, %x  yield %b }
    }
    %i1 = add %i, 1
    continue (%d, %i1)
  }
  return %r
}

; after — Cond hoisted; one specialized Loop per arm, predicate tested once
func @unswitch(%n: i32, %flag: i1, %x: i32) -> i32 {
  %r = cond %flag -> (i32) {
    case 1: { %ra = loop (%acc = 0, %i = 0) : i32 {
                %c = cmp.lt %i, %n  break unless %c -> %acc
                %a = add %acc, %x   %i1 = add %i, 1
                continue (%a, %i1) }
              yield %ra }
    case 0: { %rb = loop (%acc = 0, %i = 0) : i32 {
                %c = cmp.lt %i, %n  break unless %c -> %acc
                %b = sub %acc, %x   %i1 = add %i, 1
                continue (%b, %i1) }
              yield %rb }
  }
  return %r
}
```

The two specialized loop bodies are still hash-consed, so any shared sub-terms between the arms remain a single interned node (GVN, §2.1). This is the RVSDG IVT (invariant-value / unswitch) transform expressed as composition, not a dedicated pass.

---

## 9. Phase ordering, and why most of it disappears (D6)

Classical optimizers agonize over pass ordering because passes mutate in place and expose work for one another in order-dependent ways (failure-modes 7, 10). Helix mitigates this structurally:

- **Tier-1 runs at construction, to local fixpoint.** Folding, identities, CSE and DCE are *always already done* on any node you hold; you never schedule them, so they cannot be mis-ordered relative to each other (DC8). This also avoids V8's cache-thrash from repeated mutating sweeps (failure-mode 7): normalization is eager and append-only, not a worklist that revisits a node 20 times to lower it once.
- **The region primitive (§5) re-enters Tier-1.** After an inline/specialize, the newly-substituted nodes are constructed *through* the `World`, so they fold/CSE on the way in. Optimization "after inlining" is automatic, not a re-run of the pipeline.
- **What remains genuinely ordered** is the deliberate, documented phase boundary at the *back* end (DC15, failure-mode 10): optimize on the floating graph until lowering; then Tier-2 ISel, then schedule (read order off the state strand), then **register allocation before SSA destruction** (interference graphs are chordal only pre-destruction), then parallel-move resolution. That one boundary is chosen on purpose; everything before it is order-insensitive.

This does not abolish phase ordering — Tier-2 cost extraction and the ISel/schedule/RA sequence are still a pipeline with NP-hard sub-problems (R3). It abolishes the *mid-end* phase-ordering problem, which is where most of LLVM's pass-ordering pain lives.

---

## 10. Honest assessment (R1, R3, R4, R6)

The defensible claim is the synthesis bottom line, not a superiority claim:

- **This targets _matching_, not beating, `-O3` output quality (R1).** No prior graph IR (RVSDG/jlm, Thorin, MimIR) has been shown to beat LLVM/GCC `-O3` *output*; they emit LLVM IR and aim to match it. RVSDG already gets CSE/LICM/DCE as single passes on a small core and is "competitive, not beating." Helix's win is **code size and unification** — the same optimizations as the best graph IRs, emergent or in short verifiable rules, with comptime and codegen folded into the one engine — *not* a new high-water mark in generated-code quality.
- **Owning ISel + schedule + RA means owning NP-hard problems (R3).** The Tier-2/back-end optimizations (ISel, reassociation-for-cost) lag a mature backend until the cost model and tile set are tuned; output quality lags LLVM until this matures. We chose greedy extraction deliberately (D7) knowing it is not optimal.
- **Fine-state wins are analysis-gated (R4).** §6.3's reordering/parallelization payoff is real *only* if the alias analysis populates fine-grained state precisely; otherwise everything serializes on one token.
- **The float is smallest where programs spend effort (R6).** LICM/sinking/reordering wins shrink on effect-heavy code, because effectful ops are pinned. The hybrid keeps that case inspectable and CFG-cheap, but the reordering upside there is modest.

What is *not* hedged: the **codebase-size and unification** claims. GVN/CSE/DCE/copy-elim/LICM/sinking at **zero rules**, the five region transforms behind **one** primitive shared with comptime, and ISel sharing the **one** DSL with the peephole optimizer are concrete, defensible structural facts about the form.

---

## See also

- [Reduction Engine](14-reduction-engine.md) — Tier-1 eager normalization and the Tier-2 bounded overlay these optimizations ride on.
- [Comptime](15-comptime.md) — the `specialize`-a-region primitive shared with §5; NbE, filters, fuel (DC9–DC11).
- [Codegen](17-codegen.md) — Tier-2 instruction selection, scheduling off the state strand, and SSA register allocation (DC13/DC15).
- [Core Model](11-core-model.md) — the six node forms, the two strands, block parameters (DC1–DC8).
- [Types and Effects](13-types-and-effects.md) — fine-grained typed state tokens and linearity (DC4).
- [Design Rationale](10-design-rationale.md) — the DC/D/R provenance cited throughout.
- [Risks and Open Problems](22-risks-and-open-problems.md) — R1, R3, R4, R6 expanded.
- [Prior-art synthesis](research/00-synthesis.md) — the constraint/differentiator/risk register.
