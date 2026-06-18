# Compile-time Evaluation & Partial Evaluation as Reduction

## Overview

This note studies a single idea seen from many angles: *evaluating
program text at compile time is the same kind of operation as
reduction / optimization*. The theoretical spine is **partial
evaluation (PE)** — the work of Jones, Gomard & Sestoft, *Partial
Evaluation and Automatic Program Generation* (Prentice-Hall, 1993) —
together with **Futamura's three projections** (1971) and
**Normalization by Evaluation (NbE)**. The practical spine is a family
of "comptime" systems: Zig `comptime`, Terra (Lua-staged C), MetaOCaml
multi-stage programming, Scala LMS, JAX tracing, Lisp macros, and C++
`constexpr`/`consteval`/templates. The unifying compiler-IR question is:
**can the rewrite engine that optimizes the IR be literally the same
engine that runs compile-time code?** Thorin/MimIR (AnyDSL) answer "yes"
by doing PE *in the graph*.

The core relationship is the **mix equation** / specialization law. Given
a program `p` with two inputs `d1, d2`, partial evaluation produces a
*residual program* `r` such that running `r` on `d2` equals running `p`
on `(d1, d2)`. Jones/Sestoft call the process *partial evaluation, or
program specialization*, and the produced `r` a **residual program**.
The specializer itself is conventionally called **`mix`**.

## Core Model (nodes / edges / regions)

There is no single canonical IR for PE; rather there are several
representations of "code as data plus a reducer":

- **Term/AST model (classic PE, Lisp, Zig, C++ constexpr).** The
  "graph" is the abstract syntax tree (or a desugared core IR). Reduction
  = β-reduction + constant folding + unfolding of calls. Static
  subterms collapse to values; dynamic subterms are *rebuilt* (residualized)
  into output code. Zig is explicit that comptime is *partial
  evaluation/specialization*: marking a parameter `comptime` causes the
  compiler to "partially evaluate `f`, so only one branch will be left"
  (matklad). The evaluator runs *during semantic analysis* over Zig's
  internal IR (`AIR`/`Sema`), not over raw source.

- **CPS graph model (Thorin).** Thorin is a higher-order, functional IR
  in continuation-passing style; "its only constructs are
  **continuations** and so-called **primops** (primitive operations,
  such as arithmetic operations, loads and stores, etc.)" (AnyDSL paper).
  It abandons explicit scope nesting for a *dependency graph* (a
  sea-of-nodes variant). PE = *specializing continuations* + standard
  optimizations. **Lambda mangling** (cloning/specializing a continuation
  with some arguments fixed) is the key primitive, and is "substantially
  simpler to implement on Thorin than on existing IRs because Thorin does
  not have explicit scope nesting."

- **Dependently-typed graph model (MimIR).** MimIR is "a pure,
  graph-based, higher-order intermediate representation rooted in the
  **Calculus of Constructions**." Node kinds include `Def` (the universal
  base), `Lam` (lambda), `App`, `Pi` (dependent function type). "Terms and
  types share one graph." Structure rule: "Non-binder expressions are
  immutable" (hash-consed) while "Binders are mutable where needed." It
  has no CFG dominance: "Free variables replace dominance; the nesting tree
  replaces the dominator tree." Hash-consing, **normalization**, type
  checking, and **partial evaluation** all run *on-the-fly during graph
  construction*.

- **Tracing model (JAX).** A `Tracer` is "a boxed-up value, perhaps
  carrying some extra context data used by the interpreter," holding an
  *abstract value* (`aval`, e.g. `ShapedArray`). A `Trace` is an
  interpreter for primitives. The residual is a **jaxpr**, "explicitly
  typed, functional, first-order, and in ANF form" — the edges are
  let-bound equation dependencies, nodes are primitive applications.

## How control & effects / memory are represented

- **Control in PE / NbE.** Static control flow is *executed away*:
  static `if` picks a branch, static loops are unrolled, static calls are
  unfolded. Dynamic control flow is *residualized* (rebuilt in the output).
  Zig's `if constexpr`-style discarding is exactly this: comptime `if`
  with a comptime-known condition leaves only the taken branch. C++
  `if constexpr` (C++17) "evaluates conditions at compile time and discards
  the untaken branch entirely."

