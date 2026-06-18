# Sea of Nodes

## Overview

The **Sea of Nodes** (sometimes "soup of nodes") is a graph-based SSA intermediate
representation introduced by **Cliff Click and Michael Paleczny** in *"A Simple
Graph-Based Intermediate Representation"* (ACM SIGPLAN Workshop on Intermediate
Representations, IR'95, San Francisco, pp. 35–49) and developed in detail in Click's
1995 Rice University PhD thesis *"Combining Analyses, Combining Optimizations."* The
companion PLDI'95 paper *"Global Code Motion / Global Value Numbering"* (La Jolla, CA,
pp. 246–257) supplies the scheduling and redundancy-elimination algorithms.

The defining idea: a program is a single **directed graph that fuses data flow and
control flow into one representation**, and the IR "relaxes the control flow from a
total order to a partial order," keeping "only the orderings required by data flow"
(Wikipedia, *Sea of nodes*). Instructions are not pinned to basic blocks. They "float"
in a *sea* constrained only by their data, control, and memory dependency edges, and a
concrete instruction order/CFG is only recovered late, during scheduling. This is the
opposite of a classical CFG-of-basic-blocks IR (e.g. LLVM), where every instruction has
a fixed position in a fixed block from the start.

It is one of the most consequential industrial IRs: it is the core IR of **HotSpot's C2
("server") JIT** — running, per the SeaOfNodes/Simple project, "literally trillions of
times daily" — and of **GraalVM**, **libFIRM**, and historically **V8's TurboFan**. V8
has since publicly retreated from it (see *Weaknesses & criticisms*).

## Core Model (nodes / edges / regions)

Every computation is a **Node** (the paper's "vertex"), labeled with an opcode. Nodes
have **ordered inputs but unordered outputs** (the paper: "ordered inputs ... unordered
outputs"). An SSA value corresponds one-to-one with a node, so SSA names are implicit —
"each SSA value corresponds to a node and vice versa" (libFIRM, *Introduction*).

Edge / node kinds (terminology from the paper, C2, Graal, and the Simple project):

- **Data edges** — a node's input that consumes the *value* produced by another node.
  In Graal's visualizer these are drawn as "thin green" edges (Chris Seaton, *Basic
  Graal Graphs*).
- **Control edges** — impose ordering on nodes with no value dependency, modeling where
  control flows; in Graal drawn "thick red." A node's **control input lives at input
  index 0** in C2: per the OpenJDK *C2 IR Graph and Nodes* wiki, "every node has a
  control input that has index 0 ... referenced ... as `node->in(0)`" (it may be null
  for nodes not dependent on control).
- **Region node** — a control-flow **merge point** (the join of an `if`/loop). It takes
  one control input per incoming control path. Graal calls the equivalent a **Merge**
  node, with **Begin/End** nodes delimiting control regions.
- **Phi node** — a classic SSA φ. It selects a value based on which control predecessor
  was taken, and is **tied to a Region**: its i-th data input corresponds to the
  Region's i-th control input. Seaton notes a Phi is "not a real instruction" — it
  exists only to merge values and is resolved during scheduling/register allocation.
- **Start node** — the unique entry; in the paper it *produces* the initial program
  state (including initial memory). libFIRM: the **Start** and **End** nodes are "the
  source and sink for all data and control flow."
- **Stop / Return / End node** — the unique exit/sink; consumes the final control and
  memory state.
- **Proj (Projection) node** — projects one component out of a node that produces a
  **tuple** of results. E.g. a call or arithmetic-with-overflow or the Start node
  produces a tuple, and Projs pull out the individual control/value/memory pieces
  ("Proj nodes ... project specific outputs from nodes with multiple results", eme64).
  An `If` produces two control Projs (true/false).

Because outputs are unordered and nodes are not in blocks, the graph is a true *sea*:
the only structure is the dependency edges themselves.

## How control & effects/memory are represented

**Control** is represented by the control edges plus Region/Phi/Proj/Start/Stop. The
paper deliberately uses "the same vertex and edge structures" for the CFG as for data —
control flow is just another kind of edge in the unified graph, not a separate side
structure. Branch decisions appear as control Projs off an `If`; merges are Regions;
loop back-edges feed a Region whose Phis carry loop-carried values.

