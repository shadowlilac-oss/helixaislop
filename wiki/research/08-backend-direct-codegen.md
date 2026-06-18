# Direct-to-Machine-Code: ISel, Scheduling, and Register Allocation From a Graph

> Research note for the design of a new unified IR. Topic 08 of the backend series.
> Focus: instruction selection as term/graph rewriting, register allocation on SSA/region
> IRs, instruction scheduling from a dataflow graph, and the "graph straight to bytes"
> single-IR backend pattern. Primary case study: **Cranelift** (the most fully-realized
> production "graph -> rewrite-rule lowering -> SSA regalloc -> direct emission" pipeline),
> contrasted with LLVM SelectionDAG/GlobalISel and the classical BURS/BURG literature.

## Overview

The backend question this note addresses is: given an *optimized graph IR in which target
operations are already chosen as nodes*, what is the minimal, real-world path to scheduled,
register-allocated, encoded machine bytes? The literature gives three relevant pillars:

1. **Instruction selection (ISel)** as rewriting — tree pattern matching / maximal munch,
   BURS/BURG dynamic-programming tiling, DAG-based ISel (LLVM SelectionDAG), and rewrite-rule
   lowering DSLs (Cranelift ISLE). There is a deep theory result here: tiling a *tree* is
   polynomial (BURS), tiling a *DAG* is NP-complete (Koes & Goldstein, CGO 2008).
2. **Register allocation** that runs directly on SSA — the 2005-era discovery that SSA
   interference graphs are *chordal* (Hack; Brisk et al.; Pereira & Palsberg), which lets
   coloring, spilling, and coalescing be **decoupled** and coloring be done optimally in
   near-linear time. Production allocators (Cranelift `regalloc2`, IonMonkey) instead use
   SSA-aware backtracking/linear-scan with live-range splitting.
3. **Instruction scheduling** as recovering a linear order from a dataflow/region graph —
   list scheduling over a dependence DAG, and the more radical "scheduling falls out of
   elaboration" approach in Cranelift's acyclic e-graph mid-end.

The recurring modern theme — strongest in Cranelift — is the *elimination of a secondary
backend IR*. Cranelift lowers CLIF directly into `VCode` (virtual-register machine code) in a
single pass, register-allocates in place, and emits bytes through a streaming `MachBuffer`
that does branch fixups inline with no fixpoint. LLVM's GlobalISel is a parallel move in the
same direction (replacing the separate SelectionDAG IR with generic-MIR over the function).

## Core Model (nodes / edges / regions)

### Tree / DAG tiling (the classical model)

The classical ISel model treats the IR expression as a **tree of operators** and the target
ISA as a set of **tree patterns (tiles)**, each with a cost. Instruction selection is then a
*tree-covering* problem: tile the input tree with patterns so every node is covered and total
cost is minimized.

- **Maximal munch** greedily picks the largest matching pattern at each node — fast, locally
  greedy, not globally optimal.
- **BURS (Bottom-Up Rewrite System)** / **BURG** solve the covering *optimally* for trees
  using **dynamic programming**. BURG (Fraser, Henry, Proebsting 1992) is the canonical
  code-generator generator built on BURS theory; it "encode[s] the problem as a finite
  tree-matching automaton and shift[s] cost calculations to the code selector generation
  phase" — i.e. BURG does the DP at *compile-compile time* (table generation), whereas iBURG
  does it at compile time. (Source: BURG paper; Wikipedia BURS; ScienceDirect.)

The critical structural fact: **a node in a tree has a unique parent**, so the DP has no
overlapping subproblems across uses. The moment you allow *sharing* — a value computed once
and used by multiple consumers, i.e. a **DAG** rather than a tree — instruction selection
becomes **NP-complete** (Koes & Goldstein, "Near-Optimal Instruction Selection on DAGs",
CGO 2008). The hardness is exactly the shared-node problem: the selector must simultaneously
decide which tiles cover shared nodes, because a tile that "absorbs" a shared subexpression
may force recomputation or extra copies for the other consumers. Production compilers
therefore either split the DAG back into trees (losing CSE), or use heuristic/greedy DAG
covering (LLVM SelectionDAG), or, as discussed below, push the problem into a cost-based
extraction over a richer graph (e-graphs) — which is *also* NP-complete.

### LLVM SelectionDAG model

