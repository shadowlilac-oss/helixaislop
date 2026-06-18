# Helix — Glossary

*Every Helix term, alphabetized, each defined in one to three lines, cross-linked to the page that defines it in full. When a term is anchored to a constraint, differentiator, or risk, the ID (DC*, D*, R*) traces to [the synthesis](research/00-synthesis.md).*

---

### Acyclic (invariant)
The Helix graph is a **DAG** — no back-edges, ever (DC7). Loops are `Loop` regions and recursion is a `Module` recursion group, so iteration and recursion are expressed by region nodes, never by graph cycles. Cycles are exactly what defeat optimal extraction. See [Core Model §7](11-core-model.md).

### Alias analysis
A graph pass that partitions memory operations into alias classes and **writes its results back into the graph as additional fine-grained state tokens** (fork/join). It is the load-bearing analysis that makes fine-grained state pay off; if weak, states collapse to one and Helix inherits the conservative trap (R4). See [Types and Effects §4](13-types-and-effects.md).

### Block parameter
See **Port**. Helix uses block parameters for region entry/exit and loop-carried values, never phi nodes (DC5). See [Core Model §2](11-core-model.md).

### Borrow
A weaker form of `fork` for reads: many concurrent immutable readers share one state token and `join` returns it unchanged, modeling read/read non-interference without claiming disjoint footprints. See [Types and Effects §3.2](13-types-and-effects.md).

