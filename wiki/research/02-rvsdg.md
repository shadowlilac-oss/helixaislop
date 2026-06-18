# RVSDG (Regionalized Value-State Dependence Graph)

## Overview

The **Regionalized Value State Dependence Graph (RVSDG)** is a data-flow-centric
intermediate representation for optimizing compilers, developed primarily by
**Helge Bahmann, Nico Reissmann, Jan Christian Meyer, Magnus Jahre, and Magnus
Själander** at NTNU. The canonical reference is Reissmann, Meyer, Bahmann &
Själander, *"RVSDG: An Intermediate Representation for Optimizing Compilers"*
(ACM TECS 19(6), Art. 49, 2020; arXiv:1912.05036). The control-flow recovery
theory comes from the earlier Bahmann, Reissmann, Jahre & Meyer paper *"Perfect
Reconstructability of Control Flow from Demand Dependence Graphs"* (ACM TACO
11(4):66, 2015). Both articles are reproduced as Article B1 and Article B2 of
Reissmann's PhD thesis (*Principles, Techniques, and Tools for Explicit and
Automatic Parallelization*, NTNU 2019).

The paper's own one-line definition: an RVSDG is *"an acyclic hierarchical
multigraph consisting of nested regions"* where *"nodes represent computations,
edges represent computational dependencies, and regions capture the hierarchical
structure of programs."* It *"represents programs in demand-dependence form,
implicitly supports structured control flow, and models entire programs within a
single IR."*