SelectionDAG introduces a **dedicated per-basic-block DAG IR**. The pipeline is: build a DAG
from LLVM IR -> **legalize** (rewrite types/ops the target cannot handle into legal ones) ->
DAG-combine/optimize -> **instruction selection** (pattern-match DAG nodes to machine ops) ->
**scheduling** (assign a linear order to the DAG) -> emit `MachineInstr`. Two documented
weaknesses motivated its replacement: it operates **per-basic-block** (losing cross-block
opportunities), and it is a *separate IR* with real compile-time cost.
(Sources: LLVM GlobalISel docs; compilercodegen.hashnode.dev SelectionDAG writeup.)

### LLVM GlobalISel model (single-IR-ish)

GlobalISel deliberately avoids a separate IR: it "directly operates on the post-isel
representation used by the rest of the code generator, **MIR**", extended with **Generic
Machine IR (gMIR)**. Its four passes — **IRTranslator** (IR -> gMIR), **Legalizer** (illegal
generic ops -> legal generic ops), **RegBankSelect** (assign register *banks* to vregs),
**InstructionSelect** (gMIR -> target instructions) — all run over the *whole function* on
one IR, unlike SelectionDAG's per-block DAG. (Source: LLVM GlobalISel docs.)

### Cranelift ISLE model (term rewriting)

ISLE ("Instruction Selection and Lowering Expressions") models the input IR as, conceptually,
**a tree of terms**, with a set of **rewrite rules** that turn input terms (CLIF instructions)
into output terms (`MachInst`). Key vocabulary from the language reference and Fallin's blog:

- **`decl`**: declares a *term* with parameter types and a return type, e.g.
  `(decl lower (Inst) InstOutput)`. Terms may be **pure**, **partial** (fallible match), or
  **`rec`** (recursive).
- **Rule** = LHS *pattern* + RHS *expression*. "A term can only be rewritten to another term
  of the same type."
- **Extractor**: a function that takes a complete term and *possibly matches* it (fallible) —
  used on the LHS. **Constructor**: builds/rewrites a term on the RHS.
- **Internal** terms are defined by ISLE rules; **external** terms bind to Rust functions via
  a generated `Context` trait (`(extern constructor ...)`, `(extern extractor ...)`). This is
  how "all interactions with the rest of the compiler [are] via 'virtual' terms, implemented
  by Rust functions" — ISLE itself "knows nothing of the rest of the compiler, or 'ASTs'".
- **Priorities**: rules carry signed-integer priorities; "an applicable rule with a higher
  priority will always match before a rule with a lower priority", and an **overlap checker**
  enforces that no two *overlapping* rules share a priority.