- **Control in Thorin/MimIR.** Control is data: continuations *are* the
  graph nodes; a `branch` is just a higher-order primop choosing which
  continuation to call. PE specializes continuations, so eliminating a
  conditional is the same operation as inlining a known function. This is
  why "code transformations are often expressible as specializations" — a
  polymorphic loop "can be directly specified and partially evaluated to
  generate the unrolled, specialized version."

- **Effects / memory.** The hardest part of comptime-as-reduction is
  side effects. The systems split sharply:
  - **Hermetic / pure comptime.** Zig forbids it outright: "Zig comptime
    does not allow any kind of input output" and observes the *target*
    architecture, not the host — "in exchange, compile time evaluation is
    hermetic, reproducible, safe, and cacheable" (matklad). Purity is what
    makes memoization sound (see below).
  - **Memory in graph IRs.** Thorin models loads/stores as primops; PE
    folds those whose addresses/values are static. MimIR threads memory as
    an explicit value in the dependency graph (sea-of-nodes style) so the
    normalizer can reason about it.
  - **MetaOCaml** allows effects in the generator (stage-0 code that
    builds code), but the *generated* code's effects are residual. Its key
    invariant is about *binding*, not purity (scope extrusion, below).

## Optimization approach

The headline lesson is **PE = optimization, run by the same engine**:

- **Offline vs online PE** (Jones/Sestoft; Cook tutorial). *Offline* PE
  runs a **binding-time analysis (BTA)** first, annotating each
  expression as **static** (known at specialization time) or **dynamic**
  (depends on runtime input); the specializer then mechanically obeys the
  annotations. *Online* PE makes the static/dynamic decision *during*
  specialization, using the actual partially-known values, so it can be
  more precise but is harder to make terminate. Thorin is explicitly an
  **online** partial evaluator ("a novel online partial evaluator for
  continuation-passing style languages", Leißa GPCE'15).
- **Polyvariant specialization.** Generate *multiple* specialized
  versions of a function for different static-argument combinations,
  rather than one generic version. This is exactly C++ template
  instantiation and Zig generics-via-comptime.
- **Reduction-to-normal-form as the unifying primitive.** NbE shows the
  cleanest version: normalize a term by (1) *evaluating* it into a
  semantic domain, then (2) *reifying* the value back to a β-normal,
  η-long term. "A term is first interpreted into a denotational model of
  the λ-term structure, and then a canonical (β-normal and η-long)
  representative is extracted by reifying the denotation." Free/dynamic
  variables are handled by **reflect** (↑) into the semantics as *neutral
  terms*; **reify** (↓) extracts them back. NbE is *reduction-free*: there
  is no explicit β-rewrite loop, you just compute in the host and read the
  answer back — which is precisely how Zig/LMS/MetaOCaml "run" comptime.

## Code generation / lowering

- **The First Futamura Projection** *is* a compiler-codegen story:
  specializing an interpreter `int` to a fixed source program `p` yields a
  *compiled* `p` — `mix(int, p)` = `target`. The **Second** projection
  specializes `mix` to `int`, yielding a *compiler* — `mix(mix, int)` =
  `compiler`. The **Third** specializes `mix` to itself, yielding a
  *compiler generator* (a `cogen`) — `mix(mix, mix)` = `cogen`. The cogen
  is **self-generating** rather than a self-reproducing Quine: a compiler
  generator `cogen` satisfies `cogen(mix) = cogen` (applying it to the
  partial evaluator `mix` regenerates the cogen), and any such
  self-generating cogen can be obtained by repeated self-application of a
  partial evaluator (Glück; Williams, *Revisiting the Futamura
  Projections*). Self-application of `mix` is what makes the 2nd/3rd
  projections work and is the historical motivation for `mix`.
- **MetaOCaml / Terra / LMS: generate-then-compile.** Brackets `.<e>.`
  build a code value of type `'a code` ("computed later"); escape `.~e`
  "is computed now but produces the result for later"; **run**
  (`Runcode.run`) "compiles and executes delayed expressions." Terra stages
  Lua (stage 0) to build Terra (stage 1) and "Terra code is compiled
  directly to machine code and optimized using LLVM"; Lua and Terra share
  one lexical environment but Terra runs free of the Lua runtime. **LMS**
  drops quasiquotation entirely: "it uses only types to distinguish between
  binding times" — `Rep[T]` is a `T`-typed value at the *second* stage; an
  ordinary `T` is stage-0, so staging is encoded in the type system and a
  graph IR is built from operations on `Rep` values, then code-generated.
- **MetaOCaml offshoring** lowers staged OCaml code to C/LLVM (e.g.
  generating `int power7(int x){...}`), connecting staging directly to
  low-level codegen.
- **JAX staging-out.** Tracing produces a jaxpr that is then handed to
  XLA for lowering. "We want only to form a jaxpr for those operations
  that *must* be delayed due to a dependence on unknown inputs" — i.e.
  codegen targets only the residual.
- **Thorin/MimIR lowering.** After PE specializes the graph, a
  **scheduling** pass turns the scheduling-free dependency graph back into
  a CFG with dominance/blocks, then emits LLVM. Because higher-order
  abstraction is removed by PE before emission, accelerator kernels (CPU
  vectorization, GPU) come out efficient.

## Compile-time evaluation / partial evaluation

Survey of how each system *bounds and defines* comptime:

- **Zig.** Comptime is reduction over the IR during semantic analysis;
  "functions called at compile-time are memoized"; comptime is *lazy*.
  Deliberately *not* a macro system: "there's absolutely no facility for
  dynamic source code generation in Zig" — no string injection, no custom
  syntax, no new methods on generated types. Termination is bounded by a
  **branch quota** (`@setEvalBranchQuota`); the default global quota caps
  the number of backward branches the comptime interpreter will take before
  erroring (matklad/issues note "a single global branch quota").
- **C++.** `constexpr` functions *may* run at compile time; `consteval`
  (C++20) *must*. Templates are a separate, accidentally-Turing-complete
  metalanguage (recursion + specialization-as-base-case). Implementations
  bound it with `-ftemplate-depth` / `-fconstexpr-steps` (Clang) and
  `-fconstexpr-ops-limit` (GCC) — explicit *step budgets*.
- **MetaOCaml / Terra / LMS.** Stage-0 generators are ordinary Turing-
  complete programs; there is *no* automatic bound — if your generator
  loops, the compiler loops. The guarantee they provide is **type/scope
  safety of the output**, not termination of the generator.
- **Lisp/Racket macros.** Compile-time reduction of syntax to syntax;
  `macroexpand`/`macroexpand-1` expose it. The hard problem is **hygiene**
  (avoiding unintended capture); Racket solves it with **sets of scopes**.
  Macro expansion can diverge — bounded only by recursion limits.
- **JAX.** Tracing is partial evaluation: "only the primal part of
  evaluation can happen during tracing"; `PartialEvalTrace` builds
  `PartialEvalTracer`/`JaxprRecipe` nodes and, "when all inputs to a
  primitive are known, it executes immediately; otherwise, it stages the
  operation into `JaxprEqnRecipe`." Termination is bounded by Python (the
  trace is finite because the Python program runs once).
