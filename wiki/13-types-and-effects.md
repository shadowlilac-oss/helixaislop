# Types & Effects

*Helix's shallow value-typed type system and its linear, fine-grained state-strand effect algebra: how types are ordinary `Const`/`Op` nodes, how effects are typed `state` tokens threaded linearly and enforced, and how provably-independent effects are reordered via fork/join.*

> **⚠️ Design vs. built** (see [24-implementation-status](24-implementation-status.md), [25-path-to-production](25-path-to-production.md)). **Built today:** width/kind type tags (i8/i16/i32/i64/bool/ptr) on nodes; a *single* linear `$mem` state token threaded through loads/stores and enforced by the verifier; total signed division (`x/0→0`, `INT64_MIN/-1→INT64_MIN`, consistent across interpreter and both backends). **Not built:** an actual type *checker* (ill-typed programs are not rejected), unsigned/float/aggregate types, and — most importantly — the **fine-grained effect algebra**: there is exactly one global state token, with **no `fork`/`join`/`borrow` and no alias analysis**, so independent effects are *not* reordered. The "multiple independent states from day one" differentiator is design, not code.

This page specifies the **type layer** and the **effect/state layer** of Helix. Both ride on
the same two strands (see [Core Model](11-core-model.md)): the **value strand** (pure,
duplicable, floats) carries types and data; the **state strand** (linear, pinned, ordered)
carries the side-effect skeleton. Types are values, so the whole type system is just more
value-strand nodes. Effects are `state`-typed values, so the whole effect algebra is just
linear discipline on one distinguished type. Nothing here adds a seventh node form, a side
table, or a separate type graph — that minimality is the lever for "smaller codebase" (DC17).

---

## 1. The type system is shallow by design

Helix types are **ordinary values** built from `Const` and `Op` nodes (DC1, DC17). There is
**no separate type graph and no type universe hierarchy** — a type is just a value whose own
type is the kind `type`. This is deliberately *shallow*: types may be computed at comptime
(they are first-class values flowing on the value strand), but Helix does **not** adopt
MimIR's full dependent Pure Type System.

### 1.1 Why shallow — the MimIR contrast

MimIR collapses term/type/kind/phase into one dependently-typed expression category rooted in
the Calculus of Constructions. That buys enormous expressiveness (size-indexed arrays that
prove in-bounds access, DSL type systems hosted for free) at a price Helix declines to pay:

| Concern | MimIR (dependent PTS) | Helix (shallow, value-typed) |
|---|---|---|
| Type equality | Requires **program equivalence** → undecidable in general; decided by normalization that *can diverge* | **Structural / hash-cons equality** of closed type nodes → pointer equality, always decidable |
| Type checking | Coupled to partial evaluation; a `tt` filter on a non-constant can make the **type checker diverge** | Checking never runs comptime to a fixpoint to decide a type; no divergence path in the checker |
| Author burden | Plugin authors must understand dependent types, CPS-as-`T→⊥`, normalizer trigger counts | A type is a value of kind `type`; ordinary `Op` folding rules apply |
| In-bounds guarantees | Encoded in the type (`Idx n`) | **Not** encoded in the type; bounds are runtime checks or comptime-folded predicates |

The risk MimIR carries here is exactly **R2** (comptime termination/divergence is fundamentally
unsolved) leaking into *type checking itself*. Helix confines R2 to the comptime *evaluator*
(see [Comptime](15-comptime.md)), where a fuel budget bounds it and a divergence is a
localizable comptime error — **never** a diverging type checker. Type checking in Helix is
structural equality over hash-consed closed type nodes (DC8): decidable, O(1) per comparison,
and a direct reuse of the GVN machinery. We keep dependent *hosting* as a non-core stretch goal
only (synthesis table (e), "optional dependent-type hosting … kept as a stretch, not the core").

> Honest scope note: "shallow" means type-level computation is permitted (a type can be the
> result of a comptime fold, e.g. `vec(f32, {N})`) but type *checking* is never the thing that
> might not terminate. We give up MimIR's proof-carrying index types to keep checking trivial.

