# Risks & Open Problems

*The honest ledger: every load-bearing risk in the Helix thesis, why it is real (cited to prior art), what Helix does about it, and what danger survives the mitigation — plus the Helix-specific open problems and a research agenda to retire them.*

This page is deliberately adversarial toward Helix's own pitch. The thesis (see
[Overview](00-overview.md)) is **not** "Helix beats LLVM/GCC -O3 output quality." No prior graph
IR — RVSDG/jlm, Thorin, MimIR — has been *shown* to beat -O3; they emit LLVM IR and aim to match
it. Helix's defensible claims are narrower: (1) conceptually simpler and a dramatically smaller
codebase than Sea-of-Nodes / RVSDG / Thorin / MimIR; (2) *matches* the best graph IRs on
optimization through **structural** wins (GVN/LICM/DCE emergent from the form, see
[Optimizations](16-optimizations.md)) rather than via expensive equality saturation; (3) uniquely
unifies comptime evaluation *and* machine-code emission into the graph itself; (4) zero IR
round-trip at either end. Each of those claims has a counter-pressure, catalogued below.

Provenance: risks R1–R7 and recurring failure-modes 1–12 are reproduced and expanded from
`research/00-synthesis.md`; design decisions are cited as DC1–DC17 and differentiators as D1–D7.

---

## How to read the risk register

Each entry has four fixed parts:

| Field | Meaning |
|---|---|
| **Risk** | The claim or design choice that may not hold. |
| **Why it is real** | Prior-art evidence that the danger has bitten someone before. |
| **Mitigation** | The specific Helix mechanism intended to blunt it (with DC/D citation). |
| **Residual danger** | What is *still* unsolved after the mitigation. Never zero. |

A severity/likelihood summary, then each risk in full.

| Risk | One-line | Severity | Likelihood | Retired by |
|---|---|---|---|---|
| **R1** | Beating -O3 output quality is unproven | High | High (as stated) | downgrading the claim, not engineering |
| **R2** | Comptime termination is undecidable | High | Certain | bounded only, never solved |
| **R3** | Owning ISel + schedule + RA = owning NP-hard problems | High | High | years of backend maturation |
| **R4** | Fine-grained state needs alias analysis or it degenerates | Med-High | High | a good alias pass that writes states back |
| **R5** | Any sea-of-nodes is hard to debug | Medium | High | tooling (DC16), a mitigation not a cure |
| **R6** | Hybrid float gives small wins on effect-heavy code | Medium | Certain | nothing — it is a structural ceiling |
| **R7** | One rule DSL may not stretch to comptime cleanly | Medium | Unknown | a prototype that proves or kills it |

---

## R1 — "Optimizes better" vs "smaller codebase" is partly in tension; *beating* -O3 is unproven

**Risk.** The headline temptation is "optimizes better than all four prior systems *and* in less
code." These pull against each other, and the stronger half — strict output-quality superiority
over LLVM/GCC -O3 — is unproven by *any* graph IR.

**Why it is real.**
- RVSDG already gets CSE/LICM/DCE as *single passes* on a small core, yet jlm is reported as
  "competitive, not beating" LLVM -O3 `[02]`. The structural form buys *parsimony*, not a quality
  ceiling above LLVM.
- Cranelift's radical acyclic-e-graph mid-end bought roughly **~0.1% runtime for ~0.005% compile
  cost** `[05][08]` — i.e. the exotic representation is nearly free *and nearly weightless*. The
  real wins were ordinary GVN/LICM/DCE the structure enables (this is exactly D7).
- LLVM/GCC have decades of pass tuning and target-specific lore that no clean-room IR reproduces
  cheaply.

**Mitigation.** Reframe, per the honest pitch and D7: Helix targets the *structural* wins
(GVN/CSE for free via hash-consing DC8, LICM/sinking via scheduling DC15, DCE via the acyclic
DAG) and explicitly **declines full equality saturation** (D7). The defensible bar is "matches the
best graph IRs on optimization, in dramatically less code, with comptime and codegen folded into
the graph." Where a number is needed, see [Evaluation](20-evaluation.md): the metric is
*output-quality parity at a fraction of the code*, not victory over -O3.