- **Thorin/MimIR.** Online PE driven by **filters** (à la Schism). "The
  evaluator is entirely driven by implicit and explicit annotations (in the
  style of Schism filters)"; the `@` annotation marks call sites to be
  specialized. Termination relies on (a) the filter telling the evaluator
  *when to stop unfolding* and (b) memoizing already-specialized
  continuations so recursion through identical static arguments halts.

**The termination problem, sharply.** Comptime is Turing-complete, so by
the halting problem no specializer can always terminate *and* always
maximize specialization. The classic offline answer is BTA + **termination
analysis**: e.g. "if whenever something grows, something gets smaller then
the program will only enter finitely many different states," used to decide
which arguments must be made *dynamic* to guarantee termination
(Springer LNCS 1110). Online PE instead uses **generalization**: "widening
abstract values when recursion or complex data structures threaten
non-termination — essentially trading some specialization gain for
guaranteed termination" (Cook). Engineering systems sidestep the theory
with **fuel / step / depth budgets** (Zig branch quota, Clang
`-fconstexpr-steps`, template depth) and **memoization** of specialized
instances.

## Strengths

- **One engine for two jobs.** PE/NbE makes "run it now" and "optimize
  it" the *same* rewrite. Thorin's "code transformations are often
  expressible as specializations" is the strongest evidence: inlining,
  constant folding, loop unrolling, and comptime all reduce to
  specialize-a-continuation.