### 1.2 Types as nodes

Types are interned `Const`/`Op` nodes on the value strand. Canonical constructors:

```
; primitive types — Const nodes of kind `type`
%i32   = ty.int 32                ; signed/unsigned is an op attribute, width is the operand
%u8    = ty.uint 8
%f64   = ty.float 64
%b     = ty.bool
%unit  = ty.unit                  ; the empty tuple type, () — result of pure effect-free ops
%type  = ty.kind                  ; the type of types (one shallow level; NOT a tower)

; aggregates — Op nodes over element/field types
%pair  = ty.tuple (%i32, %f64)         ; positional product
%pt    = ty.struct {x: %i32, y: %i32}  ; named product (field names are part of the node)
%arr   = ty.array %f32, 16             ; fixed-length array; length is a value operand
%slice = ty.slice %f32                 ; ptr+len pair type (length dynamic)

; pointers and functions
%pi    = ty.ptr %i32                   ; pointer to i32 (address space is an attribute)
%fty   = ty.func (%i32, %i32) -> %i32  ; function type (used by Func / indirect call)

; the effect type(s) — see §2
%st    = ty.state                      ; an opaque, linear state token type
%st.m  = ty.state @class("mem")        ; a fine-grained state for one alias class (§2.2)
%st.io = ty.state @class("io")         ; an independent state for externally-visible I/O
```

Because these are hash-consed value nodes, `ty.int 32` constructed twice is the *same* node:
type identity is pointer identity (DC8). Type-level folds are ordinary Tier-1 rules
(see [Reduction Engine](14-reduction-engine.md)):

```
rule ty-arr-elem : (ty.array (ty.array ?t ?n) ?m) => (ty.array ?t {n * m})   ; flatten, host fold
rule ty-ptr-id   : (ty.deref (ty.ptr ?t)) => ?t
```

Pointer-equality fast path for type comparison is valid only for **closed** type terms (DC8 /
MimIR §5.1 caveat); a type that mentions a region port (an open term) falls back to structural
α-comparison. Helix avoids open type terms in practice because the type system is shallow —
types rarely capture binders.

### 1.3 What "shallow" excludes (on purpose)

- No Π/Σ dependent function/tuple types. A `Func` type is a plain arrow `ty.func (...) -> ...`.
- No type-indexed length proofs. `ty.array %t 16` carries a length *value*, not a proof obligation.
- No type universes beyond one `ty.kind`. There is no `Type 0 : Type 1 : …` tower; Girard's
  paradox is sidestepped by simply not having an impredicative type-of-types game to play.
- No subtyping lattice. Conversions are explicit `Op` nodes (`conv.sext`, `conv.bitcast`, …).

This is the simplicity/decidability trade (DC17): fewer type forms ⇒ fewer rule patterns ⇒
smaller checker ⇒ no divergence risk in the checker. It is strictly weaker than MimIR's types
and we say so.

---

## 2. The effect / state algebra

All side effects in Helix are expressed as data flow on the **state strand**. A `state` is a
value of type `ty.state`; it is **linear** — produced once, consumed exactly once (DC4). An
effectful `Op` is precisely an `Op` that takes a `state` operand and produces a new `state`
result; a pure `Op` touches no state and therefore **floats** (DC3). This is the "pure floats /
effects pinned" hybrid taken wholesale from Cranelift's side-effect skeleton and RVSDG's state
edges (D2), and it directly answers failure mode 2 (effect-ordering ambiguity) and failure
mode 3 (the float collapsing to a CFG): effectful code reads exactly like a totally-ordered
CFG because the state strand *is* that order.

### 2.1 Linear state tokens

A state token threads through effectful ops, making program order an explicit data dependency:

```
; effectful: state token threaded linearly (canonical from the spine)
func @bump(%p: ptr, %s0: state) -> (i32, state) {
  (%v, %s1) = load %p, %s0          ; consumes %s0, produces %s1
  %v1 = add %v, 1                   ; pure: NO state edge — floats freely
  %s2 = store %p, %v1, %s1          ; consumes %s1, produces %s2
  return (%v, %s2)
}
```