### Closed term
A node containing no free region binders (ports still in scope). The hash-consing pointer-equality fast path (structural equality == pointer equality) is valid **only** for closed terms; open terms are compared structurally (MimIR's α-equality caveat, DC8). See [Core Model, Invariant C](11-core-model.md).

### Comptime
Compile-time evaluation. In Helix it is **not** a separate interpreter — it *is* Tier-1 graph reduction (DC9), implemented as Normalization-by-Evaluation, driven by filters and bounded by fuel. Constant folding, inlining, loop unrolling, and comptime call evaluation are literally one operation. See [Comptime](15-comptime.md).

### `Cond` (node form 3)
The symmetric conditional / switch — the RVSDG **gamma (γ)**. Hosts k+1 branch regions that all share the same input ports and produce the same result ports; the merge is the result ports, so there is nowhere a phi could live. Maps `if`/`match`/`switch`. See [Core Model §2.3](11-core-model.md).

### `Const` (node form 1)
A structural leaf covering **both literals and types** (types are ordinary values; shallow, not full dependent types). `42`, `i32`, and a function type are all `Const` nodes, hash-consed like any structural node. See [Core Model §2.1](11-core-model.md).

### Critical-pair / overlap check
An offline static check that the `=>` rule set has no two overlapping rules rewriting the same redex to different normal forms without a priority ordering. It substitutes for unprovable global confluence (an open problem, OP-3). See [Reduction Engine §2.4](14-reduction-engine.md).

### DAG
Directed Acyclic Graph — the shape of the Helix node graph (see **Acyclic**). See [Core Model](11-core-model.md).

### Effectful Op
An `Op` with a state operand and a state result: it consumes one state token and produces the next, and is **pinned** in the state strand. Purity is exactly the absence of a state edge — there is no separate "pure" flag. See [Core Model §1, §2.2](11-core-model.md).

### Extraction
The Tier-2 step that picks one alternative per node from the equivalence overlay so total cost is minimized — i.e. instruction selection. Done **greedy bottom-up** (BURS-style DP), not optimal (which is NP-complete), exploiting that overlay e-classes are tiny in practice. See [Reduction Engine §4.2](14-reduction-engine.md) and [Codegen §2](17-codegen.md).

### Filter
A programmer-visible staging annotation (`@comptime`, `static`, `@filter(...)`) attached to a `Func` or parameter, controlling what to specialize — the Thorin/Schism-style alternative to opaque heuristics (DC10). Default policy: specialize type-level and higher-order args aggressively, defer the final run-time continuation. See [Comptime §3](15-comptime.md).

### Fork / Join
The only sanctioned relaxation of state linearity: `fork` splits one state token into N independent sub-tokens; `join` recombines them. Between fork and join the sub-token effects have no ordering edge, so provably-disjoint effects reorder freely. Balanced by construction, so use-exactly-once still holds. See [Types and Effects §3](13-types-and-effects.md).

### Fuel
A per-call, introspectable, localizable step budget bounding comptime reduction (DC11) — learning from Zig's ungettable global quota. Exhaustion names the offending call and static args. It bounds the termination wall (R2) but cannot abolish it. See [Comptime §4](15-comptime.md).

### `Func` (node form 5)
The function / closure — the RVSDG **lambda (λ)**. Has parameter ports, result ports, and an optional state in/out pair (a pure function has none; an effectful one threads a token through its signature). Closures are `Func` nodes with captured free values. See [Core Model §2.5](11-core-model.md).

### Gamma (γ)
The RVSDG name for the symmetric conditional; Helix's `Cond`. See **`Cond`**.

### Hash-consing
Interning structural nodes (`Const`, `Op`) so that **structural equality == pointer equality == automatic GVN/CSE** (DC8). Building a structurally-identical node returns the existing one — value numbering for free, with no worklist sweep. Valid as a pointer-equality fast path only for closed terms. See [Core Model, Invariant C](11-core-model.md) and [Reduction Engine §2.1](14-reduction-engine.md).

### Host fold
The `{..}` form in a rule, e.g. `(const ?t {a + b})` — a comptime computation run on the host (the compiler's runtime) over already-known constant operands. This is the seam where Tier-1 reduction and comptime are literally the same code. See [Reduction Engine §2.2](14-reduction-engine.md).

### Linear state / Linearity (invariant)
Each `state` token is consumed **exactly once** — enforced structurally by a use-count check (fan-out and drop both forbidden), closing MimIR's admitted unenforced-linearity hole (DC4). This makes the state strand a genuine total order per token: the inspectable effect skeleton. See [Core Model, Invariant D](11-core-model.md) and [Types and Effects §2.3](13-types-and-effects.md).

### `Loop` (node form 4)
The tail-controlled (do-while) loop — the RVSDG **theta (θ)** — expressed **without any graph cycle**. Loop-carried values are ports initialized once; `continue` rebinds them, `break unless`/`break if` exits. `while`/`for` are compositions. See [Core Model §2.4](11-core-model.md).

### Lowering rule
A `lower NAME : LHS => RHS @cost N if GUARD` rule that rewrites a portable `Op` pattern into a target-ISA `Op` (e.g. `x64.lea`), recorded as a Tier-2 alternative rather than applied eagerly — because lowering is the canonical "no single best form" case. SMT-verifiable. See [Codegen §1](17-codegen.md) and [Reduction Engine §4](14-reduction-engine.md).

### `Module` (node form 6)
The translation unit — the RVSDG **omega (ω)** plus **delta (δ)** globals and the **phi (φ)** recursion-group role. Holds top-level funcs, globals, and recursion groups; **recursion lives here as a named group, not as a graph cycle** (DC7). See [Core Model §2.6](11-core-model.md).

### NbE (Normalization-by-Evaluation)
The reduction core for comptime: **eval** a graph term into a host semantic domain, then **reify** the result back into graph nodes. Reduction-free (no explicit rewrite worklist); dynamic values become neutral terms. Cleanly splits static from dynamic. See [Comptime §2](15-comptime.md).

### Neutral term
During NbE, a value not known at compile time (a parameter, a runtime `load` result) is reflected into the semantic domain as an opaque **neutral term** carrying its type and defining node. Operations on neutrals stay symbolic; on reify, a neutral term becomes (is "residualized" back into) an `Op` node. See [Comptime §2.1](15-comptime.md).

### Nominal / region node
The mutable family of node forms (`Cond`, `Loop`, `Func`, `Module`): they introduce binders (ports) and host sub-graphs, and are **not** interned by structure (they have identity). Contrast structural nodes. "Introduces a variable ⇒ nominal/mutable." See [Core Model §2](11-core-model.md).

### `Op` (node form 2)
The workhorse structural node: an opcode plus value-operand edges, with an optional state-in / state-out pair. Covers arithmetic, compare, `select`, address calc, memory access, and — after lowering — **target-ISA opcodes**. Pure (no state edge) ⇒ floats; effectful ⇒ pinned. See [Core Model §2.2](11-core-model.md).

### Overlay (Tier-2 / equivalence overlay)
An **acyclic, append-only** structure of union nodes layered over the DAG, recording `~` equivalences and `lower @cost` alternatives. Used sparingly (mostly ISel). Never saturated; resolved by greedy extraction. Stored separately from the interned node table so the graph stays canonical. See [Reduction Engine §4](14-reduction-engine.md) and [Format §7.3](12-format.md).

### Parallel moves
A set of simultaneous `dst <- src` register/stack copies that resolve region-port (block-parameter) transfers at the very end of register allocation — the single place SSA is destructed. Chains are ordered; cycles ("windmills") are broken with a scratch register. See [Codegen §4](17-codegen.md).

### Port (block parameter)
A named, typed entry/exit of a region — the binder a region introduces. Ports carry every merged or loop-carried value that a phi-based IR would need a phi for. **Helix has no phi nodes anywhere** (DC5); the allocator consumes ports directly. See [Core Model §2](11-core-model.md).

### Pure / Purity
A node is pure exactly when it has no state edge: it floats, is hash-consed, is freely duplicable and foldable. Purity is a structural fact (strand membership), not an annotation that can drift. A pure `Func` is one whose returned state origin is its incoming state argument — the gate that makes comptime memoization sound. See [Core Model §1](11-core-model.md) and [Types and Effects §2.1](13-types-and-effects.md).

### Residual / Residualize
The graph left over after comptime reduction fixes the static inputs — the "run-time code" (the mix equation of partial evaluation). To *residualize* is to reify a neutral term back into an `Op` node. See [Comptime §2.1, §7](15-comptime.md).

### Schedule / Scheduling
Assigning a total instruction order. Helix **reads the effect order off the state strand** (the skeleton — no analysis, no fixpoint) and **list-schedules** the floating pure Ops into it. A unique legal schedule always exists because control is canonicalized to structured `Cond`/`Loop`. See [Codegen §3](17-codegen.md).

### Skeleton (side-effect skeleton)
The state strand viewed as the program's pinned, ordered effect backbone — the "pure floats / effects pinned" hybrid made native (DC3). Scheduling is *reading* this skeleton, not recovering it, which sidesteps the VSDG sequentialization disaster. See [Core Model §1](11-core-model.md) and [Codegen §3](17-codegen.md).

### Smart constructor
The **World** factory's only node-creation entry point: it canonicalizes operand order, applies oriented `=>` rules to local fixpoint, then hash-conses — giving folding, identities, and GVN/CSE for free at construction time with no worklist (DC8, Thorin/MimIR/Cranelift lineage). See [Reduction Engine §2.1](14-reduction-engine.md).

### Source map
A side table mapping each `NodeId` to a source span `(file, byte-range)`, populated during direct parsing and propagated through rewrites (a rewritten node inherits its principal input's span). Kept out-of-band so it never pollutes diffs and never defeats GVN. Mitigates R5. See [Format §6.4](12-format.md).

### `specialize(region, bindings)`
The single region-rewrite primitive — lift the free variables, substitute the supplied arguments, re-intern — that subsumes inlining, specialization, unrolling, peeling, and tail-recursion elimination. It is the **same code path comptime uses** (Thorin lambda-mangling analogue). See [Optimizations §5](16-optimizations.md).

### SSA (strict, by construction) (invariant)
Every value edge is **single-origin** (use→def): a value *is* its defining node, so no SSA-restoration pass is ever needed (DC2). There is no mutable storage cell to assign twice; ports are the only merge mechanism. See [Core Model, Invariant B](11-core-model.md).

### State strand
The linear effect-token strand: a `state`-typed value threaded through effectful ops, used exactly once, pinned and ordered. It is the side-effect skeleton (see **Skeleton**) and one of the two strands every edge belongs to. Replaces sea-of-nodes' control + memory edges and RVSDG's state edges. See [Core Model §1](11-core-model.md).

### State token
A single `state`-typed value on the state strand — produced once, consumed exactly once. Multiple **fine-grained** tokens (per alias class / region / effect family) exist from day one; effects on different tokens are representationally independent. See [Core Model §1](11-core-model.md) and [Types and Effects §2.2](13-types-and-effects.md).

### Strand
One of the exactly two kinds of Helix edge: the **value strand** (pure, duplicable, floats) or the **state strand** (linear, pinned, ordered). The two-strand split is the central unifying metaphor. See [Core Model §1](11-core-model.md).

### Structural node
The immutable, hash-consed family (`Const`, `Op`): pure, on the value strand, forming a DAG that floats. Structural equality is pointer equality. Contrast nominal/region nodes. See [Core Model §2](11-core-model.md).

### Theta (θ)
The RVSDG name for the tail-controlled loop; Helix's `Loop`. See **`Loop`**.

### Tier 1 (eager oriented normalization)
The main engine: oriented `=>` rewrites applied to local fixpoint at construction time inside the World factory — fold, identities, CSE/GVN via hash-consing, comptime β/δ. No worklist, no saturation. Almost everything is Tier 1. See [Reduction Engine §2](14-reduction-engine.md).

### Tier 2 (bounded equivalence overlay)
The sparing overlay: `~` equivalences and `lower @cost` alternatives recorded as union nodes in an acyclic, append-only overlay, resolved by greedy bottom-up cost extraction. Used mainly for instruction selection and a little reassociation — deliberately **not** equality saturation (D7). See [Reduction Engine §4](14-reduction-engine.md).

### Tile / Tiling
A many-to-one pattern that covers one root node plus some pure producers, emitting one (or few) target ops — the unit of instruction selection. Tiling matches **up the pure value strand**, stopping at region-port and state-strand boundaries. See [Codegen §2](17-codegen.md).

### Types as values
Types are ordinary `Const`/`Op` nodes of kind `type`, hash-consed like any value — so the **same** reduction engine does type-level and value-level computation (DC9). Deliberately **shallow**: not MimIR's full dependent PTS, so type checking is decidable structural equality and never diverges (contains R2). See [Core Model §5](11-core-model.md) and [Types and Effects §1](13-types-and-effects.md).

### Union node
An overlay entry recording that a value may also be realized as one or more alternatives (e.g. a portable form plus a target tile, each `@cost`). Extraction picks one member. Union nodes live in the Tier-2 overlay, never in the interned node table. See [Reduction Engine §4.1](14-reduction-engine.md) and [Format §7.3](12-format.md).

### Value strand
The pure-dataflow strand: any non-`state` value, with one origin and many users (duplicable), that **floats** and is placed only at scheduling time. One of the two strands; carries data and types. See [Core Model §1](11-core-model.md).

### World (factory)
The hash-consing factory and arena owner — the **only** way to create a node. It is simultaneously the Tier-1 reducer and the comptime evaluator: source flows in through it, becomes interned normalized nodes, and flows out as machine bytes with no secondary IR (DC8). See [Reduction Engine §2.1](14-reduction-engine.md) and [Implementation Plan §1](19-implementation-plan.md).

---

## See also

- [Core Model](11-core-model.md) — the authoritative definitions of strand, port, the six node forms, and the four invariants.
- [Reduction Engine](14-reduction-engine.md) — World factory, Tier-1/Tier-2, the rule DSL, hash-consing.
- [Comptime](15-comptime.md) — NbE, neutral term, residual, filter, fuel.
- [Types and Effects](13-types-and-effects.md) — types-as-values, linear state, fork/join, alias analysis.
- [Codegen](17-codegen.md) — lowering rule, tile, extraction, schedule, parallel moves.
- [Overview](00-overview.md) — the thesis and page map.
- [research/00 — Synthesis](research/00-synthesis.md) — the DC/D/R/failure-mode registers terms trace to.
