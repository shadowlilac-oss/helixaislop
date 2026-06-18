# 00 — Synthesis: Design Constraints for a New Unified Compiler IR Graph

> Lead-architect synthesis of prior-art notes 01–08. Goal: drive the design of a NEW
> unified IR graph that is (1) conceptually simpler/shorter than Sea of Nodes, RVSDG,
> Thorin, and MimIR; (2) optimizes better with a smaller codebase; (3) evaluates
> compile-time code *as* graph reduction; (4) parses DIRECTLY to the graph and emits
> machine code DIRECTLY from the graph with **no secondary IR**.
>
> Every claim below is traced to a numbered source file (`01`..`08`). Where a source
> hedged, the hedge is carried forward (see "Differentiators → risks").

---

## (a) Comparison Matrix

Columns: Core abstraction · Control flow · Effects/memory · Optimization approach ·
Comptime / partial-eval · Path to machine code · Codebase complexity · Key strength ·
Key weakness.

| System | Core abstraction | How control flow is modeled | How effects/memory are modeled | Optimization approach | Comptime / partial-eval support | Path to machine code | Codebase complexity | Key strength | Key weakness |
|---|---|---|---|---|---|---|---|---|---|
| **Sea of Nodes** (Click '95; HotSpot C2, Graal, libFIRM, ex-V8 TurboFan) `[01]` | Single directed graph fusing data + control + memory; value ≡ node; ordered inputs, **unordered outputs**; relaxes total order to partial order | Explicit **control edges** + `Region` (merge), `Phi`, `If`+`Proj`, `Start`/`Stop`; control input at index 0 in C2 | SSA-of-memory: `Store` produces a new memory slice, `Load` consumes one; **memory Phis** at merges; **alias/equivalence classes** thread independent memory edges; **anti-dependence edges** keep loads before later stores `[01]` | Combined CCP + DCE + GVN as one optimistic fixpoint (Click thesis, O(n²) / O(n log n)); per-node `idealize()`/peephole to fixpoint; IGVN worklist `[01]` | Constant folding falls out of peephole + conditional constant propagation; Graal/Truffle layers full PE (1st Futamura) on top — not in Click's core `[01]` | Stay in the sea until **Global Code Motion** (schedule early/late + loop-depth pick) recovers a CFG; then Matcher→Mach graph→Chaitin regalloc (C2). CFG is an *output* `[01]` | Teaching impl (SeaOfNodes/Simple) chapterized; C2/Graal are among the hardest code in the JDK; exact LOC unverified `[01]` | Unified graph genuinely enables combined analyses; implicit always-valid SSA; LICM/sinking for free via GCM `[01]` | With pervasive effects the "float" collapses to a CFG (V8); hard to read/debug; scheduler must re-duplicate shared nodes; ~3–7× more L1 dcache misses; V8 retreat → CFG halved compile time, 190× faster load-elim `[01][06]` |
| **RVSDG** (Reissmann/Bahmann et al.; jlm) `[02]` | **Acyclic hierarchical multigraph of nested regions**; demand-dependence form; nodes = computations, regions = hierarchy; descended from VSDG | **Implicit & structured**: no branch/jump edges; γ (gamma) = symmetric conditional, θ (theta) = tail-controlled loop; region nesting *is* the program-structure tree → loops never rediscovered | **Typed edges: value vs state.** State edges order side effects; one state arg/result added per λ-region (total order); can split into **multiple disjoint states** (memory vs I/O, per alias class) → memory-SSA "not formally different from value SSA" `[02]` | Optimizations are graph traversals on the *unoptimized* RVSDG in a single regular pass: CNE (CSE+VN), DNE (dead code+func), INV/PSH/PLL (LICM, sink), IVT (unswitch), RED (fold), inlining, unroll `[02]` | **No dedicated PE/comptime engine** in the papers; only RED (fold/strength-reduce) and CNE congruence. Structure is "amenable" to PE but it's future work `[02]` | **Lowering = "destruction"**: control-flow recovery (SCFR structured-but-bloaty, or PCFR arbitrary/irreducible) + EVALORDER + VARNAMES (re-SSA + copy coalescing); jlm emits LLVM IR (secondary IR!) `[02]` | jlm ≈ 161k LoC C++; **IR core ≈ 22.8k**, LLVM↔RVSDG bridge ≈ 94.6k — most cost is the round-trip, not the IR `[02]` | Strong invariants for free (always-SSA, canonical loops); normalization collapses reorderings; one node algebra spans scalar→loop→function→module; explicit independence/parallelism `[02]` | **Costly CFG round-trip** (construct *and* destruct); irreducible CF needs extra predicates/branches; conservative single-state in practice; research prototype (no exceptions/intrinsics); SCFR code bloat / PCFR GPU-hostile `[02]` |
| **Thorin** (AnyDSL; Leißa/Köster/Hack '15) `[03]` | Higher-order **CPS sea-of-nodes**; everything is a `Def`; three kinds: **continuations** (mutable), **primops** (immutable, hash-consed), **params**; **blockless** — scope by data dependency, not nesting | All control = **continuations** (calls, branches, returns, exceptions, break/continue, loops); `branch : fn(bool, fn(), fn())`; SSA↔CPS correspondence (φ = continuation param) | Memory threaded as an explicit **`mem` token** value through load/store primops (functional store); SSA-construction (Braun '13) even for tuples/structs/arrays so PE treats most state functionally `[03]` | **Lambda mangling** (lift+drop+substitute) is the single primitive subsuming TRE, peeling, unrolling, partial inlining, static-arg transform; `World` does on-the-fly fold/CSE/UCE during construction `[03]` | **Online partial evaluator** (~500 LoC) driven by **Schism-style filters** `@(?x)`/`@@`/`$`; auto-annotates higher-order params; memoizes specialized call sites to avoid induced divergence `[03][07]` | Classify funcs → CFF; `lower2cff`/closure elimination via mangling; Kelsey CFF→SSA; **schedule** floating primops (Click-style early/late); emit via **LLVM** backend (+ C/SPIR-V/etc.) — secondary IR `[03]` | Repo ~97.6% C++; CGO Halstead: `mangle.cpp` ≈132 SLoC vs LLVM's 4-file 1687 SLoC (~2.8× lower difficulty, ~50× lower effort) `[03]` | HOFs first-class & zero-cost (all closures eliminated in CFF programs); blockless removes block float/sink + α-renaming; one rewrite primitive; tiny predictable PE `[03]` | CPS mental model is a real cognitive/debugging cost; non-semantic names; PE termination undecidable (heuristic retreat → explicit filters); destructive heap updates ignored by evaluator; leans on LLVM for codegen; narrow adoption `[03]` |
| **MimIR / Thorin 2** (Leißa/Ullrich/Meyer/Hack; POPL '25) `[04]` | **Sea-of-nodes over a dependently-typed PTS** (Calculus of Constructions); **one syntactic category: expressions** (terms = types = kinds = phases); `Def` base; **immutable/structural** (DAG, hash-consed) vs **mutable/nominal** (cyclic, binders only) | **CPS as `Cn T ≡ T → ⊥`** (continuation = function to Bot); φ = continuation param; unstructured CF = mutually recursive mutable continuations; supports **both direct & CPS** (`%direct` converts); **SSA without dominance** → nesting tree from free-vars (PLDI '26) | **Linear state token `%mem.M`** ("like the IO monad"); load/store/alloc consume+produce `%mem.M`; `%mem.lea` has a dependent result type; linearity **not yet enforced** by the type system (admitted hole) `[04]` | Three layers on the graph: **normalizers** (eager local rewrites at construction, per-axiom C++), β-reduction/PE, and a **pass/phase pipeline that is itself a Mim program** (`%compile`/`%opt`); GVN via hash-consing; eyeing equality saturation (`Rule`/`Reform`) `[04]` | **Filters `@e` per function** drive PE: substitute arg, normalize; if `tt`, β-reduce/inline at compile time. **Same engine does dependent type checking** (type equality via normalization). Default: specialize aggressively except final continuation `[04][07]` | Axioms are **opaque**; lowering is by **plugins**: `%core`/`%math`/`%mem` modeled on LLVM; LLVM backend plugin emits **textual LLVM IR** → LLVM tools (secondary IR); `%clos`/`%direct` convert closures/CPS `[04]` | ~32k LoC C++ total; core `src/mim` ≈9.4k; ~8.4k across 21 plugins; strikingly small for a full extensible typed IR — supports "fewer syntactic categories" claim `[04]` | Radical unification (one graph for terms+types+phases); expressive type system **hosts DSL type systems** (no per-dialect checkers, vs MLIR); first-class type-checked extensibility; principled comptime = PE = type-check; construction-time guarantees `[04]` | Steep barrier (dependent types, CPS-as-⊥, normalizer trigger counts); **type checking can diverge** (relies on filters); normalizer discipline on plugin author (must be deterministic/cycle-free/type-preserving); sea-of-nodes debuggability; thin docs; research maturity `[04]` |
| *Context — LLVM SSA-CFG* `[06]` | SSA over an explicit **CFG of basic blocks**; φ at iterated dominance frontier (Cytron) | Explicit CFG; terminators + φ | Single global mutable memory; independence **re-discovered** by alias analysis repeatedly | Long pipeline of hand-written passes; ordering matters; Alive found 8/334 InstCombine bugs (~2.4%) | Constant folding per-op; no first-class PE | SSA → SelectionDAG/GlobalISel → MIR → regalloc-out-of-SSA → emit (multiple IRs) | The reference; mature, debuggable, ubiquitous; huge | Simplicity, debuggability, total order obvious, mature regalloc/tooling — *why SSA won* | CFG over-specifies order; can't express that two effects are independent → endless alias re-analysis; 6/13 hottest passes just rediscover SSA/loops (~21% of invocations) `[02][06]` |
| *Context — MLIR* `[06]` | **Everything is an Operation**; nested **Regions** of **Blocks**; per-**dialect** ops/types | Structured: regions/blocks + **block arguments** (no φ); `IsolatedFromAbove` enables parallel compilation | Per-op `MemoryEffects` traits/interfaces (no single canonical mechanism) | Pattern rewriting (DRR/PDL), dialect conversion, **progressive lowering** (levels coexist) | Compile-time data in **attributes**; folding hooks per op; PE is dialect-specific | Progressive lowering through dialects down to LLVM/target dialects | Large infra; "region-aware algorithms more involved"; risk of dialect fragmentation | One infra spans many abstraction levels; dialects coexist & lower gradually; block args | Region/nesting overhead; dominance must handle multi-region; "everything is an op" can fragment into weak-semantics dialects `[06]` |
| *Context — Cranelift (aegraph + ISLE + VCode)* `[05][06][08]` | CLIF SSA + **acyclic e-graph** mid-end: pure ops float in a sea-of-nodes, **side-effect skeleton** (CFG) for impure ops; e-node enum = Param/Pure/Inst/Result; **union nodes** record equivalences | CFG skeleton kept for effects; block parameters (no φ); rewrites **prohibit control-flow rewrites** | Pure/effect split is first-class: impure ops pinned in program order in the skeleton; load/store fwd + alias as **separate analyses** | **Acyclic e-graph, eager single-pass rewrites** ("smart constructors", Cascades-style), **no rebuild/fixpoint**; greedy DP extraction; **ISLE** declarative rules shared by mid-end *and* backend lowering; VeriISLE = SMT-verified rules | Constant folding = pure-op rewrite rules; no general PE/staging | **Direct: graph → bytes, no secondary IR.** Single postorder lowering pass → `VCode` (virtual-reg machine code); **regalloc2** in place; **MachBuffer** streaming emit with inline branch fixups/veneers/islands, **no fixpoint** `[08]` | ISLE rules ~27k LoC DSL; regalloc2 >10k LoC dense Rust; deliberately *minimal* aegraph change | Hybrid keeps CFG debuggability + most e-graph wins; declarative SMT-checkable lowering; single-pass everything; one rule DSL twice | aegraph multi-representation paid only **~0.1% runtime for ~0.005% compile cost** — radical e-graphs are *not* a free lunch; avg e-class only 1.13 nodes; optimal ISel/extraction is NP-complete `[05][06][08]` |

---

## (b) Recurring failure modes the new IR must avoid

Each is traced to the system(s) that demonstrated it.

1. **CFG round-trip cost (construction *and* destruction).** Building a structured/dataflow
   graph from imperative source and recovering a legal CFG for codegen is where the real
   engineering lives. **RVSDG/jlm**: the LLVM↔RVSDG bridge is ~94.6k LoC vs ~22.8k for the IR
   core — ~6× more code in the round-trip than the IR itself `[02]`. **VSDG**: Upton/Lawrence —
   "the sequentialization problem is the major obstacle preventing widespread usage of the
   VSDG" `[06]`. *Constraint: the new IR must not need a round-trip — parse directly in, emit
   directly out (task requirement 4).* 

2. **Effect-ordering ambiguity / "what order do effects run."** A flat value-dependence graph
   under-specifies evaluation order; getting it wrong changes observable behavior or
   termination. **VSDG**: "evaluation may terminate even if the original program would not"
   (a VSDG ≈ a *lazy* functional program; eager/speculative eval can introduce non-termination)
   `[06]`. **V8 TurboFan**: engineers juggled "3 different effect chains," "got it wrong
   initially," found the bug "after a few months" `[01]`. *Constraint: effect order must be an
   explicit, typed, fine-grained part of the graph.*

3. **The float collapses to a CFG under pervasive effects.** Sea-of-nodes' partial-order
   benefit is largest for pure numeric code and smallest for effect-heavy code. **V8**: "almost
   all generic JS operations can have arbitrary side effects," so "the control nodes and control
   chain always mirror the structure of the equivalent CFG" — you pay graph complexity for
   little reordering benefit `[01][06]`. *Constraint: make the common effectful case as cheap
   and inspectable as a CFG; only let *pure* nodes float.*

4. **Scheduling / sequentialization cost and oscillation.** Recovering a linear order from a
   floating graph is fiddly and expensive. **V8**: scheduler had to *re-duplicate* shared pure
   nodes onto paths that need them, could oscillate (optimize to 1 division → re-expand to 2),
   "hard to figure out what is inside each loop" `[01][06]`. **VSDG**: "duplicate everything →
   exponential growth" vs "compute early → wasted work / non-termination" `[06]`. *Constraint:
   keep a structure where a unique legal schedule always exists and is cheap to read off.*

5. **E-graph blowup + NP-hard extraction.** Equality saturation often doesn't terminate
   (Peggy saturated only 84% of methods; distributivity/associativity explode the graph) and
   optimal **DAG-aware extraction is NP-complete** `[05][08]`. **Peggy**: Pueblo ILP averaged
   1,499 ms/method (1% hit a 1-min timeout) `[05]`. **Cranelift** *declined* general EqSat
   (needs a "fuel"-bounded strategy driver "not [suitable] for a fast compiler") and the
   acyclic-e-graph multi-representation paid only ~0.1% runtime `[05][08]`. *Constraint: if we
   use equality/rewriting, bound it (fuel), keep it acyclic, extract greedily, and don't expect
   a free lunch from saturation.*

6. **Debuggability / "messy soup of nodes."** A first-order reason for V8's retreat: "Manually/
   visually inspecting and understanding a Sea of Nodes graph is hard" `[01]`. MimIR concedes
   graph IRs need external graph tools to visualize, whereas an instruction list "is
   straightforward to dump even if incomplete" `[04]`. *Constraint: printable, diffable,
   source-mappable from day one.*

7. **Cache thrash from in-place graph mutation / wasted visits.** **V8**: nodes "visited 3
   times but only lowered once," "changed only once every 20 visits," ~3× (up to 7×) more L1
   dcache misses `[01][06]`. *Constraint: favor append-only / construction-time normalization
   over repeated mutating worklist sweeps where possible.*

8. **Comptime non-termination & the termination wall.** Comptime is Turing-complete; no
   specializer always terminates *and* always maximizes specialization. **Thorin** *abandoned*
   its 2015 termination heuristic because "the effects... were difficult to assess for the
   programmer," moving to explicit filters `[03][07]`. **MimIR**: a `tt` filter on a function
   applied to a non-constant can make the **type checker diverge** `[04][07]`. **Zig/C++**:
   fall back on branch-quota / `-fconstexpr-steps` / template-depth budgets `[07]`. *Constraint:
   ship an introspectable, localizable fuel/step budget; memoize by (function, static-args).* 

9. **Conservative single-state memory in practice.** The elegant multi-state story tends to
   stay aspirational. **RVSDG/jlm** uses a single memory state + one loop state and "leaves
   significant parallelization potential unused" `[02]`. *Constraint: design the type system for
   many fine-grained states from day one, not as a retrofit.*

10. **Phase-ordering and the optimization-vs-lowering boundary.** Combined
    scheduling+register-allocation is **NP-hard**; schedule-first raises register pressure,
    allocate-first adds false dependences `[08]`. SSA interference graphs are chordal **only
    before SSA destruction**; RA-after-SSA-elimination is NP-complete `[08]`. **Click**: a clean
    phase boundary (sea until GCM, then Mach graph) is what keeps float where it pays `[01]`.
    *Constraint: pick the RA/schedule order deliberately; allocate before destructing SSA.*

11. **Effects can't be floated or rewritten freely.** Both **PEG/Peggy** (coarse single-σ heap
    summary; "no optimizations across try/catch or synchronization boundaries") and
    **Cranelift** (skeleton "prohibits control-flow rewrites") had to wall off effects from the
    rewrite engine `[05]`. *Constraint: pure values float and rewrite; effectful/control nodes
    are pinned and ordered — make that split first-class, not retrofitted.*