**Residual danger.** "Matches the best graph IRs" still means *matching jlm/MimIR*, which
themselves only *match* (not beat) LLVM. So even full success leaves Helix at parity with -O3, not
above it. Any page or talk that slips into "beats -O3" is making the riskiest possible claim and
must be corrected. R1 is the risk most likely to be over-stated by enthusiasm rather than by
engineering reality.

---

## R2 — Comptime termination / divergence is fundamentally unsolved

**Risk.** Helix's comptime is Tier-1 graph reduction (DC9), and Tier-1 reduction is
Turing-complete. No specializer both always terminates *and* always maximally specializes. A
user-written `@comptime` function, or an aggressive `static` argument, can make compilation hang
or diverge.

**Why it is real.**
- **MimIR**: a `tt` filter on a function applied to a non-constant can make the **type checker
  itself diverge** `[04][07]`. The divergence is not in user code — it is in the compiler's own
  fixpoint.
- **Thorin** *abandoned* its 2015 termination heuristic because "the effects... were difficult to
  assess for the programmer," retreating to explicit Schism-style filters `[03][07]`.
- **Zig / C++**: both ship blunt global budgets — branch-quota, `-fconstexpr-steps`,
  template-depth — precisely because the general problem is undecidable `[07]`. Zig's quota is
  famously *ungettable* from within the program (failure-mode 8).

**Mitigation (DC11).** Comptime reduction runs under an **introspectable, localizable fuel/step
budget**: per-call, reported in diagnostics, attributable to a source span — explicitly *not*
Zig's opaque global quota. Comptime is **hermetic and deterministic** (paired with the state
strand for purity, DC4), so results are **cacheable and memoized by (function, static-args)**.
Staging is **programmer-visible via filters/annotations** (DC10, Thorin Schism-style) so divergence
is steerable rather than mysterious:

```
; @comptime function with a static (compile-time) parameter and a fuel annotation
func @pow(static %n: i32, %x: f64) -> f64 @comptime @fuel 4096 {
  ; %n is reduced at construction time (Tier-1 NbE); %x stays a neutral term
  %r = loop (%acc = 1.0, %i = 0) : f64 {
    %c = cmp.lt %i, %n
    break unless %c -> %acc
    %acc1 = mul %acc, %x
    %i1   = add %i, 1
    continue (%acc1, %i1)
  }
  return %r
}
```

When `@pow` is applied with a static `%n`, NbE unrolls the loop into a chain of `mul` Ops; with a
dynamic `%n` it residualizes as a runtime `Loop` region (neutral term → Op nodes, see
[Comptime](15-comptime.md)). The fuel bound caps the unroll.

**Residual danger.** Fuel **leaks into UX exactly as Zig's quota and C++'s step limit do** (R2,
failure-mode 8). A budget that is too low breaks legitimate metaprograms; too high and a typo hangs
the build. We can localize the blame and report the budget, but we cannot promise *both* maximal
specialization *and* guaranteed termination — that is a theorem, not a missing feature. The honest
position is "bounded, diagnosable, cacheable," never "solved."

---

## R3 — Direct codegen forfeits LLVM's backend maturity (we own the NP-hard problems)

**Risk.** Emitting bytes directly (DC13, D1) means Helix owns instruction selection, instruction
scheduling, **and register allocation** — no LLVM safety net. Several of these are intractable in
the worst case and non-local in cost.

**Why it is real.**
- **Optimal DAG instruction selection is NP-complete** (Koes & Goldstein, CGO 2008): the moment a
  value is shared by multiple consumers — a DAG, not a tree — tiling is NP-complete `[08]`. Helix's
  pure value strand is exactly such a shared DAG.
- **Combined schedule + register allocation is NP-hard**, and the two pull opposite ways:
  schedule-first raises register pressure, allocate-first injects false dependences `[08]`
  (failure-mode 10).
- **SSA interference graphs are chordal only *before* SSA destruction**; register allocation after
  SSA elimination is NP-complete `[08]`. Get the phase order wrong and you forfeit the one
  near-linear-time guarantee available.