The ISLE meta-compiler merges all rules into a **decision trie** ("each node ... is either an
internal decision node, or a leaf 'expression' node"; "if a match op does not succeed, we try
the next out-edge in sequence"), and emits **Rust source** that runs in a *single pass* over
the input, sharing match work across rules. One rewrite step is one Rust function call;
intermediate terms exist only as ephemeral stack frames, not heap data structures.
(Sources: ISLE language-reference.md; cfallin.org ISLE blog.)

## How Control and Effects/Memory Are Represented

Cranelift is the most instructive case because it explicitly *splits* control/effects from
pure dataflow at the IR level and re-fuses them in the backend.

- **Mid-end (acyclic e-graph / "ægraph")**: Cranelift keeps the **CFG for side-effectful
  instructions** — the "**side-effect skeleton**" (loads, stores, calls stay anchored to
  control-flow locations) — and uses a **sea-of-nodes-style pure dataflow graph** for
  everything side-effect-free. "what if we kept the CFG for the side-effectful instructions
  (call it the 'side-effect skeleton') and used a sea-of-nodes for the rest?" Pure operators
  float; impure ones are pinned. This is the *hybrid* that makes GVN/LICM/DCE emergent rather
  than separate passes. (Source: cfallin.org aegraph blog, 2026.)

- **Tree matching across the skeleton during lowering**: Cranelift's lowering does **tree
  matching** over CLIF — at an instruction it "can look 'up' the program to find its producer
  (def)", enabling many-to-one (e.g. `iadd(imul x y) z` -> `madd`). Matching **stops at block
  parameters (φ-nodes)**, treating them as tree leaves: "it is perfectly alright to
  pattern-match on a tree that is a *subset* of the true dataflow." Effects and ordering are
  preserved because side-effecting CLIF instructions are visited in program order in the
  single lowering pass; only *pure* producers are pulled into a match tree.

- **Operand effect/timing model for RA**: In `VCode`, operands carry **use/def** semantics,
  **timing** ("early" before execution vs "late" after), and **constraints** (any register,
  fixed register, stack slot, or *reused-input*). This is how implicit-register and
  read-modify-write effects (e.g. x86 multiply, tied operands) are expressed to the allocator
  without a separate effect IR. (Source: regalloc2 blog.)

- **SSA φ / block parameters as the control-merge effect**: SSA's φ is exactly the device
  that "breaks chordless cycles" in the interference graph (see RA section). Cranelift uses
  **block parameters** instead of φ-nodes and hands them directly to `regalloc2`, which merges
  the associated live ranges into a bundle.

## Optimization Approach

Two distinct philosophies appear:

**(a) Optimization separate from ISel (LLVM, classical).** Mid-level optimization runs on the
optimizer IR; ISel is a downstream covering/lowering phase with its own (limited) local
DAG-combine. Phase ordering between optimization, scheduling, and RA is a known, *NP-hard*
source of pain (Motwani et al. proved combined scheduling+RA is NP-hard; the CRISP line of
work and the ACM "Combinatorial Register Allocation and Instruction Scheduling" survey
document the tradeoffs: schedule-first raises register pressure; allocate-first adds false
dependences that constrain the scheduler).

**(b) Optimization unified with lowering via one rewrite engine.** Cranelift uses **the same
ISLE rewrite-rule DSL for mid-end optimization** (the ægraph rewrites) *and* for backend
lowering. The mid-end is an **acyclic e-graph**: "once we create a node, we never update it
... We've now maintained acyclicity, by construction." Equivalences are recorded by **union
nodes** (an immutable binary tree of options per value), append-only, with **no rebuild phase
and no fixpoint** — unlike classical equality saturation. Extraction is **cost-based dynamic
programming** that "deliberately ignores shared-substructure accounting (which would be
NP-hard)". GVN (hash-consing), LICM, rematerialization, and DCE all emerge from
**elaboration** rather than being coded as passes.

### Can ISel be unified with optimization in one e-graph?

This is the speculative-but-active frontier the topic asks about. What is known:

- **It is conceptually clean**: a sea-of-nodes IR "is already implicitly an e-graph, with
  'trivial' (one-member) e-classes for each e-node." Adding *target-op nodes* as alternative
  members of an e-class, with a **machine cost model**, makes ISel literally an *extraction*
  problem: pick the minimum-cost covering that includes target ops. Cranelift's design points
  directly at this — same DSL, same graph.
- **It is hard in exactly the place tiling is hard**: **e-graph extraction is NP-complete**
  (the well-known result; HN/Lobsters discussions, and e-boost / "E-Graphs as Circuits"
  papers), for the *same* shared-node reason DAG instruction selection is NP-complete
  (Koes 2008). Optimal extraction needs ILP (scales poorly), FPT-by-treewidth algorithms, or
  heuristics. Practical systems (egg, egglog, Cranelift) extract **greedily / with local cost
  functions** and accept suboptimality.
- **What's genuinely hard beyond extraction cost**: cost in a real backend is *not local* —
  register pressure, scheduling, and shared-tile interactions make a node's true cost depend
  on global context, which a bottom-up DP cannot see. So "ISel = e-graph extraction with a
  machine cost model" is correct in spirit but glosses over RA/scheduling feedback. No
  production compiler today does fully-unified opt+ISel+RA in one e-graph; Cranelift unifies
  *opt + ISel rule language* but still runs RA and final ordering as later phases.

## Code Generation / Lowering

Cranelift gives the cleanest worked example of "graph straight to bytes":