12. **Secondary-IR drift.** Every "graph" system above still emits to a secondary IR for
    codegen: RVSDG→LLVM IR, Thorin→LLVM, MimIR→textual LLVM IR `[02][03][04]`. LLVM itself is
    retreating from the *separate* SelectionDAG IR to GlobalISel/MIR for being slow and
    per-block `[08]`. *Constraint (task req 4): the new IR's nodes must be lowerable in place to
    target ops with no separate backend graph — Cranelift's VCode/MachBuffer is the proof it can
    be done `[08]`.*

---

## (c) Design constraints the new IR must satisfy

Each justified by a specific prior-art finding.

- **DC1 — One graph fuses data + control + (multi-)state.** *Justified by:* Click's combined
  CCP+DCE+GVN fixpoint only works because control and data share one structure `[01]`; RVSDG's
  one node algebra collapses dead-code/dead-func/unreachable into one pass `[02]`. Do not bolt
  control on as a side table.

- **DC2 — Single-origin (use→def) edges = SSA as an enforced structural invariant.** *Justified
  by:* RVSDG's "every input/result is the user of exactly one edge" is *precisely* what makes it
  "always in strict SSA form," erasing 14 LLVM SSA-restoration passes `[02]`; in Sea of Nodes "a
  value *is* a node" so SSA never needs maintenance `[01]`. Make it structural, not derived.

