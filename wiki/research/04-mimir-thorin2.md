# Mimir / Thorin2 (AnyDSL/MimIR)

> Research notes on MimIR — formerly *Thorin2* — the dependently-typed, graph-based
> higher-order IR by Roland Leißa, Marcel Ullrich, Joachim Meyer, and Sebastian Hack
> (University of Mannheim / Saarland University). Primary sources: the POPL 2025 paper
> (arXiv:2411.07443 / doi:10.1145/3704840), the PLDI 2026 "SSA without Dominance" paper
> (arXiv:2604.09961), the CC 2025 "MimIrADe" AD paper, the project docs at
> <https://mimir.github.io>, and the source tree at github.com/AnyDSL/MimIR (commit
> dfb94d6, 2026-06-13).

## Overview

MimIR is an *extensible, type-safe, higher-order intermediate representation* aimed at
the "DSL age": image processing, ML, numerical simulation, etc., where optimizations
should happen at the level of the domain's primitive operations rather than after they
have been smashed down to LLVM. Its slogan-level pitch is that MimIR provides a
**single, expressive type system that DSLs can nest inside**, so DSL authors stop
re-implementing type checkers, standard optimizations, code generators, and boilerplate
for every new language.

The radical design choice is that **the IR core is a typed lambda calculus** — a *pure
type system* (PTS) rooted in the **Calculus of Constructions (CC)** and Barendregt's
**λ-cube**. Consequently MimIR "only knows a single syntactic category: expressions."
Types are expressions, values are expressions, operations are expressions, and even the
*kinds* of types are expressions. This collapses the usual term/type/kind stratification
into one graph.

MimIR descends from **Thorin** [Leißa, Köster, Hack, CGO 2015], the AnyDSL CPS sea-of-nodes
IR, and was internally called **Thorin2** during development. The authors state MimIR "is
now entirely different from its predecessor" and "MimIR's sources hardly contain any
Thorin-derived code anymore," which is why they renamed it rather than bumping Thorin's
version number. The textual surface syntax is called **Mim**.

Three published case studies back the claims: low-level LLVM-equivalent plugins (matching
C/LLVM performance on the Computer Language Benchmarks Game), a `%regex` plugin that
out-performs CTRE/std::regex/PCRE2, and a `%tensor` + `%autodiff` stack with
state-of-the-art reverse-mode AD performance at roughly a tenth of Enzyme's code size.

## Core Model (nodes / edges / regions)

The implementation is a **"sea of nodes"** data-dependence graph (Click & Paleczny 1995),
extended — as Thorin first did — to a *higher-order* language and then further to a
PTS. Every node is an expression; the whole program (terms *and* types) lives in one
graph. The C++ base class is `Def` (a "definition"); a graph is owned by a `World`.

- **Edges = operands + type.** "If `e′` is a subexpression/operand of an expression `e`,
  the graph contains an edge `e → e′`." In addition **every expression has exactly one
  type edge `e → T`, and `T` is itself an expression node.** There is therefore *no
  separate type graph* — types that depend on terms (dependent types) are just ordinary
  edges into the same sea.
- **No auxiliary structures.** Apart from one internal hash set used for hash-consing,
  "MimIR does not need any other auxiliary data structures such as instruction lists,
  basic blocks, CFGs, or special regions." `let`-bindings and explicit function nesting
  exist only in Mim and are gone by the time you reach the MimIR graph. *There are no
  regions* (a deliberate contrast with MLIR).

Concrete `Def` node kinds, confirmed from the headers (`include/mim/def.h`, `lam.h`,
`tuple.h`, `axm.h`, `check.h`):