1. **Lowering to `VCode`**: a **single pass** over CLIF, postorder ("we iterate in postorder;
   this way, all instructions are seen before instructions that dominate them"), invoking the
   ISLE-generated lowering function per instruction. A **reference-counting scheme** in the
   pass means an instruction's code is only generated if its result is actually used
   (lowering-time DCE). Output is `VCode` = **"virtual-register code"**: a linear array of
   `MachInst`s organized into basic blocks, referencing virtual regs `v0..vN` or real regs
   `r0..rN`. `VCode` is *not* designed for insertion/removal/reordering — "code is lowered
   into VCode with a single pass, generating instructions in their final or near-final order."
2. **Block ordering**: the `blockorder` module computes "lowered blocks" via a DFS over the
   CLIF-plus-edge-block graph (never materialized; defined by a `successors` function) to get
   reverse-postorder. Edge blocks handle critical edges / branch args.
3. **Register allocation in place** (next section), editing `MachInst`s to replace virtual
   regs with real regs and inserting moves/spills.
4. **Emission via `MachBuffer`**: a "smart machine-code buffer that knows about branches and
   edits them on-the-fly as we emit them." It is "not merely a `Vec<u8>`": it tracks emitted
   bytes, bound labels, label-offset tables, label aliases, and the last contiguous branches.
   It runs a **linear-time branch peephole at the tail** when labels bind (branch inversion to
   drop fall-through jumps, jump threading via an alias table, dead-block elision). For
   out-of-range branches it inserts **veneers** (longer-form indirect branches) and emits
   **islands** "just-in-time when we're in danger of going out of range," tracking outstanding
   references and **deadlines** — **"without any sort of fixpoint algorithm or later moving of
   code."** This replaces the traditional multi-pass (critical-edge splitting, block ordering,
   redundant-block elimination, branch relaxation, target resolution) with inline transforms.
   (Sources: Mozilla Hacks "New Backend" Part 1; cfallin.org ISel Part 2; machinst docs.)

Encoding itself is per-`MachInst`: each instruction implements an `emit` that writes its bytes
into the `MachBuffer` and declares its branch label-uses through a `LabelUse` trait
(defining reference kinds, how to patch a resolved offset, and how to emit a veneer).

## Compile-Time Evaluation / Partial Evaluation

- **ISLE's "constant folding" is term rewriting**: because the same rewrite engine runs the
  mid-end, compile-time evaluation of pure operators is just rules that rewrite a pure-op term
  with constant arguments into a constant term. The acyclic e-graph performs this as part of
  optimization; folded constants become new (immutable) nodes joined by union nodes.
- **There is no general partial evaluation / staged metaprogramming in these backends.** ISLE
  is itself a *meta-compiler* (compile-compile-time: rules -> Rust), which is the
  partial-evaluation analogue at the *generator* level — the decision trie is specialized once
  at build time, not per-program. BURG/BURS are the classic example of the same idea: the DP
  cost tables are *precomputed at code-generator-generation time*.
- For a *new* IR with comptime semantics, the lesson is that the rewrite/extraction layer is
  the natural home for compile-time evaluation of pure nodes, but anything stateful must stay
  pinned to the side-effect skeleton (you cannot float a comptime side effect).

## Strengths

- **No secondary backend IR** (Cranelift `VCode`, LLVM gMIR): less code, faster compiles, one
  representation to reason about and to register-allocate. GlobalISel's stated motivation is
  exactly removing SelectionDAG's "dedicated intermediate representation" cost and its
  per-block scope.
- **Rewrite-rule lowering is auditable and machine-checkable**: ISLE rules are declarative,
  type-checked, overlap-checked, and have been the subject of formal verification (VeriISLE,
  CMU-CS-23-126). One DSL covers x86-64, aarch64, s390x, riscv64.
- **Single-pass everything**: lowering (one pass), emission (one streaming pass with inline
  fixups, no fixpoint), and acyclic-e-graph optimization (no saturation fixpoint) all avoid
  iteration — good for a JIT/Wasm setting where compile time matters.
- **SSA RA is theoretically clean**: SSA interference graphs are **chordal** -> optimal
  coloring in O(|E|+|V|), and **coloring / spilling / coalescing decouple** completely (Hack;
  Pereira & Palsberg; SSA register-allocation survey). You can *spill to reach Maxlive <= k*
  first, then color with guaranteed success, then coalesce last.
- **Scheduling can be emergent**: in the ægraph, "scoped elaboration" produces instruction
  placement, code motion, and value-identity unification *simultaneously* — scheduling is a
  byproduct of extraction + dominance-scoped placement, not a separate list-scheduler.

## Weaknesses and Criticisms

- **DAG / e-graph optimal ISel is NP-complete** (Koes 2008; e-graph extraction hardness). All
  shipping systems use greedy/heuristic covering and accept suboptimality; "optimal" tiling
  only scales to tiny DAGs (ILP). A new IR cannot expect free optimal selection from
  extraction alone.