- **Abstraction without cost.** AnyDSL/Thorin and LMS let you write
  generic, higher-order library code and *partially evaluate it away* to
  target-specific kernels — "shallow embedding of DSLs via online partial
  evaluation."
- **Type-safe staging.** MetaOCaml's "a well-typed MetaOCaml program
  generates only well-typed programs" and LMS's `Rep[T]` give static
  guarantees that ad-hoc macros/string-templating cannot.
- **Determinism & cacheability.** Hermetic comptime (Zig) is
  "reproducible, safe, and cacheable" — crucial for incremental compilers.
- **Decidable equality via NbE.** For dependent type checking, NbE "gives
  a practical method for deciding definitional equality even in complicated
  dependent type theories, where rewriting becomes prohibitively
  complicated." MimIR exploits exactly this — its "type checker tightly
  interacts with normalizations and a partial evaluator."

## Weaknesses & criticisms

- **Self-applicable offline PE is hard.** The whole reason offline PE
  with BTA exists is that *online* self-application was too unpredictable;
  but BTA loses precision (it must be conservative). The field never fully
  delivered automatic, predictable, terminating self-application for
  realistic languages — much of the 1990s effort (MIX, Similix) was about
  taming this.
- **Termination is fundamentally unsolved.** Everyone falls back on
  fuel/quota/depth limits, which leak into the user experience (Zig's
  "branch quota exceeded", C++'s "constexpr step limit", template-depth
  errors). The "single global branch quota that cannot be read" in Zig is
  an acknowledged wart.
- **Effects break the clean theory.** PE-as-reduction is cleanest for
  *pure* terms; once you admit I/O or mutation in comptime, memoization can
  become unsound and "comptime state bundled with runtime data … can lead
  to counterintuitive results" (Zig issue discussion). Zig's response is to
  ban effects entirely — expressive cost.
- **Macros: hygiene & debuggability.** Lisp-style procedural macros risk
  variable capture; even with scope-sets hygiene, *debugging* expanded code
  is hard (Culpepper/Felleisen, "Debugging Hygienic Macros").
- **Online PE non-termination / code blowup.** Aggressive online
  specialization (Thorin without good filters) can loop or produce code
  explosion; correct **filter** placement is a real burden on the
  programmer.
- **Scope extrusion in MSP.** MetaOCaml had to *retreat* from the
  static **environment classifiers** approach to a *dynamic* scope-extrusion
  check: "attempting to build code values with unbound or mistakenly bound
  variables … is caught early, raising an exception" — i.e. a runtime
  guard, not a static guarantee.

## Codebase / complexity notes

- **MimIR**: implemented in **C++23** with Python 3.10 bindings, version
  "0.2-dev"; the design point is that a DSL author "declare[s] new types,
  operations, and normalizers in a single `.mim` file" while C++ handles
  optimization/codegen. Exact LOC not stated in the docs I read
  (uncertainty below).
- **Thorin** (predecessor): minimalistic core — only continuations +
  primops — which is *why* lambda mangling/PE is simpler than on LLVM-style
  IRs (no scope nesting to fix up).
- **Jones/Gomard/Sestoft** book is 415 pp (1993); the seminal **MIX**
  self-applicable partial evaluator (Jones, Sestoft, Søndergaard, *LISP and
  Symbolic Computation*, 1989) was the proof-of-concept for the 2nd/3rd
  projections.