| Node | Role |
|------|------|
| `Univ`, `Type`, `UInc`, `UMax` | the sort hierarchy (`*` = `Sort 0` = `Type 0`); predicative, stratified to avoid Girard's paradox |
| `Nat`, `Idx`, `Lit` | built-in naturals, the bounded-integer type `Idx n`, and literals |
| `Var` | a binder's variable (only mutables introduce a `Var`) |
| `Axm` | an **axiom** — a declared but unimplemented operation/type constructor |
| `Pi`, `Lam`, `App` | dependent function type `[x:T] → U`, lambda, application |
| `Prod` (Σ/`Sigma`), `Seq` (array/pack) | dependent tuple types and arrays/packs |
| `Extract`, `Insert` | tuple/array element read and non-destructive update |
| `Hole` | a metavariable filled in by type inference (always mutable) |
| `Global`, `Proxy` | global memory slots; pass-internal proxies |

Mim's textual sugar (`let`, `lam`/`con`/`fun`, `Cn`/`Fn`, named recursion) all desugars
into these few node kinds (paper Fig. 1–2).

### Nominal vs structural: **immutable** vs **mutable**

This is MimIR's central representational distinction (paper §4, Table 2; `enum class Mut
{ Mut, Imm }` in `def.h`):

- **Immutables** ("structural"): built operands-first, then the node. They form a
  **DAG**, are **hash-consed**, and are **non-parametric** (introduce no variable).
  Examples: `App`, `Lit`, most types.
- **Mutables** ("nominal"): the node is created *first* and its operands are *set later*.
  This allows **cyclic** graphs (hence recursion) and lets operands change in place
  (hence "mutable"). Mutables are **not** hash-consed; instead MimIR checks them for
  α-equivalence when needed. **Every binder that introduces a variable is a mutable** —
  `Lam`, `Pi`, `Sigma`, recursive `con`/`fun`, and `Hole`.

This is the same nominal/structural split Thorin pioneered, here renamed and unified
across the term/type boundary.

### Hash-consing / interning

"All expressions are hash-consed: whenever an expression is created, MimIR first checks
whether a syntactically equal expression already exists" and reuses it. The payoff is
**pointer equality ⇔ normalized syntactic equality** for closed terms, giving
semi-global value numbering "for free" during construction. Caveat (paper §5.1): pointer
equality does *not* imply α-equivalence in the presence of free variables, so MimIR only
uses pointer comparison for α-equivalence checks on *closed* terms; otherwise it does a
structural α-check (the `assignable` relation also re-checks α-equivalence while resolving
tuple shapes and implicits).

## How control & effects / memory are represented

**Control flow via CPS, but not mandatory.** Thorin forced *all* functions into
continuation-passing style. MimIR instead models a **continuation as a function whose
codomain is `⊥` (bottom / `Bot`)** — a type with no inhabitants — written `Cn T ≡ T → ⊥`.
Mim offers `cn`/`con`/`Cn` sugar for continuations and `fn`/`fun`/`Fn` for the common
"function + explicit return continuation" idiom. Because a continuation never returns, it
behaves like a basic block; `(F, T)#cond ()` selects between two `Cn[]` continuations
(an "if-diamond") and applies the chosen one. **φ-functions become continuation
parameters** and the φ operands become the call arguments — the classic CPS≈SSA
correspondence (Appel; Kelsey). Unstructured control flow is modeled by **mutually
recursive mutable continuations** (cyclic graph). Crucially MimIR supports **both direct
style and CPS**; the `%direct` plugin converts between them (`%direct.cps2ds f : T → U`),
and CPS-converts to feed the LLVM backend.

**Effects / memory via a linear state token.** The `%mem` plugin introduces a type
`%mem.M` that "abstracts from the machine state." Any potentially side-effecting operation
**consumes an `%mem.M` and produces a new one** — "similar to the IO monad in Haskell."
So load/store/alloc are pure data-dependence nodes that thread a state value, making
ordering an explicit data dependency rather than implicit instruction order. `%mem.Ptr`,
`%mem.slot`/`%mem.alloc`, `%mem.load`/`%mem.store`, and `%mem.lea` (pointer arithmetic, à
la LLVM `getelementptr`) live here; `%mem.lea`'s *dependent* result type computes the
element pointer type via normalization. The authors note `%mem.M` "must be used linearly,
but this is right now not enforced by the type system" — a known gap they hope to close
with linear types / QTT.