- **Non-local cost**: bottom-up DP / greedy extraction cannot model register pressure or
  scheduling effects, so a cost-driven "ISel = extraction" story is optimistic. The phase
  *order* between RA and scheduling is itself NP-hard and unavoidable (Motwani et al.; CRISP).
- **SSA chordality has a sharp caveat**: chordality holds for *strict SSA*; **after SSA
  elimination (φ -> copies) the interference graph can become non-chordal, non-perfect, even
  non-1-perfect**, and "register allocation after SSA elimination is NP-complete" (Pereira &
  Palsberg, citing Hack). So you must allocate *before* destructing SSA, and the φ/copy
  resolution (parallel moves, "windmills"/cycles needing a scratch register) is where
  real-world complexity hides.
- **Production allocators don't actually use chordal coloring**: Cranelift `regalloc2` and
  IonMonkey use **backtracking with live-range splitting and bundles**, not chordal-graph
  coloring — the elegant theory is not what ships, because spilling/coalescing quality and
  constraint handling (fixed regs, reused inputs, calling conventions) dominate.
- **`VCode` is rigid by design**: "not designed to easily allow inserting or removing
  instructions or reordering code." This is a deliberate speed tradeoff but means late changes
  are painful; almost everything must be decided in the single lowering pass.
- **SelectionDAG's documented retreat**: LLVM is actively migrating *away* from SelectionDAG to
  GlobalISel precisely because the separate DAG IR is slow and per-block. This is a concrete
  "moved away from an approach" data point for any team tempted to build a separate backend
  graph IR.

## Codebase / Complexity Notes

- **ISLE adoption** replaced handwritten Rust backends with **~27k lines of DSL code** (per
  Fallin's ISLE blog). The ISLE meta-compiler emits Rust; the per-target lowering lives in
  `.isle` files (e.g. `cranelift/codegen/src/isa/aarch64/inst.isle`).
- **`regalloc2`** is **">10K lines of very dense Rust code"**. It *started as an effort to
  port* IonMonkey's register allocator (`BacktrackingAllocator.cpp`) to Rust but, per Fallin,
  "quickly evolved during a focused optimization effort to have its own unique design and
  implementation aspects" — so it is IonMonkey-derived in lineage, not a ~50% literal port (no
  such fraction is stated in the source). It adds substantial fuzzing/checker infrastructure
  absent from the original. The reported effects of switching to it are, precisely: **compiler
  speed (compile time) improved by "20%-ish"** (regalloc dominated compile time); **generated-
  code (runtime) performance improved up to 10-20% on register-pressure-impacted Wasmtime
  benchmarks**; and **runtime improved up to ~7%** when Cranelift is used as a rustc backend
  via `rustc_codegen_cranelift`. (Note: the 10-20% figure is a *runtime* improvement, not a
  compile-time one.) (Source: cfallin.org regalloc2 blog.)
- **BURG/BURS**: the cost is in the *generator* (table/automaton construction is the expensive
  step, done once); the runtime tree-matcher is tiny and fast — the original selling point.
- **Chordal coloring core** is famously small: MCS (maximum cardinality search) for the
  perfect-elimination/simplicial ordering is O(|E|+|V|) and a few lines; greedy coloring is a
  few lines. The complexity that ships is in spilling and coalescing heuristics, not coloring.

## Lessons for a New Unified IR

A concrete, minimal path from *an optimized graph whose nodes already include target ops* to
machine bytes (synthesized from Cranelift + the SSA-RA literature):

1. **Keep SSA and a hybrid graph.** Pure ops in a dataflow/sea-of-nodes; side-effecting ops
   pinned to a CFG "skeleton" in program order. Use **block parameters**, not raw φ, so the
   allocator can consume them directly. Do *not* destruct SSA before RA.
2. **Lower in one postorder pass into a `VCode`-like linear array of target instructions with
   virtual registers.** Do **tree matching** up the pure-dataflow graph (stop at
   block-param/skeleton boundaries). If you want a DSL, ISLE-style rule compilation to a
   decision trie is the proven design; expect greedy/priority-ordered matching, not optimal
   tiling. Reference-count during lowering for free DCE.
3. **Recover a linear schedule from the graph.** Either (a) **list-schedule** each block over
   its dependence DAG (ready list ordered by a priority such as critical-path length —
   forward or backward, both ship), using explicit ordering/effect edges as the precedence
   relation; or (b) Cranelift-style: let **scoped elaboration** (preorder over the dominator
   tree, demand-driven evaluation of e-classes referenced by the skeleton, memoized via an
   eclass-to-SSA-value map) place instructions — scheduling, code motion, and GVN fall out
   together.