- **LMS**: deliberately *library-sized* ("lightweight … can be implemented
  as a library") rather than a language extension — its whole pitch is low
  implementation cost relative to a compiler.
- Engineering bounds are cheap to add: Zig's branch quota and C++'s
  step/depth limits are a handful of counters in the evaluator.

## Lessons for a NEW unified IR

1. **Make comptime literally the optimizer's reduction rule.** Follow
   Thorin/MimIR: do not build a separate comptime interpreter; let PE be a
   pass over the *same* graph that constant-folding and inlining use.
   "Specialize a continuation/`Def`" should subsume inlining, unrolling,
   and comptime call evaluation.
2. **Adopt NbE for the reduction core.** A reduction-free
   eval-then-reify gives β-normal/η-long forms, sound definitional
   equality (needed if the IR has dependent types), and a clean handling of
   dynamic values via *neutral terms* — exactly the static/dynamic split PE
   needs.
3. **Use filters/annotations, not just BTA.** Online PE driven by
   explicit `@`/filter annotations (Thorin) gives precision where it
   matters and a programmer-visible knob for termination — more practical
   than fully-automatic offline BTA, which is conservative.
4. **Demand confluence + determinism for the rewrite system.** If
   comptime is the rewrite engine, the engine must be **confluent** (result
   independent of reduction order) and **deterministic/hermetic**
   (reproducible, host-independent) so that results are cacheable and
   incremental — copy Zig's hermeticity, not Lisp's effectful macros.
5. **Memoize specialized instances by (function, static-args) key.** This
   is the shared termination + performance trick across Zig, online PE, and
   Thorin; it is *only sound under purity*, so pair it with rule 4.
6. **Always ship a fuel/step/depth budget with good diagnostics.**
   Turing-completeness makes this non-negotiable; learn from Zig's
   ungettable global quota and make the budget *introspectable and
   localizable* per evaluation.
7. **Guarantee output well-formedness statically where possible.** Adopt
   MetaOCaml's "well-typed generator ⇒ well-typed residual" and detect
   scope extrusion; prefer LMS's type-based binding-time tracking
   (`Rep[T]`) over string templating to keep generated code structurally
   valid by construction.
8. **Keep staging multi-level.** Support the Futamura idea directly: the
   IR's specializer applied to an embedded interpreter should yield a
   compiled program; design so self-application/`cogen` patterns are at
   least expressible.

## Sources

- [Jones, Gomard, Sestoft — *Partial Evaluation and Automatic Program
  Generation* (book home)](https://www.itu.dk/~sestoft/pebook/pebook.html)
  — canonical text; defines residual programs, `mix`, BTA, Futamura
  projections.
- [Jones/Gomard/Sestoft — Table of
  Contents](https://www.itu.dk/people/sestoft/pebook/toc.html) — confirms
  offline/online, BTA granularity, self-application chapters.
- [William Cook — *Tutorial on Online Partial Evaluation*
  (arXiv 1109.0781)](https://arxiv.org/pdf/1109.0781) — online vs offline,
  static/dynamic, polyvariance, memoization, generalization, mix equation.
- [Williams — *Revisiting the Futamura
  Projections*](https://journals.pan.pl/Content/118538/PDF/williams_Revisiting%20the%20Futamura%20Projections.pdf?handler=pdf)
  — three projections (1st→target, 2nd→compiler, 3rd→cogen) and
  self-application.
- [Williams et al. — *Revisiting the Futamura Projections: A Diagrammatic
  Approach* (arXiv 1611.09906)](https://ar5iv.labs.arxiv.org/html/1611.09906)
  — precise equations `mix(mix,int)=compiler`, `mix(mix,mix)=cogen`; the
  self-generating property of a compiler generator (`cogen(mix)=cogen`); no
  Quine claim.
- [Glück — *Is There a Fourth Futamura
  Projection?* (PDF)](https://gwern.net/doc/cs/algorithm/2009-gluck.pdf)
  — self-generating compiler generators and repeated self-application of a
  partial evaluator.
- [Leißa et al. — *AnyDSL: A Partial Evaluation Framework for Programming
  High-Performance Libraries* (PDF)](https://compilers.cs.uni-saarland.de/papers/anydsl.pdf)
  — Thorin = continuations + primops; online PE; filters; accelerator codegen.
- [Leißa et al. — *Shallow Embedding of DSLs via Online Partial
  Evaluation* (GPCE'15)](https://graphics.cg.uni-saarland.de/publications/leissa-2015-gpce-shallow-embedding.html)
  — Schism-style filters; lambda mangling; PE for CPS.
- [Leißa, Köster, Hack — *A Graph-Based Higher-Order Intermediate
  Representation* (CGO'15, PDF)](https://compilers.cs.uni-saarland.de/papers/lkh15_cgo.pdf)
  — Thorin's scope-free dependency graph; lambda mangling primitive.
- [*MimIR: An Extensible and Type-Safe IR for the DSL Age* (arXiv
  2411.07443)](https://arxiv.org/pdf/2411.07443) — sea-of-nodes + CoC;
  on-the-fly normalization, type-checking, PE; plugins/axioms/normalizers.
- [MimIR project docs](https://mimir.github.io/) — `Def`/`Lam`/`App`/`Pi`;
  "free-variable nesting replaces dominance"; C++23 implementation.
- [Oleg Kiselyov — *MetaOCaml* reference](https://okmij.org/ftp/ML/MetaOCaml.html)
  — brackets/escape/run, `'a code`, CSP, offshoring, scope-extrusion,
  Ershov `power` example, PE/Futamura link.
- [Rompf & Odersky — *Lightweight Modular
  Staging* (PDF)](https://web.stanford.edu/class/cs442/lectures_unrestricted/cs442-lms.pdf)
  — `Rep[T]`, types-as-binding-times, library-level staged compilers.
- [DeVito et al. — *Terra: A Multi-Stage Language for HPC*
  (PDF)](https://cs.stanford.edu/~zdevito/pldi071-devito.pdf) and
  [DeVito PhD thesis](https://cs.stanford.edu/~zdevito/zdevito_thesis.pdf)
  — Lua-staged Terra; shared lexical env; LLVM codegen.
- [JAX — *Autodidax: JAX core from
  scratch*](https://docs.jax.dev/en/latest/autodidax.html) — Trace/Tracer,
  ShapedArray, jaxpr, `PartialEvalTrace`/recipes, known/unknown split.
- [matklad — *Things Zig comptime Won't
  Do*](https://matklad.github.io/2025/04/19/things-zig-comptime-wont-do.html)
  — comptime = PE during sema; hermetic/reproducible; no codegen-from-strings.
- [Zig issue #14536 — function-level comptime
  semantics](https://github.com/ziglang/zig/issues/14536) and
  [#5895 — comptime memory reform](https://github.com/ziglang/zig/issues/5895)
  — memoization, global branch quota, effect/state hazards.
- [Wikipedia — *Normalisation by
  evaluation*](https://en.wikipedia.org/wiki/Normalisation_by_evaluation)
  — eval/reify/reflect, neutral terms, β-normal/η-long, reduction-free.
- [Bowman — *NbE Four Ways*](https://williamjbowman.com/tmp/nbe-four-ways/)
  — reconstructs NbE designs; reflect/reify pairs by type.
- [*Termination analysis for offline PE of a higher-order functional
  language* (LNCS 1110, PDF)](https://link.springer.com/content/pdf/10.1007/3-540-61739-6_34.pdf)
  — "if something grows, something gets smaller" termination argument.
- [Culpepper & Felleisen — *Debugging Hygienic
  Macros* (PDF)](https://www2.ccs.neu.edu/racket/pubs/cf-sp09.pdf) — macro
  hygiene, sets-of-scopes, debuggability criticism.

## Open questions / uncertainties

- **MimIR / Thorin LOC** is not stated in the sources I fetched; the
  "minimalistic core" claim is qualitative, not a measured number.
- The **AnyDSL and MimIR PDFs failed to text-extract** via my fetch tool;
  the AnyDSL/Thorin filter and termination details above come from the
  GPCE/CGO descriptions, the AnyDSL search summary, the Illinois slide deck
  summary, and the MimIR docs site — I did not read the full PE-algorithm
  pseudocode, so the *exact* memoization/termination guarantee in Thorin is
  inferred, not verified line-by-line.
- **Exact Zig branch-quota semantics** (whether it counts all backward
  branches, function calls, or evaluation steps, and its default value)
  were described only via issue summaries and matklad; I did not confirm
  against the current Zig reference for 0.13+/0.14.
- Whether **MimIR's on-the-fly PE is guaranteed confluent/terminating**
  for arbitrary user axioms/normalizers is unclear from the docs; plugins
  can introduce arbitrary normalizers, which in principle could diverge.
- The precise **mix equation** rendering varies by source ("mix(m,s) ≈
  m(s)" in the Cook summary vs the book's `[[mix]](p, d1)` notation); I used
  the book's residual-program framing as authoritative.
- **C++ step/depth flag names** (`-fconstexpr-steps`, `-fconstexpr-ops-limit`,
  `-ftemplate-depth`) are from general knowledge of Clang/GCC, corroborated
  by the constexpr search results but not fetched from each compiler's manual.