- regalloc2 is **>10k LoC of dense Rust for a reason** `[08]`. This is not incidental complexity.

**Mitigation (DC13/DC15, D1; see [Codegen](17-codegen.md)).**
- **ISel** is bounded Tier-2: lowering rules (`lower ... @cost`) record target-op alternatives as
  union nodes in the **acyclic, append-only overlay**, then a **greedy bottom-up BURS-style cost
  extraction** picks a min-cost tiling — tree-matching *up the pure value strand, stopping at
  region-port / state-strand boundaries*. We do **not** attempt optimal DAG covering; we accept
  greedy (D7, see [Reduction Engine](14-reduction-engine.md)).
- **Schedule** is *read off the state strand skeleton* (the linear effect order is already in the
  graph, DC3/DC4) and pure floating Ops are list-scheduled into it — so a legal schedule always
  exists and is cheap to recover (DC6), sidestepping the VSDG sequentialization disaster.
- **Register allocation runs on SSA *before* destruction** (DC15), preserving chordality;
  region-port (block-parameter) transfers resolve as **parallel moves at the very end**.
- **Existence proof:** Cranelift's VCode + regalloc2 + MachBuffer is precisely this pipeline in
  production `[08]`.

```
; a lowering rule recorded as a Tier-2 alternative, then chosen by greedy cost extraction
lower lea : (add ?b (mul ?i (const i64 ?s))) => (x64.lea ?b ?i ?s) @cost 1
            if member(?s, {1,2,4,8})
```

**Residual danger.** "There exists a production pipeline shaped like this" is *not* "our output
matches LLVM." Greedy extraction is provably sub-optimal on shared DAGs (see Open Problem OP-2),
and matching LLVM's instruction-selection lore, peephole catalogue, and machine schedulers is years
of work. **Output quality will lag LLVM until the backend matures, and this directly threatens R1.**
R3 is the risk most likely to be underestimated on a timeline.

---

## R4 — Fine-grained state needs a good alias analysis or it degenerates

**Risk.** D4's whole payoff — non-aliasing effects being *provably independent in the
representation* — is gated on actually *populating* many fine-grained state tokens precisely. If we
cannot, every effect threads one coarse state and we inherit the conservative trap we set out to
beat.

**Why it is real.**
- **RVSDG/jlm had the representation and still fell back to a single memory state + one loop
  state**, "leaving significant parallelization potential unused" `[02]` (failure-mode 9). Having
  the *expressive form* is necessary but nowhere near sufficient.
- The hard part is the analysis that decides *which* alias class each load/store belongs to,
  cheaply and soundly, without exploding the number of states.

**Mitigation (DC4, D4).** Helix designs the type system for **multiple independent fine-grained
state tokens from day one** (one per alias class / region), not as a retrofit. An alias analysis is
expected to *write its results back into the graph as additional state strands* — independence
becomes data, consumed by everything downstream for free. Linearity of each token is structurally
enforced (DC4), closing MimIR's admitted unenforced-linearity hole `[04]`.

```
; two non-aliasing regions thread two INDEPENDENT state tokens; the stores cannot be ordered
; against each other, so they float relative to one another and can reorder/parallelize
func @copy2(%a: ptr, %b: ptr, %sa0: state, %sb0: state) -> (state, state) {
  (%va, %sa1) = load %a, %sa0        ; alias class A
  (%vb, %sb1) = load %b, %sb0        ; alias class B (provably disjoint)
  %sa2 = store %a, %vb, %sa1
  %sb2 = store %b, %va, %sb1
  return (%sa2, %sb2)
}
```

**Residual danger.** The representation is understood; **the alias analysis that populates fine
state without blowing up is open work** (see Open Problem OP-1). If the analysis is conservative,
states collapse and we are back to RVSDG/jlm's single-state regret — having paid for the
fine-grained machinery and gotten none of the benefit. The danger is real precisely because a prior
system with the *same representation* hit it.

---

## R5 — Debuggability of any sea-of-nodes is a standing liability

**Risk.** Helix is a graph IR. Graph IRs are hard for humans to read, diff, and reason about, and
this is a recurring, first-order objection — not a cosmetic one.