4. **Register-allocate on the SSA `VCode` in place.** Theory option: chordal coloring with
   decoupled spill (reach Maxlive <= k) -> color (optimal, near-linear) -> coalesce last.
   Practical option (recommended): **SSA-aware backtracking / linear scan with live-range
   splitting and bundles** (regalloc2 / Wimmer-Franz). Express operand timing (early/late),
   fixed-register and reused-input constraints on operands. Resolve φ/block-param transfers as
   **parallel moves** (handle cycles with a scratch register) only at the very end.
5. **Emit bytes through a streaming buffer with inline branch fixups.** Per-instruction
   `emit` into a `MachBuffer`-like sink; bind labels as you go; do a tail branch-peephole
   (invert/thread/elide); handle range with veneers + islands on deadlines. **No fixpoint, no
   post-pass code motion.**

Caveats to bake into the design: optimal ISel-by-extraction is NP-complete and cost is
non-local, so treat extraction as a *good heuristic*, not an oracle; the RA/scheduling phase
order is NP-hard, so pick one order deliberately and tolerate the known downside; and SSA's
chordality only buys you anything *before* SSA destruction.

## Sources

- [Cranelift's Instruction Selector DSL, ISLE: Term-Rewriting Made Practical (Chris Fallin, 2023)](https://cfallin.org/blog/2023/01-20/cranelift-isle/) — ISLE design, terms/extractors/constructors, decision-trie compilation, priorities, ~27k LOC claim.
- [ISLE language reference (wasmtime repo)](https://github.com/bytecodealliance/wasmtime/blob/main/cranelift/isle/docs/language-reference.md) — exact semantics of `decl`, rules, partial/pure/rec terms, internal vs external etors, decision trie.
- [The acyclic e-graph: Cranelift's mid-end optimizer (Chris Fallin, 2026)](https://cfallin.org/blog/2026/04/09/aegraph/) — side-effect skeleton, scoped elaboration, acyclic/append-only e-graph, emergent GVN/LICM/DCE, scheduling-as-elaboration.
- [A New Backend for Cranelift, Part 1: Instruction Selection (Mozilla Hacks, 2020)](https://hacks.mozilla.org/2020/10/a-new-backend-for-cranelift-part-1-instruction-selection/) — single-pass tree-matching lowering into VCode, virtual registers, no secondary IR.
- [Cranelift, Part 2: Compiler Efficiency, CFGs, and a Branch Peephole Optimizer (Chris Fallin, 2021)](https://cfallin.org/blog/2021/01/22/cranelift-isel-2/) — MachBuffer streaming emission, inline branch fixups/islands/veneers without fixpoint, "graph straight to bytes".
- [Cranelift, Part 4: A New Register Allocator / regalloc2 (Chris Fallin, 2022)](https://cfallin.org/blog/2022/06/09/cranelift-regalloc2/) — backtracking allocator, bundles, live-range splitting, operand timing/constraints, parallel-move resolution; "started as an effort to port" IonMonkey's `BacktrackingAllocator.cpp` but "evolved... [its] own unique design"; ">10K lines"; "20%-ish" compiler-speed (compile-time) gain, up to 10-20% *runtime* on Wasmtime register-pressure benchmarks, up to 7% runtime via rustc_codegen_cranelift.
- [Register Allocation via Coloring of Chordal Graphs (Pereira & Palsberg, APLAS 2005)](http://web.cs.ucla.edu/~palsberg/paper/aplas05.pdf) — SSA interference graphs are chordal; MCS + greedy optimal coloring; decoupled coloring/spilling/coalescing; non-chordality after SSA elimination; RA-after-SSA-elim is NP-complete.
- [Register Allocation for Programs in SSA-Form (Hack, Grund, Goos, CC 2006)](https://dl.acm.org/doi/10.1007/11688839_20) — chordality of SSA interference graphs; decoupling coloring/spilling/coalescing (paper landing page; text via Pereira & Palsberg's account of Hack's result).
- [Linear Scan Register Allocation on SSA Form (Wimmer & Franz, CGO 2010)](https://dl.acm.org/doi/pdf/10.1145/1772954.1772979) — SSA simplifies linear scan; def dominates uses; interference checked at def points; spilling decoupled from coloring.
- [Near-Optimal Instruction Selection on DAGs (Koes & Goldstein, CGO 2008)](https://llvm.org/pubs/2008-CGO-DagISel.pdf) — DAG ISel is NP-complete (shared subexpressions); contrast with polynomial tree BURS; near-optimal heuristic.
- [BURG: Fast Optimal Instruction Selection and Tree Parsing (Fraser, Henry, Proebsting 1992)](https://www.semanticscholar.org/paper/BURG:-fast-optimal-instruction-selection-and-tree-Fraser-Henry/0bc1bb2167bfb0ddcb1b36815331c3f8b56b45ce) — canonical BURS-based code-gen generator; DP at compile-compile time.
- [BURS (Wikipedia)](https://en.wikipedia.org/wiki/BURS) — BURS theory overview, tree-matching automaton, DP cost shifting.
- [Global Instruction Selection (LLVM docs)](https://llvm.org/docs/GlobalISel/index.html) — IRTranslator/Legalizer/RegBankSelect/InstructionSelect over gMIR/MIR; whole-function; motivation vs SelectionDAG.
- [LLVM Instruction Selection: SelectionDAG Building](https://compilercodegen.hashnode.dev/llvm-instruction-selection-selectiondag-building) — SelectionDAG pipeline (build/legalize/select/schedule/emit), per-block scope.
- [The E-graph extraction problem is NP-complete (HN discussion)](https://news.ycombinator.com/item?id=36453592) and [e-boost: Boosted E-Graph Extraction (arXiv 2508.13020)](https://arxiv.org/pdf/2508.13020) — extraction NP-hardness; ILP/heuristic/FPT extraction.
- [Combinatorial Register Allocation and Instruction Scheduling (Castañeda Lozano et al., ACM TOPLAS 2019)](https://dl.acm.org/doi/10.1145/3332373) and [Combining Register Allocation and Instruction Scheduling (Motwani et al., CS-TN-95-22)](http://i.stanford.edu/pub/cstr/reports/cs/tn/95/22/CS-TN-95-22.pdf) — phase-ordering NP-hardness; schedule-first vs allocate-first tradeoffs.
- [Lecture 7: Introduction to Instruction Scheduling (Stanford CS243)](https://suif.stanford.edu/~courses/cs243/lectures/L7.pdf) and [EPFL CS420 instruction scheduling notes](https://cs420.epfl.ch/archive/21/c/09_instr-sched.html) — list scheduling, ready/active lists, dependence DAG, critical-path priority, forward/backward.
- [machinst module docs (cranelift-codegen)](https://docs.rs/cranelift-codegen/0.66.0/cranelift_codegen/machinst/) — VCode/MachInst/MachBuffer/blockorder responsibilities.

## Open Questions / Uncertainties

- **Exact LOC of ISLE rule files per target** is not confirmed from a primary source; the
  "~27k lines of DSL" figure is the aggregate claim in Fallin's ISLE blog, not a per-backend
  breakdown. Treat per-target rule counts as unverified.
- **Whether any production compiler does fully-unified opt + ISel in a single e-graph with a
  machine cost model**: I found no evidence of one shipping. Cranelift unifies the *rule
  language* (ISLE) across mid-end and backend and uses an e-graph in the mid-end, but ISel,
  RA, and scheduling remain distinct phases. The unification is research-stage; the blocking
  facts are NP-complete extraction and non-local cost.
- **regalloc2's precise current relationship to SSA chordality**: it consumes SSA and merges
  block-param live ranges, but it is a backtracking/bundle allocator, not a chordal-coloring
  allocator. I did not verify whether it relies on any chordality property internally; the
  blog describes a port of IonMonkey's algorithm.
- **GlobalISel pattern-matching mechanics** (GICombiner, MIR-patterns-in-TableGen) were only
  named, not detailed, in the fetched docs — depth there is unverified.
- **The exact cost function regalloc2 / Cranelift uses for ægraph extraction** (the precise
  per-op machine cost weights) was not extracted from source; described only qualitatively as
  bottom-up DP ignoring shared-substructure accounting.
- **Whether Cranelift performs explicit list scheduling at all**: the evidence suggests
  scheduling is largely subsumed by elaboration + single-pass lowering rather than a separate
  list scheduler; I did not find a dedicated list-scheduler pass, but cannot rule out
  target-specific reordering. Stated as a likely-absent component, not confirmed.
