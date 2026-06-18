# Compile-time Evaluation

*Comptime in Helix is not a side interpreter — it is Tier-1 graph reduction (DC9). The same eager normalization that folds `add` and CSEs nodes is the comptime evaluator, implemented as Normalization-by-Evaluation, driven by programmer-visible filters, and bounded by introspectable per-call fuel.*

---

## 1. Thesis: comptime IS graph reduction (DC9)

Helix has no separate constant-evaluation engine, no bytecode VM for `comptime`, and no AST-walking interpreter. Compile-time evaluation is exactly the **Tier-1 eager oriented normalization** described in [Reduction Engine](14-reduction-engine.md): every node is built through the `World` factory, which applies oriented `=>` rules to local fixpoint at construction time. When the operands of an `Op` are all `Const`, the fold rule fires and the result *is* a `Const`. That is constant folding, comptime arithmetic, and value numbering — the same code path.

Because comptime and optimization are the same reduction, the four operations below are **literally one operation** (the Thorin/MimIR lesson, `[03][04][07]`):

| Surface concept | In Helix it is… |
|---|---|
| Constant folding | a `=>` fold rule firing on `Const` operands |
| Function inlining | beta-reducing a `Func` applied to operands |
| Loop unrolling | reducing a `Loop` whose trip count is a `Const` |
| Comptime call evaluation | the *same* beta/delta reduction, run to a closed `Const` |
| Generic / type specialization | beta-reducing a `Func` whose `static` params are `Const` types |

There is no rule that distinguishes "I am optimizing" from "I am running comptime." Reduction proceeds until no oriented rule applies; whatever residual graph remains is the run-time code. This is the **mix equation** of partial evaluation (`research/07`): given static inputs, reduction produces a *residual* sub-graph that, run on the dynamic inputs, equals the original program.