**SSA without dominance (PLDI 2026).** Because there are no CFGs, MimIR cannot rely on the
*dominator tree*. The companion paper argues dominance "over-approximates data flow" and
breaks for higher-order programs anyway. The replacement is **free variables**:
φ-functions and parameters *directly* express data dependencies. MimIR maintains
**free-variable sets in the mutable IR** (cached `free_vars()` on mutables; `local_muts`
/ `local_vars` are hash-consed — see `def.h`), and for analyses that need tree structure
it builds a **nesting tree** — "a relaxed analogue of the dominator tree constructed from
variable dependencies rather than control flow." The maintenance algorithms are claimed to
scale **log-linearly** with program size. The IR is described as **scopeless**: nesting
is recovered from free-variable containment, not lexical scope (Mim enforces Barendregt's
convention; the graph guarantees it structurally).

## Optimization approach

Three layers, all operating directly on the graph:

1. **Normalizers (eager, local rewrites).** "Whenever MimIR creates an expression, it is
   immediately normalized." This subsumes constant folding (`%core` folds `3+5`), peephole
   identities (`x+0 → x`), tuple/array unification (`[Nat,Nat]` ⇄ `‹2;Nat›`, 1-tuple
   elimination, `(0,1,2)#2 → 2`), and domain-specific simplifications (the `%regex`
   plugin merges quantifiers `r*? → r*` etc.). Each **axiom may register a C++ normalizer**
   (e.g. `normalize_quant`), fired — by default — "when the last curried argument is
   applied," though a plugin can specify a different trigger count (the `, N` in an axiom
   declaration). Normalizers **must be deterministic and cycle-free** (a cyclic pair like
   `x+x → 2*x` and `2*x → x+x` would diverge); determinism comes free from C++ execution.
2. **β-reduction / partial evaluation** (see next section), used to implement copy
   propagation, scalarization, inlining, loop unrolling.
3. **A modular pass manager / phase pipeline** (`include/mim/pass.h`, `phase.h`,
   `schedule.h`). "Even compilation phases are exposed as axioms" via the `%compile`
   plugin, so a user can *write their own compilation pipeline as a Mim program*; `%opt`
   is the default pipeline expressed this way. Plugins add custom passes — e.g. `%regex`
   ships a C++ pass that compiles a normalized pattern to a minimized DFA and emits
   low-level control flow.

Because the graph is a pure data-dependence sea, it is "invariant to code motion," dead
code is "only following data-dependence edges," and value numbering happens via
hash-consing. Future work mentions allowing **rewrite rules written directly in Mim** and
exploring **equality saturation** (there is already a `Rule`/`Reform` node and `rule.h`).

## Code generation / lowering

Axioms are **opaque** — they carry a type but no implementation. Lowering is the job of
plugins:

- **Low-level plugins.** `%core` (integer ops, modes for overflow behavior), `%math`
  (a floating-point type operator `%math.F p e` over precision/exponent bits, IEEE modes),
  and `%mem` (state, memory, pointers). These are "deliberately modeled on LLVM."
