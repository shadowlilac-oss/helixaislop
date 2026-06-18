# E-graphs & Equality Saturation

## Overview

Equality saturation (EqSat) is a rewrite-driven program-optimization technique built on the
**e-graph** data structure. Instead of applying rewrites destructively in a chosen order, an
optimizer adds *equalities* to a representation that simultaneously holds every program reachable
under the rule set; a separate **extraction** step then picks the cheapest representative. The
foundational compiler-IR work is Tate, Stepp, Tatlock & Lerner, *Equality Saturation: a New
Approach to Optimization* (POPL 2009 / LMCS 2011), which introduced **PEGs / E-PEGs** and the
**Peggy** Java optimizer. The technique was made fast and reusable by Willsey et al., *egg: Fast
and Extensible Equality Saturation* (POPL 2021), whose two contributions are **rebuilding**
(deferred congruence maintenance) and **e-class analyses**. Zhang et al. then unified e-graphs
with Datalog in **egglog** (*Better Together*, PLDI 2023), backed by their **Relational E-matching**
result (POPL 2022). On the production-compiler side, Chris Fallin's **aegraph** (acyclic e-graph)
mid-end in Cranelift and the **ISLE** term-rewriting DSL show what survives when you put EqSat in a
fast AOT/JIT compiler — and which constraints had to be imposed.

The central promise — verified by multiple of these papers — is "stronger optimization from a
smaller, declarative codebase": basic equality axioms interact to produce optimizations that a
traditional compiler would have to special-case. The central caveat is equally well documented:
e-graph blowup, NP-hard DAG-aware extraction, and the difficulty of modeling effects, ordering, and
loops cleanly.

## Core Model (nodes / edges / e-classes)