`%add` carries no state edge, so it floats and is scheduled wherever the value strand permits;
`load`/`store` are pinned in the linear chain `%s0 → %s1 → %s2`. A **pure function** is one
whose returned state origin *is* its incoming state argument (it threads state through
untouched) — same definition RVSDG uses; this is what makes comptime memoization sound (DC11,
purity gate).

### 2.2 Fine-grained, per-alias-class states (DC4)

A single global state is correct but "overly conservative" (RVSDG's own words; failure mode 9).
Helix supports **multiple independent state tokens from day one** — one per alias class, region,
or effect category — so that non-aliasing effects are *not* forced into one chain. This follows
the R-HLS refinement of RVSDG (I/O state edges vs memory state edges) and is differentiator D4.

```
; two disjoint memory classes + one I/O class, threaded independently
func @split(%p: ptr, %q: ptr,
            %m1: state, %m2: state, %io: state)
            -> (state, state, state) {
  %m1a = store %p, 1, %m1           ; effect on alias class 1
  %m2a = store %q, 2, %m2           ; effect on alias class 2 — INDEPENDENT of %m1a
  %io1 = call @puts, "hi", %io      ; externally-visible I/O — independent of both stores
  return (%m1a, %m2a, %io1)
}
```

Here three linear chains coexist. Within each chain order is fixed; *across* chains there is no
ordering edge, so the scheduler is free to interleave or parallelize them. A `Func`/`Loop`/`Cond`
region declares which state tokens cross its boundary as ordinary state-typed ports
(see [Core Model](11-core-model.md)); a region that does not touch a given class simply does not
mention it, which is itself the proof that the region is independent of that class.

> Honest dependency (R4): fine-grained states are only as good as the analysis that *populates*
> them. RVSDG/jlm **had** the representation and still collapsed to a single memory state,
> "leaving significant parallelization potential unused." Representation is necessary but not
> sufficient — §4 covers how an alias pass writes states back, and why this is the load-bearing
> risk for the "optimizes better" claim.

### 2.3 Enforcing linearity (closing MimIR's hole)

MimIR's `%mem.M` "must be used linearly, but this is right now not enforced by the type system"
— an admitted hole. Helix **enforces** it structurally (DC4). The rule is simple because the
graph is already single-origin SSA (DC2): on the value strand an output may be the origin of
*many* edges (it is duplicable); a `state`-typed output may be the origin of **exactly one**
edge.

**Verifier check — use-exactly-once for every `state` value:**

```
for each node n, for each result r of n with type ty.state:
    uses = { edges e in the graph whose origin is r }
    REQUIRE |uses| == 1                 ; not 0 (dropped/leaked), not >1 (duplicated/forked illegally)
for each region (Func / Loop / Cond):
    REQUIRE every state-typed entry port is consumed exactly once inside the region
    REQUIRE every state-typed exit port has exactly one producing origin
```

Three illegal shapes the verifier rejects, with the failure each prevents:

| Illegal shape | What it would mean | Failure mode it prevents |
|---|---|---|
| `state` result with **0 uses** | an effect dropped on the floor / state leaked | silent loss of ordering, DCE eating a store |
| `state` result with **≥2 uses** | two consumers think they own "the latest" state | effect-ordering ambiguity (FM 2); V8's "3 effect chains, got it wrong" |
| `state` consumed across a region boundary it didn't enter as a port | implicit effect smuggling | un-inspectable effects; breaks region isolation |

The single exception is the **explicit `fork`/`join` pair** (§3), which is the *only* sanctioned
way a state value has its linearity temporarily relaxed — and it is balanced by construction, so
the use-exactly-once invariant holds for the fork/join pair as a unit.

Because linearity is structural, no separate "effect type" or monad inference is needed; the
state strand *is* the effect system. This is checkable in one linear pass over the graph and is
part of the standard well-formedness verifier (see [Format](12-format.md) for the textual
round-trip the verifier consumes).

---

## 3. Fork / join: provably-independent effects

Linearity sequentializes a *single* chain, but we want to express that two effects on the *same*
chain are independent and may be reordered or run in parallel. Helix provides two structural
primitives on the state strand — `fork` (a.k.a. split/borrow) and `join` (a.k.a. merge) — that
temporarily split one linear token into several **independent sub-tokens** and recombine them.

```
%i32 = ty.int 32
func @two_disjoint(%p: ptr, %q: ptr, %s0: state) -> state {
  ; %p and %q are KNOWN disjoint (alias pass proved it — see §4)
  (%a, %b) = fork %s0                ; split one token into two independent sub-tokens
  %a1 = store %p, 1, %a             ; uses sub-token %a only
  %b1 = store %q, 2, %b             ; uses sub-token %b only — independent of %a1
  %s1 = join (%a1, %b1)             ; recombine; result depends on BOTH, order-agnostic
  return %s1
}
```

`fork` consumes one state and produces N sub-states (still use-exactly-once each); `join`
consumes the N sub-states and produces one. Between fork and join the two stores have **no
ordering edge relative to each other**, so `@two_disjoint` and a version with the two stores
swapped intern to the *same* graph — reordering is free and GVN sees them as equal.

The strands diagrammed (solid = value, dashed = state):

```
                 %s0
                  |  fork
        +---------+---------+
        :                   :          (: = state sub-tokens, independent)
   store %p,1            store %q,2
        :                   :
        +---------+---------+
                  |  join
                 %s1
```

### 3.1 Independence / commutativity rules the engine uses

The reduction engine (Tier 1, see [Reduction Engine](14-reduction-engine.md)) relaxes ordering
only when independence is *proven* — encoded as guarded rules in the one DSL (DC14). Two
effects commute when they sit on **distinct** state classes, or when an alias guard certifies
their footprints are disjoint, or when both are reads:

```
; introduce a fork when two adjacent effects are on disjoint footprints
rule eff-split :
  (store ?q ?w (store ?p ?v ?s)) => (join (store ?p ?v ?a) (store ?q ?w ?b))
    where (?a, ?b) = fork ?s
    if disjoint(?p, ?q)                       ; alias-analysis guard (§4)

; two reads never conflict — borrow the token immutably, read in any order, return it unchanged
rule read-read :
  (load ?q (load-thru ?p ?s)) ~ (load ?p (load-thru ?q ?s))   ; ~ : Tier-2 equivalence (commutes)

; reads commute with a write to a PROVABLY disjoint location: split the token so the
; read and the store land on independent sub-tokens and become mutually unordered
rule read-write-disjoint :
  (load ?q (store ?p ?v ?s)) =>
    (let ((?a ?b) (fork ?s))                                  ; split into two independent sub-tokens
       (join (store ?p ?v ?a) (load-state ?q ?b)))            ; store on ?a, read on ?b — no ordering edge
    if disjoint(?p, ?q)                                       ; the store still happens; both tokens rejoin

; fork/join algebra: a fork immediately joined is the identity (cleanup after motion)
rule fork-join-id : (join (fork ?s)) => ?s
```

Notes on the strand-kind discipline of these rules:

- The `=>` (Tier-1, oriented, eager) rules in this group are the **only** rewrites permitted to
  touch the state strand, and each is gated by an alias guard or a state-class disjointness
  check. Unguarded state reordering is forbidden — this is the structural answer to failure
  mode 2 and to "effects can't be floated freely" (failure mode 11).
- The `~` (Tier-2, equivalence, overlay-only, *used sparingly*) form records a commutation as a
  union-node alternative for the bounded equivalence overlay; it is never applied eagerly and
  never to a fixpoint (D7 — we deliberately avoid equality saturation).
- `read-read` is safe unconditionally because reads produce no new state in their *value* result
  and only borrow the token; in Helix a pure load on an immutable region (a `load-thru` /
  borrowed read) carries the token through unchanged, so two such loads are unordered.

### 3.2 Borrowing (shared immutable read access)

`fork` for writes splits a token into *exclusive* disjoint sub-tokens. For reads we offer a
weaker `borrow`: many concurrent immutable readers share a token and `join` returns it unchanged,
modeling read/read non-interference without claiming disjoint footprints. This is the state-strand
analogue of shared-vs-exclusive borrows; it lets a batch of loads from one region reorder freely
while still threading a single chain for the eventual write.

```
%s0 ──borrow──▶ {reader, reader, reader} ──join──▶ %s0    ; readers unordered; token returned intact
```

---

## 4. Populating fine-grained states from alias analysis

Fine-grained states are a *representation*; an **alias analysis** is what makes them pay off
(D4, R4). The analysis is itself a graph pass that runs over the state strand and **writes its
results back into the graph as additional, finer state tokens** — exactly the R-HLS / RVSDG
idea of disambiguating a single coarse memory state into many.

Pipeline (all on the same graph, no secondary IR):

```
1. CONSTRUCT  Frontend (DC12) emits ONE coarse memory state per Func (plus one I/O state).
              Correct but conservative: every store/load on the single %m chain.
2. ANALYZE    Alias pass partitions memory operations into alias classes
              (by allocation site, type-based disambiguation, region/escape, points-to).
3. REWRITE    Replace the single %m chain with one state token per class; insert fork at the
              point the classes diverge and join where they must re-merge (e.g. a call that
              may touch several classes, or a region exit).
4. RELAX      Tier-1 eff-split / read-write-disjoint rules now fire under the disjoint() guards
              the analysis proved, letting independent effects float / reorder / parallelize.
```

```
; BEFORE alias analysis — one coarse chain, everything ordered
%m1 = store %p, 1, %m0
%m2 = store %q, 2, %m1            ; forced after %m1 even if p,q disjoint

; AFTER alias analysis proves class(p) ≠ class(q) — two chains via fork/join
(%a, %b) = fork %m0
%a1 = store %p, 1, %a             ; class A
%b1 = store %q, 2, %b            ; class B — now provably independent
%m2 = join (%a1, %b1)
```

The `disjoint(?p, ?q)` guard in §3.1 is **answered by this analysis** — it is not a syntactic
check but a query into the alias result the pass wrote back. The state strand thus carries alias
facts *as data dependencies*, surviving all the way to scheduling and codegen (the schedule is
read off the state strand — see [Codegen](17-codegen.md)).

### 4.1 The honest dependency (R4) — representation is not enough

This is the load-bearing risk for "optimizes better." RVSDG/jlm **had** disjoint state edges and
still, in the prototype, fell back to a single memory state plus one loop state — "leaving
significant parallelization potential unused" (failure mode 9). The lesson:

- If the alias pass is weak, every `disjoint(?p, ?q)` guard fails conservatively, no `fork` is
  inserted, and Helix degenerates to exactly the single-chain RVSDG behavior it set out to beat.
  We inherit the conservative trap, not beat it.
- If the alias pass is *too* eager and unsound, the verifier's linearity check still holds (forks
  are balanced) but the *semantics* are wrong — a write reordered past an aliasing read. So alias
  results must be **sound**; the representation gives us a place to record them but does not
  validate them.
