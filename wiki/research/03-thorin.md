# Thorin (AnyDSL) — A Graph-Based Higher-Order Intermediate Representation

## Overview

Thorin is a **higher-order, functional intermediate representation based on
continuation-passing style (CPS)** developed by Roland Leißa, Marcel Köster, and
Sebastian Hack at Saarland University and presented at CGO 2015 ("A Graph-Based
Higher-Order Intermediate Representation"). Its defining thesis is stated in the
abstract: imperative IRs (LLVM, JVM bytecode) "cannot natively represent crucial
functional concepts (like higher-order functions)," while functional IRs "employ
an explicit scope nesting, which is cumbersome to maintain across certain
transformations." Thorin's contribution is to keep CPS / higher-order functions
as first-class citizens *but to abandon explicit scope nesting in favor of a
dependency graph*. The result is "equally well-suited to represent imperative as
well as functional programs."

Thorin is the IR of the **AnyDSL** system (OOPSLA 2018, "AnyDSL: A Partial
Evaluation Framework for Programming High-Performance Libraries"). AnyDSL has
three parts (Figure 1 of the OOPSLA paper): the front-end language **Impala**
(a Rust dialect), the IR **Thorin** (which also performs partial evaluation, "PE"),
and a runtime. The 2018 work extended the 2015 IR with a **simple online partial
evaluator** driven by annotations ("filters"), making Thorin a practical
zero-cost-abstraction / staging engine that targets CPUs (with vectorization via
RV and threads via TBB/C++ threads) and GPUs (CUDA, OpenCL, NVVM, AMDGPU).

The implementation lives at `github.com/AnyDSL/thorin` — C++ (~97.6% C++,
LGPL-3.0). The line of work continues as **Thorin 2 → MimIR** ("MimIR is my
Intermediate Representation"), published at POPL 2025, which generalizes the
graph IR to the Calculus of Constructions with dependent types. This document
focuses on the original Thorin (CGO 2015) and the AnyDSL PE layer (OOPSLA 2018),
with notes on the MimIR successor.

## Core Model (nodes / edges / regions)

**Everything is a node in a graph; every reference is an edge.** Quoting the
paper: "In Thorin, each value is a node in a graph and every reference to a value
is an edge to this node. Therefore, Thorin does not require explicit scope
nesting." Names in the paper's examples "have no semantic meaning"; they are
purely for human readability. This makes Thorin "graph-based like progressive
imperative IRs (e.g. HotSpot or Firm)," yet higher-order.

There are exactly **three kinds of definitions** (the repo's `Def` base class has
three concrete subclasses, per the Programming Guide):

1. **Continuations / lambdas** (`Continuation`, historically `Lambda`). A function
   that "introduces parameters and in turn solely consists of a *call*." Because
   Thorin uses CPS, a call **does not return**; consequently function types
   `fn(T)` (written `fn(t)` in the formalism) **have no return type**. All control
   flow — "jumps to basic blocks, function calls, generators, exceptions, etc." —
   is uniformly a continuation. Continuations are the only **mutable** nodes: per
   the Programming Guide, "lambdas are always mutable. You can always modify their
   contents" (i.e. retarget their jump/body).

2. **Primops** (`PrimOp`). "A primitive operation (primop) is a simple operation
   which references other definitions to produce a new value" — arithmetic,
   comparisons, `load`/`store`, tuple/struct/array extract/insert, casts, etc.
   Primops "induce an acyclic, functional data-flow graph which allows for simple
   code analyses and transformations." Primops are **immutable** and
   **hash-consed**.

3. **Parameters** (`Param`). "A reference to a parameter projects the corresponding
   type from the function's signature." Params are created automatically when a
   lambda is instantiated and are immutable.

**Edge kinds** (from the CGO paper's figures): references can point to *functions*
(dotted edges), *function parameters* (dashed edges), or *primops* (solid edges).
A function body `b = e0(e)` is "a call to `e0` with arguments `e`," where the
callee and each argument is an edge to another definition.

**The `World`.** In the implementation, the central object is the `World` class:
a **factory** that constructs functions, primops and types (constructors of the
concrete classes are private; you must go through `World`). `World` "contains the
entire program," performs **constant folding, simple CSE and simple UCE on the fly
during construction of the IR nodes" (the OOPSLA paper notes this "vastly
decreases the number of iterations in the fixed-point loops"). Factory methods
return `Def`, not the concrete type, because on-the-fly optimization may rewrite
what you asked for — "never assume anything about `def`."

**Sea-of-nodes.** The official docs state Thorin "embraces the sea-of-nodes
paradigm. … each value is represented as a node [in] a graph." Primops are
**floating**: their data dependencies are the only constraints on where they may
be placed. A separate **scheduling** step (see Code Generation) decides concrete
placement.

## How control & effects / memory are represented

**Control flow = continuations.** Thorin exploits "the long-known correspondence
between SSA and CPS" (Kelsey; Appel, "SSA is functional programming"). In SSA, a
basic block with φ-functions corresponds in CPS to a local function whose
parameters are the φ's; the φ's arguments become the call arguments of that
function. Figure 2 of the CGO paper makes this concrete: SSA `head` block with
`r1 = φ(r0, r2)`, `i1 = φ(i0, i2)` becomes a Thorin function `head(i, r)` jumped
to with `head(2,1)` and `head(i+1, i*r)`. Conditional control flow uses a
distinguished `branch` function of type `fn(bool, fn(), fn())`.

**Scoping without nesting — "dependent" vs "free" handled by data dependency.**
This is Thorin's signature idea. Functions are **not explicitly nested**; nesting
is "implicitly given by data dependencies between functions. If a function `f`
uses a variable defined in another function `g`, `f` is implicitly nested in `g`."
Thorin is therefore **"blockless"** — it "abolishes block floating and sinking."
The motivating problem (Figure 3): in classic CPS, adding a new branch `z` to a
function `c` forces *let/block floating* to repair the scope so `c` is visible
from both `b` and `z`; the reverse is *block sinking*. These pervasive
bookkeeping transforms "occur, for example, during jump threading." Thorin
eliminates them entirely because there is no syntactic nesting to repair.

It also solves **name capture** for free. Naïve β-reduction
`λa.(λx.(λa.x)) a → λa.λa.a` is wrong because the inner `a` captures the outer.
GHC's inliner renames during β-reduction (compile-time + implementation cost). In
a graph "each use directly points to its definition," so β-reduction "becomes
straightforward … It is not necessary to rename anything."

**The Scope and liveness.** Many transforms still need to know, *from the
viewpoint of a function `e`, which functions are live*. Thorin computes this with
a **liveness analysis**: `scope_p(λe) = { λ | p ⊢ λe ▷ λ live }`. `L-Param` finds
**direct** dependencies (a function references another's parameter); `L-Abs`
**transitively** finds **indirect** dependencies (e.g. `else` depends on `fac`
not by using `fac`'s params directly, but because `else` invokes `head` which
does). `λe` is the **entry function** of that scope. In the codebase this is
`analyses/scope.{h,cpp}`, with `free_defs`, `cfg`, `domtree`, `looptree`,
`schedule` built on top.

**Memory and effects.** In the formal big-step semantics, "memory operations or
side-effects are explicitly tracked by a **functional store**" (Steensgaard's
"sparse functional stores," Strachey). In the implementation, memory is threaded
explicitly as a value of a **`mem` type**: `load`/`store` primops consume and
produce a `mem` token, giving a pure data-flow encoding of side-effect ordering.
Crucially, before PE the OOPSLA paper runs **SSA construction** (Braun et al.
2013) that "translate[s] all mutable variables whose addresses are not taken into
functional variables that are non-destructively updated" — and "unlike common
imperative IRs like LLVM, this is also performed for compound data structures:
tuples, structs, and arrays." This lets the partial evaluator avoid reasoning
about side effects for most variables. Destructive updates on heap data or
address-taken variables "are ignored by the evaluator."

**Mutual recursion.** Recursion is detected via a **higher-order call graph**: one
node per Thorin function, an edge from `λ` to each function occurring in its body;
a **strongly connected component (SCC) forms a recursion**. The paper introduces
precise nomenclature because textbook tail-recursion definitions "do not apply to
CPS programs": *recursive call* (any call within an SCC), *simple recursive call*
(within the callee's scope), *mutual recursive call* (all others), *static
parameter* (a parameter unchanged within an SCC), *first-order recursive call*
(uses only static parameters as higher-order arguments), and *loop* (an SCC of
only zeroth-order recursive calls). For mutual recursion (e.g. `iseven`/`isodd`),
lambda mangling considers **all functions in one SCC simultaneously** and applies
the known substitution maps `M_e`, `M_o` together, so recursive cross-calls can
be rewritten to the new specialized functions — yielding **mutual
tail-recursion elimination** (the two functions fuse into one loop).

## Optimization approach

The central transformation primitive is **lambda mangling** — "a combination of
lambda lifting and dropping." Reading Figure 5 left-to-right is **lambda lifting**
(a free variable `ret` of `g` becomes a new parameter `ret'` of the lifted `g'`);
right-to-left is **lambda dropping** (eliminate a parameter and reintroduce it as
a free variable / substitute a constant). Mangling does both **simultaneously**:
`Algorithm 1 mangle(p, λe, t, M)` clones the entry's scope, allocates a fresh
label for every function in `scope_p(λe) \ {λe}`, and rewrites all bodies under a
substitution map `M` (e.g. `M = {a↦a_d, b↦3}` to specialize `pow` for `b=3`).
Because Thorin is blockless, "we do not have to move `g`/`g'` out of/into `f`" —
the original lifting (Johnsson) and dropping (Danvy–Schultz) algorithms required a
block-floating/sinking pass that Thorin's representation makes unnecessary. The
authors stress mangling produces "one new generalized and/or specialized
function … not connected to the rest of the program"; **other passes orchestrate
mangling** and reconnect call sites.

Lambda mangling **subsumes classic optimizations**, demonstrated in the paper:

- **Tail-recursion elimination** (drop a helper's static params → it becomes a loop).
- **Loop peeling** (drop with the initial values; recursive call can't be rewritten).
- **Loop unrolling** (drop with `i+1`, `r*i`).
- **Static argument transformation** (de Santos) — generalized to whole SCCs.
- **(Partial) inlining** — partial inlining "is the crucial step in closure elimination."

On top of mangling, AnyDSL's **online partial evaluator** (OOPSLA 2018) reduces to
"specializing continuations and performing standard optimizations (constant
folding, algebraic simplifications, folding branches)." It is hooked into Thorin's
optimizer in a **fixed-point loop** (~50 LoC of glue): run common optimizations
incl. UCE, then run the evaluator for a constant number of steps, repeat until the
evaluator has nothing left to do. The evaluator **always inlines a sole call** to a
function (so the programmer's filters behave predictably). To avoid **induced
divergence**, the evaluator tracks specialized call sites and, on re-specializing,
inserts a call to the already-specialized function instead (Cook & Lämmel /
Futamura technique).

## Code generation / lowering

Lowering targets a CFG / SSA representation (and ultimately LLVM). The pipeline:

1. **Classify functions** by order/shape: *BB-like* (first-order function = basic
   block), *returning* (second-order with exactly one first-order "return"
   parameter), *top-level* (scope has no free variables), and **bad** (none of
   the above). A scope that uses no bad functions is in **Control Flow Form (CFF)**.

2. **`lower2cff` / closure elimination.** `Algorithm 2` repeatedly finds bad
   functions and `mangle`s their uses to specialize away higher-order arguments,
   removing one use per specialization, until no bad functions remain. The
   guarantee: Thorin **eliminates *all* closures in CFF-convertible programs** —
   strictly more aggressive than the k-CFA-based super-β-inlining of SML/NJ etc.,
   because it *specializes* each call site rather than relying on inline
   heuristics. (Caveat: by the halting problem it is undecidable in general whether
   all bad functions can be removed; `lower2cff` is only guaranteed to terminate
   for non-recursive and first-order-recursive uses.) In the source this is the
   `transform/closure_conversion.{h,cpp}` + `transform/mangle.{h,cpp}` machinery;
   a genuine heap **closure conversion** path also exists for the residual cases.

3. **Kelsey's algorithm** translates the resulting CFF program to SSA:
   returning functions → ordinary functions (the first-order param acts as
   "return"); BB-like functions → basic blocks (each parameter → a φ-function,
   predecessors' arguments supply the φ's operands); calls to returning functions →
   normal calls (the continuation's parameter is the result value). This step "is
   only of syntactic nature." Impala constructs Thorin directly from the AST using
   the **simple/efficient SSA-construction algorithm** (Braun et al. 2013), so no
   dominance-tree computation is needed up front.

4. **Scheduling the sea-of-nodes.** Floating primops must be placed into concrete
   functions/blocks before emission. `analyses/schedule.{h,cpp}` performs **code
   placement**: data dependencies constrain where a primop may go; the scheduler
   (early/late style placement à la Click) commits each primop to a function. This
   produces a textually representable, CFG-shaped program.

5. **Backend emission.** `be/codegen.{h,cpp}` + `be/emitter.h` drive target
   backends. The repo's `src/thorin/be` contains backends **`llvm`** (with NVVM /
   AMDGPU / OpenCL / CUDA emission paths under the LLVM umbrella), **`c`** (C/CUDA/
   OpenCL/HLS source emission), **`spirv`**, **`shady`** (Vulkan), and **`json`**.
   Accelerator support (OOPSLA §5) is exposed as **compiler-known higher-order
   intrinsics**: `parallel` lowers to a runtime call `anydsl_parallel_for`
   (TBB or C++ threads) after **closure-converting** the body; `vectorize` triggers
   the **RV** region vectorizer after **lambda-lifting** the body to remove free
   variables (loop counter marked continuous/aligned, former free vars marked
   *uniform*); `cuda`/GPU intrinsics lambda-lift the body, build a new module with
   all transitively used functions (so it has no free functions), emit a CUDA/PTX
   kernel, and rewrite the call to `anydsl_launch_kernel`. Pointer **address
   spaces** (`global`, `shared`, `private`, generic) are part of the pointer type.

## Compile-time evaluation / partial evaluation

PE is the headline feature of AnyDSL. It is an **online partial evaluator** (no
prior binding-time analysis): "specializes a program on the fly and without prior
analysis." The OOPSLA paper argues online PE beats offline PE here because a BTA
"might leave too much of the code dynamic" — e.g. an online evaluator folds
`f(23, x)` so that `if s==23 {42} else {d}` collapses to `use(42)`, which a
straightforward BTA cannot (it only knows `s` is static, not its value), and
because higher-order BTA needs an imprecise, costly CFA.

**Filters** (borrowed from Consel's **Schism**) give the programmer control. The
annotation `@(e)` "delimits PE filters" — a Boolean expression on a function's
signature. Concrete syntax (from the papers/docs):

- `fn @(?n) pow(x: i32, n: i32) -> i32 { … }` — the filter `?n` says "specialize
  calls to `pow` to **all** arguments **if `n` is a constant**." `?param` tests
  staticness of a parameter; filters can be conjoined: `@(?a & ?b & ?s)`.
- A bare `@` before a function (`fn @range(...)`) or a parameter (`@b: i32`) is
  shorthand forcing specialization of that function / parameter.
- `@@f(a)` forces specialization at a **call site** (inline this application).
- `$expr` is the **opposite**: it **prevents** the evaluator from specializing /
  inlining at that point (e.g. `unroll_step($a, b, …)` blocks whole-call inlining
  so only `b,s,f,return` get specialized, producing a residual counting loop).

**Automatic annotations (default policy).** Because partially evaluating
higher-order functions w.r.t. their function arguments is essential for
performance (otherwise `compute`'s `op` "ends up having to call a closure for
every pixel"), Impala **automatically annotates every higher-order parameter**
*unless* (1) it is first-order, (2) its continuation is a second-order
continuation with no other higher-order params, and (3) its continuation has no
free variables. This default "transforms the program to control-flow form … the
resulting residuum will not need costly closures at runtime." Hence the case
studies "hardly need explicit filters."

**Static arguments & recursion.** When specializing `f→f'`, "all functions nested
inside its body are rewritten … Free variables … stay as they are." Recursive uses
are by default left free, but **"if in a recursive call, all specialized arguments
are static, the partial evaluator will rewrite this call to recursively call the
specialization instead."** This is how `unroll_step` either becomes a residual SSA
loop (when only the body/bounds are static) or fully unrolls (when the counter is
static). Termination of PE under a filter relies on the filter only depending on
parameters that drive termination (e.g. `pow`'s `n`).

**Relation to staging / generic programming.** AnyDSL realizes the **first
Futamura projection** as a *shallow* DSL embedding (tagless-interpreter style,
Carette et al.): the embedded program is "not represented as a tree of data but
as a tree of closures," and partially evaluating that closure tree "leads to the
same code a syntax-directed compiler would have emitted." Unlike MetaOCaml
quasiquotation, Terra, or Scala/LMS (Figure 3 compares all four `pow`
implementations), AnyDSL needs **no quotation, no `Rep[T]` types, and no separate
DSL compiler** — the same `pow` reads like ordinary code, annotated with one
filter. The trade-off (vs deep embedding / LMS): being shallow, "it is not
possible to perform a-posteriori program transformations of the embedded
programs." The PE implementation is tiny: ~200 LoC to instantiate filters per call
site + ~250 LoC for the specializer + ~50 LoC of optimizer glue.

## Strengths

- **Higher-order functions are first-class and zero-cost.** CFF-convertible
  programs have **all closures provably eliminated** — generic `map`/`fold`/
  generators compile to loops matching hand-written C (benchmarks "consistently as
  fast as C programs"; OOPSLA case studies within ~10% of Halide/Embree/OptiX/
  SeqAn).
- **Blockless / scopeless graph** removes a whole class of fragile transforms
  (block floating/sinking, α-renaming for capture avoidance), making
  control-flow-restructuring transforms much simpler.
- **One primitive, many optimizations.** Lambda mangling subsumes TRE, loop
  peeling/unrolling, partial inlining, static-argument transformation.
- **Tiny, predictable PE.** Online + filters → the programmer "precisely
  anticipate[s] what parts of the program are partially evaluated"; the whole
  evaluator is ~500 LoC.
- **Unifies imperative + functional**, and serves as a clean compilation target
  for accelerator code (CPU SIMD via RV, threads via TBB, GPUs via CUDA/OpenCL/
  NVVM/AMDGPU/SPIR-V) through ordinary higher-order intrinsics rather than pragmas.
- **Engineering economy:** Halstead metrics (CGO 2015, Table 2) show Thorin's
  `mangle.cpp` (**132 SLoC**, difficulty 75, effort 496 757) is markedly leaner
  than LLVM's combined `CloneFunction`/`CodeExtractor`/`InlineFunction`/`LoopUnroll`
  (**1687 SLoC total**, difficulty 207, effort 24 968 883) — about **2.8× lower
  Halstead difficulty** (207/75) and **~50× lower Halstead effort/time-to-program**
  (24 968 883/496 757). And the LLVM passes still lack HOFs and TRE. (Note: the
  ~1687-line figure is the *LLVM four-file total*, not `mangle.cpp`; the present-day
  raw source of `mangle.cpp` in the repo is larger than the 132 SLoC the 2015
  Halstead table reports.)

## Weaknesses & criticisms

- **CPS mental model.** Functions never return; `branch`, `return`, `continue`,
  `break` are all continuations. Even the authors had to invent new
  recursion/tail-call nomenclature because "standard text book definitions for
  tail-recursion do not apply to CPS programs." This is a real cognitive cost for
  contributors and for debugging.
- **Names are non-semantic / debugging is hard.** In the graph "names … have no
  semantic meaning," so mapping optimized Thorin back to source for debugging is
  non-trivial.
- **PE termination is undecidable in general.** `lower2cff` is only guaranteed to
  remove bad functions for non-recursive / first-order-recursive uses; otherwise
  closures may remain. The 2018 paper *abandoned* the 2015 termination heuristic
  ("partially evaluate everything until the first dynamic conditional, resume at
  the post-dominator") because "the effects of its termination heuristics were
  difficult to assess for the programmer" — a documented retreat in favor of
  explicit filters (which can themselves induce divergence if a user annotates a
  recursive call wrongly).
- **Limited effect handling in the evaluator.** Destructive updates on
  heap-allocated / address-taken data "are ignored by the evaluator" — sufficient
  for the case studies but a real limitation.
- **Relies on the LLVM backend** (and external RV vectorizer) for final machine
  code and most low-level optimization; Thorin itself is a higher-level
  optimizer/specializer, not a register allocator/instruction scheduler.
- **Alignment/correctness obligations pushed to the programmer** for vectorization
  ("The programmer is responsible for ensuring that the alignment assumptions
  actually hold").
- **Adoption is narrow** — primarily the Saarland/DFKI research ecosystem
  (Impala, image processing, ray tracing, sequence alignment, HLS). Not used as a
  mainstream production compiler IR.

## Codebase / complexity notes

- Repo `github.com/AnyDSL/thorin`: C++ (~97.6%), LGPL-3.0; on the order of ~6.7k
  commits historically. Core IR: `world.{h,cpp}`, `def.{h,cpp}`,
  `primop.{h,cpp}`, `continuation.{h,cpp}`, `type.{h,cpp}`, `enums.{h,cpp}`.
- `analyses/`: `scope`, `cfg`, `domtree`, `looptree`, `schedule`, `free_defs`,
  `verify`.
- `transform/`: `partial_evaluation`, `mangle`, `closure_conversion`, `importer`,
  `inliner`, `dead_load_opt`, `resolve_loads`, `split_slots`, `flatten_tuples`,
  `hoist_enters`, `lift_builtins`, `codegen_prepare`, `cleanup_world`,
  `hls_channels`/`hls_kernel_launch`, `rewrite`.
- `be/`: `llvm`, `c`, `spirv`, `shady`, `json` plus `codegen.{h,cpp}`, `emitter.h`,
  `runtime.h`, `kernel_config.h`.
- PE size (OOPSLA §4.6): filter instantiation ~200 LoC, specializer ~250 LoC,
  optimizer glue ~50 LoC. Lambda mangling `mangle.cpp` is reported at **132 SLoC**
  in the CGO 2015 Halstead table (Table 2); the ~1687 SLoC figure in that table is
  the *combined* LLVM `CloneFunction`+`CodeExtractor`+`InlineFunction`+`LoopUnroll`
  total, not `mangle.cpp`.
- **Successor: Thorin 2 → MimIR.** The MimIR paper is *"MimIR: An Extensible and
  Type-Safe Intermediate Representation for the DSL Age"* (POPL 2025); MimIR is
  "a pure, graph-based, higher-order intermediate representation rooted in the
  **Calculus of Constructions**," adding dependent types, parametric polymorphism,
  **axioms + normalizers + plugins**, and a graph where "terms and types share one
  graph," with a mutable/immutable split (binders mutable where needed for
  recursion; non-binder expressions immutable + hash-consed). The project was
  *renamed* rather than version-bumped because MimIR's core calculus diverged so far
  from Thorin's that little Thorin-derived code remains. The **"SSA without
  dominance"** framing — "a scopeless IR for higher-order programs based on
  free-variable nesting," where free variables replace dominance and a *nesting
  tree* replaces the dominator tree — comes from a *separate, later* paper, **"SSA
  without Dominance for Higher-Order Programs," Roland Leißa & Johannes Griebler,
  PLDI 2026** (not the POPL 2025 MimIR paper).

## Lessons for a NEW unified IR

1. **Decouple data flow from control flow with floating nodes + a scheduler.**
   Pure, hash-consed, immutable data nodes (primops) over mutable control nodes
   (continuations/blocks), with placement decided by a late scheduling pass, gives
   on-the-fly CSE/constant-folding and trivializes restructuring transforms.
2. **Scope by data dependency, not syntactic nesting.** "If `f` uses something
   defined in `g`, `f` is implicitly nested in `g`." This eliminates block
   floating/sinking and α-renaming for capture — a recurring source of bugs in
   nested functional IRs. Compute scopes on demand via liveness.
3. **Unify control constructs as one node kind.** Calls, branches, returns,
   exceptions, break/continue, loops are all continuations — one mechanism instead
   of many CFG special cases (mirrors SSA↔CPS correspondence).
4. **Make a single, composable rewrite primitive.** Lambda mangling
   (lift+drop+substitute) reproduces TRE, unrolling, peeling, partial inlining and
   specialization; build higher-level passes by orchestrating one well-understood
   primitive rather than many bespoke ones.
5. **Put the user in control of staging via local annotations on the IR, not a
   separate metalanguage.** Filters (`@(?x)`, `@@`, `$`) on ordinary functions are
   far cheaper to learn/implement (~500 LoC online evaluator) than quotation
   (`.<>.`) or `Rep[T]` staging, and a sensible **default policy** (auto-annotate
   higher-order params) means most code needs no annotations.
6. **Thread memory/effects as explicit value tokens** and SSA-construct even
   compound data (tuples/structs/arrays) so a specializer can treat most state
   functionally and skip effect reasoning.
7. **Keep the IR minimal so the partial evaluator is small and predictable** —
   "the only constructs are continuations and primops" was a deliberate choice to
   bound the cases PE must handle.
8. **Know the limits:** prefer explicit, user-anticipatable specialization over
   clever termination heuristics (AnyDSL's own documented retreat); accept that
   full closure elimination is undecidable and design a graceful fallback (heap
   closure conversion) for residual cases; lean on a mature backend (LLVM) rather
   than reimplementing low-level codegen.
9. **If aiming higher (MimIR direction):** a single dependently-typed `Def` node
   unifying terms/types/type-level computation can subsume the three-node design
   while keeping the scopeless graph — at the cost of significantly more
   type-system machinery.

## Sources

- [Leißa, Köster, Hack — "A Graph-Based Higher-Order Intermediate Representation," CGO 2015 (PDF)](https://compilers.cs.uni-saarland.de/papers/lkh15_cgo.pdf) — primary source; defines Thorin, blockless graph, scope/liveness, lambda mangling, CFF, lower2cff, Kelsey lowering. (Full text read.)
- [Leißa et al. — "AnyDSL: A Partial Evaluation Framework for Programming High-Performance Libraries," OOPSLA 2018 (PDF)](https://compilers.cs.uni-saarland.de/papers/anydsl.pdf) — primary source; online PE, filters `@(?x)`/`@@`/`$`, automatic annotations, static arguments, accelerator support, implementation LoC. (Full text read.)
- [AnyDSL project site](https://anydsl.github.io/) — overview of Impala/Thorin/runtime.
- [Thorin page — anydsl.github.io/Thorin.html](https://anydsl.github.io/Thorin.html) — "CPS … sea-of-nodes," `World` factory, three definition kinds.
- [AnyDSL Programming Guide](https://anydsl.github.io/Programming-Guide.html) — `World`/`Def`/`PrimOp`/`Lambda`/`Param`, immutability model, on-the-fly CSE/UCE, scheduling.
- [AnyDSL Impala page](https://anydsl.github.io/Impala.html) — `@` partial-evaluation marker, generator/`for` desugaring.
- [github.com/AnyDSL/thorin](https://github.com/AnyDSL/thorin) — C++ source; verified directory layout (`analyses/`, `transform/`, `be/{llvm,c,spirv,shady,json}`).
- [Leißa et al. — "MimIR: An Extensible and Type-Safe Intermediate Representation for the DSL Age," POPL 2025 (arXiv 2411.07443)](https://arxiv.org/abs/2411.07443) and [mimir.github.io](https://mimir.github.io/) / [github.com/mimir/mimir](https://github.com/mimir/mimir) (the old [AnyDSL/MimIR](https://github.com/AnyDSL/MimIR) now redirects) — successor to Thorin (renamed, not version-bumped); pure graph IR rooted in the **Calculus of Constructions**, dependent types, axioms/normalizers/plugins, terms+types share one graph, mutable/immutable split. Verified: tagline "MimIR is my Intermediate Representation."
- [Leißa & Griebler — "SSA without Dominance for Higher-Order Programs," PLDI 2026 (arXiv 2604.09961)](https://arxiv.org/abs/2604.09961) — *separate, later* paper (NOT the POPL 2025 MimIR paper) that introduces the "scopeless IR / free-variable nesting" framing: free variables replace dominance, a nesting tree replaces the dominator tree.
- [DBLP record for the CGO 2015 paper](https://dblp.uni-trier.de/rec/conf/cgo/LeissaKH15.html) — bibliographic metadata (Leißa, Köster, Hack; CGO 2015, pp. 202–212; verified).
- [Thorin.html (official)](https://anydsl.github.io/Thorin.html) — verified quote: "Thorin embraces the sea-of-nodes paradigm. This means that each value is represented as [a] node [in] a graph."
- [CGO 2015 Halstead Table 2](https://compilers.cs.uni-saarland.de/papers/lkh15_cgo.pdf) — verified figures: `mangle.cpp` 132 SLoC / difficulty 75 / effort 496 757 vs. LLVM `CloneFunction`+`CodeExtractor`+`InlineFunction`+`LoopUnroll` total 1687 SLoC / difficulty 207 / effort 24 968 883 (≈2.8× difficulty, ≈50× effort).

## Open questions / uncertainties

- **Exact scheduler algorithm.** Docs describe "code placement" of floating
  primops and the source has `analyses/schedule.{h,cpp}`; I infer an early/late
  (Click-style) placement but did not read the implementation to confirm the exact
  policy.
- **`mem`-type details.** The functional-store semantics is documented in the
  paper; the precise primop set and how the `mem` token is typed/threaded in the
  current codebase was inferred from the docs, not read line-by-line.
- **Whether `lower2cff` exists as a named file today.** The CGO algorithm is
  `lower2cff`; the current repo exposes `closure_conversion.{cpp,h}` and
  `mangle.{cpp,h}` but no file literally named `lower2cff` — the algorithm may
  have been folded into closure conversion. Not fully verified.
- **`$` operator semantics.** I read `$expr` as "prevent specialization/inlining
  here" from the `unroll_step($a, …)` example; the docs use `@` prominently but I
  did not find an authoritative, standalone definition of `$` beyond that usage.
- **Current maintenance status of Thorin vs MimIR.** MimIR (POPL 2025) is the
  active line; per the MimIR docs the project was *renamed* from Thorin (rather than
  version-bumped) because its core calculus diverged so far that little
  Thorin-derived code remains, and the `AnyDSL/MimIR` repo now redirects to
  `github.com/mimir/mimir`. The degree to which the original `AnyDSL/thorin` repo is
  still maintained / whether Impala still targets it vs MimIR was not definitively
  established.
- **GPU backend routing.** The repo shows `be/llvm`, `be/c`, `be/spirv`,
  `be/shady`; the precise mapping of CUDA/OpenCL/NVVM/AMDGPU onto these
  (LLVM-NVVM vs C-source emission) is described in the OOPSLA paper but the exact
  current code paths were not traced.