- **The LLVM backend lives in a plugin** (`%mem`'s backend, understanding `%core`/`%math`)
  and emits **textual LLVM IR**, then defers to LLVM tools to produce an executable. So
  MimIR targets "any CPU supported by LLVM." There is also a `gpu` plugin directory.
- **Higher-level plugins lower to the low-level ones.** A plugin either substitutes its
  domain axioms down to `%core`/`%math`/`%mem` (which the backend understands) or generates
  target code directly. `%clos` does typed closure conversion for escaping continuations;
  `%direct` does CPS conversion (the backend expects CPS).

A central claim: writing transformations is *no harder* than in LLVM, and is often *easier*
because the starting point can be **well-typed polymorphic Mim** that you partially
evaluate, instead of error-prone C++ that emits IR. The `%regex` C++ pass is 919 LoC +
22 lines of Mim; `%autodiff` is ~10× smaller than Enzyme with ~an order of magnitude lower
cyclomatic complexity (paper Tables 4–5).

## Compile-time evaluation / partial evaluation

MimIR is essentially a **reduction engine on a typed graph**, and this is load-bearing for
both optimization *and* type checking.

- **Filters drive partial evaluation.** Every function carries a boolean **filter** `@e`
  (the `@e` in `λ x:T @e : U = body`) that may depend on the parameter. At each call site
  MimIR substitutes the argument into the filter and normalizes it; **if it reduces to
  `tt`, the call is β-reduced (inlined) at compile time** — recursively. This makes the
  compile-time/run-time boundary *explicit and programmer-controlled*, which the authors
  contrast favorably with most dependently-typed languages (Idris 2's QTT erasure being a
  noted exception).
- **Default policy.** Mim defaults elided filters to `tt` (specialize aggressively, like
  C++ templates / Rust generics / MLton monomorphization) **except** the final continuation
  of a `cn`/`con`/`fn`/`fun`, which defaults to `ff` — so type abstractions get specialized
  but actual run-time computation is deferred. `%core.pe.known b` is a predicate used to
  trigger unrolling only when an argument is a compile-time constant.
- **Type checking via reduction.** Because the system is dependently typed with full
  recursion, type equality requires **program equivalence**, which is undecidable in
  general. MimIR's novelty: **couple partial evaluation + normalization with type
  checking**. Checking whether arg type `‹pow(m,3); Nat›` is assignable to a domain
  triggers β-reduction of `pow(m,3)` until both sides have a common normal form. The
  typing rules are *syntax-directed and deterministic* (Pollack–Poll style) with **no
  conversion rule**; normalization rules pin down exactly where reduction happens. The
  downside is fully transparent non-termination: a `tt` filter on a function applied to a
  non-constant can make the type checker diverge — but "this termination behavior is not
  random and completely transparent to the programmer," as it is dictated by stated filters.
- **Same engine from C++ or Mim.** `world.call<core::minus>(mode, world.lit_idx(256,42))`
  in C++ runs the *same* normalization/inference/partial-evaluation and can yield a literal
  (`244 : Idx 256`); constructing ill-typed IR throws a C++ exception immediately.

Type safety is mechanized in **Coq**: a nondeterministic β-reduction relation with
**Progress** and **Preservation** lemmas. Preservation is *not* "strong" — a β-step can
change a dependent type — but the new type evolves from the old by zero or more β-steps
(modeled via type-level β-equivalence). Plugin axioms can *locally* violate progress
(undefined behavior, e.g. loading a dangling pointer) or preservation (a buggy
normalizer), but a preservation violation surfaces as a type error the moment the bad
rewrite fires, preserving **type-safe composition of plugins**.

## Strengths

- **Radical unification.** One node kind family, one graph, one type system for terms,
  types, kinds, operations, and even compilation phases. Fewer syntactic categories ⇒
  fewer cases in every analysis ("less syntax also implies fewer patterns to match").
- **Expressive type system that hosts DSL type systems.** Dependent types, parametric +
  higher-kinded + higher-order polymorphism, dependent tuples (Σ), size-indexed arrays
  (`Idx n` guarantees in-bounds), and type operators — all out of the box, so plugins do
  **not** write their own C++ type checkers or validators (a direct improvement over MLIR
  dialects).
- **First-class, type-checked extensibility.** Axioms are *first-class citizens* (unlike
  LLVM/MLIR/Thorin instructions): a partial application of an axiom can be passed to a
  higher-order function. Plugins bundle axioms + normalizers + passes + backends in one
  `.mim` file plus C++.
- **Principled compile-time computation.** Partial-evaluation filters give explicit,
  programmer-visible control of the static/dynamic boundary and double as the mechanism for
  decidable-in-practice dependent type checking — a genuinely novel combination.
- **Construction-time guarantees.** Eager normalization + type checking + inference means
  ill-formed IR is rejected the instant it's built, from either Mim or C++.
- **Demonstrated performance & compactness.** Matches C/LLVM on benchmarks; AD plugin ~10×
  smaller than Enzyme; `%regex` competitive with hand-tuned engines.
- **Scopeless higher-order SSA** with log-linear free-variable maintenance and a nesting
  tree replacing the dominator tree.

## Weaknesses & criticisms

- **Steep conceptual barrier.** The substrate is a dependently-typed PTS / Calculus of
  Constructions. Anyone implementing a plugin must be comfortable with dependent types,
  CPS-as-`T→⊥`, mutable vs immutable nodes, filters, and normalizer trigger counts. This is
  a far higher bar than emitting LLVM/MLIR ops.
- **Undecidable type checking / possible divergence.** Type checking can diverge for
  general recursive dependent types; the system relies on programmer-chosen filters to keep
  it terminating. Mitigated but not eliminated.
- **Normalizer discipline is on the plugin author.** Normalizers must be hand-written in
  C++, must be deterministic and cycle-free, and must preserve typing — MimIR only catches
  preservation violations *after* the bad rewrite fires; progress violations are not caught
  at all. Rewrite rules in Mim with static checking are still future work.
- **Debuggability of a sea-of-nodes graph.** The paper itself concedes graph IRs need
  external graph tools to visualize (especially when broken), whereas an instruction list
  "is straightforward to dump even if incomplete." Ordering dependencies (memory) must be
  made explicit, which "can feel cumbersome."
- **Maturity & ecosystem.** A research IR from two German university groups. The project
  was renamed from Thorin to MimIR (the GitHub repo remains AnyDSL/MimIR; only the docs
  site moved from anydsl.github.io/MimIR to mimir.github.io) and is actively churning
  (latest commit mid-2026).
  Only a CPU/LLVM backend is fully demonstrated; GPU/accelerator support, linear/QTT typing
  for `%mem.M`, union/intersection/singleton types, and Mim-level rewrite rules are all
  listed as *future work*.
- **Docs are thin/scattered.** Concepts are best learned from the papers; the website and
  Doxygen-style API docs assume substantial PL theory background. Far less documented than
  LLVM or MLIR.
- **Single-language formalization caveat vs MLIR's pragmatism.** MimIR bets that one
  expressive base language is better than MLIR's "little builtin, everything customizable."
  That bet costs simplicity/learnability and assumes a DSL's semantics fit in CC.

## Codebase / complexity notes

Measured from the cloned repo (github.com/AnyDSL/MimIR @ dfb94d6, 2026-06):

- **~32k LoC** total C++ (`.cpp`/`.h`, excluding `build`/external).
- **Core IR**: header set ~2.8k LoC (`def.h` 993, `world.h` 935, `lam.h` 381, `tuple.h`
  365, `check.h` 141); core `src/mim` (excluding plugins) ~**9.4k LoC**.
- **Plugins**: ~**8.4k LoC** across **21 plugin directories**: `affine`, `autodiff`,
  `clos`, `compile`, `core`, `demo`, `direct`, `gpu`, `ll` (LLVM backend), `math`,
  `matrix`, `mem`, `opt`, `option`, `ord`, `refly`, `regex`, `tensor`, `tuple`, `vec`.
- Per-plugin complexity is small: `%regex` = 919 LoC C++ + 22 lines Mim; `%autodiff`
  ≈ 1/10 of Enzyme by both LoC and cyclomatic complexity (paper Table 5).
- Build is CMake; depends on LLVM for the backend, plus an in-tree front-end library
  (`fe`) and an `automaton` library for `%regex`.

The whole-system size (~32k LoC for a full extensible IR + 21 plugins + LLVM backend) is
strikingly small versus LLVM/MLIR, supporting the "fewer syntactic categories" claim.

## Lessons for a NEW unified IR

- **Unify types and values as graph nodes** if you can afford a PTS-style core: it removes
  whole classes of duplicated machinery (no separate type IR, no per-dialect type checkers,
  type aliases are just `let`, type operators are just functions).
- **Two node modes are enough:** immutable/structural (DAG, hash-consed, value-numbered)
  for everything pure, and mutable/nominal (cyclic, identity-based) *only* for binders and
  recursion. Make "introduces a variable ⇒ mutable" an invariant.
- **Hash-cons aggressively, but remember pointer-equality ≠ α-equality with free vars.**
  Restrict pointer-equality fast paths to closed terms; fall back to structural α-checks.
- **Model effects/ordering as explicit linear state tokens** (`%mem.M`) instead of implicit
  instruction order — and seriously consider *enforcing* linearity in the type system
  (MimIR's unenforced linearity is an admitted hole).
- **Make the compile-time/run-time boundary first-class** via per-function filters rather
  than ad-hoc constant-folding heuristics; the same reduction engine then serves
  optimization *and* dependent type checking.
- **Eager construction-time normalization + type checking** catches malformed IR
  immediately and gives value numbering for free — but demands that every rewrite be
  deterministic, terminating, and type-preserving.
- **CPS as `T → ⊥` lets one IR carry both direct and CPS style**; pick the representation
  per pipeline stage instead of committing globally (Thorin's all-CPS rigidity was a step
  MimIR explicitly walked back).
- **Avoid regions/CFGs as primitives**; recover structure (the *nesting tree*) from
  free-variable dependencies. This generalizes to higher-order code where dominance breaks.
- **Beware the cost**: dependent types make type checking undecidable and the learning
  curve steep; you need mechanized type-safety proofs (MimIR used Coq) and excellent
  tooling/visualization to make a sea-of-nodes graph debuggable.

## Sources

- [MimIR: An Extensible and Type-Safe IR for the DSL Age (Roland Leißa, Marcel Ullrich, Joachim Meyer, Sebastian Hack; arXiv:2411.07443v2, 20 Nov 2024; POPL/PACMPL 2025)](https://arxiv.org/pdf/2411.07443) — the primary paper; core calculus, normalization, partial evaluation, graph (sea of nodes), immutable/mutable, plugins, case studies, Coq type-safety. Read in full. Affiliations verified: Leißa = University of Mannheim; Ullrich/Meyer/Hack = Saarland University. Verified verbatim against the PDF: §2.6 "MimIR is now entirely different from its predecessor" and "MimIR's sources hardly contain any Thorin-derived code anymore, that we have renamed the project rather than bumping Thorin's version number"; continuations "as functions whose codomain is ⊥ (Bot)"; rooted in "the λ-cube, the Calculus of Constructions (CC), and pure type system (PTS)"; Coq Progress (Lemma 3.13) + Preservation (Lemma 3.14); RegEx "engine outperforms state-of-the-art RegEx engines" (919 LoC C++ + 22 lines Mim); "Enzyme has roughly 10× more code than %autodiff." Note: the paper itself never uses the string "Thorin2."
- [Same paper, ACM DL (doi:10.1145/3704840)](https://dl.acm.org/doi/10.1145/3704840) — canonical citation / DOI.
- [SSA without Dominance for Higher-Order Programs (Roland Leißa & Johannes Griebler, arXiv:2604.09961, PLDI 2026)](https://arxiv.org/abs/2604.09961) — free-variable foundation, nesting tree ("a relaxed analogue of the dominator tree constructed from variable dependencies rather than control flow"), log-linear free-var maintenance, dominance "overapproximates data flow." Verified: submitted 10 Apr 2026, PLDI 2026. (Note: the abstract does not use the word "scopeless"; that characterization is from the MimIR repo/docs, not this paper's abstract.)
- [MimIR project site & docs](https://mimir.github.io/) — feature matrix vs LLVM/MLIR, "rooted in the Calculus of Constructions," automatic hash-consing/normalization/type-checking/PE, plugin registry.
- [MimIR source repository (github.com/AnyDSL/MimIR)](https://github.com/AnyDSL/MimIR) — `include/mim/def.h`, `world.h`, `lam.h`, `tuple.h`, `axm.h`, `check.h`; `src/mim/plug/*`; LoC and node-kind names (`Def`, `Univ`, `Type`, `Nat`, `Idx`, `Lit`, `Var`, `Axm`, `Pi`, `Lam`, `App`, `Prod`, `Seq`, `Extract`, `Insert`, `Hole`, `Mut`/`Imm`). Cloned commit dfb94d6.
- [A graph-based higher-order IR (Leißa, Köster, Hack, CGO 2015)](https://www.researchgate.net/publication/301407377_A_graph-based_higher-order_intermediate_representation) — Thorin, the CPS predecessor MimIR/Thorin2 derives from.
- [AnyDSL: a partial evaluation framework (Leißa et al., OOPSLA 2018, doi:10.1145/3276489)](https://doi.org/10.1145/3276489) — Thorin's partial-evaluation lineage and filter heritage (SCHISM-style filters).
- [MimIrADe: Automatic Differentiation in MimIR (Marcel Ullrich, Sebastian Hack, Roland Leißa, CC 2025, doi:10.1145/3708493.3712685)](https://compilers.cs.uni-saarland.de/papers/ullrich_mimirade.pdf) — the `%autodiff` plugin; "Enzyme has roughly 10× more code than MimIrADe" and "Enzyme's cyclomatic complexity is roughly an order of magnitude larger." Its footnote 5 cites `github.com/NeuralCoder3/thorin2` for the downstream AD build, confirming that MimIR was "back then called Thorin2." Verified from the PDF (CC '25, Las Vegas).

## Open questions / uncertainties

- **Exact internal terminology "annex" vs "axiom."** The paper says every `%plugin`-prefixed
  name is an *annex*, of which *axioms* (`axm`) are one kind (others: `let`/`lam`-bound
  Mim definitions). I did not fully verify the complete annex taxonomy from the docs.
- **Canonical home (resolved).** The canonical source repository is `github.com/AnyDSL/MimIR`
  (verified: active issues into 2026, cited by the POPL paper). There is no `github.com/mimir/mimir`
  for this project — that namespace belongs to unrelated projects (e.g. Grafana Mimir). Only the
  documentation site moved: `anydsl.github.io/MimIR` now shows "MimIR moved" and redirects to
  `mimir.github.io`. (An earlier draft of these notes incorrectly stated the *repo* was renamed
  to `mimir/mimir`; corrected.)
- **Precise LoC attribution.** My ~32k/9.4k/8.4k figures come from `wc -l` on one commit
  excluding `external`/`build`; counting conventions (generated headers, submodules) could
  shift these by a few thousand lines.
- **Nesting-tree internals.** I have the PLDI 2026 abstract and the `nest.h`/`free_vars`
  API surface, but not the full algorithm/complexity proof from the paper body.
- **GPU/accelerator backend maturity.** A `gpu` plugin dir exists, but the papers only
  demonstrate a CPU/LLVM backend; how complete the GPU path is, I could not verify.
- **"Polytypic" claim.** The paper repeatedly calls axiom types "possibly polymorphic,
  polytypic, and/or dependent"; the precise sense of *polytypic* (vs higher-kinded) was not
  separately pinned down beyond the array-rank / variadic examples.
- **Dissertation.** A dedicated PhD thesis specifically on MimIR/Thorin2 was referenced in
  the task prompt; I located the three papers above but did not confirm a separate
  standalone dissertation document.