- **DC3 — Pure nodes float; effectful/control nodes are pinned and ordered — a first-class
  hybrid.** *Justified by:* V8's collapse-to-CFG `[01]`, Cranelift's side-effect skeleton, and
  Peggy's σ-summaries independently converged on "pure floats, effects pinned" `[05][06]`. This
  also gives the common effectful case CFG-level inspectability (failure mode 3, 6).

- **DC4 — Effects/memory are explicit, typed, *fine-grained* state tokens, threaded linearly.**
  *Justified by:* RVSDG state edges + multiple disjoint states expose independence a CFG cannot
  `[02][06]`; MimIR's `%mem.M` linear token `[04]`. **Design the type system for many states
  from day one** (RVSDG/jlm's single-state regret, failure mode 9) and **consider *enforcing*
  linearity** (MimIR's admitted unenforced-linearity hole) `[02][04]`.

- **DC5 — Block parameters, not φ-nodes.** *Justified by:* MLIR and Cranelift both ditched φ for
  block/region arguments — cleaner semantics, no "must be first in block," easier rewriting, and
  the allocator consumes them directly `[06][08]`.

- **DC6 — Canonicalize control to a minimal structured set so a unique legal schedule always
  exists.** *Justified by:* RVSDG's single loop form (tail-controlled θ) + symmetric γ make
  optimizations single-pass and guarantee a legal schedule `[02]`; the VSDG sequentialization
  disaster is what happens without it `[06]`. Express while/for as compositions.

- **DC7 — Acyclicity (recursion without graph cycles).** *Justified by:* RVSDG expresses
  recursion via φ-regions without cycles; Cranelift's aegraph is acyclic by construction; both
  cite far easier traversal/scheduling/reasoning `[02][05][06]`. Cycles are exactly what defeat
  optimal extraction `[05]`.

- **DC8 — Hash-consing + construction-time normalization (eager smart constructors).**
  *Justified by:* Thorin's `World` and MimIR both fold/CSE/UCE/normalize *on the fly during
  construction*, which "vastly decreases iterations in the fixed-point loops" and gives value
  numbering for free `[03][04]`; Cranelift's eager single-pass rewrites avoid egg-style
  rebuild/fixpoint and the V8 cache-thrash `[05][08]`. Caveat: pointer-equality ⇒ α-equality
  only for *closed* terms `[04]`.

- **DC9 — Comptime IS graph reduction: one rewrite/PE engine, not a separate interpreter.**
  *Justified by:* the explicit task requirement, and Thorin/MimIR proving it — "specialize a
  continuation/`Def`" subsumes inlining, unrolling, constant folding, and comptime call eval
  `[03][04][07]`. NbE gives a reduction-free eval→reify core and (if we go dependent) decidable
  definitional equality `[07]`.

- **DC10 — Programmer-visible static/dynamic control via filters/annotations, not opaque
  heuristics.** *Justified by:* Thorin's documented *retreat* from termination heuristics to
  Schism-style filters `@(?x)`/`@@`/`$` `[03][07]`; MimIR per-function `@e` filters `[04]`. With
  a sensible default policy (auto-specialize type-level / higher-order params, defer the final
  run-time continuation) most code needs no annotations `[03][04]`.

- **DC11 — Bounded, hermetic, memoized comptime.** *Justified by:* the universal termination
  wall — ship an **introspectable, localizable fuel/step budget** (learn from Zig's ungettable
  global quota); make comptime **hermetic/deterministic** (Zig) so results are cacheable;
  **memoize by (function, static-args)** — sound only under purity `[07]`. Pair purity with the
  effect split (DC3/DC4).

- **DC12 — Parse directly into the graph; no front-end IR.** *Justified by:* Thorin's Impala and
  MimIR's Mim construct the graph directly from the AST via Braun-'13 SSA construction (no
  up-front dominance computation) `[03][04]`. Eliminates a whole stage and its bugs.

- **DC13 — Emit machine code directly from the graph; no secondary backend IR.** *Justified by:*
  Cranelift is the existence proof — single postorder lowering pass → `VCode` → in-place
  regalloc2 → streaming `MachBuffer` emit with inline branch fixups, **no fixpoint, no separate
  backend graph** `[08]`; GlobalISel is LLVM moving the same direction `[08]`. Every prior graph
  IR (RVSDG/Thorin/MimIR) failed this by emitting LLVM IR `[02][03][04]`.

- **DC14 — One declarative, verifiable rewrite DSL shared by optimization *and* lowering.**
  *Justified by:* Cranelift's ISLE drives both mid-end aegraph rewrites and backend instruction
  selection, is overlap/priority-checked, and is SMT-verifiable (VeriISLE) `[05][08]`; Alive
  found 2.4% of hand-written LLVM InstCombine transforms buggy `[06]`. Reuse one rule engine for
  comptime folding, peepholes, and ISel.

- **DC15 — Lower via tree-matching up the pure-dataflow graph, stopping at block-param/skeleton
  boundaries; allocate on SSA before destruction.** *Justified by:* Cranelift's lowering pulls
  only pure producers into a match tree `[08]`; SSA interference graphs are chordal only before
  SSA destruction, and combined schedule+RA is NP-hard `[08]`. Resolve block-param transfers as
  parallel moves at the very end.

- **DC16 — Printable, diffable, source-mapped from day one.** *Justified by:* "soup of nodes" /
  "hard to debug" was a first-order reason for V8's retreat `[01]`, and MimIR concedes the same
  `[04]`. Tooling is not optional for a graph IR; non-semantic names (Thorin) hurt debugging
  `[03]`.

- **DC17 — Aim for a small, minimal node taxonomy.** *Justified by:* MimIR's "fewer syntactic
  categories ⇒ fewer patterns to match," delivering a full extensible typed IR + 21 plugins in
  ~32k LoC `[04]`; Thorin's deliberate "only continuations + primops" kept its PE ~500 LoC
  `[03]`. Minimality is the lever for both "smaller codebase" and "predictable PE."

---

## (d) Differentiators — how the new IR could plausibly beat all four

Concrete mechanisms (with feasibility notes), followed by an honest list of where the claims
are risky.

### Mechanisms

- **D1 — No round-trip at *either* end (parse-in + emit-out direct).** RVSDG/Thorin/MimIR each
  pay a large bridge (RVSDG ~6× the IR core in LoC `[02]`; all three emit LLVM IR
  `[02][03][04]`). If we parse straight into the graph (DC12, proven by Impala/Mim `[03][04]`)
  *and* emit straight to bytes Cranelift-style (DC13, proven by VCode/MachBuffer `[08]`), we
  delete both bridges — the single biggest codebase-size win available. **Feasibility: high** —
  both halves exist in production today; the novelty is doing *both* in one IR that no prior
  system did.

- **D2 — Hybrid float (DC3) as the *default*, sidestepping V8's collapse.** Make the
  side-effect skeleton + floating pure sub-graph the native model rather than a retrofit, so the
  effectful common case is already CFG-cheap/inspectable and we never pay the
  collapse-to-CFG-anyway tax `[01][05]`. **Feasibility: high** — Cranelift demonstrates it; we
  adopt the model wholesale rather than discovering it late.

- **D3 — Comptime = the optimizer's reduction rule, with NbE + filters (DC9/DC10).** A single
  reduction engine that is simultaneously constant-folder, inliner, loop-unroller, and comptime
  evaluator means *one* well-tested code path instead of N. NbE's eval→reify handles the
  static/dynamic split via neutral terms `[07]`; filters give programmer-visible staging `[03]
  [04]`. **Feasibility: medium-high** — Thorin/MimIR prove the *idea*; the win is shipping it
  *without* MimIR's dependent-type machinery (keep PE, drop the PTS — see D5).

- **D4 — Fine-grained typed state from day one (DC4) to beat the conservative-single-state
  trap.** Where RVSDG/jlm regretted collapsing to one state `[02]`, we type memory by alias
  class / region so non-aliasing effects are *provably* independent in the representation, and
  let an alias pass *write its results back as extra states* `[06]`. This is the lever for
  optimizing *better*, not just smaller. **Feasibility: medium** — the representation is
  understood; the open work is the alias analysis that populates fine state without blowing up.

- **D5 — Minimal node taxonomy + one rewrite DSL (DC14/DC17) for "optimizes better, smaller
  codebase."** Combine MimIR's "fewer syntactic categories" `[04]` with Cranelift's
  one-DSL-for-opt-and-ISel `[05][08]`: peepholes, comptime folds, and instruction selection are
  all rules in the same overlap-checked, SMT-verifiable engine. Fewer hand-written passes ⇒
  fewer of Alive's 2.4% bugs `[06]`. **Feasibility: medium-high** — ISLE is the proof for
  opt+ISel; extending the *same* rules to comptime folding is the new step.

- **D6 — Acyclic + construction-time normalization (DC7/DC8) avoids both the e-graph rebuild/
  blowup *and* V8's cache thrash.** Eager smart constructors (Thorin/MimIR/Cranelift) give GVN/
  fold for free with no saturation fixpoint and no repeated mutating sweeps `[03][04][05][08]`.
  **Feasibility: high** — three systems already do this; we are composing proven pieces.

- **D7 — Better optimization at lower cost than EqSat by *not* doing full EqSat.** Cranelift's
  honest ~0.1% aegraph payoff says the real wins are ordinary GVN/LICM/DCE the structure enables
  `[05][08]`. We deliberately target those structural wins (which RVSDG gets as single passes
  `[02]`) instead of chasing saturation — strictly smaller and, per the data, no worse. **
  Feasibility: high**, but it is a *restraint* claim, not a magic-bullet claim.

### Where these claims are risky (honest list)

- **R1 — "Optimizes better with a smaller codebase" is partly in tension.** RVSDG already gets
  CSE/LICM/DCE as single passes on a small core `[02]`, yet jlm is "competitive, not beating"
  LLVM -O3 `[02]`, and Cranelift's radical mid-end bought ~0.1% `[05][08]`. *Beating* LLVM/GCC
  on output quality is unproven by *any* prior graph IR; "better" may realistically mean "matches
  -O2/-O3 with far less code," not "beats -O3." Claiming strict superiority is the riskiest part.

- **R2 — Comptime termination/divergence is fundamentally unsolved.** MimIR's *type checker* can
  diverge under a bad filter `[04][07]`; Thorin retreated from heuristics `[03]`. Our fuel/budget
  (DC11) bounds it but leaks into UX exactly as Zig's quota / C++'s step limits do `[07]`. We
  cannot promise both maximal specialization and guaranteed termination.

- **R3 — Direct codegen forfeits LLVM's backend maturity.** Emitting bytes ourselves (DC13)
  means we own instruction selection, scheduling, and **register allocation** — and
  combined schedule+RA is NP-hard, optimal ISel/extraction is NP-complete, cost is non-local,
  and SSA chordality evaporates after SSA destruction `[08]`. regalloc2 is >10k LoC of dense
  Rust for a *reason* `[08]`. Output quality will lag LLVM until this is mature; this directly
  threatens R1.

- **R4 — Fine-grained state needs a good alias analysis or it degenerates.** D4's payoff is
  gated on populating many states precisely; RVSDG/jlm *had* the representation and still fell
  back to one state `[02]`. Without the analysis, we inherit the conservative trap we set out to
  beat.

- **R5 — Debuggability of any sea-of-nodes is a standing liability.** V8 left partly over this
  `[01]`; MimIR concedes it `[04]`. DC16 is a mitigation, not a solution — tooling cost is real
  and recurring, and "non-semantic names" must be actively avoided `[03]`.

- **R6 — The hybrid float gives smaller reordering wins on effect-heavy code.** The very V8
  lesson that motivates D2 also caps its upside: when most ops are effectful, little floats, so
  the "optimize better" claim is weakest exactly where real programs spend their effects
  `[01][06]`.

- **R7 — One-rule-DSL-for-everything may not stretch to comptime cleanly.** ISLE proves opt+ISel
  `[08]`; nobody has shown the *same* rule engine also serving general partial evaluation with
  filters and fuel. Folding pure constants is easy; staging higher-order/recursive comptime in
  the same engine is unproven (D3/D5 risk).

---

## (e) "Stolen ideas" mapping — which good idea we take from which system

| Idea we take | Source | Why / what it buys us |
|---|---|---|
| Unified data+control(+memory) graph enabling combined analyses | **Sea of Nodes** `[01]` | CCP+DCE+GVN as one fixpoint; SSA implicit (value ≡ node) |
| Global Code Motion intuition (early/late, loop-depth pick) for placing pure nodes | **Sea of Nodes** `[01]` | LICM/sinking fall out of placement, not bespoke passes |
| Single-origin edges = enforced strict SSA invariant | **RVSDG** `[02]` | Erases SSA-restoration/loop-rediscovery passes by construction |
| Canonical structured control (one tail-loop form θ, symmetric γ) → unique legal schedule | **RVSDG** `[02]` | Single-pass opts; avoids the VSDG sequentialization disaster |
| Recursion without graph cycles (φ-region style) → acyclic graph | **RVSDG** `[02]` | Easier traversal/scheduling; doesn't defeat extraction |
| Typed value-vs-state edges + *multiple disjoint* fine-grained states | **RVSDG** (+ R-HLS I/O vs memory states) `[02][06]` | Express that two effects are independent; encode alias results in the IR |
| One uniform node algebra across scalar→loop→function→module | **RVSDG** `[02]` | Collapses pass count (one DNE = dead code+func+unreachable) |
| Blockless: scope by data dependency, not syntactic nesting | **Thorin** `[03]` | Removes block float/sink + α-renaming bug classes |
| One composable rewrite primitive (lambda mangling = lift+drop+substitute) | **Thorin** `[03]` | TRE/unroll/peel/partial-inline/spec from a single well-understood op |
| Online PE driven by Schism-style filters (`@(?x)`/`@@`/`$`) + auto-annotation default | **Thorin** `[03][07]` | Programmer-visible staging; tiny (~500 LoC) predictable evaluator |
| Memory as an explicit functional token; SSA-construct even compound data | **Thorin** `[03]` | Lets the evaluator treat most state functionally |
| Construction-time fold/CSE/UCE in the `World` factory (eager smart constructors) | **Thorin** / **MimIR** `[03][04]` | Free value numbering; fewer fixpoint iterations |
| Immutable/structural (DAG, hash-consed) vs mutable/nominal (cyclic, binders only) split | **MimIR** `[04]` | "Introduces a variable ⇒ mutable" invariant; everything pure is interned |
| Comptime = PE = the optimizer's reduction rule (one engine) | **MimIR** / **Thorin** `[04][07]` | Inlining/unroll/fold/comptime are the same operation |
| Linear `%mem.M` state token (IO-monad style) — and the lesson to *enforce* linearity | **MimIR** `[04]` | Explicit ordering as data dependency; close MimIR's unenforced hole |
| Minimal node taxonomy / "fewer syntactic categories ⇒ fewer patterns" | **MimIR** `[04]` | Smaller codebase + simpler PE |
| Optional dependent-type *hosting* for DSLs (kept as a stretch, not the core) | **MimIR** `[04]` | Powerful, but its undecidable-checking risk argues for keeping it off the critical path |
| Pure/effect split: pure floats, effects pinned in a **side-effect skeleton** | **Cranelift aegraph** / **PEG σ-summaries** `[05][08]` | Makes GVN/LICM/DCE emergent; keeps effects orderable & inspectable |
| Acyclic, append-only, **eager single-pass** rewrites (no rebuild/saturation fixpoint) | **Cranelift aegraph** `[05][08]` | Avoids EqSat blowup *and* V8 cache thrash |
| Greedy/local-cost extraction (exploit tiny e-classes; avg ~1.13) | **Cranelift** `[05]` | Accepts NP-hard-optimal is not worth it; fast in practice |
| Read-phase/write-phase split + deferred deduplicated `rebuild` (*if* we ever do EqSat-style) | **egg** `[05]` | Order-independence + asymptotic congruence speedup, kept in reserve |
| Semilattice e-class analyses / lattice `:merge` for constants/ranges/shapes | **egg/egglog** `[05]` | Semantic facts ride along with rewrites instead of ad-hoc passes |
| One declarative, overlap/priority-checked, SMT-verifiable rule DSL for opt *and* ISel | **Cranelift ISLE / VeriISLE** `[05][08]` | Fewer hand-written-pass bugs (Alive 2.4%); one engine, two jobs |
| Block parameters instead of φ-nodes | **MLIR** / **Cranelift** `[06][08]` | Cleaner semantics; allocator consumes them directly |
| `IsolatedFromAbove`-style isolation for parallel compilation | **MLIR** `[06]` | Use-def chains don't cross the barrier ⇒ parallelizable |
| Progressive/multi-level lowering preserving high-level facts | **MLIR** / **GRIN** `[06]` | Keep non-aliasing/ownership facts alive deep into the pipeline |
| Direct graph→`VCode`→in-place regalloc→streaming `MachBuffer` (no secondary IR, no fixpoint) | **Cranelift backend** `[08]` | The existence proof for task requirement 4 |
| Tree-matching up the pure graph, stopping at block-param/skeleton leaves; many-to-one tiles | **Cranelift lowering** `[08]` | Instruction selection without a separate DAG IR |
| SSA-aware backtracking regalloc with live-range splitting/bundles; allocate before SSA destruction | **regalloc2** + SSA-RA theory `[08]` | What actually ships; chordality only helps pre-destruction |
| NbE eval→reify reduction core; decidable definitional equality (if dependent) | **comptime/PE lineage** `[07]` | Clean static/dynamic split via neutral terms; reduction-free |
| Hermetic, memoized-by-(fn,static-args), fuel-bounded comptime with introspectable budget | **Zig / C++ / online-PE** `[07]` | Cacheable, reproducible; bounds the termination wall with good diagnostics |

---

## Bottom line for the design

Build a **single acyclic graph** with: strict SSA via single-origin edges (DC2), **block
parameters not φ** (DC5), a **pure-floats / effects-pinned-in-a-skeleton** hybrid (DC3),
**fine-grained typed state tokens** (DC4), and a **minimal node taxonomy** (DC17). Construct it
**directly from the parser** with eager hash-consing/normalization (DC8, DC12). Make
**comptime the same reduction engine** as the optimizer, filter-driven and fuel-bounded (DC9–
DC11). Optimize with **one declarative, SMT-verifiable rule DSL** shared by peepholes, comptime
folds, and instruction selection (DC14), targeting the *structural* GVN/LICM/DCE wins (D7)
rather than full equality saturation. Emit **directly to machine code** — single-pass lowering
to a VCode-like array, in-place SSA register allocation, streaming buffer with inline branch
fixups (DC13, DC15) — **with no secondary IR at either end** (D1). The honest caveat: matching
LLVM -O3 *output quality* while owning the entire backend (R3) and *beating* all four on
optimization (R1) are the load-bearing risks; the defensible pitch is **"matches the best graph
IRs on optimization, in dramatically less code, with comptime and codegen unified into the graph
itself."**