- Therefore the "fine-grained state beats single-state" claim (D4) is **gated on alias-analysis
  quality**, and we say so up front. The representation removes the *RVSDG excuse* (no place to
  put the facts) but transfers the difficulty to the analysis (R4). The win is real but earned,
  not free.

Mitigations Helix leans on: type-based disambiguation falls out of the shallow type system for
free (two `ty.ptr` of incompatible element types do not alias — same heuristic RVSDG used on
"incompatible types"); allocation-site/escape information from `Func`/`Module` structure (DC1)
gives cheap region classes; and because comptime is the same reduction engine (DC9), addresses
that fold to comptime constants give exact disambiguation at no extra cost.

---

## 5. Worked example: multi-state function with reordering

Putting types, linear state, fork/join, and the disjoint-store case together:

```
module demo {
  %i32 = ty.int 32
  %p32 = ty.ptr %i32

  ; Two stores to disjoint regions are independent and may be reordered.
  ; The alias pass proved class(%a) != class(%b); the frontend's single chain
  ; has been rewritten into a fork/join pair.
  func @disjoint_stores(%a: ptr, %b: ptr, %x: i32, %y: i32, %s0: state) -> state {
    %x2 = add %x, %x                 ; pure — floats; no state edge
    (%sa, %sb) = fork %s0            ; split coarse state into two independent sub-tokens
    %sa1 = store %a, %x2, %sa        ; \  these two stores have NO ordering edge between them;
    %sb1 = store %b, %y,  %sb        ; /  swapping the two lines interns to the SAME graph (GVN)
    %s1  = join (%sa1, %sb1)         ; recombine: %s1 depends on both, order-agnostic
    return %s1
  }

  ; A read that provably does not alias a prior store floats above it.
  func @read_floats(%a: ptr, %b: ptr, %s0: state) -> (i32, state) {
    %s1 = store %a, 7, %s0           ; write to class A
    (%v, %s2) = load %b, %s1         ; read class B — read-write-disjoint lets %v move ABOVE the store
    return (%v, %s2)                 ; ordering only matters within a class; here there is none
  }
}
```

