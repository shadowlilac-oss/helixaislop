# SSA / LLVM / MLIR / Cranelift / PDG / VSDG Landscape

> Research brief for positioning a new unified IR. Every non-obvious claim below
> is traced to a primary source fetched during research (papers/PDFs, official
> docs, project repos, author blogs). Items I could not verify are isolated under
> "Open questions / uncertainties".

## Overview

The dominant production IR design is **SSA over an explicit control-flow graph (CFG)**:
LLVM IR, GCC GIMPLE/RTL, and most JIT tiers. The competing family is the
**dataflow / region graph**: sea-of-nodes (Click → HotSpot C2, Graal, V8 TurboFan),
the Program Dependence Graph (PDG), the Value (State) Dependence Graph
(VDG/VSDG → RVSDG), and MLIR's nested-region model. The defining tension between
the two families is *where ordering lives*: a CFG over-specifies a total order on
operations; a pure dataflow graph under-specifies it and must *re-derive* a legal
schedule before emitting machine code.

The landscape in 2024-2026 shows a notable **partial retreat from radical
sea-of-nodes**: V8 publicly abandoned sea-of-nodes for the block-based
*Turboshaft* IR (v8.dev, 2025), while Cranelift adopted a deliberately *hybrid*
design (CFG skeleton + floating pure nodes + acyclic e-graph) rather than going
fully graph-based. Meanwhile MLIR generalized the CFG-of-blocks into
**nested regions of operations** and won broad adoption for multi-level lowering.
The recurring lessons are about effect ordering, scheduling/sequentialization,
CFG round-trips, and debuggability.

## Core Model (nodes / edges / regions)

### Classic SSA + CFG (LLVM IR)
- A function is a CFG of **basic blocks**; each block is a totally-ordered list of
  instructions ending in a terminator. Values obey **Static Single Assignment**:
  each value has exactly one definition.
- Merges use **φ (phi) nodes**: "a basic block starts with a (possibly empty)
  sequence of φ nodes that assign different values to variables depending on the
  predecessor executed at run time." φ nodes must be first in the block, grouped,
  with exactly one entry per predecessor (LLVM docs / Aalto course notes).