**Why it is real.**
- **V8 retreated from Sea-of-Nodes partly because** "manually/visually inspecting and
  understanding a Sea of Nodes graph is hard" `[01]` (failure-mode 6).
- **MimIR concedes** graph IRs need external graph tools to visualize, whereas an instruction list
  "is straightforward to dump even if incomplete" `[04]`.
- **Thorin** hurt itself further with *non-semantic names* `[03]`.

**Mitigation (DC16).** Helix is **printable, diffable, and source-mapped from day one** with a
**canonical textual format** that reads like an instruction list, not a node dump (see
[Format](12-format.md)). Names are **stable and semantic** (explicitly avoiding Thorin's
non-semantic names). The two-strand structure helps: the state strand *is* a linear skeleton, so
the textual form has an obvious top-to-bottom effect order even though pure Ops float.

```
module demo {
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

**Residual danger.** **DC16 is a mitigation, not a solution.** A nice printer does not make a
floating pure DAG as locally inspectable as a CFG basic block; "where does this value get computed"
is still answered by *scheduling*, which is downstream of what the programmer reads. Tooling cost is
**real and recurring** — it must be funded continuously, not built once. V8 had good tooling and
left anyway.

---

## R6 — The hybrid float gives smaller reordering wins on effect-heavy code

**Risk.** The pure-floats / effects-pinned hybrid (DC3, D2) delivers its reordering benefit only to
*pure* nodes. On effect-heavy code, almost everything is pinned to the state strand, so little
floats and the "optimizes better" claim is weakest exactly where many real programs spend their
time.

**Why it is real.**
- This is the **V8 collapse-to-CFG lesson**, stated as a structural ceiling: "almost all generic JS
  operations can have arbitrary side effects," so "the control nodes and control chain always
  mirror the structure of the equivalent CFG" `[01][06]` (failure-mode 3). The same observation
  that *motivates* D2 also *caps* it.

**Mitigation (DC3/D2, partly DC4/D4).** Helix makes the side-effect skeleton the **native default**
rather than a retrofit, so the effectful common case is already CFG-cheap and CFG-inspectable —
we never pay the "collapse-to-CFG-anyway" tax that V8 paid late. **Fine-grained state (D4) is the
one lever that widens the float on effectful code:** if an alias pass proves two effects
independent, they reside on different state strands and *can* reorder despite both being effectful
(see the `@copy2` example under R4). So R4's success is partly what mitigates R6.

**Residual danger.** **Nothing fully removes this — it is a structural ceiling, not a bug.** When
effects genuinely depend on each other (a true linear chain through one alias class), the strand is
a chain and nothing floats. On such code Helix is, at best, a tidy CFG with free GVN — good, but not
the place the "optimizes better" story shines. Be honest in benchmarks: separate pure-numeric
kernels from effect-dense code and report them apart ([Evaluation](20-evaluation.md)).

---

## R7 — One rule DSL may not stretch to comptime cleanly

**Risk.** Helix proposes *one* declarative rule DSL (DC14) shared by peepholes, comptime folds, *and*
instruction selection (D5). Cranelift's ISLE proves *opt + ISel* in one DSL — but **nobody has
shown the same rule engine also serving general partial evaluation** with filters, staging, and
fuel.

**Why it is real.**
- ISLE drives mid-end rewrites and backend lowering, overlap/priority-checked and SMT-verifiable
  (VeriISLE) `[05][08]` — that part is proven.
- Folding pure constants is easy (`fold-add`). **Staging higher-order and recursive comptime in the
  *same* engine is unproven** (the D3/D5 risk explicitly flagged in the synthesis). The leap from
  "rewrite a fixed pattern" to "specialize a recursive `Func` under a fuel budget with neutral
  terms" is a different kind of operation.

**Mitigation (DC14, D3/D5).** The rule DSL has a precise division of labor:
- `=>` **oriented** rules are Tier-1, eager, run to local fixpoint at construction — these *are* the
  comptime folds and peepholes.
- `~` **equivalence** rules are Tier-2, overlay-only, used *sparingly* for ISel alternatives and a
  little reassociation.
- `{..}` host computation does compile-time arithmetic; `if` guards constrain matches.

```
rule add-zero : (add ?x (const _ 0)) => ?x
rule fold-add : (add (const ?t ?a) (const ?t ?b)) => (const ?t {a + b})   ; {..} host fold
rule mul-pow2 : (mul ?x (const i32 2)) => (shl ?x (const i32 1))
rule comm-add : (add ?x ?y) ~ (add ?y ?x)                                 ; Tier-2 ISel only, sparingly
```

General comptime that the *pattern* DSL cannot express (recursive specialization, higher-order
arguments) is handled by **NbE eval→reify** (DC9), with the rule DSL supplying the *leaf folds* that
NbE calls. The DSL is the algebra of primitives; NbE is the driver. That keeps each piece in its
proven lane.

**Residual danger.** **This is the least-proven differentiator.** If the clean `=>` / `~` / NbE
split turns out to be leaky — e.g. comptime needs ad-hoc rules that don't fit the pattern shape, or
fuel accounting tangles with the eager-normalization fixpoint — we may end up with a second engine
after all, eroding the "one engine, less code" claim (D5). The first prototype must *try to break
this seam on purpose* (see Research Agenda, item 1).

---

## Helix-specific open problems

These are not in the inherited R1–R7 register; they are problems Helix's *specific* design choices
create. Each is genuinely open.

### OP-1 — Enforcing linearity efficiently

DC4 demands state tokens be **used exactly once**, *enforced* (closing MimIR's unenforced hole
`[04]`). A naive check is a use-count pass, but Helix wants this as a **structural invariant**
maintained by the World factory at construction time, not a re-verifying sweep (failure-mode 7,
cache thrash). Open: can linearity be a *typing* invariant on state-typed edges cheap enough to hold
at every smart-constructor call, including across `Cond`/`Loop`/`Func` region ports where a token
enters and leaves a region? What is the cost when an alias pass *adds* new state strands after the
fact — does re-threading re-validate linearity globally?

### OP-2 — The greedy-extraction optimality gap

Tier-2 picks tilings by **greedy bottom-up cost extraction** (D7), deliberately *not* solving the
NP-complete optimal DAG covering `[08]`. Cranelift's data says greedy is fine because e-classes are
tiny (avg ~1.13 nodes) `[05]`. Open: **is that small-e-class assumption preserved under Helix's
lowering rules**, where one portable Op may expand into many target-Op alternatives across ISAs? If
e-classes grow, greedy's gap to optimal widens precisely on the hot tiling decisions (addressing
modes, fused multiply-add, `lea`). We need to *measure* the e-class size distribution Helix's own
`lower` rules produce, not assume Cranelift's.

### OP-3 — Confluence of the Tier-1 rule set

Tier-1 applies oriented `=>` rules to **local fixpoint at construction time** (DC8). If the rule set
is **not confluent**, the normal form depends on rule-application order — meaning GVN/CSE is no
longer canonical (two equal terms might not hash-cons to the same node) and builds become
non-deterministic, breaking comptime caching (DC11). Open: prove (or test, via a critical-pair /
Knuth-Bendix-style check) that the shipped `=>` rule set is confluent and terminating, or define a
fixed deterministic application order and accept that "canonical" means "canonical *under this
order*." This interacts with R7: comptime folds are `=>` rules, so comptime determinism rides on
this.

### OP-4 — Comptime fuel ↔ memoization/caching interaction

DC11 says comptime is memoized by `(function, static-args)` and bounded by per-call fuel. These
interact badly if unexamined: **is a cached result valid for a caller with a *different* fuel
budget?** A specialization computed under fuel 4096 may be a *partial* residual that a fuel-8192
caller would have reduced further. Open: define the cache key to include the *fuel actually
consumed* (or the saturation status), so a richer budget triggers recomputation rather than reusing
an under-reduced residual. Also: is the budget charged *before or after* a cache hit — i.e. does
reusing a memoized result cost fuel? Getting this wrong makes builds non-reproducible (the opposite
of DC11's hermetic intent).

### OP-5 — Exception / unwinding modeling in the state strand

The synthesis notes RVSDG is a research prototype with **no exceptions/intrinsics** `[02]`, and
PEG/Peggy did "no optimizations across try/catch boundaries" `[05]` (failure-mode 11). Open: how do
unwinding edges live in the two-strand model? A throwing Op has *two* successors (normal, unwind),
which is control flow that is not a clean `Cond`. Candidate: model the unwind path as an extra
*result port* on the effectful Op carrying the state strand into a landing-pad region, keeping the
graph acyclic (DC7) and phi-free (DC5). Unverified. This is a prerequisite for any real language and
must not be deferred to "later" the way RVSDG did.

### OP-6 — Irreducible control flow

DC6 canonicalizes control to a minimal structured set (`Cond` = gamma, `Loop` = tail-controlled
theta). Structured forms cannot *directly* express irreducible CFGs (multiple-entry loops). RVSDG's
two recovery strategies are both bad: SCFR is structured-but-bloaty, PCFR is arbitrary-but-GPU-hostile
`[02]`. Open: since Helix parses *directly* into the graph (DC12) rather than recovering structure
from an existing CFG, can it **avoid ever constructing irreducible control** by building `Loop`
regions on the fly (Braun-2013-style)? `goto`-heavy source and computed gotos are the stress test.
If a source language *demands* irreducible flow, Helix must either node-split (bloat) or reject — and
must say which.

### OP-7 — Debug info through aggressive comptime

Comptime is graph reduction (DC9): a `@comptime` loop is unrolled, a static call is inlined and
folded, neutral terms are reified into fresh Ops. **What source location does a folded constant
carry?** Aggressive specialization destroys the 1:1 source↔node mapping that DC16 promises. Open:
maintain a *provenance trail* on reduced nodes (which rule fired, from which source span, at which
fuel step) so a debugger can explain "this `0` came from folding `n*0` where `n` was the static arg
at line 12." This is the comptime-specific face of R5: the better the comptime, the worse the debug
mapping, unless provenance is first-class.

---

## How the open problems and risks connect

```
              R1 (beat -O3 unproven)
                  ^        ^
   threatens via  |        |  threatens via
                  |        |
   R3 (NP-hard backend)   R6 (float weak on effects)
        |  \                    ^
   OP-2 |   \ OP-5/OP-6          | mitigated by
 (greedy gap)  (control models)  |
                                 R4 (needs alias analysis) --- D4
                                 ^
                            OP-1 (linearity)

   R2 (comptime termination) --- DC11 fuel
        |                              |
   OP-4 (fuel x cache)            OP-7 (debug info)
        |
   R7 (one DSL for comptime?) --- OP-3 (confluence)