> Provenance: DC9 (comptime = the optimizer's reduction rule), D3 (one engine: folder + inliner + unroller + comptime), DC8 (construction-time normalization). The honest framing of the spine — Helix *uniquely* unifies comptime evaluation AND machine-code emission into the graph — rests on this section plus [Codegen](17-codegen.md).

---

## 2. Normalization-by-Evaluation (NbE): eval → reify

The reduction core is **Normalization-by-Evaluation**. Rather than repeatedly rewriting the graph in place (which causes V8-style cache thrash, failure mode 7), NbE does two passes:

1. **eval** — interpret a graph term into a *semantic domain* in the host (the compiler's own runtime): a static `Const` becomes a host value (an `i64`, a host closure for a `Func`, a host tuple for a struct), and operations compute directly in the host.
2. **reify** — read the semantic value back out as Helix graph nodes (`Const`, `Op`, region nodes).

NbE is **reduction-free**: there is no explicit beta-rewrite worklist. You compute in the host and read the answer back, which is precisely how Zig/LMS/MetaOCaml "run" comptime (`research/07`). This is what lets Tier-1 avoid both e-graph saturation fixpoints *and* mutating worklist sweeps (D6).

### 2.1 The static/dynamic split via NEUTRAL terms

The key to mixing known and unknown values is the **neutral term**. A value that is *not* known at compile time — a function parameter, a run-time `load` result, anything depending on run-time input — does not have a host value. During `eval` it is **reflected** (↑) into the semantic domain as a *neutral term*: an opaque marker carrying its type and its defining graph node. Operations on neutrals stay symbolic; operations on fully-static values compute. During `reify` (↓):

- a fully-static semantic value reifies to a `Const`;
- a neutral term reifies back into an `Op` node (rebuilt — "residualized").

This cleanly partitions the program. Static control is *executed away*; dynamic control is *residualized* into Cond/Loop regions.

```
                 eval (↑ reflect)                 reify (↓)
 graph term  ───────────────────────▶ semantic ───────────────▶ residual graph
                                       value
   Const 7   ───────────────────────▶ host 7  ───────────────▶ Const 7
   %p (param)───────────── reflect ──▶ NEUTRAL(%p) ───────────▶ Op uses %p   (rebuilt)
 add 7, %p  ─── 7 static, %p neutral ▶ NEUTRAL(add 7 %p) ─────▶ (add (const 7) %p)
 add 7, 3   ─── both static ─────────▶ host 10 ───────────────▶ Const 10
```

### 2.2 Worked split

Consider partial application where one argument is static:

```
; before reduction: %k is a run-time value, 8 is static
func @scale(%k: i32) -> i32 {
  %t = mul %k, 8        ; %k neutral, 8 static
  %r = add %t, 8        ; both operands reflect/compute
  return %r
}
```

`eval` produces the semantic value `NEUTRAL(add (mul NEUTRAL(%k) 8) 8)`. The inner `mul` cannot fold (one operand is neutral), but it *can* be rewritten by an oriented strength-reduction rule that fires regardless of staticness, and the outer `add 8` stays symbolic. `reify` emits:

```
func @scale(%k: i32) -> i32 {
  %t = shl %k, 3        ; mul-pow2 rule, Tier-1, fired during reduction
  %r = add %t, 8        ; residual: %k was dynamic
  return %r
}
```

The same engine folded the static part, kept the neutral part, and applied a peephole — one pass, no separate "constant evaluator."

---

## 3. Filters and annotations (DC10)

Helix does **not** rely on opaque heuristics to decide what to specialize — Thorin abandoned exactly that approach because "the effects were difficult to assess for the programmer" and retreated to explicit filters (`[03][07]`, failure mode 8). Staging is **programmer-visible** via filters/annotations in the spirit of Thorin's Schism filters and MimIR's per-function `@e` filters.

### 3.1 Surface syntax

| Annotation | Meaning |
|---|---|
| `@comptime` on a `func` | the function is forced to fully reduce to a `Const` at compile time; a residual call is an error (cf. C++ `consteval`) |
| `static` on a parameter | that parameter is a compile-time binding; callers must supply a `Const`; the function is *specialized* (beta-reduced) per distinct static-arg tuple |
| `@filter(...)` on a `func` | an explicit predicate over arguments controlling *when* to unfold/specialize this call (the general Thorin-style knob) |

```
; a parameter known at compile time
func @shift(static %n: i32, %x: i32) -> i32 {
  %r = shl %x, %n         ; %n is a Const inside the body
  return %r
}

; forced full evaluation — like consteval
@comptime
func @table_size(static %lanes: i32) -> i32 {
  %r = mul %lanes, 4
  return %r
}

; explicit filter: only specialize when the divisor is a power of two
@filter(is_pow2(%d))
func @divmaybe(%x: i32, static %d: i32) -> i32 { ... }
```

### 3.2 Default specialization policy

Most code needs **no** annotations. The default online policy (Thorin/MimIR-style, `[03][04]`) is:

- **specialize type-level arguments aggressively** — a parameter whose type is `ty.*` (a type-as-value `Const`, DC: types are ordinary `Const` values) is treated as `static`. This is how generics monomorphize: passing a type produces a specialized `Func`.
- **specialize higher-order arguments aggressively** — a parameter whose type is a `Func` type is treated as `static`, so closures are specialized away at their call sites (zero-cost abstraction).
- **defer the final run-time continuation** — the last, value-level, run-time-dependent argument is left dynamic; we do *not* try to unfold past it. This is the single most important termination guard the default policy gives for free.

Filters override the default both ways: `static` forces specialization of an argument the default would defer; a `@filter` guard *withholds* specialization the default would perform.

> Provenance: DC10 (programmer-visible filters over heuristics), D3 (filters give staging), failure mode 8 (Thorin's heuristic retreat). Staging is "programmer-visible via filters/annotations, NOT opaque heuristics."

---

## 4. Fuel, hermeticity, and memoization (DC11)

Comptime is Turing-complete, so by the halting problem no specializer both always terminates and always maximally specializes (**R2**). Helix does not pretend otherwise. It bounds reduction with an **introspectable, localizable fuel budget**, runs comptime **hermetically**, and **memoizes** specialized instances.

### 4.1 Per-call fuel

Each comptime reduction is given a **per-call** step budget — a count of reduction steps (rule firings + unfold/unroll iterations). Crucially, the budget is:

- **per-call, not global** — learning from Zig's "single global branch quota that cannot be read" wart (`research/07`). Each `@comptime` call site, each generic instantiation, has its own budget, settable locally.
- **introspectable** — the remaining and consumed fuel is reportable; a diagnostic names *which* call exhausted it and how much it used (see §6).
- **localizable** — a budget annotation can be attached at the call site or function, so blowing the budget points at the offending term rather than the whole compile.

```
; localized budget at the definition
@comptime @fuel(50_000)
func @fib(static %n: i32) -> i64 { ... }
```

### 4.2 Hermeticity and determinism

Comptime reduction is **hermetic**: it cannot perform run-time I/O and observes only the *target* configuration, not the host (the Zig discipline, `research/07`). Combined with the orientation-toward-a-canonical-form discipline and offline overlap/priority checking of the rule set ([Reduction Engine](14-reduction-engine.md) §2.4 — note Helix does *not* claim global confluence), reduction is **deterministic** — the same `(function, static-args)` always yields the same residual. Determinism is what makes results **cacheable** across incremental builds.

### 4.3 Memoization keyed by (function, static-args)

Specialized instances are memoized by the key **(function, static-arg tuple)**. A recursive comptime call that recurs *through the same static arguments* hits the memo table and terminates instead of diverging; distinct static arguments produce distinct specializations (polyvariant specialization, `research/07`).

Memoization is **sound only under purity**. This is where the **state strand** earns its keep: a `Func` with no `state` in/out port is pure, so its `(function, static-args)` result is a pure function of its inputs and is safe to memoize and cache. A function that threads a `state` token is effectful; its result is *not* keyed by value alone and is **not** memoized as a comptime constant. The linear state strand (DC4) thus structurally delimits exactly the region where memoization is sound — closing the soundness gap that bit Zig when "comptime state bundled with runtime data" produced counterintuitive results (`research/07`).

### 4.4 Contrast with prior comptime budgets

| System | Budget mechanism | Granularity | Introspectable? | Hermetic? |
|---|---|---|---|---|
| **Zig** `comptime` | `@setEvalBranchQuota` (backward-branch count) | one **global** quota | no — "cannot be read" | yes (no I/O; observes target) |
| **C++** `constexpr`/`consteval` | `-fconstexpr-steps` (Clang), `-fconstexpr-ops-limit` (GCC), `-ftemplate-depth` | compiler-global flag | no | mostly (constexpr is pure) |
| **MimIR** filters | `@e` filter + memoized continuations; no explicit step budget | per-function filter | partial | n/a (type-checker can diverge) |
| **Helix** | per-call `@fuel(N)` step budget + memo by (fn, static-args) | **per-call, localizable** | **yes** (reported, see §6) | **yes** (state strand delimits purity) |

> Provenance: DC11 (bounded/hermetic/memoized; introspectable fuel), R2 (termination wall is unsolved), DC4 (purity from the state strand makes memoization sound), failure mode 8.

---

## 5. Worked examples

All examples use the canonical Helix textual format (`;` comments, `%name = op operands`, types-are-values).

### 5.1 Comptime-folded constant expression

A pure expression whose operands reduce to `Const` collapses entirely during Tier-1 construction. The applicable oriented rules from the shared rule DSL (DC14):

```
rule fold-add : (add (const ?t ?a) (const ?t ?b)) => (const ?t {a + b})   ; {..} = host fold
rule fold-mul : (mul (const ?t ?a) (const ?t ?b)) => (const ?t {a * b})
```

Source builds `(8 * 4) + 1`. The parser interns each node through the `World`; folds fire immediately:

```
; --- as parsed, before any fold (conceptual) ---
%a  = mul (const i32 8) (const i32 4)     ; fold-mul ⇒ const i32 32
%r  = add %a (const i32 1)                ; fold-add ⇒ const i32 33

; --- residual graph actually stored ---
func @k() -> i32 {
  return 33                                ; %r interned as a single Const
}
```

No worklist, no separate evaluator: the constant existed only momentarily as host integers inside `eval` and was reified once to `const i32 33`.

### 5.2 Comptime-specialized generic function (power, unrolled)

A classic Ershov `power` specialized at a static exponent. The exponent `%n` is `static`, so the default policy beta-reduces and the static `Loop` unrolls (loop unrolling = reducing a `Loop` with a `Const` trip count — same operation as everything else, §1):

```
@comptime @fuel(10_000)
func @powc(static %n: i32, %x: f64) -> f64 {
  %r = loop (%acc = 1.0, %i = 0) : f64 {
    %c = cmp.lt %i, %n           ; %i and %n both static ⇒ condition folds each step
    break unless %c -> %acc
    %acc1 = mul %acc, %x         ; %x is NEUTRAL ⇒ residualized
    %i1   = add %i, 1            ; static ⇒ folds
    continue (%acc1, %i1)
  }
  return %r
}
```

Specializing at `%n = 3`: the loop condition is static at every step so the `Loop` is fully unrolled; `%acc` starts as the static `1.0`; each `mul %acc, %x` has a neutral operand (`%x`) so it is residualized; `mul 1.0, %x` folds via the `mul-one` identity. Residual:

```
func @powc$n3(%x: f64) -> f64 {     ; memoized under key (@powc, n=3)
  %t1 = mul %x,  %x                  ; acc = 1*x  ⇒ x   (mul-one), then *x
  %t2 = mul %t1, %x
  return %t2
}
```

No loop, no counter, no comparison survive — they were all static and reduced away. A second call with `%n = 3` reuses `@powc$n3` from the memo table; a call with `%n = 5` produces a distinct specialization. This is **polyvariant specialization** and it is the **First Futamura Projection** in miniature: `@powc` plays the role of an interpreter of the exponent, and specializing it to a fixed exponent yields "compiled" straight-line code (`research/07`).

### 5.3 Comptime-computed lookup table embedded as a Const

A table computed at compile time becomes a single aggregate `Const` baked into the module — no run-time initialization code, no static initializer ordering. `@comptime` forces full evaluation:

```
@comptime @fuel(100_000)
func @sin_table() -> [256 x i16] {
  %t = loop (%arr = const.undef [256 x i16], %i = 0) : [256 x i16] {
    %c = cmp.lt %i, 256
    break unless %c -> %arr
    %ang  = comptime.sin_fixed %i      ; host fold: pure, hermetic, deterministic
    %arr1 = insert %arr, %i, %ang      ; all operands static ⇒ folds in the host
    %i1   = add %i, 1
    continue (%arr1, %i1)
  }
  return %t
}

; after reduction, the call site holds the materialized aggregate Const:
%SIN = const [256 x i16] [0, 402, 804, 1206, ... ]   ; 256 entries, fully baked
```

Because every operand is static, `eval` computes the whole array as a host value and `reify` emits one aggregate `Const`. The result is hermetic and cached: an incremental rebuild that does not touch `@sin_table` reuses the baked `%SIN` (§4.2/§4.3). The table lives in the value strand as immutable data and is hash-consed — two modules computing the same table share one node (DC8).

---

## 6. Honesty: termination and the fuel-exhaustion diagnostic (R2)

Comptime is Turing-complete. Fuel bounds it, but **the bound leaks into the UX** exactly as Zig's quota and C++'s step limits do (R2, `research/07`). Helix cannot promise both guaranteed termination *and* maximal specialization; this is not an implementation gap, it is the halting problem. MimIR shows the sharp edge: a bad filter can make even the *type checker* diverge (`[04][07]`). Helix's mitigations are precisely DC11 — per-call fuel, hermeticity, memoization — but they are mitigations, not a cure.

What Helix *does* promise is a **good diagnostic**: introspectable and localizable, naming the offending call and the static arguments in flight, unlike Zig's unreadable global quota.

```
error[E-COMPTIME-FUEL]: comptime evaluation exhausted its fuel budget
  --> demo.hx:42:11
   |
42 |   %r = call @ackermann(static %m, static %n)
   |            ^^^^^^^^^^^^ specializing @ackermann with (m = 3, n = 14)
   |
   = consumed: 10_000 / 10_000 reduction steps (budget set at demo.hx:39 via @fuel)
   = deepest residual frame: @ackermann$m3 -> @ackermann$m2 (recursion not memoizable:
     distinct static args at each level, so the memo key (function, static-args) never repeats)
   = note: this call may not terminate at compile time.
   help: raise the budget locally with @fuel(N) on the call or function,
         or mark a parameter dynamic (remove `static`) to residualize instead of unfold.
```

The diagnostic distinguishes the two failure shapes:

- **non-termination** — recursion whose `(function, static-args)` key never repeats, so memoization (§4.3) cannot halt it. The fix is to make an argument dynamic (residualize) or accept the bound.
- **code blowup** — termination that simply produces too much residual (aggressive online specialization without filters, `research/07`). The fix is a `@filter` to withhold unfolding.

> Provenance: R2 (termination/divergence fundamentally unsolved), DC11 (introspectable/localizable budget with good diagnostics), failure mode 8. This page deliberately does not claim Helix solves comptime termination; it claims a *better-bounded, better-reported* comptime than the prior art, which is the honest pitch.

---

## 7. Futamura framing and the "one operation" claim

Partial evaluation theory gives the spine its vocabulary (`research/07`):

- The **mix equation**: reducing a program with static inputs fixed yields a *residual* that, on the dynamic inputs, behaves like the original. Helix's Tier-1 reduction *is* `mix`.
- The **First Futamura Projection**: specializing an interpreter to a fixed program yields a compiled program. §5.2 is a micro-instance (specialize a power "interpreter" of the exponent → straight-line code). Helix is designed so the specializer applied to an embedded interpreter expresses compilation, though Helix does not claim a self-applying `cogen`.

The payoff of DC9/D3 is that **inlining, loop unrolling, constant folding, and comptime call evaluation are the same operation** — beta/delta reduction over the graph (Thorin/MimIR, `[03][04]`). One well-tested code path replaces N hand-written passes, which also means fewer of the Alive-class bugs that plague hand-written transforms (`[06]`, D5). The risk is the converse: extending *one* rule engine to general filtered, fuelled, higher-order partial evaluation is unproven at scale (**R7**) — folding closed constants is easy; staging recursive higher-order comptime in the same engine is the new step, and we flag it as such.

---

## See also

- [Reduction Engine](14-reduction-engine.md) — the two-tier engine; comptime is Tier-1.
- [Core Model](11-core-model.md) — the six node forms; `Const` (types are values), `Func`, `Loop`, `Cond`.
- [Types and Effects](13-types-and-effects.md) — the state strand and linear state that delimit memoization soundness.
- [Optimizations](16-optimizations.md) — GVN/LICM/DCE as structural wins, sharing the same reduction.
- [Codegen](17-codegen.md) — the *other* half of the unification: emitting machine code from the same graph.
- [Design Rationale](10-design-rationale.md) — DC9/DC10/DC11, D3, R2/R7.
- [Risks and Open Problems](22-risks-and-open-problems.md) — R2 (termination) and R7 (one-DSL-for-comptime) in full.
- [Glossary](23-glossary.md) — NbE, neutral term, residual, filter, fuel, polyvariant.
- Research grounding: [research/07 — Comptime & Partial Evaluation](research/07-comptime-partial-eval.md), [research/03 — Thorin](research/03-thorin.md), [research/04 — MimIR](research/04-mimir-thorin2.md), [research/00 — Synthesis](research/00-synthesis.md).