- SSA construction is **Cytron et al. (1991)**: place φ at the **iterated
  dominance frontier** ("the set of all CFG nodes Y such that X dominates a
  predecessor of Y but does not strictly dominate Y"), producing minimal φ
  placement; this is what made SSA practical and is still the basis of LLVM's
  `mem2reg` (which promotes `alloca`s with only load/store uses by placing φ at
  dominance frontiers, then rewriting in depth-first order). Sources: Cytron history
  (Grokipedia/handwiki), mem2reg writeups.

### Sea-of-nodes (Click 1995 — primary source read in full)
From Cliff Click & Michael Paleczny, *A Simple Graph-Based Intermediate
Representation* (IR'95):
- **"a directed graph with labeled vertices and ordered inputs but unordered
  outputs."** Vertices = **nodes** labeled with opcodes; edges are unlabeled and
  **represent use-def chains** ("Moving from the use of a value to the definition
  of a value requires a single pointer dereference").
- It is a **"single-tiered structure instead of a two-tiered CFG"**: "Control and
  data dependencies have the same form and implementation." Optimizations like
  constant propagation and value numbering "use the same support code for both
  control and data values."
- Special **values**: besides word/pointer/float, there are abstract values for
  **memory (state)**, **I/O (i/o)**, and **control**. This is the seed of the
  later "effect chain" idea.
- Basic blocks are replaced by a **REGION node** that "takes a control value from
  each predecessor block as inputs and produces a merged control as an output."
- **PHI nodes**: the control input "comes from the REGION node defining control
  for the PHI node"; the i-th data input to the PHI aligns with the i-th control
  input to its REGION. φ "have no run-time operation"; at codegen they are
  "folded back into normal basic blocks and CFG behavior."
- **IF nodes** consume control + predicate and emit labeled true/false control;
  multi-result ops emit a tuple stripped by **PROJECTION** nodes.
- Crucially, the **control input on a primitive node is optional**: "Removing it
  enables a number of global optimizations but requires a more complex
  serialization operation for output." This is the floating-node tradeoff in one
  sentence. Click's thesis supplies the **Global Code Motion (GCM)** + GVN
  scheduler that re-derives a CFG.

### MLIR (Lattner et al. 2020 — primary source read)
The IR has five hierarchical building blocks, "everything is an **Operation**":
- **Operation**: the universal unit; carries operands, results, **attributes**
  (compile-time constants), and **regions**.
- **Region**: "a list of blocks" held *inside* an operation — enables hierarchical
  nesting (function bodies, loop bodies, etc.).
- **Block**: a sequence of operations ending in a terminator, with successor edges
  and **block arguments**.
- **Value** (SSA) and **Type**; everything is owned by exactly one **dialect**.
- MLIR **abandons φ nodes entirely**: terminators pass values into the successor
  block's **block arguments** ("a functional form of SSA"). This is the same idea
  Cranelift uses.

### PDG / VDG / VSDG / RVSDG (the dataflow lineage)
- **PDG** (Ferrante, Ottenstein, Warren, TOPLAS 1987): "makes explicit both the
  data and control dependences for each operation." Control dependences are
  *derived from the CFG*; a single walk of dependence edges drives many
  optimizations and unifies vectorization's treatment of control & data.
  Cost (per RVSDG paper): many edge *types* (five in Ferrante, four in Horwitz),
  no register/memory distinction → aliasing/side-effect problems that "can even
  preclude its construction altogether."
- **VDG** (Weise et al.): "abandons the explicit representation of control flow and
  only models the flow of values using ports … implicitly in SSA." Fatal flaw
  quoted in the RVSDG paper: **"evaluation of the VDG may terminate even if the
  original program would not."**
- **VSDG** (Johnson & Mycroft): adds **state edges** to the VDG "to model the
  sequential execution of stateful computations," fixing the termination problem,
  and adds explicit **loop nodes**. Still flat, implicitly SSA, selection is
  2-way only.
- **RVSDG** (Reissmann, Bahmann, et al., TOECS 2020 — primary source read in full):
  a **demand-dependence** graph organized into nested **regions**. Structural
  nodes (exact terminology):
  - **gamma-node (γ)**: "conditionals with symmetric split and joins" (if-then-else
    / switch); has *multiple* regions, one per branch.
  - **theta-node (θ)**: "tail-controlled loops, i.e., do-while loops"; one region.
  - **lambda-node (λ)**: a function; one region = the body.
  - **delta-node (δ)**: a global variable; one region = its initializer.
  - **phi-node (φ)**: "mutually recursive environments" — wraps λ-nodes so
    recursion is expressed **without cycles**, preserving acyclicity.
  - **omega-node (ω)**: the whole translation unit; the top-level node.
  - Edges are **value-typed** or **state-typed**; an **apply-node** is a call.

## How control & effects / memory are represented

This is the axis on which the whole landscape divides.

- **LLVM/CFG**: control is explicit (the CFG); effect order is *implicit in the
  block instruction order*. Memory is a single global mutable thing; independence
  of two memory ops must be *(re)discovered* by alias analysis, repeatedly. The
  RVSDG authors note LLVM is "inherently sequential … incapable of explicitly
  encoding independent operations," and that 6 of LLVM's 13 most-invoked passes
  exist only to (re)discover SSA/loop invariants — "21% of all invocations."

- **Sea-of-nodes**: effects ride a **memory/I/O value threaded through nodes** (an
  *effect chain*) parallel to the control chain. Pure nodes float; effectful nodes
  are pinned by the chain.

- **MLIR**: control is structured (regions/blocks + block arguments). SSA
  visibility is by **dominance + region nesting**: "if the operand to an Op is
  outside the current region, then it must be defined lexically above and outside
  the region of the use." The **`IsolatedFromAbove`** trait asserts a region
  "will not capture or reference SSA values defined above" — which lets a module
  be processed *in parallel* because no use-def chains cross the isolation barrier.
  Effects are modeled by traits/interfaces (e.g. `MemoryEffects`) rather than a
  single canonical mechanism.

- **VSDG/RVSDG — state edges**: side effects are ordered by **state edges**
  threaded between effectful nodes. RVSDG quote: state edges "impose an ordering
  on operations with side-effects … to preserve the observational semantics of the
  input program," covering "memory read and writes, as well as exceptions."
  Construction adds **"an additional state argument and result to every λ-node …
  a single state ensures that the order of operations with side-effects in the
  RVSDG is according to the total order specified in the original program."**
  The big payoff: you can use **multiple independent states** — two non-aliasing
  stores or two independent loops become provably parallel *in the representation*
  (jlm uses two states: one for memory, one for non-terminating loops). Alias
  analysis can *write its results back into the graph* as extra states, and pure
  functions are recognizable because they only thread state through untouched.

### The VSDG "what order do effects run" problem (the cautionary tale)
From Alan Lawrence's PhD thesis *Optimizing compilation with the Value State
Dependence Graph* (Cambridge UCAM-CL-TR-705, 2007 — primary source read in full):
- The hard part is the **rear end**: "the VSDG must first be sequentialized into a
  CFG, and this has not previously been seen as straightforward." Lawrence cites
  Upton that **"the sequentialization problem is the major obstacle preventing the
  widespread usage of the VSDG in real compilers."**
- Why it's hard: a VSDG fixes data dependence but **does not fix an evaluation
  strategy**. Lawrence's key framing: an SSA-CFG "corresponds to a functional
  program in a strict language," whereas **"a VSDG [corresponds] to its equivalent
  functional program in a lazy language"** (call-by-need). The compiler may pick a
  less-lazy strategy for speed *only if it preserves termination semantics*, and
  must "avoid infinite speculation."
- The two extreme "solutions" are both bad: **"duplicate everything" → exponential
  growth**, or **"compute early" (eager/speculative)** which wastes work and can
  introduce non-termination. The RVSDG paper independently confirms: in the VSDG
  "operations with side-effects are no longer guarded by predicates … for graphs
  with stateful computations, **lazy evaluation is the only safe strategy** … and
  requires a detour over the PDG to arrive at a unique CFG."
- Lawrence's fix: break sequentialization into **proceduralization** (functional →
  imperative, VSDG → PDG, choosing an evaluation strategy and synthesizing the
  missing **control dependence subgraph**) → **PDG sequentialization** to a
  *duplication-free PDG (df-PDG)* → CFG. He even names his intermediate the
  **"Regionalized VSDG"** — a direct ancestor of the Reissmann RVSDG.
- RVSDG's answer to all this is **structured regions** (γ guards each branch, θ is
  a canonical tail-loop) plus **Control Flow Restructuring (CFR)**, so effects are
  always *inside* the region that guards them and a unique legal schedule exists.

## Optimization approach

- **LLVM**: a long pipeline of hand-written passes over SSA-CFG; ordering matters
  and passes are re-run (GVN, LICM, instcombine, alias analysis). Alive found
  **8 bugs in 334 hand-coded InstCombine transforms (~2.4%)** — cited by Cranelift
  as motivation to move to declarative rewrites.
- **Sea-of-nodes**: GVN/CSE and constant folding are nearly free because nodes are
  hash-consed and float; LICM falls out of scheduling. But see V8's retreat below.
- **MLIR**: pattern-based rewriting (DRR / PDL), dialect conversion, and
  **progressive lowering** — high- and low-level dialect ops coexist in one module
  and are rewritten stepwise toward the target.
- **Cranelift mid-end** (cfallin RFC + 2026 aegraph blog — primary sources read):
  an **acyclic e-graph ("aegraph")**. Motivation: phase-ordering ("redundant
  re-application of a pass is bounded only by the length of the code"),
  verifiability, and contributor friction. Mechanism:
  - Redefine existing CLIF values as an implicit e-graph: **"An SSA value is either
    an ordinary operator result or a block parameter (as before), or a *union* of
    two other SSA values."** Union nodes form binary trees encoding equivalent
    forms; "pay-as-you-go" — avg e-class is **1.13 e-nodes**.
  - **Rewrites apply at node-creation time** ("we apply all rewrites immediately,
    then join those rewrites with the original form with union nodes"), preserving
    acyclicity and avoiding egg-style rebuild/parent-pointers — likened to database
    "cascades."
  - **ISLE** drives the rewrites; `def_inst` becomes a *multi-extractor* yielding
    every e-node in a class.
  - **Honest result**: aegraph multi-representation gave **~0.1% execution-time
    improvement for ~0.005% compile-time cost** — the real wins were ordinary GVN,
    LICM, and DCE that the sea-of-nodes+CFG structure enables. A sober data point
    that radical e-graphs are *not* a free lunch.
- **RVSDG**: many optimizations are *implicit in normalization*. "Programs
  differing only in the ordering of (independent) operations result in the same
  RVSDG." LICM, common-subexpr (their **Common Node Elimination**), and dead-code
  (**Dead Node Elimination**) run in a single regular pass with no helper passes
  or loop rediscovery.

## Code generation / lowering

- **LLVM**: SSA → SelectionDAG / GlobalISel → MIR (still SSA over MachineBBs) →
  **register allocation out of SSA** (φ elimination, then graph-coloring/linear
  scan). Register allocation happens *after* SSA optimizations because MIR virtual
  values must be mapped to physical registers.
- **Sea-of-nodes**: must **schedule** floating nodes back into blocks via **Global
  Code Motion** (place each node in the latest/earliest legal block, sink into
  loops' pre-headers, etc.) before instruction selection.
- **Cranelift**: **scoped elaboration** walks the dominator tree and "generates
  nodes 'as low as possible'", reusing values within their dominance scope —
  naturally giving GVN consolidation + LICM hoisting + rematerialization. The
  **CFG skeleton** (side-effecting ops kept in original block order) is *never
  rewritten*; only pure dataflow floats over it. Backend = **ISLE** instruction
  selection ("A New Backend for Cranelift", Mozilla Hacks / cfallin 2020).
- **RVSDG destruction** (Bahmann's "perfect reconstructability"): two recoveries —
  **SCFR** (Structured Control Flow Recovery; only if/switch/do-while shapes) and
  **PCFR** (Predicative CFR; recovers *arbitrary* control flow, fewer static
  branches, but can produce GPU-unfriendly flow). **CFR avoids exponential code
  blowup** by "inserting additional predicates and branches instead of cloning
  nodes" (contrast Lawrence's node-splitting).
- **Swift SIL**: a high-level SSA IR lowered from the AST; **Guaranteed/Mandatory
  passes** (in `lib/SILOptimizer/Mandatory`) run even at `-Onone` to produce
  *canonical SIL* and emit dataflow diagnostics (uninitialized use). **Ownership
  SSA (OSSA)** "imbues value-operand edges with semantic ownership information":
  every value has an ownership kind; `guaranteed` values have scoped lifetimes
  ended by `end_borrow`. (Swift docs / gottesmm).

## Compile-time evaluation / partial evaluation

- **GHC Core / STG** (the comptime-ish functional lineage): **Core** is a tiny
  typed lambda calculus (System FC) — "variables, literals, let, case, lambda,
  application"; "let means allocation and case means evaluation." GHC's whole
  optimizer is **term rewriting on Core** (inlining, the worker/wrapper transform,
  rewrite RULES, strictness analysis). **STG** lowers Core to closures/thunks for
  the spineless tagless G-machine. Relevant lesson: a *normalizing, referentially
  transparent* IR makes aggressive compile-time evaluation/specialization natural.
- **GRIN** (Graph Reduction Intermediate Notation): a **monadic** back-end IR for
  *lazy* functional languages with **whole-program optimization**. It makes
  laziness explicit via `store`/`fetch`/`update` heap ops, then runs interprocedural
  analysis to *defunctionalize* (eliminate unknown/indirect calls) and **unbox**
  heap values to registers, finally handing simplified code to LLVM. Lesson:
  representing evaluation (graph reduction) *in the IR* enables optimizations a
  CFG cannot see; the recurring cost is precise whole-program pointer/heap analysis.
- **MLIR**: compile-time data lives in **attributes**; folding/canonicalization
  hooks do constant folding per op. Partial evaluation is dialect-specific.
- **RVSDG**: because it can hold ops "at vastly different abstraction levels" and
  preserve source semantics (e.g. two `std::vector`s as independent states), it
  can in principle evaluate/specialize across levels — but this is future work, not
  demonstrated at scale.

## Strengths

- **SSA-CFG**: simple, debuggable, total order is obvious; mature register
  allocation and tooling; matches hardware's sequential model. This is *why SSA
  won* — Cytron made it cheap, and it composes with everything.
- **Sea-of-nodes**: free GVN/CSE, implicit code motion, dense representation; proven
  at massive scale in HotSpot C2 ("trillions of times daily").
- **MLIR regions**: one infrastructure spans many abstraction levels; nesting
  captures structured control naturally; `IsolatedFromAbove` enables parallel
  compilation; dialects let domains coexist and lower progressively.
- **Cranelift hybrid**: keeps CFG debuggability and correctness while getting most
  e-graph benefits; declarative ISLE rewrites reduce hand-written-pass bugs.
- **RVSDG**: strong normalization (independent-order programs collapse to one
  graph); explicit multiple states expose parallelism and let alias results be
  encoded *in* the IR; no loop/SSA rediscovery passes; whole program in one IR;
  linear space/time overhead measured in jlm.

## Weaknesses & criticisms

- **Sea-of-nodes — the documented retreat (V8, "Land ahoy", 2025)**: after ~10
  years V8 concluded "complexity outweighed its benefits" for JavaScript because
  **"almost all generic JS operations can have arbitrary side effects,"** so the
  effect chain ended up "mirroring the control chain" — only pure nodes float, but
  all the complexity remains. Concrete pains: scheduler had to duplicate
  instructions and could oscillate (optimize to 1 division → re-expand to 2);
  **~3× (up to 7×) more L1 dcache misses** costing up to 5% of compile time;
  load-elimination had to *bail out on large graphs* (CFG version up to **190×
  faster**); and the "messy soup of nodes" is hard to read/debug. Turboshaft
  returns to **basic blocks with explicit control-flow edges instead of implicit
  effect/control chains**.
- **VSDG**: the **sequentialization/proceduralization problem** (above) — flat
  graph, lazy semantics, exponential-duplication vs speculation dilemma; never
  reached production.
- **PDG**: too many edge types to maintain; no register/memory distinction; needs
  SSA/structure rediscovery; construction can be precluded by aliasing.
- **RVSDG**: requires **Control Flow Restructuring** for unstructured languages
  (C/C++ goto), an extra round-trip; conservative single-state modeling leaves
  parallelism unused; jlm is a *prototype* (no exceptions/intrinsics; code ~39%
  larger than `-Os` due to naive unrolling); destruction must choose SCFR vs PCFR
  with target-specific consequences. Empirically competitive but not beating
  LLVM/GCC.
- **MLIR**: region/nesting raises implementation overhead; "region-aware algorithms
  become more involved than flat IR traversal"; dominance must account for
  multi-region structure; the freedom of "everything is an op" can fragment into
  many bespoke dialects with weak shared semantics.

## Codebase / complexity notes

- **RVSDG / jlm**: prototype consuming & producing LLVM IR; paper reports
  **linear** node-count vs LLVM-instruction-count and linear translation time;
  two states (memory, non-termination). (repo: github.com/phate/jlm).
- **Cranelift aegraph**: deliberately *minimal* change — reuses existing CLIF value
  space (union nodes), "only a minimal change" to ISLE; net optimization win was
  tiny (~0.1%) which is itself a strong complexity-vs-payoff signal.
- **LLVM**: pass-count evidence — RVSDG paper's Table 1 shows 6/13 top passes are
  pure invariant rediscovery (~21% of invocations); Alive's 2.4% bug rate in
  InstCombine quantifies hand-written-pass risk.
- **V8 Turboshaft**: quantified wins — ~2× faster compile, 3-7× fewer L1 misses,
  up to 190× faster load elimination vs sea-of-nodes.
- **Sea-of-nodes reference size**: the "Simple" tutorial (SeaOfNodes org) is the
  canonical small implementation; HotSpot C2 / Graal are the large production ones.

## Lessons for a NEW unified IR

1. **Make effect ordering first-class and explicit, not implicit in block order or
   in a single hidden chain.** State/effect edges (RVSDG) let you express *that two
   effects are independent*; CFG/LLVM cannot, forcing endless alias re-analysis.
   But heed V8: a single monolithic effect chain that every op touches buys nothing
   and costs everything — support **multiple, typed, fine-grained states**.
2. **Keep the schedule cheap to recover.** The VSDG cautionary tale: a pure
   dataflow graph is a *lazy* program; round-tripping it to imperative code is the
   hard, under-appreciated part (proceduralization), with exponential-duplication
   vs speculation hazards. Either keep a CFG skeleton (Cranelift) or use
   **structured regions** (γ/θ, RVSDG) so a unique legal schedule always exists.
3. **A hybrid beats a purist.** Cranelift (CFG skeleton + floating pure nodes) and
   Turboshaft (blocks + explicit edges) both chose hybrids after the field learned
   that fully-floating graphs hurt debuggability, cache behavior, and effect
   reasoning. Pure nodes float; effectful nodes stay pinned and ordered.
4. **Normalization is an optimization multiplier.** RVSDG's "reorderings collapse
   to one graph", sea-of-nodes hash-consing, and Core's referential transparency
   all make GVN/LICM/CSE fall out for free. Design the IR so equal computations are
   *syntactically* equal.
5. **Prefer block arguments over φ nodes.** MLIR and Cranelift both ditched φ for
   block/region arguments — cleaner semantics, no "must be first in block" rules,
   easier rewriting.
6. **Declarative rewrites reduce bugs but are not magic.** ISLE/Alive evidence: 2.4%
   of hand-written transforms were buggy; declarative rules help. But Cranelift's
   ~0.1% e-graph payoff warns against over-investing in equality saturation.
7. **Multi-level / progressive lowering pays** (MLIR, GRIN): preserve high-level
   semantics (e.g. non-aliasing of distinct containers, ownership) and lower
   gradually rather than collapsing to a machine model early.
8. **Recurring failure modes to design against**: CFG↔graph round-trips
   (proceduralization cost, code blowup); effect-ordering ambiguity (VSDG
   termination bug); scheduler oscillation/instruction duplication (V8); cache
   thrash from in-place graph mutation (V8); and **debuggability** — engineers must
   be able to *read* the IR ("soup of nodes" was a real, cited reason V8 left).
9. **Acyclicity is worth preserving**: RVSDG uses φ-nodes (mutual recursion) and
   Cranelift uses union nodes / eager rewrite to stay acyclic — acyclic graphs are
   far easier to traverse, schedule, and reason about.

## Sources

- [Click & Paleczny, *A Simple Graph-Based Intermediate Representation* (IR'95)](https://grothoff.org/christian/teaching/2007/3353/papers/click95simple.pdf) — original sea-of-nodes; REGION/PHI/IF/PROJECTION nodes, memory/io/control values, optional control edge tradeoff. *(read full text)*
- [Reissmann, Bahmann, Meyer, Själander, *RVSDG: An Intermediate Representation for Optimizing Compilers* (TOECS 2020, arXiv:1912.05036)](https://arxiv.org/pdf/1912.05036) — gamma/theta/lambda/delta/phi/omega nodes, state edges, CFR, SCFR/PCFR, jlm. *(read full text)*
- [Lawrence, *Optimizing compilation with the Value State Dependence Graph* (Cambridge UCAM-CL-TR-705, 2007)](https://www.cl.cam.ac.uk/techreports/UCAM-CL-TR-705.pdf) — the sequentialization/proceduralization problem; VSDG ≈ lazy functional program; df-PDG. *(read full text)*
- [Lattner et al., *MLIR: A Compiler Infrastructure for the End of Moore's Law* (arXiv:2002.11054)](https://arxiv.org/pdf/2002.11054) — Operations/Regions/Blocks/block-arguments/dialects, progressive lowering.
- [V8 team, *Land ahoy: leaving the Sea of Nodes* (v8.dev, 2025)](https://v8.dev/blog/leaving-the-sea-of-nodes) — documented retreat; effect-chain mirroring, cache misses, 190× load-elim, Turboshaft.
- [Fallin, *The acyclic e-graph: Cranelift's mid-end optimizer* (cfallin.org, 2026)](https://cfallin.org/blog/2026/04/09/aegraph/) — CFG skeleton, union nodes, eager rewrite, scoped elaboration, 1.13 e-nodes/class, ~0.1% win.
- [Cranelift e-graph RFC (bytecodealliance/rfcs)](https://github.com/bytecodealliance/rfcs/blob/main/accepted/cranelift-egraph.md) — design rationale, phase-ordering, Alive 8/334 bugs, aegraph vs egg.
- [Fallin, *A New Backend for Cranelift, Part 1: Instruction Selection* (Mozilla Hacks, 2020)](https://hacks.mozilla.org/2020/10/a-new-backend-for-cranelift-part-1-instruction-selection/) — ISLE backend, block params.
- [Ferrante, Ottenstein, Warren, *The Program Dependence Graph and Its Use in Optimization* (TOPLAS 1987)](https://dl.acm.org/doi/10.1145/24039.24041) — PDG control + data dependence.
- [Swift SIL reference (swiftlang/swift docs)](https://github.com/swiftlang/swift/blob/main/docs/SIL/Ownership.md) — SSA SIL, mandatory passes, Ownership SSA.
- [GRIN compiler project](https://github.com/grin-compiler/grin) — graph reduction IR, whole-program optimization, explicit laziness.
- [Simon Marlow / GHC notes — Core & STG](https://www.scs.stanford.edu/11au-cs240h/notes/ghc.html) — Core (let=alloc, case=eval), thunks/closures, STG.
- [MLIR Traits (`IsolatedFromAbove`) docs](https://mlir.llvm.org/docs/Traits/) — region SSA visibility, parallel compilation.
- [Max Bernstein, *A catalog of ways to generate SSA*](https://bernsteinbear.com/blog/ssa/) — SSA history, Cytron dominance frontier, φ placement.

## Open questions / uncertainties

- The **"Simple" sea-of-nodes tutorial README** (SeaOfNodes org) 404'd at the URL I
  tried; my node-kind details for sea-of-nodes come from Click's original paper
  (read in full) plus secondary summaries — the *tutorial's* exact API naming is
  unverified.
- **Turboshaft's** precise IR data structures (how it represents block arguments,
  whether it keeps any floating nodes, its exact effect model) are not detailed in
  the "Land ahoy" post (it promises a follow-up); I only have the *rationale* and
  benchmark numbers, not the full design.
- **Firm / libFIRM** specifics (exact node taxonomy, memory-edge model) come from
  search summaries, not a primary doc I fetched; treat the libFIRM detail as
  secondary.
- **Exact LOC / codebase sizes** for HotSpot C2, Graal, libFIRM, MLIR were not
  verified; I report qualitative scale and the RVSDG/Cranelift/V8 *quantitative*
  claims that I did source.
- **GHC System FC** details (coercions, type families) are summarized from secondary
  sources; I did not fetch the System FC paper directly.
- RVSDG **performance vs LLVM**: the paper states jlm is "competitive" and *not*
  aiming to beat LLVM/GCC; I did not extract the exact per-benchmark speedup
  geomean figure (Figure 8) as a number.