**Memory** is the subtle part. Memory cannot float freely like a pure arithmetic node
(a load can't be reordered past an aliasing store), so memory ordering is threaded
through the graph as an explicit dependency, in SSA form:

- The Simple project (chapter 10) models memory as "a single `Memory` value that
  encapsulates the whole of available memory; `Start` produces memory and `Return`
  consumes memory." **Store** nodes take a memory slice + ptr + field + value and
  **produce a new memory slice**; **Load** nodes take a memory slice + ptr + field and
  return a value. This is SSA-of-memory: each store creates a new memory version.
- Where control merges, memory needs a **memory Phi** (a Phi whose data type is memory),
  exactly mirroring value Phis. This is how distinct memory states from two branches are
  merged into one.
- **Alias classes / equivalence classes**: the Simple project splits memory by
  struct+field into integer-keyed alias classes — "Memory in different alias classes
  *never* aliases; memory in the same alias class *always* aliases." Each class is
  threaded as an independent memory edge ("blue edges show the threading of a memory op
  via the equivalence aliasing"), so non-aliasing loads/stores are independent and can
  be reordered, while same-class ops stay serialized. This enables store-after-store
  elimination and load-after-store forwarding as ordinary peepholes.
- **Anti-dependence edges**: to keep a load from being scheduled after a later store to
  the same location, the IR adds anti-dependence (a.k.a. precedence) edges from the load
  to the store so the scheduler respects read-before-write ordering even though there is
  no value dependency.
- **libFIRM** models side effects with a dedicated **memory mode "M"**: memory-affecting
  nodes consume and produce an `M` value, threading a memory dependency chain through
  the graph (its docs describe "a novel concept to model side effects"; it implements
  "most of the concepts described by Cliff Click").

The key design tension — which directly motivated V8's retreat — is that *the more
operations touch memory/effects, the more the floating "sea" collapses back toward a
fixed CFG-like chain* (see below).

## Optimization approach

The thesis title — *Combining Analyses, Combining Optimizations* — is the agenda. Click
argues that running optimizations as separate sequential phases suffers a
**phase-ordering problem**: each pass makes pessimistic assumptions about what the
others already did, so opportunities are missed. The thesis presents a **monotone
analysis framework** that *combines* **Conditional Constant Propagation**,
**Unreachable (Dead) Code Elimination**, and **Global Value Numbering / Global Congruence
Finding** into one optimistic fixpoint that finds strictly more facts than any sequence
of the separate passes. The combined framework's iterative algorithm runs in **O(n²)**
time, and the thesis also presents an **O(n log n)** algorithm for the same combined
optimizations (Rice/ACM TOPLAS abstract of *Combining Analyses, Combining Optimizations*). The unified data+control graph is what
makes this practical: constant propagation and dead-code elimination operate on the same
nodes and edges.

Day-to-day, optimization in Sea of Nodes is driven by two mechanisms:

- **Peephole / `idealize()`**: each node knows how to rewrite itself into a simpler
  equivalent subgraph (constant folding, algebraic identities like `x*1 → x`,
  `x+0 → x`, strength reduction, removing trivial Phis). C2 calls this `Node::Ideal` /
  `Node::Identity`; the Simple project calls it `idealize()` / peephole. Applied to
  fixpoint over a worklist, local peepholes compose into global effects.
- **Global Value Numbering (GVN)**: a hash table keyed on (opcode, input nodes)
  deduplicates congruent nodes. Because the graph is SSA and nodes are value-identified,
  GVN is essentially free CSE that works globally rather than per-block. C2 runs it
  iteratively as **IGVN (Iterative GVN)** with a worklist: when a node changes, its
  users are re-queued (eme64: `apply_ideal` "tries to construct a more ideal subgraph";
  IGVN runs with `can_reshape` enabled). GVN can momentarily create dominance
  violations, which **GCM later repairs by re-scheduling**.

## Code generation / lowering

Because nodes float, the compiler must eventually pick a concrete order — this is
**Global Code Motion (GCM)** (Click & Cooper, PLDI'95). GCM operates over the dominator
tree and loop-nesting forest:

1. **Pinned vs floating.** Control nodes (Region, Phi, branches, calls, anything with a
   control input) are *pinned* to a block; pure data nodes *float* (Simple ch.11:
   "Control nodes ... are immovable"; floating data nodes "do not have a control input").
2. **Schedule early.** Walk dependencies; place each floating node in the **earliest
   legal block = the deepest (in dominator depth) block that is dominated by all of its
   inputs' blocks** ("the first control block where they are dominated by their inputs").
   This is the highest the node can possibly go.
3. **Schedule late.** Compute the **lowest common ancestor (LCA) in the dominator tree
   of all the node's uses** (for a Phi input, the use is attributed to the corresponding
   predecessor block). This is the latest legal block.
4. **Pick the best block.** Walk the dominator path from the late block (LCA) up to the
   early block and choose the block with the **shallowest loop nesting depth** (ties
   broken by greatest dominator depth) — Simple ch.11: "Least loop depth first, then
   largest idepth." This hoists invariant code out of loops and sinks code into the
   branch that actually needs it, *for free*, as a side effect of scheduling.
5. **Recover the CFG.** The control subgraph already encodes the block structure: "the
   Basic Block structure of the CFG can be easily constructed by recognizing that
   certain control nodes start a Basic Block, whereas certain others end a BB" (Simple
   ch.11). Now there is an ordered CFG of basic blocks again.

In **HotSpot C2** the back end then runs (eme64; HotSpot glossary):

- **`PhaseCFG`** — the scheduler that builds the CFG of blocks from the ideal graph (GCM
  + local scheduling), ordering nodes within each block.
- **Matcher** — maps the machine-independent ideal graph to a machine-dependent **Mach
  graph** (Mach nodes) via a bottom-up rewrite / instruction-selection grammar.
- **`PhaseChaitin`** — **Chaitin/Briggs-style graph-coloring register allocation**: build
  the interference graph, color it with the available machine registers, and when it is
  not colorable, insert spill/reload code and retry (with coalescing).

So Sea of Nodes is high-level and target-independent right up until GCM; *the CFG and
fixed instruction order are an output of compilation, not an input.*

## Compile-time evaluation / partial evaluation

The IR itself is not a partial-evaluation system, but constant evaluation falls out
naturally. The combined **Conditional Constant Propagation** in the thesis is an
optimistic constant-folding lattice fused with unreachable-code elimination, so
constant arms of branches make the other arm unreachable and vice versa, iterated to a
fixpoint — strictly more than separate passes. Peephole/`idealize()` performs ordinary
**constant folding** on each node as the graph is built and re-optimized. Notably,
**GraalVM** takes partial evaluation furthest: Truffle uses Graal's Sea-of-Nodes graphs
as the substrate for **partial evaluation of language interpreters** (the first Futamura
projection), specializing an interpreter against a guest program into an optimized
Sea-of-Nodes graph — but that is a Graal/Truffle capability layered on the IR, not part
of Click's original design. (Confidence: high for C2/Simple constant folding; the
Truffle-PE link is well known but I did not re-fetch a Truffle primary source here.)