```

Reading: **R1 is downstream of almost everything** — both R3 (a weak backend caps output quality)
and R6 (a structural ceiling on effectful code) feed it. **R4 is the hinge**: solving it (via D4 +
OP-1's enforced linearity + a real alias pass) is what would *widen* R6's float and is the strongest
available lever on the "optimize better" half of the thesis. **R2/R7 form the comptime cluster**,
gated by OP-3 (confluence) and OP-4 (fuel/cache) for soundness and OP-7 for debuggability.

---

## Research agenda — what to prototype first to retire the biggest risks

Ordered by *risk retired per unit effort*. The goal of an early prototype is to **fail fast on the
unproven seams**, not to demo the happy path.

### Phase 0 — Kill or confirm the unproven seam (attacks R7, OP-3)

Build the **World factory + Tier-1 `=>` engine + NbE driver only** — no backend, no Tier-2. Then
*try to break the one-DSL claim on purpose*:
- Express a recursive `@comptime` `factorial` and a higher-order `@comptime` `map` using only `=>`
  rules + NbE eval→reify. If this needs machinery outside the DSL, R7 has bitten and we learn it in
  weeks, not years.
- Run a **critical-pair / confluence check** on the starter rule set (OP-3). A non-confluent set
  discovered here is cheap to fix; discovered after the optimizer is built on canonicality, it is
  not.

*Most-informative result:* a published or internal demonstration that **a single rule engine drives
constant folding, peepholes, *and* recursive/higher-order comptime** with verified confluence would
strongly *strengthen* the thesis (it is the one wholly-novel claim, D3/D5). The complementary result
— that comptime provably needs a second engine — would *kill* the "one engine, less code"
differentiator and should force a redesign now.

### Phase 1 — Prove the parse-in / emit-out spine end-to-end (attacks R3, D1)

Smallest possible vertical slice: parse a trivial language **directly into the graph** (DC12) and
emit machine bytes for **one target (x64)** via greedy extraction + state-strand scheduling +
SSA-pre-destruction regalloc + streaming encode (DC13/DC15). No optimization quality goal yet — just
**zero secondary IR at either end**, working. This retires the *feasibility* half of R3 (the
existence-proof shape exists; do *we* have it) and proves D1 concretely.

*Most-informative result:* measure the **e-class size distribution** produced by our own `lower`
rules (OP-2). If it matches Cranelift's ~1.13 avg, greedy extraction is justified; if it is much
larger, OP-2 is a live problem and we need a smarter (still-bounded) extractor before chasing
output quality.

### Phase 2 — Make fine-grained state pay (attacks R4, R6, OP-1)

Implement enforced linearity in the factory (OP-1) and a **first alias analysis that writes extra
state strands back into the graph** (D4). Benchmark effect-dense code *with* and *without* the
split. This is the only experiment that directly addresses the "optimize better" half of R1 and the
R6 ceiling.

*Most-informative result:* a benchmark where **two provably-disjoint effects reorder/parallelize**
because they sit on distinct state strands — something a single-state IR (RVSDG/jlm in practice
`[02]`) cannot show. That is the concrete evidence that Helix's representation buys real
optimization, not just parsimony.

### Phase 3 — Honest measurement vs the field (attacks R1, R5)

Only now compare against LLVM -O2/-O3 and jlm/MimIR on output quality *and* codebase size, reporting
pure-numeric and effect-heavy benchmarks **separately** (R6). Ship the debug-info provenance trail
(OP-7) and dump/diff tooling (DC16) as a deliverable, not an afterthought — R5 is retired by
*sustained* tooling, and a prototype with no debugger is not a fair test of debuggability.

### The single result that would most strengthen — or kill — the thesis

> **Strengthen:** a verified-confluent single rule engine that demonstrably serves folding,
> peepholes, ISel, *and* recursive/higher-order comptime (Phase 0 + Phase 1). This is the one claim
> no prior system (ISLE, Thorin, MimIR) has shown end-to-end, and it underwrites both "less code"
> and "comptime unified into the graph."
>
> **Kill:** a demonstration that direct greedy-extraction codegen *cannot* close the output-quality
> gap to LLVM on the hot tiling decisions within a small-codebase budget (R3 → R1), **or** that
> comptime fundamentally requires a separate engine (R7). Either would force retreating to the
> *defensible* pitch (parity + smaller code + unified comptime) and abandoning any "optimizes
> better" framing entirely.

---

## See also

- [Overview](00-overview.md) — the thesis and the honest pitch these risks qualify.
- [Design Rationale](10-design-rationale.md) — DC1–DC17 / D1–D7 in full.
- [Reduction Engine](14-reduction-engine.md) — the two-tier engine; Tier-1 confluence (OP-3), Tier-2 greedy extraction (OP-2, R3).
- [Comptime](15-comptime.md) — NbE, filters, fuel; sources of R2, R7, OP-4, OP-7.
- [Optimizations](16-optimizations.md) — the structural GVN/LICM/DCE wins behind the R1/D7 framing.
- [Codegen](17-codegen.md) — direct ISel/schedule/RA; the home of R3, OP-2, OP-5, OP-6.
- [Types & Effects](13-types-and-effects.md) — fine-grained state tokens and linearity; R4, OP-1.
- [Frontend](18-frontend.md) — parse-directly-into-the-graph; irreducible control (OP-6).
- [Evaluation](20-evaluation.md) — how R1/R6 get measured honestly.
- [Implementation Plan](19-implementation-plan.md) — sequencing aligned with the research agenda above.
- `research/00-synthesis.md` — source of R1–R7, failure-modes 1–12, DC1–DC17, D1–D7.