Key reading of the example:

1. `%x2 = add ...` has no state operand → it is on the value strand → it floats and is placed at
   schedule time, not pinned (DC3).
2. The two `store`s sit on **different sub-tokens** produced by `fork`, so they are mutually
   unordered. Reordering them is not an optimization that *moves* anything — both orderings are
   the *same* hash-consed graph. That is the structural GVN win (D7), not equality saturation.
3. `join` re-establishes a single linear token for the caller, so `@disjoint_stores` still
   presents a clean linear `state` interface (use-exactly-once holds for the function as a whole).
4. In `@read_floats`, the read on a disjoint class can be scheduled before the store; the
   `state` chain still type-checks because the load merely threads its class's token.

---

## 6. Summary of invariants this page adds

- **Types are value-strand `Const`/`Op` nodes** of kind `type`; identity = hash-cons identity;
  checking = structural equality (decidable, no divergence) — strictly shallower than MimIR (R2 contained).
- **`state` is a linear type**: every `state` result is the origin of **exactly one** edge,
  checked in one verifier pass (closes MimIR's unenforced-linearity hole, DC4).
- **Many independent states from day one**: per alias class / I/O — designed in, not retrofit
  (failure mode 9), but only as good as the alias pass that populates them (R4).
- **`fork`/`join` (and `borrow`)** are the only sanctioned relaxations of linearity; they are
  balanced by construction and let provably-independent effects reorder/parallelize.
- **Independence rules** (`eff-split`, `read-write-disjoint` as Tier-1 guarded `=>`; `read-read`,
  `comm`-style as Tier-2 `~`) are the engine's commutativity laws; state-strand rewrites are
  *always* alias-guarded (failure modes 2, 11).

---

## See also

- [Core Model](11-core-model.md) — the six node forms, the two strands, region ports.
- [Format](12-format.md) — the textual syntax the verifier round-trips, including `state` printing.
- [Reduction Engine](14-reduction-engine.md) — Tier-1 eager `=>` rules and the Tier-2 `~` overlay.
- [Comptime](15-comptime.md) — comptime as graph reduction; why R2 lives here, not in the type checker.
- [Optimizations](16-optimizations.md) — how disjoint-state independence feeds GVN/LICM/DCE (D7).
- [Codegen](17-codegen.md) — scheduling read off the state strand; parallel-move resolution of ports.
- [Risks & Open Problems](22-risks-and-open-problems.md) — R2 (comptime/type-check divergence) and R4 (alias quality).
- [Synthesis](research/00-synthesis.md) — DC1/DC4/DC8/DC17, differentiators D2/D4/D7, risks R2/R4, failure modes 2/3/9/11.