**E-graph (egg's definition).** An e-graph is a set of **e-classes**; each e-class is a set of
equivalent **e-nodes**; each e-node is "a function symbol with children e-classes (not e-nodes)."
Equivalence among e-nodes is tracked by a **union-find** over e-class ids; e-nodes themselves have
no identity (egg notes that "making e-classes but not e-nodes identifiable is unique to our
definition"). A **hashcons** maps canonical e-nodes to their e-class id. The structure represents
the equivalence relation *and* its **congruence closure**: if `a ≡ b`, then `f(a) ≡ f(b)`.

egg states two invariants explicitly:

- **The Congruence Invariant** — the e-node equivalence relation is "closed over congruence," so
  congruent e-nodes must live in the same e-class. With canonicalization, congruence "amounts to
  deduplication of e-nodes."
- **The Hashcons Invariant** — the hashcons "must map all canonical e-nodes to their e-class ids,"
  enabling `lookup(n) = H[canonicalize(n)]` and a fast `add`.

**PEGs / E-PEGs (the original IR).** A **Program Expression Graph** is a triple `(N, L, C)`: nodes
`N`, a labeling `L` mapping each node to a semantic function, and `C` mapping each node to its
ordered children. PEGs are *referentially transparent* ("the value of an expression depends only on
the value of its constituent expressions") and *complete* ("there is no need to maintain any
additional representation such as a control flow graph"). An **E-PEG** is "a PEG with a set of
equalities E between nodes," partitioning nodes into equivalence classes — i.e. an e-graph
specialized to PEGs. The authors call E-PEGs "in essence specialized E-graphs for reasoning about
PEGs," lineage traced back to Nelson–Oppen congruence closure and the Simplify prover.

## How control & effects / memory are represented

This is the hardest part of using e-graphs *end to end*, and each system answers it differently.

**PEGs encode control and loops as pure functions.** The primitive functions are
`Prims = {φ, θ, eval, pass}`:

- `φ(cond, t, f)` — a *gated* select (like gated-SSA's γ), choosing `t`/`f` per the boolean `cond`.
- `θ(base, loop)` — produces the *sequence of values a variable takes across loop iterations*; the
  left child is the first-iteration value, the right child the value at the current iteration in
  terms of the previous one. (Analogous to gated-SSA's μ.)
- `eval(loop, idx)` and `pass(cond)` — `pass` "returns the index of the first element in the
  sequence that is true"; `eval` selects that element. Together they recover loop trip counts and
  loop-exit values, "essentially lead[ing] to the traditional operational semantics for loops."

Values are **θ-lifted** (a ⊥ value is added per type to model nontermination/failure) and
**loop-lifted** (a node's value is a map from a per-loop *iteration index* `i ∈ L → ℕ` to values).
Well-formedness conditions require that "all cycles pass through the second child edge of a θ" —
i.e. *the only cycles are loop back-edges*. State (the heap, exceptions) is modeled with explicit
**heap-summary (σ) nodes** threaded through side-effecting operations; Peggy uses a single coarse σ
summary object, and it does "not perform any optimizations across try/catch boundaries or
synchronization boundaries." Code *placement* is not represented in the PEG at all (following
Click's separation of placement from the IR); it is recovered during PEG→CFG conversion, with
equal-condition φ-nodes fused into branches and equal-`pass` θ-nodes fused into loops.

**egg / egglog leave effects to the user.** Core egg is purely functional and syntactic; binding,
state, and effects are handled by encodings (e.g. the lambda-calculus partial evaluator uses
*explicit substitution*) or by **e-class analyses** that carry semantic facts. There is no built-in
notion of memory order.

**Cranelift's aegraph keeps effects in a CFG skeleton.** Fallin's design splits the program: "keep
the CFG for the side-effectful instructions (call it the *side-effect skeleton*) and use a
sea-of-nodes for the rest." The e-node enum has four variants — `Param`, `Pure`, `Inst`
(side-effecting), and `Result` (a projection of one result of an `Inst`/`Pure`) — and "all impure
values [have] their own identity, distinct from any other such value." Pure operators float and are
placed on demand; impure ops stay pinned in original
order in the skeleton. This deliberately "prohibits control-flow rewrites," and load/store
forwarding and alias analysis are done by *separate* analyses rather than inside the rewrite engine.

## Optimization approach

The EqSat loop: build an initial e-graph from the input term; repeatedly **e-match** rule LHS
patterns against the e-graph and **merge** each match's RHS into the matched e-class; stop at
**saturation** (fixpoint) or a timeout; then extract.

- **Original Peggy** drives this with *equality analyses* = `(trigger, callback)` pairs; the engine
  "continuously monitors all the triggers" and uses an adaptation of the **Rete** algorithm
  (from rule-based expert systems) to find matches incrementally. Saturation is formalized: each
  analysis is *monotonic*, giving a **non-interference theorem** ("applying an equality analysis `a`
  before `b` cannot make `b` less effective"), so the result is order-independent when saturation
  terminates ("canonizing"). When it doesn't, a bound on processed expressions plus breadth-first
  processing keeps the search from "running astray."

- **egg's key algorithmic move is splitting each iteration into a read phase and a write phase.**
  Traditional EqSat "alternates between searching and applying rules, and the e-graph maintains its
  invariants throughout." egg instead does a "read-only phase" (collect *all* matches against a
  consistent e-graph) then a "write-only phase" (add/merge, "temporarily break invariants"), and
  restores invariants once per iteration via `rebuild()`. This makes rewrites "truly unordered"
  (invariant to rule-list order even without saturation) and is what makes deferred rebuilding
  *sound*.

- **egglog reframes the whole loop as Datalog.** A rule is `head :- body` (query → actions); the
  **immediate consequence operator** applies all rules, and a **rebuilding operator**
  re-establishes functional dependencies and congruence to fixpoint. EqSat falls out: `rewrite p1
  p2` desugars to `(rule ((= v p1)) ((union v p2)))`. Crucially egglog adds **semi-naïve
  evaluation** from databases, making e-matching *incremental* (only matching against newly derived
  facts each iteration) — something previously found only in SMT solvers like Z3, not EqSat tools.

### Rebuilding (egg's core contribution)

Traditionally invariant restoration is "part of the `merge` operation itself," via **upward
merging** over per-e-class **parent lists**. egg separates it: `merge` only unions the e-classes and
"adds the new e-class to the **worklist**"; a later `rebuild()` "essentially calls `repair` on the
e-classes from the worklist until the worklist is empty." `rebuild` first moves the worklist to a
local and **deduplicates e-classes** before calling `repair` (which canonicalizes parent e-nodes in
the hashcons and performs "one 'layer' of upward merging"). Deduplication coalesces overlapping
"paths of congruence," eliminating redundant work. Documented asymptotic effects: a width-`w`,
depth-`d` merge workload drops from `O(d·w)` `repair` calls to `O(d)`; an `O(n²)` hashcons-update
workload drops to `O(n)`. Empirically (egg's 32-test suite), deferring to once-per-iteration gives
an aggregate **88× speedup on congruence closure and 21× over all of EqSat**, with the multiplier
*growing* with problem size (an asymptotic, not constant, speedup).

### E-class analyses (egg's extensibility contribution)

An e-class analysis attaches a value from a semilattice domain `D` to each e-class via three
operations:

- `make(n) → D` — build data for a new singleton e-class from the analysis data of `n`'s children.
- `join(d1, d2) → D` — combine data when two e-classes merge (must form a join-semilattice).
- `modify(eclass)` — optionally mutate the e-class (e.g. add a constant e-node); must be idempotent.

The **analysis invariant** ties data to terms: `d_c = ⊔ make(n)` over e-nodes `n ∈ c`, and
`modify(c) = c`. Rebuilding is extended to restore this invariant alongside congruence. This is how
constant folding, interval/sign analysis, free-variable sets, tensor shapes, etc. ride along with
syntactic rewrites — replacing the "ad hoc passes over the e-graph" earlier systems needed. egglog
generalizes this further: any function's `:merge` expression can be a lattice join (e.g.
`(min old new)`), and analyses become ordinary rules, removing egg's limits ("a single e-class
analysis… only propagate[s] information upwards… requires writing low-level [Rust] code").

## Code generation / lowering (extraction)

**The extraction problem** is selecting one e-node per chosen e-class to form a concrete term of
minimal cost. Approaches, in increasing cost/accuracy:

- **Greedy bottom-up / local cost.** When the cost of `f(c1,…)` depends only on `f` and children's
  costs (a *local* cost function), "a bottom-up, greedy traversal of the e-graph suffices." egg
  implements this as an e-class analysis whose data is `(cheapest enode, cost)`; common cost is
  **AST size**, optionally op-weighted. This is polynomial but uses a **tree cost model** that
  *double-counts shared subexpressions*, so it can be suboptimal.

- **DAG-aware extraction** counts each shared subexpression once. This is the *right* model for real
  compilers (sharing matters) but **DAG extraction is NP-hard**; recent work formulates it as ILP,
  via treewidth-bounded algorithms ("E-Graphs as Circuits"), differentiable relaxations (SmoothE),
  or curated benchmarks (extraction-gym). ILP gives optimal results but "scales poorly with e-graph
  size," and ILP-based *acyclic* extraction "offloads a topological-sorting problem to the solver"
  that struggles when the e-graph has many cycles (loops).

- **Pseudo-Boolean / ILP (original Peggy).** Peggy used the **Pueblo** pseudo-Boolean (0/1 ILP)
  solver: 0/1 variables per node, constraints that "if a node is selected, we must select its
  children's equivalence classes" and that the chosen PEG is well-formed, minimizing `Σ Bn·Cn` with
  `Cn = basic_cost(n)·k^{depth(n)}` (`k=20`, depth = loop-nesting depth). This was the dominant cost:
  ~1.5 s/method total, of which **Pueblo alone averaged 1,499 ms** (1% of methods hit a 1-minute
  timeout). PEG→CFG conversion then handles placement (GVN/CSE/PRE/code motion fall out "for free"
  just from the round trip).

- **Cranelift aegraph: extraction *is* elaboration.** Because the graph is acyclic and rooted in a
  CFG skeleton, "scoped elaboration" walks the dominator tree, memoizing computed values in a
  scoped hashmap and emitting pure nodes on demand "as low as possible." It uses a greedy DP that
  "ignore[s] shared substructure, pretending that each use of a subtree counts that subtree's cost
  anew" — accepting suboptimality for speed. This works because measured average e-class size was
  only **1.13 nodes** (most values have no alternative), so extraction is cheap in practice.
  ISLE-generated lowering then turns chosen IR terms into machine instructions.

## Compile-time evaluation / partial evaluation

EqSat is *itself* a form of partial evaluation. Interpreted/computational rewrites and analyses fold
constants and specialize:

- egglog's example `(rewrite (Add (Num a) (Num b)) (Num (+ a b)))` folds integer arithmetic during
  saturation; its lambda-calculus and Hindley–Milner pearls do type inference and evaluation by
  saturation. egg ships a lambda-calculus **partial evaluator** with explicit substitution as a
  worked example, and constant folding is the canonical e-class analysis (`modify` adds the folded
  constant e-node).
- Peggy's `Constant Folding` equality analysis (e.g. `0*5 = 0`, `3*5 = 15`) cooperates with
  algebraic axioms; *Inlining* and *tail-recursion elimination* are equality analyses, so
  inter-procedural specialization is expressible (Peggy "simultaneously represents the program where
  inlining is performed and where it is not," and the global heuristic decides).
- **Ruler** (OOPSLA 2021) inverts the relationship: it *learns* rewrite rules by enumerating terms in
  an e-graph and using EqSat to find candidate equalities (validated by SMT/fuzzing), producing
  rulesets "5.8× smaller, 25× faster" than a CVC4-based baseline — compile-time evaluation used as
  rule synthesis.

Open hard part: **cyclic terms (loops).** Cranelift's "fuel"/bounded-effort discussion and Peggy's
bound-of-500 plus breadth-first traversal both exist because saturation over loops/recursion need
not terminate; PEGs make loops first-class via θ/eval/pass, but extraction over the resulting cycles
is exactly where DAG-/acyclic-ILP extraction breaks down.

## Strengths

- **Phase-ordering is dissolved**, with a formal guarantee (Peggy's non-interference theorem): no
  rewrite can irrevocably disable another, because nothing is destroyed.
- **Global, post-hoc profitability**: choose among *fully optimized* programs, enabling e.g. "do an
  expensive transform now because it unlocks a later win" (Peggy's inter-loop strength reduction;
  inlining decided after seeing downstream effects).
- **Compact representation of exponentially many programs**: Peggy represented `>2^{103}` program
  versions in 200 MB; one E-PEG with 7 equalities encodes 128 programs.
- **Declarative & small**: egg is "~5000 lines of Rust including code, tests, and documentation,"
  generic over language/analysis/cost; the Herbie egg backend is "under 500 lines of Rust" yet
  ">3000× faster than Herbie's initial simplifier."
- **Reuse across domains**: same engine for optimization, **translation validation** (Peggy validated
  98% of Soot runs and *found a real Soot bug* that turned a terminating loop infinite), tensor graph
  superopt (TenSat), DSP **vectorization** (Diospyros, ASPLOS 2021; Isaria), float accuracy (Herbie),
  and rule synthesis (Ruler).
- **egglog adds incrementality and rich analyses**: semi-naïve e-matching, lattice merges, multiple
  user sorts/functions; a reimplemented points-to analysis ran "4.96× faster" than *patched* — "the
  fastest sound encoding in Soufflé available" (also 1.94× over cclyzer++, 1.59× over the
  non-incremental egglog) — and a sound Herbie ruleset was faster *and* removed unsound rewrites.
- **Relational e-matching** gives **worst-case-optimal** e-matching with the first data-complexity
  bound, "orders of magnitude faster in practice" than top-down backtracking.

## Weaknesses & criticisms

- **E-graph blowup / non-termination.** Saturation often doesn't complete: Peggy fully saturated only
  84% of methods; the rest were bounded. Distributivity/associativity rules explode the e-graph.
  Mitigations are heuristic: triggers, rule scheduling (egg's `Runner`/back-off scheduler), bounds,
  BFS ordering, "fuel."
- **Extraction is the real bottleneck.** Local/greedy extraction double-counts sharing; optimal
  DAG-aware extraction is **NP-hard**; ILP doesn't scale and degrades with cycles. (egg's own
  discussion #268 names extraction as the bottleneck of EqSat.)
- **Effects, ordering, and control flow are not handled natively.** Pure-functional e-graphs cannot
  express memory order or control-flow rewrites without extra machinery — PEGs need σ heap summaries
  (Peggy's coarse single-σ model limited its translation validation), and Cranelift had to bolt on a
  CFG skeleton and forbid control-flow rewrites.
- **Production retreat from full EqSat.** Cranelift explicitly *declined* general equality saturation:
  it needs "some kind of meta/strategy driver layer that uses 'fuel' to bound effort… not [suitable]
  for a fast compiler like Cranelift." It applies rewrites once at node-creation ("smart
  constructors," Cascades-style), keeps the graph acyclic, and uses cheap greedy elaboration. This is
  the clearest documented "we constrained it because the general version is too expensive" data point.
- **Cost models are weak.** Per-node static costs ignore scheduling, register pressure, and ISA
  effects (Peggy itself notes Denali's per-sequence cost model is "more precise").
- **Relational e-matching's "dual representation" problem**: switching between e-graph and relational
  DB representations "can take a significant amount of run time" — a motivation for egglog's
  DB-first design.

## Codebase / complexity notes

- **egg**: "~5000 lines of Rust" (incl. tests/docs), generic library on crates.io. Herbie backend
  <500 LOC; 88×/21× rebuilding speedups measured on a 32-test suite.
- **Peggy**: Java bytecode optimizer; per-method ~1.5 s (13.9 ms CFG→PEG, 87.4 ms saturation,
  1,499 ms Pueblo, 52.8 ms PEG→CFG), ~6× slower than Soot; ran SpecJVM (2,461 methods) in 200 MB.
- **egglog**: a language + library; relational engine with semi-naïve eval; points-to 4.96× over the
  fastest sound Soufflé encoding (*patched*). **Relational e-matching** prototyped inside egg.
- **Cranelift aegraph**: e-node enum with 4 variants (`Param`/`Pure`/`Inst`/`Result`); replaces GVN+LICM+simple_preopt+alias/RLE
  passes; prototype showed SpiderMonkey compile +1% / runtime −13%, bz2 compile −15% / runtime −3%.
- **ISLE**: a "statically typed term-rewriting DSL," compiles rules to a Rust decision tree/matching
  automaton via **external extractors** (pattern side) and **external constructors** (build side);
  enforces "no two overlapping rules can have the same priority"; reused unchanged for both backend
  lowering *and* the aegraph mid-end (a "new prelude" of extractors/constructors). **VeriISLE** lowers
  each rule to SMT to check functional equivalence — "the first formal verification effort for the
  instruction-lowering phase of an efficiency-focused production compiler."

## Lessons for a NEW unified IR

1. **If you want EqSat's order-independence, you need referential transparency for the rewritten
   part — and an explicit place to put effects.** PEGs (pure + σ summaries) and Cranelift (pure
   sea-of-nodes + CFG skeleton) independently converged on "pure values float, effects are pinned in
   order." A unified IR should make the pure/effectful split first-class rather than retrofitted.
2. **Loops need a principled encoding, but it makes extraction cyclic.** θ/eval/pass elegantly turn
   loops into pure functions, but the resulting cycles are exactly what defeats optimal extraction.
   Either constrain to acyclic (Cranelift) or budget for NP-hard extraction.
3. **Separate read and write phases** (egg) to get order-independence *and* to make deferred
   invariant maintenance sound — this is cheap to adopt and gives the 21× win.
4. **Defer congruence maintenance with a deduplicated worklist** (rebuilding). It's a localized
   change with asymptotic payoff; do not restore invariants after every merge.
5. **Model analyses as semilattice e-class analyses / lattice `:merge`** so semantic facts
   (constants, ranges, shapes, types) cooperate with rewrites instead of living in ad hoc passes.
6. **Treat e-matching as a relational/WCOJ query** and run it **semi-naïvely** (egglog) for
   incremental, scalable matching — avoid top-down backtracking.
7. **Extraction is the hard, load-bearing decision.** Decide up front whether you need DAG-aware
   (NP-hard) extraction or can accept greedy tree-cost; in a fast compiler, exploit the empirical
   fact that most e-classes are tiny (Cranelift's 1.13 avg) and elaborate greedily.
8. **A typed term-rewriting DSL (ISLE) pays off twice**: the same rule language can drive mid-end IR
   rewrites and backend lowering, and typed rules are SMT-verifiable. Bake rule overlap/priority
   checking in.
9. **Bound the search.** Provide fuel/rule-scheduling/iteration limits as a first-class mechanism;
   real workloads rarely saturate.
10. **EqSat is dual-use.** The same engine yields optimization, translation validation, and rule
    synthesis (Ruler) — design the IR so equivalence proofs are extractable, not just final terms.

## Sources

- [Tate, Stepp, Tatlock, Lerner — *Equality Saturation: a New Approach to Optimization* (POPL 2009 PDF)](https://homes.cs.washington.edu/~ztatlock/pubs/eqsat-tate-popl09.pdf) — PEG/E-PEG IR, θ/eval/pass, Pueblo extraction, Peggy results; primary source, read in full.
- [LMCS 2011 journal version](https://goto.ucsd.edu/~mstepp/peggy/pubs/lmcs2010.pdf) — extended journal version of the same work.
- [Peggy project page](https://goto.ucsd.edu/~mstepp/peggy/) — the equality-saturation system implementing the POPL'09 approach.
- [Willsey et al. — *egg: Fast and Extensible Equality Saturation* (arXiv 2004.03082, POPL 2021)](https://arxiv.org/abs/2004.03082) — congruence/hashcons invariants, rebuilding, e-class analyses, extraction, ~5000 LOC; read in full via the PDF.
- [egg SIGPLAN blog post](https://blog.sigplan.org/2021/04/06/equality-saturation-with-egg/) — accessible author summary of egg.
- [Zhang et al. — *Better Together: Unifying Datalog and Equality Saturation* (egglog, arXiv 2304.04332, PLDI 2023)](https://arxiv.org/pdf/2304.04332) — functions-as-tables, `:merge`, semi-naïve eval, rebuilding operator; read in full.
- [Zhang et al. — *Relational E-matching* (arXiv 2108.02290, POPL 2022)](https://arxiv.org/pdf/2108.02290) — e-matching as a worst-case-optimal relational join; first data-complexity bound.
- [Fallin — *The acyclic e-graph: Cranelift's mid-end optimizer* (2026 blog)](https://cfallin.org/blog/2026/04/09/aegraph/) — side-effect skeleton, union nodes, scoped elaboration, why not full EqSat, 1.13 avg e-class size.
- [Cranelift egraph RFC (bytecodealliance/rfcs)](https://github.com/bytecodealliance/rfcs/blob/main/accepted/cranelift-egraph.md) — design rationale; e-node enum has **four** variants `Param`/`Pure`/`Inst`/`Result` (verified); "apply once at node creation" likened to the Cascades query optimizer; fuel/blowup discussion; prototype perf SpiderMonkey +1% compile / −13% runtime, bz2 −15% compile / −3% runtime; replaces GVN/LICM/simple_preopt/alias analysis (all verified against RFC text).
- [Fallin — *Cranelift's Instruction Selector DSL, ISLE* (2023 blog)](https://cfallin.org/blog/2023/01/20/cranelift-isle/) — ISLE as typed term rewriting, extractors/constructors, decision-tree compilation, VeriISLE.
- [Nandi et al. — *Rewrite Rule Inference Using Equality Saturation* (Ruler, arXiv 2108.10436, OOPSLA 2021)](https://arxiv.org/pdf/2108.10436) — synthesizing rewrite rules with EqSat.
- [Vasilache/VanHattum et al. — *Vectorization for Digital Signal Processors via Equality Saturation* (Diospyros, ASPLOS 2021 PDF)](https://jamesbornholt.com/papers/diospyros-asplos21.pdf) — EqSat for SLP-style vectorization.
- [*E-Graphs as Circuits, and Optimal Extraction via Treewidth* (arXiv 2408.17042)](https://arxiv.org/pdf/2408.17042) — extraction complexity / treewidth-parameterized optimal extraction.
- [*e-boost: Boosted E-Graph Extraction* (arXiv 2508.13020)](https://arxiv.org/html/2508.13020) — confirms DAG extraction is NP-hard; ILP-vs-heuristic trade-offs and extraction-gym.

### Adversarial fact-check (2026-06)

Primary-source verification of the highest-risk numeric/attribution claims; two were corrected, the rest confirmed.

- **Corrected — Cranelift e-node variant count.** Text previously said the aegraph e-node type has "exactly three variants — `Param`, `Pure`, `Inst`." The Cranelift egraph RFC defines **four**: `Param`, `Pure`, `Inst`, and `Result` (a projection of one result of an `Inst`/`Pure`). Fixed in both the effects section and the codebase notes.
- **Corrected — egglog points-to baseline.** "4.96× faster than Soufflé" was imprecise. The egglog paper (PLDI 2023) states 4.96× over *patched*, "the fastest sound encoding in Soufflé available" (also 1.94× over cclyzer++, 1.59× over non-incremental egglog). Phrasing tightened.
- Confirmed verbatim from the egg paper (arXiv 2004.03082): "aggregate 88× speedup on congruence closure and 21× speedup over the entire equality saturation algorithm"; asymptotic (grows with problem size); the **32-test** suite ("Of the 32 total tests, 8 hit the iteration limit of 100"); egg "~5000 lines of Rust… including code, tests, and documentation"; Herbie backend "under 500 lines of Rust" and "over 3000× faster than Herbie's initial simplifier."
- Confirmed from Tate et al. (POPL 2009 / LMCS 2011): Pueblo avg **1,499 ms**, total "slightly over 1.5 seconds," "6 times slower than Soot," CFG→PEG 13.9 / Saturation 87.4 / PEG→CFG 52.8 ms; **84%** complete saturation; bound of **500**; **200 MB** heap; SpecJVM = **2,461** methods; **1%** Pueblo timeout; ">2^103 versions"; "E-PEG… encodes 128 ways… because it encodes 7 independent equalities"; cost `Cn = basic_cost(n)·k^depth(n)`, **k=20**, depth = loop nesting depth; single-σ heap summary; "Peggy does not perform any optimizations across try/catch boundaries or synchronization boundaries" (verbatim, POPL'09); translation validation 98% of Soot runs and exposed a Soot bug (terminating loop made infinite); Denali's per-sequence cost model "more precise."
- Confirmed from Fallin's aegraph blog: average e-class size **1.13** e-nodes; side-effect skeleton; eager single-pass rewrites instead of equality saturation; the "meta/strategy driver layer that uses 'fuel'… not [suitable] for a fast compiler" rationale; "prohibits control-flow rewrites."
- Confirmed: Ruler (OOPSLA 2021) "5.8× smaller rulesets 25× faster" vs the CVC4-based baseline; *Relational E-matching* is POPL 2022 (Zhang, Wang, Willsey, Tatlock), worst-case-optimal via relational join, and "establishes the first data complexity result for e-matching."

## Open questions / uncertainties

- **Exact recent egg/egglog LOC.** egg is "~5000 lines" *as of the POPL'21 paper*; current
  egg/egglog repository sizes were not measured here.
- **egglog vs egg performance specifics.** I confirmed the 4.96× points-to speedup and the
  "egglogNI grows the same e-graph with less time than egg" math-benchmark claim from the paper text,
  but the heavily formula-mangled PDF region prevented me from extracting precise per-benchmark
  numbers; treat the math-benchmark figures as approximate.
- **Cranelift aegraph blog date.** The fetched post is dated 2026-04-09; the EGRAPHS-2023 talk and
  the RFC are the contemporaneous primary records. The SpiderMonkey +1%/−13% and bz2 −15%/−3% numbers
  are the RFC's prototype figures (verified against RFC text); whether the 2026 post reports updated
  numbers was not separately checked.
- **Diospyros authorship.** Search summaries referenced co-authors loosely; I cite the canonical
  ASPLOS 2021 paper PDF rather than asserting a specific author list.
- **Relational e-matching speedup magnitude.** The abstract says "orders of magnitude" without a
  single headline multiplier; specific factors vary by pattern/benchmark and were not pinned down.
- **VeriISLE coverage.** It verifies individual lowering rules via SMT; the extent of rule coverage
  and whether the mid-end (aegraph) rules are equally covered was not verified here.