The key motivation: in LLVM at -O3, the authors counted that the 13 most-invoked
passes are dominated by data-flow re-discovery — alias analysis, SSA
construction/restoration (14 of those passes do SSA restoration), loop discovery
and canonicalization (6 of 13 passes do only this, ~21% of all invocations).
RVSDG aims to make these helper passes *redundant* by baking SSA form, loop
structure, and data dependence directly into the IR as invariants. The RVSDG is
descended from the Value State Dependence Graph (VSDG) of **Johnson and Mycroft**
(Johnson & Mycroft, *"Combined Code Motion and Register Allocation Using the Value
State Dependence Graph,"* CC 2003 — the TECS paper's ref [19]; later VSDG work
includes Alan C. Lawrence's 2007 Cambridge thesis, ref [24]). Its contribution
over the VSDG is the introduction of **regions** to solve the VSDG's eager/lazy
evaluation and side-effect-duplication problem.

The reference implementation is **jlm** (https://github.com/phate/jlm), an
"experimental compiler/optimizer that consumes and produces LLVM IR." Its RVSDG
graph library was historically a separate project called **jive**; jive is now
folded into the `jlm/rvsdg/` subtree. jlm is the first optimizing compiler known
to use a demand-dependence graph as its IR.

## Core Model (nodes / edges / regions)

A **region** `R = (A, N, E, R)` has an argument tuple `A`, node set `N`, edge set
`E`, and result tuple `R`. A region "represents a computation."

A **node** is either:
- **simple** — represents a primitive operation (add, load, store, ...). It has
  an operator; its signature must match the operator's. It maps its input value
  tuple to its output value tuple by evaluating the operator.
- **structural** — *contains regions*. Its inputs/outputs map to the
  arguments/results of the contained region(s).

Each node `n` has a tuple of **inputs I** and **outputs O**. Edges connect an
**origin** `g` (an output `o ∈ On` *or* a region argument `a ∈ A`) to a **user**
`u` (an input `i ∈ In` *or* a region result `r ∈ R`) of matching type. Critical
invariant: *"Every input or result is the user of exactly one edge, whereas
outputs or arguments can be the origins of multiple edges."* This single-origin
rule is precisely what makes the RVSDG *"always in strict SSA form."* Terminology:
the producer of an origin, the consumer of a user, the users/consumers of an
origin. The boundary "ports" of a region are its arguments (entering) and results
(leaving); structural-node inputs/outputs pass values across the region boundary.

### The node zoo

Intra-procedural structural nodes:
- **γ (Gamma) — conditional.** Models *"a decision point"* with regions
  `R0..Rk, k>0` of matching signature. First input is a **predicate** evaluating
  to integer `v, 0≤v≤k` selecting region `Rv`. Models symmetric split/join
  conditionals (if-then-else, switch *without fall-through*). Defines **entry
  variables** `ev_l = (i_{l+1}, A_l)` (one input mapped to the l-th argument of
  every region) and **exit variables** `ex_l = (R_l, o_l)` (l-th result of every
  region mapped to one output).
- **θ (Theta) — tail-controlled loop (do-while).** Contains *one* region (the loop
  body). Input/output/argument tuples all share length and signature. The **first
  region result is the predicate**; if true, results are routed back to the
  arguments for the next iteration, else to the outputs. *"The loop body of an
  iteration is always fully evaluated before the evaluation of the next
  iteration,"* avoiding deadlock between body and predicate and giving
  well-defined behavior for non-terminating loops that mutate external state.
  Defines **loop variables** `lv_l = (i_l, a_l, r_{l+1}, o_l)`. Because the body
  always executes at least once, invariant side-effecting code can be hoisted
  unconditionally. *for*/*while* loops are modeled as a θ nested inside a γ.

Inter-procedural structural nodes:
- **λ (Lambda) — function.** One region = the function body. Inputs refer to
  external dependencies; the single output *is* the function value. Region
  arguments = (external deps ++ formal parameters); region results = function
  results. An **apply-node** is a call: first ("function") input takes a λ output;
  other ("argument") inputs are the call arguments. Defines **context variables**
  `cv_l = (i_l, a_l)`.
- **δ (Delta) — global variable.** One region representing the constant's value;
  inputs = external deps, single output = the global itself; the region's single
  result is the right-hand-side value. Also has **context variables**.
- **φ (Phi) — mutually recursive environment.** One region containing λ-nodes
  whose outputs feed φ-region results; the φ-node's outputs expose the functions
  to outside callers. This is how recursion/mutual recursion is expressed
  *without introducing graph cycles*, preserving acyclicity. Defines **context
  variables** and **recursion variables** `rv_l = (r_l, a_{l+n}, o_l)`.
- **ω (Omega) — translation unit.** The unique top-level node; no inputs/outputs;
  exactly one region whose arguments are *imports* (external entities) and whose
  results are *exports*. The whole program lives under one ω.

This uniform node algebra is the headline design point: *"a def-use dependency of
one function on another is modeled the same way as the def-use dependency of
scalar quantities,"* so one transformation works at scalar, loop, and whole-module
levels (e.g. dead-variable, dead-function, and unreachable-code elimination all
become one pass).

## How control & effects / memory are represented

**Control flow is implicit and structured.** There are no explicit branch/jump
edges between operations. Conditional and iterative control is *contained* inside
γ and θ regions; the predicate is an ordinary value computed by a node. Region
nesting forms a tree that doubles as a Program-Structure-Tree, so loops never have
to be "rediscovered."

**Edges are typed value or state.** *"The types of inputs and outputs are either
values, representing arguments or results of computations, or states, used to
impose an order on operations with side-effects."* Solid lines = **value edges**;
dashed lines = **state edges**. State edges *"preserve the observational semantics
of the input program by ordering its side-effecting operations,"* including memory
reads/writes and exceptions.

**Threading effects.** Construction adds *one additional state argument and result*
to every λ-region. A side-effecting node *consumes the incoming state and produces
a new state* for the next node, sequentializing all stateful operations into a
total order matching the original program. This single global memory state is
*"overly conservative"*: the RVSDG can instead use **multiple disjoint states** to
expose independence — e.g. two non-aliasing stores on incompatible types become
two independent state chains, and two memory-free loops become provably
independent. A *pure* function is one whose state result origin is just the
state argument (it merely passes state through). In principle this generalizes to
a full **memory-SSA form that is "not formally any different from value SSA
form."** The later R-HLS work refines this into two edge kinds: **I/O state
edges** (ordering externally-visible side effects) and **memory state edges**
(ordering memory accesses to the same locations); all memory operations are
*"initially sequentialized by a single memory state edge"* and then disambiguated.

**Demand/dependence semantics.** The graph is read in *demand-dependence* form: an
operation is "demanded" if its output is (transitively) consumed by a region
result. Evaluation order *within* a region is underspecified — *"each topological
order of nodes corresponds to a valid evaluation order"* — which is exactly what
makes independent operations explicit and reorderable.

## Optimization approach

Because programs differing only in ordering of independent ops produce the *same*
RVSDG and loops/conditionals have a single canonical form, *"many optimizations
can be expressed as simple graph traversals, where subgraphs are rewritten, nodes
are moved between regions, nodes or edges are marked, or edges are diverted."* The
authors stress these run *directly on the unoptimized RVSDG in a single regular
pass* with no auxiliary data structures or helper passes.

Documented transformations (TECS §6 and jlm):
- **Common Node Elimination (CNE)** — detects *congruent* nodes (same operator,
  same arity, congruent inputs i.e. congruent origins) and diverts their result
  edges to one node, rendering the rest dead. Two-phase **mark / divert**. Unifies
  CSE + value numbering and extends across γ entry/exit variables, θ loop
  variables, and λ/δ/φ context variables — i.e. it works on conditionals, loops,
  and functions, not just scalars.
- **Dead Node Elimination (DNE)** — combines dead-code, unreachable-code, and
  dead-function elimination. Two-phase **mark / sweep**: mark live outputs/args by
  edge traversal; sweep in reverse topological order removing dead nodes. A dead
  structural node is removed with its whole subtree, so dead branches are pruned
  without descending into them.
- **Invariant Value Redirection (INV)** — for θ, a loop variable is invariant iff
  its result origin is its own argument; for γ, an exit-variable output is
  redirected to an entry-variable input when all results trace back to that input.
- **Node Push-Out (PSH)** — hoist invariant (side-effect-free) nodes out of γ/θ
  regions; exposes them to CNE. (Loop-invariant code motion falls out trivially
  because invariant inputs visibly connect a loop entry port straight to the
  matching exit port.)
- **Node Pull-In (PLL)** — sink nodes used in only one γ region *into* that region,
  forcing conditional execution while avoiding code bloat.
- **γ–θ Inversion (IVT)** — when a γ and θ share a predicate origin, swap them so
  the loop sits inside the conditional's then-case (loop unswitching).
- Plus **Inlining (ILN)**, **Node Reduction (RED)** (constant folding / strength
  reduction, an -instcombine analogue), and **Loop Unrolling (URL)**.

jlm's pass order: `ILN INV RED DNE IVT INV DNE PSH INV DNE URL INV RED CNE DNE
PLL INV DNE`.

## Code generation / lowering

Lowering is called **destruction** and is split inter- vs intra-procedural.
- **Inter-Procedural Control Flow Recovery (Inter-PCFR)** rebuilds an
  Inter-Procedure Graph (IPG) from ω/λ/δ/φ nodes, marking exports and adding
  IPG edges per context variable, then invokes intra-procedural recovery on each
  λ-region.
- **Intra-Procedural Control Flow Recovery (Intra-PCFR)** turns each λ-region into
  a CFG. A region free of γ/θ trivially becomes a *linear* CFG; γ/θ produce
  branches/loops.

This is the genuinely hard direction. Two schemes (from the 2015 TACO paper /
thesis Article B2):
- **Structured Control Flow Recovery (SCFR)** — treat γ/θ as black boxes: emit a
  branch on the γ predicate fanning to recursively-recovered sub-CFGs and rejoin;
  emit a loop back-branch after a θ sub-CFG. *Always* produces structured,
  reducible CFGs (similar to Johnson's output) but *"may result in substantial
  overhead in terms of code size and branch instructions."*
- **Predicative Control Flow Recovery (PCFR)** — interpret the *predicate
  computations* as computed continuations: emit branch vertices for **predicate
  def** nodes and follow **predicate use** nodes to their destinations. Requires
  the RVSDG to be in **predicate continuation form** (see below). PCFR is a
  two-pass process (`PREDICATIVECONTROLFLOWPREPARE` builds disconnected
  straight-line blocks; `PREDICATIVECONTROLFLOWFINISH` wires them). It recovers
  *arbitrary, even irreducible* control flow and reduces static branch count, but
  may yield control flow *"undesirable for certain architectures such as GPUs."*

Two auxiliary algorithms are needed because the RVSDG is more normalized than a
CFG: **EVALORDER** (pick one topological order; for predicate-continuation form,
def/use pairs must be adjacent) and **VARNAMES** (the RVSDG is implicitly SSA, so
recovery must color SSA values into CFG variable names and insert/coalesce copies,
e.g. via interference-graph coalescing à la Chaitin/Briggs). jlm uses **SCFR**
then builds SSA to emit LLVM IR. Note jlm must *destruct* the input LLVM IR's SSA
form (and relies on a pre-run `mem2reg`) before constructing the RVSDG, because
control-flow restructuring needs a non-SSA CFG.

**Perfect reconstruction.** The headline theoretical result (thesis Theorem
B2.5.8): the PCFR destruction algorithm *"perfectly reconstructs the original CFG
an RVSDG has been generated from,"* i.e. *"the RVSDG's restricted control flow
constructs do not limit the complexity of the recoverable control flow."* The
restructuring/recovery algorithms are proven terminating and evaluation-preserving
by structural induction (Theorems B2.5.1–B2.5.4 and onward).

## Compile-time evaluation / partial evaluation

The TECS paper does **not** present a dedicated partial-evaluator or
constant-propagation engine; it deliberately limits its optimization demos to CNE
and DNE. **Node Reduction (RED)** does perform constant folding and strength
reduction in jlm, and CNE's congruence machinery is value-numbering-like, but
there is no described whole-graph partial evaluation, symbolic execution, or
comptime-style staged evaluation. That said, the structure is *amenable* to it:
demand-dependence form plus single-origin SSA means a folded constant simply
becomes a new origin whose edges are diverted, and DNE then collects the corpse —
the same mechanism for scalars, loops, and functions. RVSDG's multi-abstraction-
level claim (operational nodes can sit at "source level" or "machine level")
points at preserving high-level semantics (e.g. that two `std::vector` instances
cannot alias) deep into the pipeline, which is the substrate partial evaluation
would exploit, but the papers leave this as future work. *(This paragraph mixes
verified facts with the IR's stated potential — see Uncertainties.)*

## Strengths

- **Strong invariants for free.** Always strict SSA (single-origin edges), loops
  always canonical tail-controlled θ, conditionals always symmetric γ — so SSA
  restoration, loop discovery, and loop canonicalization passes simply do not
  exist. Six of LLVM's 13 hottest passes are eliminated by construction.
- **Normalization.** Programs differing only by independent-op ordering map to one
  RVSDG; LICM/CSE/dead-code become single direct passes.
- **Unified whole-program model.** Scalars, loops, functions, globals, and the
  translation unit are one acyclic graph; one transformation spans all levels.
- **Explicit independence / parallelism.** Disjoint state edges expose independent
  side effects and independent loops that a CFG forcibly sequentializes.
- **Multi-level abstraction** in a single IR (ML-style dataflow up to machine
  instructions), enabling alias facts to survive lowering.
- **Theoretically complete round-trip:** PCFR can recover arbitrary/irreducible
  control flow with proven perfect reconstruction.

## Weaknesses & criticisms

- **Costly CFG round-trip.** Construction needs Control-Flow Restructuring +
  structural analysis + demand annotation + control-tree translation; destruction
  needs control-flow recovery + SSA reconstruction + copy insertion/coalescing.
  The authors openly concede it *"requires more effort to construct the RVSDG from
  imperative programs and recover control flows for code generation."*
- **Irreducible control flow.** Handled by restructuring that inserts auxiliary
  predicates `q`/`r` and demultiplexing branches (NOT node splitting, explicitly
  to avoid VSDG-style exponential blowup) — but this introduces extra predicates
  and branches, and SCFR-recovered code can carry *"substantial overhead in code
  size and branch instructions."* PCFR avoids that but can emit control flow *"
  undesirable for GPUs."*
- **Conservative state model in practice.** jlm uses a single memory state plus
  one loop state; the paper admits this *"leaves significant parallelization
  potential unused"* — the elegant disjoint-state/memory-SSA story is mostly
  aspirational in the prototype.
- **Engineering immaturity.** jlm does not (at publication) support exceptions or
  intrinsics; it is a research prototype, not competing with mature LLVM/GCC.
- **Lineage caution.** The ancestor VSDG had a real eager-evaluation hazard
  (side-effecting nodes not guarded by predicates → risk of duplicated evaluation;
  *"for graphs with stateful computations, lazy evaluation is the only safe
  strategy,"* and faithful destruction needed a detour through the PDG). RVSDG's
  regions fix this, but it is a cautionary tale about flat value-dependence graphs.

## Codebase / complexity notes

- **jlm** total C++ is ≈ **161k** lines (`*.cpp`+`*.hpp`). The core RVSDG graph
  library (`jlm/rvsdg/`, formerly *jive*) is ≈ **22.8k** lines; the LLVM
  frontend/backend layer (`jlm/llvm/`) is ≈ **94.6k** lines — i.e. *most* of the
  engineering cost is the LLVM-IR ↔ RVSDG bridge, not the IR itself. There are
  separate `hls/` (high-level synthesis) and `mlir/` (MLIR RVSDG dialect) subtrees.
- Node implementation matches the paper: `jlm/rvsdg/{gamma,theta,lambda,delta,
  Phi,control,region,simple-node,structural-node,node}.hpp`. `GammaNode`/
  `GammaOperation` expose `EntryVar`, `ExitVar`, `MatchVar`, and `predicate()`,
  mirroring the paper's entry/exit-variable formalism; `ThetaNode` similarly.
- **Representational overhead is linear.** The thesis evaluation (Fig. B1.11a)
  shows LLVM-instruction-count vs RVSDG-node-count is a clear linear relationship,
  and construction+optimization+destruction time is also linear in input size —
  empirically confirming the IR is feasible in space and time. Generated-code
  performance is *competitive* with LLVM -O3 on PolyBench despite the conservative
  single-state model (geomean speedups in the same ballpark as opt -O3, though not
  beating vectorized -O3).

## Lessons for a NEW unified IR

- **Single-origin edges give SSA for free** and erase a whole class of restoration
  passes — make this an enforced structural invariant, not a derived property.
- **Make state an explicit typed value threaded by edges.** One state token gives
  correctness cheaply; *splitting* state into disjoint chains (memory/I/O, per
  alias class) is the lever for exposing independence — design the type system to
  support many states from day one rather than retrofitting alias analysis.
- **Regions are the fix for flat value-graphs.** Nesting side-effecting ops inside
  predicate-guarded regions (γ) eliminates the VSDG's duplicated-evaluation hazard
  and makes lowering tractable. A flat global value graph without regions is a
  documented dead end.
- **One uniform node algebra across abstraction levels** (scalar→loop→function→
  module) collapses pass count: dead-code, dead-function, and unreachable-code
  elimination become *one* algorithm. Aim for this uniformity.
- **Canonicalize control to a minimal set** (one loop form = tail-controlled;
  conditionals = symmetric split/join). Express richer forms (while/for) as
  compositions. This is what makes optimizations single-pass.
- **Budget heavily for the CFG↔IR round-trip.** The structured form is wonderful
  internally but the construction (restructuring + demand annotation) and
  destruction (control-flow recovery + SSA rebuild + copy coalescing) are where
  the real engineering lives — in jlm, ~6x more code than the IR core. Decide
  early between *structured* recovery (simple, reducible, code-bloaty) and
  *predicative* recovery (arbitrary control flow, proven perfect reconstruction,
  but possibly bad for GPUs).
- **Keep destruction provably evaluation-equivalent;** the perfect-reconstruction
  theorem is what lets you trust an aggressive structured IR for codegen.

## Sources

- [RVSDG: An Intermediate Representation for Optimizing Compilers (arXiv PDF)](https://arxiv.org/pdf/1912.05036) — the TECS 2020 paper; primary source for the node zoo, edges/states, construction & destruction algorithms (I–VI), CNE/DNE. Fetched and read in full.
- [RVSDG TECS 2020 abstract / ACM DOI](https://dl.acm.org/doi/fullHtml/10.1145/3391902) — canonical published version (HTML/PDF behind ACM; 403 on direct fetch).
- [Reissmann PhD thesis: *Principles, Techniques, and Tools for Explicit and Automatic Parallelization* (PDF)](https://www.sjalander.com/research/pdf/reissmann-PhD-thesis.pdf) — embeds Article B1 (RVSDG) and Article B2 (Perfect Reconstructability); primary source for CFR (loop/branch restructuring), predicate continuation form, SCFR/PCFR, reconstruction theorems, INV/PSH/PLL/IVT optimizations. Fetched and read in full.
- [Perfect Reconstructability of Control Flow from Demand Dependence Graphs (ACM TACO 2015 DOI)](https://dl.acm.org/doi/10.1145/2693261) — Bahmann et al.; the destruction/control-flow-recovery theory (also reproduced in the thesis).
- [phate/jlm (GitHub)](https://github.com/phate/jlm) — reference RVSDG compiler; cloned and inspected for LOC and node-class structure (`jlm/rvsdg/*.hpp`).
- [R-HLS: An IR for Dynamic High-Level Synthesis and Memory Disambiguation based on Regions and State Edges (arXiv HTML)](https://arxiv.org/html/2408.08712v1) — modern RVSDG extension; primary source for I/O vs memory state edges and memory disambiguation built on state edges; confirms RVSDG/jlm active use.
- [Johnson & Mycroft, *Combined Code Motion and Register Allocation Using the Value State Dependence Graph* (CC 2003, Springer LNCS 2622)](https://link.springer.com/chapter/10.1007/3-540-36579-6_1) — the originating VSDG paper (TECS ref [19]); fact-check source confirming the VSDG is due to **Johnson and Mycroft**, not "Johnson and Lawrence." Alan C. Lawrence's *Optimizing Compilation with the Value State Dependence Graph* (Cambridge tech report UCAM-CL-TR-705, 2007; TECS ref [24]) is later VSDG work, not its origin.

## Open questions / uncertainties

- **Partial evaluation / comptime:** I found no dedicated partial-evaluation or
  symbolic-execution facility in the RVSDG papers; the "amenable to it" framing in
  the comptime section is my inference from the IR's structure (demand form +
  SSA + multi-level nodes), not a documented feature. The only verified
  compile-time-evaluation facility is jlm's RED pass (constant folding / strength
  reduction).
- **Exact LOC figures** are from a shallow clone of jlm's current `master`
  (June 2026), not the version evaluated in the 2020 paper; the proportion
  (IR core ≪ LLVM bridge) is the robust takeaway, not the precise numbers.
- The thesis OCR rendered Greek letters inconsistently (e.g. "PCFR" appears once
  where "SCFR" is meant in the definitions paragraph at thesis line ~4693); I have
  used the algorithm bodies, which are unambiguous (SCFR = structured/region-based;
  PCFR = predicative/predicate-continuation-based).
- Quantitative speedup/code-size results are summarized qualitatively here; the
  exact per-benchmark numbers live in TECS §7 / thesis §B1.7 figures, which I read
  as figures rather than tabulated text.
- Whether any *production* compiler has adopted RVSDG: I found only research
  use (jlm, the R-HLS/HLS work, an MLIR RVSDG dialect, and a EuroSciPy 2024 talk
  on regularizing Python via structured control flow). No evidence of a mainstream
  production compiler adopting RVSDG, and no documented large-scale "retreat"
  either — it remains a research IR.