## Strengths

- **Unified model**: one graph for data + control (+ memory) means analyses compose;
  constant propagation, DCE, and GVN run on the same structure (thesis thesis statement).
- **Implicit, always-valid SSA**: a value *is* a node, so SSA never needs separate
  maintenance; GVN/CSE is global and nearly free.
- **Optimization for free via scheduling**: GCM gives loop-invariant code motion and
  code sinking as a *consequence* of early/late scheduling rather than as bespoke passes.
- **Aggressive reordering**: relaxing total order to partial order exposes more freedom
  to the optimizer than a fixed-block CFG.
- **Proven at scale**: C2 and Graal optimize trillions of executions; libFIRM ships a
  full optimizing C backend "exclusively on a graph-based SSA representation ... until
  emission of assembly code."

## Weaknesses & criticisms

The most precisely documented retreat is **V8's**, in the official post *"Land ahoy:
leaving the Sea of Nodes"* (v8.dev). V8 replaced the Sea-of-Nodes **TurboFan** with the
CFG-based **Turboshaft** for the whole JS and WebAssembly back ends. Stated reasons:

- **Hard to read / debug**: "Manually/visually inspecting and understanding a Sea of
  Nodes graph is hard"; graphs degrade into a "messy soup of nodes."
- **Effect chains are error-prone**: TurboFan used **control edges** ("ordering on nodes
  that don't have value dependency") and a single **effect chain** ("ordering on nodes
  that have side effects ... splits on branches, merges when control flow merges"). In
  practice engineers juggled "**3 different effect chains**" in one place, "got it wrong
  initially," and only found the bug "after a few months."
- **The float is largely illusory for JS**: because JS has so many side effects, "in
  practice the control nodes and control chain always mirror the structure of the
  equivalent CFG" — i.e. "SoN is just CFG where pure nodes are floating." Memory ops did
  not float freely as the theory promised.
- **Scheduling is costly and fiddly**: deduplicated pure nodes have to be **re-duplicated**
  by the scheduler onto the paths that need them; it is "hard to introduce new control
  flow" during lowering; and "it's hard to figure out what is inside each loop,"
  hampering loop peeling/unrolling.
- **Poor cache behavior & wasted visits**: nodes "visited 3 times but only lowered once,"
  "changed only once every 20 visits"; Sea of Nodes had "about **3 times more L1 dcache
  misses**" on average and "up to **7 times more** in some phases."
- **Net result**: with Turboshaft (CFG), "**compile time got divided by 2**," the CFG
  load-elimination phase was "**up to 190 times faster**," and the compiler code became
  "a lot simpler and shorter" with "much easier" debugging.

Turboshaft instead uses a **CFG-based SSA graph**: a `Block` is "a basic block
containing a linear sequence of operations, ending in a control operation" and the
`Graph` owns `Block` and `Operation` objects (DeepWiki, *Turboshaft*). Operations have a
fixed in-block order — the opposite of floating.

Beyond V8, the general criticism is that Sea of Nodes trades a familiar, locally
inspectable instruction stream for a global graph that is harder to print, diff, and
reason about, and whose benefits are largest for **numeric/low-level code with few side
effects** and smallest for **effect-heavy dynamic languages**.

## Codebase / complexity notes

- **SeaOfNodes/Simple** — the canonical teaching implementation by Click et al., **~99%
  Java**, organized as self-contained chapters: ch.1–8 build literals→arithmetic→SSA→
  control flow→loops; **ch.9 GVN + iterative peephole to fixpoint**; **ch.10 memory
  effects / memory edges / alias classes / structs**; **ch.11 Global Code Motion
  scheduling**; ch.12–18 floats/arrays/functions; **ch.19–21 instruction selection,
  register allocation, ELF encoding**. Each chapter is small and readable — the project
  exists precisely because Sea of Nodes "is not taught in compiler classes."
- **HotSpot C2** — the production reference; the ideal-graph optimizer, `PhaseCFG`,
  `Matcher`/Mach graph, and `PhaseChaitin` allocator together are tens of thousands of
  lines of intricate C++ and are widely considered among the hardest parts of the JDK to
  maintain.
- **libFIRM** — a standalone C library ("graph based intermediate representation and
  backend for optimising compilers") with a full set of analyses and a real assembly
  back end; a practical mid-size implementation of the model.

(Exact LOC numbers for C2 and libFIRM were not verified from a primary source — see
*Open questions*.)

## Lessons for a NEW unified IR

1. **A unified data+control(+memory) graph genuinely enables combined analyses** (CCP +
   DCE + GVN as one fixpoint). If the new IR wants this, it must keep control and data in
   one structure, not bolt control on as a side table.
2. **Floating is only as good as your effect model.** V8's hard lesson: with pervasive
   side effects, the floating sea collapses to a CFG anyway, so you pay graph complexity
   for little reordering benefit. Consider making the *common, effectful* case as cheap
   and inspectable as a CFG, and let *pure* nodes float — or keep an explicit block
   structure and float selectively.
3. **Make the IR printable/inspectable and diffable from day one.** "Hard to visualize"
   and "hard to debug" were first-order reasons for V8's retreat — tooling is not
   optional for a graph IR.
4. **Memory needs first-class SSA modeling**: memory Phis + alias/equivalence classes +
   anti-dependence edges. This is what makes load/store optimization fall out as
   peepholes; design it in, not after.
5. **Scheduling is where the cost hides.** GCM's early/late + loop-depth selection is
   elegant and gives LICM/sinking for free, but the scheduler must also *re-duplicate*
   shared pure nodes and reconstruct loop membership — budget for that complexity, and
   make loop nesting cheap to query.
6. **Decide the boundary** between "high-level optimizable graph" and "lowered
   CFG/machine IR." C2 keeps Sea of Nodes until GCM, then matches to a Mach graph; a
   clean phase boundary keeps the float where it pays and the linear order where codegen
   needs it.
7. **Peephole-as-rewrite (`idealize()`) scales** when run to fixpoint with a worklist and
   GVN — a good model for a self-optimizing node taxonomy, provided you handle the
   transient dominance violations (re-schedule to repair).

## Sources

- [A Simple Graph-Based Intermediate Representation — Click & Paleczny, IR'95 (ACM)](https://dl.acm.org/doi/10.1145/202530.202534) — the originating paper; nodes/edges, Region/Phi, Start/Stop, peephole. (ACM landing; Oracle's PDF mirror was 403 during research.)
- [Combining Analyses, Combining Optimizations — Click, PhD thesis (Semantic Scholar)](https://www.semanticscholar.org/paper/Combining-analyses,-combining-optimizations-Click-Cooper/9834a7794acc843e3f3d471275b70c6664a6fd9f) — combined CCP + dead-code + GVN framework; the IR appendix.
- [Global Code Motion / Global Value Numbering — Click, PLDI'95 (ACM)](https://dl.acm.org/doi/10.1145/207110.207154) — the GCM early/late scheduling + GVN algorithms.
- [roife — notes on Click 1995 GCM/GVN](https://roife.github.io/posts/click1995/) — detailed early/late schedule, LCA, loop-depth block selection (corroborated).
- [Land ahoy: leaving the Sea of Nodes — V8 blog](https://v8.dev/blog/leaving-the-sea-of-nodes) — primary source for V8's retreat: effect chains, scheduling cost, 2× compile time, 190× load-elim, dcache misses.
- [Turboshaft Optimizing Compiler — DeepWiki (v8/v8)](https://deepwiki.com/v8/v8/3.3-turboshaft-and-turbofan:-optimizing-compilers) — Turboshaft's CFG/Block/Operation model that replaced TurboFan.
- [SeaOfNodes/Simple — teaching implementation (GitHub)](https://github.com/SeaOfNodes/Simple) — chapter structure; ch.9 GVN, ch.10 memory, ch.11 GCM; ~99% Java.
- [SeaOfNodes/Simple chapter 11 README — GCM scheduling](https://github.com/SeaOfNodes/Simple/blob/main/chapter11/README.md) — pinned vs floating, schedule early/late, loop-depth selection, CFG recovery.
- [Introduction to HotSpot JVM C2 JIT Compiler, Part 3 — eme64](https://eme64.github.io/blog/2025/01/23/Intro-to-C2-Part03.html) — ideal graph node kinds, Proj nodes, IGVN worklist / `can_reshape` / `apply_ideal`, and the PhaseCFG (Scheduler) / Matcher (Mach graph) / PhaseChaitin (Regalloc) lowering pipeline. (Verified: this post does NOT state "control input at index 0" — that detail is sourced to the OpenJDK wiki below.)
- [C2 IR Graph and Nodes — OpenJDK HotSpot Wiki](https://wiki.openjdk.org/display/HotSpot/C2+IR+Graph+and+Nodes) — node inputs are an ordered collection; "every node has a control input that has index 0," referenced as `node->in(0)` (null when not control-dependent).
- [Combining Analysis, Combining Optimizations — Click thesis (Rice Scholarship repository)](https://scholarship.rice.edu/handle/1911/96451) — confirms Rice 1995, chair Keith D. Cooper, the combined CCP + Unreachable/Dead-Code + Global Congruence/GVN framework, and the **O(n²)** iterative / **O(n log n)** complexity bounds.
- [Understanding Basic Graal Graphs — Chris Seaton](https://chrisseaton.com/truffleruby/basic-graal-graphs/) — data (green) vs control (red) edges, Begin/End/Merge, Phi, framestates, floating nodes.
- [Firm — Introduction (libFIRM)](https://libfirm.github.io/Introduction) — Block/Start/End/Proj nodes, control edges, "novel concept to model side effects" (mode M).
- [Sea of nodes — Wikipedia](https://en.wikipedia.org/wiki/Sea_of_nodes) — partial-order definition, adopters (HotSpot, Graal, libFIRM, TurboFan), 2022 V8 conclusion.

## Open questions / uncertainties

- I could not fetch the **primary PDFs** of the IR'95 paper (Oracle mirror 403), the PLDI'95
  GCM paper, or the thesis full text directly; node/edge details are corroborated across
  Wikipedia, the Simple project, C2 blog, Graal, and libFIRM, but exact original wording
  of some node definitions is inferred, not quoted from the source PDF.
- The thesis complexity bounds (**O(n²)** iterative, **O(n log n)**) and the combined set
  of analyses (Constant Propagation, Unreachable/Dead Code Elimination, Global Congruence
  Finding / Global Value Numbering) are confirmed by the official Rice repository and the
  ACM TOPLAS abstract; the surrounding full thesis PDF was not fetched line-by-line.
- **Anti-dependence / precedence edge** mechanics are described from general knowledge +
  the Simple memory-threading model; I did not confirm C2's exact precedence-edge API
  from a primary source.
- **LOC / codebase size** for C2 and libFIRM are characterized qualitatively; I have no
  verified line counts.
- The **GraalVM/Truffle partial-evaluation** connection is widely documented but I relied
  on background knowledge here rather than a freshly fetched Truffle primary source.
- libFIRM's **mode "M"** memory model is stated from general knowledge plus its docs'
  "novel concept to model side effects"; the fetched page did not spell out mode M.
