# Helix Format — Textual & Binary

*The canonical human-readable text form and the interned in-memory/binary form of the Helix graph, plus the tooling contract (printable, diffable, source-mapped) that pays down the sea-of-nodes debuggability liability (R5, failure-mode 6).*

Helix has exactly one IR graph, but two serializations of it:

- the **textual format** — an elegant, line-oriented surface that a human reads, diffs, and reasons about;
- the **binary / in-memory format** — a flat interned node table that *is* the hash-consed DAG.

Both serializations describe the same six node forms (`Const`, `Op`, `Cond`, `Loop`, `Func`, `Module`) connected by the two strands (VALUE and STATE). Neither serialization is a *secondary IR*: the text is a faithful print of the live graph, and the binary table is the live graph laid out in memory. Printing then re-parsing is the identity on the graph (deterministic round-trip, defined in [§7](#7-round-trip-text--nodes--text)).

> Provenance: this page realizes **DC16** (printable/diffable/source-mapped from day one), supports **DC8** (hash-consing ⇒ the binary table), **DC5** (block parameters / ports, never phi), **DC2** (single-origin edges), **DC4** (linear state tokens), and is the answer to **failure-mode 6** and **R5** (sea-of-nodes is hard to read). See [Core Model](11-core-model.md) for node semantics and [Codegen](17-codegen.md) for how the same node table is lowered in place.

---

## 1. Lexical grammar

The textual format is line-oriented; whitespace (other than newlines separating statements) is insignificant. Comments run from `;` to end of line. There is no block-comment form — diffs stay line-granular on purpose (DC16).

```
comment    ::= ';' .* EOL
ident-sigil::= '%' | '@' | '#'           ; value / global-or-func / type-or-attr
name       ::= ('%' | '@' | '#') (alpha | '_') (alnum | '_' | '.')*
opcode     ::= (alpha | '_') (alnum | '_' | '.')*     ; e.g. add, cmp.lt, x64.lea, ty.int
int-lit    ::= ['-'] digit+ | '0x' hexdigit+
str-lit    ::= '"' (char | esc)* '"'
hostexpr   ::= '{' .* '}'                ; comptime host computation, opaque to the grammar
```

Sigil convention (this is part of DC16's *stable semantic naming* — every printed name carries its namespace):

| Sigil | Namespace | Examples |
|-------|-----------|----------|
| `%`   | local SSA value or state token (function-scoped) | `%x`, `%r`, `%s0`, `%acc` |
| `@`   | module-level symbol: function, global | `@add`, `@bump`, `@global_table` |
| `#`   | type values and attributes (types ARE values — `Const`) | `#i32`, `#ptr`, `#state` |

`%`-names are *not* semantically meaningful to the graph — a value IS its defining node (DC2). They are stable, human-assigned labels printed deterministically so diffs are stable (see [§6](#6-tooling-dc16-printable-diffable-source-mapped)). State tokens are ordinary `%`-values whose type is `#state` (or a fine-grained `#state.<class>`); they are distinguished only by type and by the linearity invariant (DC4), not by a separate sigil.

---

## 2. Type syntax (types are values)

Types are ordinary `Const` nodes (DC17: no separate type category — *shallow* types, NOT full dependent types). They are produced by `ty.*` opcodes and are hash-consed like any other structural node, so `#i32` appearing twice is the *same* node.

```
%t = ty.int 32                ; the type "32-bit integer"
%u = ty.int 1                 ; i1 / bool
%p = ty.ptr                   ; opaque pointer
%s = ty.state                 ; the default (coarse) state type
%sm = ty.state #mem           ; a fine-grained state class (alias class "mem")  (DC4)
%f = ty.func (#i32, #i32) -> #i32
%a = ty.array #i32 16
```

For brevity the printer offers **canonical type shorthands** — `#i32`, `#i1`, `#ptr`, `#state`, `#state.mem` — which are pure sugar for the corresponding `ty.*` `Const`. They appear wherever a type is used as a standalone *value* (an operand, a `type_id`, a `ty.func`/`ty.array` argument). In a `: type` annotation or a region header (`%x: i32`, `-> i32`, `: state.mem`) the bare name is written — this is the canonical surface form used in every example below. The `ty.*` long form appears only when a type is bound to a name. All three (`#i32`, bare `i32`, and `%t = ty.int 32`) parse to the identical interned `Const` node.

---

## 3. Definition syntax

The core statement is a value definition. Every value edge is single-origin (DC2): the left-hand name is *defined* exactly once and every later use refers back to it.

```
%name        = opcode operand* (: type)?            ; pure Op or Const
(%a, %b, ..) = opcode operand*                      ; multi-result (e.g. effectful Op)
```

- Operands are comma-separated value names (or inline literals / type shorthands).
- The optional `: type` annotation is checked by the parser but is redundant with the node's computed type; the printer emits it for readability and omits it where it is obvious (configurable; the *diffable* canonical printer always emits it — [§6](#6-tooling-dc16-printable-diffable-source-mapped)).
- **Effectful Ops** are exactly the Ops that take a state operand and produce a new one (spine: "Effectful Op = an Op with a state operand and a state result"). They are written as multi-result defs that thread the state token explicitly.

```
%v   = add %x, %y : i32                ; pure Op, no state
(%v, %s1) = load %p, %s0               ; effectful Op: consumes state %s0, yields value %v and new state %s1
%s2  = store %p, %v1, %s1              ; effectful Op: consumes %s1, yields new state %s2
```

The state strand is **linear** (DC4): `%s0` must be used exactly once. The parser rejects a token that is dropped or used twice — this is the structural enforcement that closes MimIR's admitted unenforced-linearity hole. Pure Ops (`add`, `cmp.lt`, address calc, …) have no state operand and FLOAT; their textual *order* is irrelevant and is recovered only at scheduling time by reading the state strand ([Codegen](17-codegen.md), DC15).

---

## 4. Region syntax (Cond / Loop / Func / Module)

The four nominal/region forms host sub-graphs and introduce **ports** = block parameters (DC5). There are no phi nodes anywhere. Region results are produced by `yield` / `continue` / `break` / `return`, which bind the region's result ports positionally.

### 4.1 Func (the lambda)

```
func @name(%p0: t0, %p1: t1, ...) -> r0 { ... return (...) }
func @name(%p0: t0, ..., %s0: state) -> (r0, state) { ... return (..., %sN) }   ; effectful func
```

Parameter ports are the `%p`-names in the header; result ports are positional in `return`. A function that performs effects takes an incoming state port and returns an outgoing one, threading the strand through its body. `@comptime` and per-parameter `static` annotations (the Schism-style filters, DC10) attach in the header:

```
@comptime
func @pow(static %n: i32, %x: i32) -> i32 { ... }   ; %n is a static (comptime) parameter
```

### 4.2 Cond (the gamma — symmetric conditional / switch)

A `cond` selects among branch *regions* that share the same input ports and produce the same result ports (DC6). Each `case` is a region; `yield` binds the shared result ports. No phi — the merge is the `cond`'s result ports.

```
%r = cond %pred -> (i32) {
  case 1: { %t = sub %b, %a   yield %t }
  case 0: { %u = sub %a, %b   yield %u }
}
```

A multi-way switch lists more cases (`case 0:`, `case 1:`, … `default:`); all share the result-port arity. If a branch performs effects, the `cond` also threads a state port in and out (the `-> (...)` arity then includes `state`, and each `yield` produces the outgoing token).

### 4.3 Loop (the theta — tail-controlled, acyclic)

A `loop` is expressed WITHOUT a graph cycle (DC7). Loop-carried values are ports declared in the header with their initial values; `continue (...)` rebinds them for the next iteration, `break unless`/`break if` exits with the result ports. The body runs, then the tail decides — this is the single canonical (tail-controlled) loop form (DC6).

```
%r = loop (%acc = 0, %i = 0) : i32 {
  %c = cmp.lt %i, %n
  break unless %c -> %acc          ; exit, yielding %acc as the loop result
  %acc1 = add %acc, %i
  %i1   = add %i, 1
  continue (%acc1, %i1)            ; rebind loop-carried ports
}
```

An effectful loop adds a state port to the carried set, e.g. `loop (%acc = 0, %i = 0, %s = %s0) : (i32, state) { ... }`, and `continue`/`break` carry the threaded token.

### 4.4 Module (the omega/delta — translation unit)

The top level. Hosts funcs, globals, and **recursion groups** — recursion is expressed *here* as a named group, NOT as a graph cycle (DC7).

```
module demo {
  global @table : array #i32 16

  rec group {                      ; mutually recursive funcs live in a rec group, not a cycle
    func @even(%n: i32) -> i1 { ... %r = call @odd, %n1 ... return %r }
    func @odd (%n: i32) -> i1 { ... %r = call @even, %n1 ... return %r }
  }

  func @add(%x: i32, %y: i32) -> i32 { ... }
}
```

---

## 5. Complete examples

These reuse the spine examples verbatim and extend them in the same style. Each is a faithful print of a real graph; each round-trips (§7).

### 5.1 Pure function, effectful function, gamma

```
module demo {
  func @add(%x: i32, %y: i32) -> i32 {
    %r = add %x, %y
    return %r
  }

  ; effectful: state token threaded linearly (DC4)
  func @bump(%p: ptr, %s0: state) -> (i32, state) {
    (%v, %s1) = load %p, %s0          ; effectful Op: consumes %s0, produces %s1
    %v1 = add %v, 1
    %s2 = store %p, %v1, %s1          ; consumes %s1, produces %s2
    return (%v, %s2)
  }

  ; symmetric conditional (gamma) — results are ports (block params), no phi (DC5)
  func @absdiff(%a: i32, %b: i32) -> i32 {
    %p = cmp.lt %a, %b
    %r = cond %p -> (i32) {
      case 1: { %t = sub %b, %a   yield %t }
      case 0: { %u = sub %a, %b   yield %u }
    }
    return %r
  }
}
```

### 5.2 Tail-controlled loop (theta)

```
module demo {
  func @sum(%n: i32) -> i32 {
    %r = loop (%acc = 0, %i = 0) : i32 {
      %c = cmp.lt %i, %n
      break unless %c -> %acc
      %acc1 = add %acc, %i
      %i1   = add %i, 1
      continue (%acc1, %i1)
    }
    return %r
  }
}
```

### 5.3 Effectful loop with a fine-grained state class

A loop that sums an array through memory. The state token has class `#mem` (DC4) so an alias pass can later prove independence from, say, an `#io` token without re-analysis (failure-mode 9, D4).

```
module demo {
  func @sum_arr(%p: ptr, %n: i32, %s0: state.mem) -> (i32, state.mem) {
    %r = loop (%acc = 0, %i = 0, %s = %s0) : (i32, state.mem) {
      %c = cmp.lt %i, %n
      break unless %c -> (%acc, %s)
      %addr        = add %p, %i               ; pure address calc — FLOATS
      (%v, %s1)    = load %addr, %s            ; effectful load on the #mem strand
      %acc1        = add %acc, %v
      %i1          = add %i, 1
      continue (%acc1, %i1, %s1)
    }
    return %r
  }
}
```

### 5.4 After lowering — target Ops in the same graph (no secondary IR, DC13)

Lowering rewrites portable Ops into TARGET Ops *in the same node table*; the printer just shows target opcodes. The `lea` rule from the spine fired on the address calc:

```
; lower lea : (add ?b (mul ?i (const i64 ?s))) => (x64.lea ?b ?i ?s) @cost 1  if member(?s,{1,2,4,8})
func @index(%b: ptr, %i: i64) -> ptr {
  %a = x64.lea %b, %i, 4 : ptr        ; one target Op replacing add+mul+const  (Tier-2 pick)
  return %a
}
```

---

## 6. Tooling (DC16): printable, diffable, source-mapped

Sea-of-nodes' first-order liability is that the graph is a "messy soup of nodes" that needs external visualization (V8's retreat; MimIR's concession — failure-mode 6, R5). Helix makes the *text* the primary debugging surface and guarantees three properties:

### 6.1 Printable

Any sub-graph reachable from a `Module`, `Func`, or single node prints to the canonical text above with no external tool. Because the graph is acyclic (DC7) and strict-SSA (DC2), a single post-order walk emits a valid def-before-use listing; ports are emitted at their region headers. Effect order is *visible* because the state strand threads through the printed lines — you read effect order straight off the `%sN` chain, unlike a pure value-dependence soup (failure-mode 2).

### 6.2 Stable semantic naming

Thorin's non-semantic names were called out as a real debugging cost (R5, D-table). Helix assigns **stable, deterministic, source-derived names**:

1. If a value originates from a named source binding, its `%`-name is that source name (disambiguated with a numeric suffix on collision: `%x`, `%x.1`).
2. Otherwise the name is derived from the opcode and a deterministic ordinal (`%add.3`).
3. Global symbols keep their source identifier under `@`.

Names are a *function of the graph and the source map*, never of allocation order or visitation order, so they do not churn between runs.

### 6.3 Diffable (deterministic printing)

The canonical printer is a pure function of the graph: a fixed traversal order (post-order, operands left-to-right; regions in declaration order; module members sorted by a stable key — source order, then name). Given the same graph it emits byte-identical text. Consequences:

- two compiles of the same source diff to *nothing*;
- an optimization's effect is a *minimal* textual diff (only the rewritten lines move), which is exactly what a developer or a test snapshot wants;
- the `: type` annotation is always emitted in canonical mode so a type change shows as a line diff rather than silently.

### 6.4 Source-map encoding (node → source span)

Every node carries an optional **source span** = `(file-id, byte-start, byte-end)`. The parser populates it during direct-to-graph construction (DC12); rewrites *propagate* it (a rewritten node inherits the span of its principal input, and Tier-2 alternatives keep all contributing spans so a lowered `x64.lea` can point back to the `a[i]` that produced it).

In text, spans are an out-of-band sidecar so they never pollute diffs:

```
; --- demo.helix.map (sidecar; one line per node id) ---
n42  add    demo.src:118-123
n43  load   demo.src:140-149
n44  x64.lea demo.src:118-123     ; lowered node inherits the source span of the add it replaced
```

A `@loc(file:start-end)` inline annotation form exists for hand-written tests, but the canonical pipeline keeps spans in the sidecar. This is the concrete mitigation for R5: a node is always traceable to source, and an optimization log is a diff plus a span map — *not* a graph-visualization session. (Honest caveat: this *mitigates* R5, it does not eliminate it — deeply rewritten code can still be hard to follow even with spans; visualization tooling remains useful for tangled state strands.)

---

## 7. Binary / in-memory format

The binary form is the graph itself: a flat, append-only **interned node table**. Hash-consing (DC8) means construction *is* interning — building a structural node that already exists returns the existing id, which is automatic GVN/CSE. The table doubles as the on-disk format (a length-prefixed dump of the same records), so there is no separate serializer/deserializer IR.

### 7.1 Node record layout

Every node is a fixed-header + variable-tail record. Ids are dense 32-bit indices into the table (`NodeId`); there are no pointers, which keeps the table relocatable and cheap to mmap.

```
struct NodeRecord {            // little-endian
  u8    form;                  // 0=Const 1=Op 2=Cond 3=Loop 4=Func 5=Module
  u8    flags;                 // bit0 STRUCTURAL(interned) ; bit1 HAS_STATE_IN ; bit2 HAS_STATE_OUT
  u16   opcode;                // interned opcode id (e.g. add, x64.lea, ty.int); 0 for non-Op forms
  u32   type_id;              // NodeId of this node's type (a Const) ; 0 if N/A
  u32   state_in;             // NodeId of consumed state token, or NIL(0) if pure
  u32   n_operands;           // count of value operands that follow
  u32   operands[n_operands]; // NodeId of each value operand (the VALUE strand, single-origin DC2)
  // --- region tail (forms Cond/Loop/Func/Module only) ---
  u32   n_ports;              // block-parameter ports introduced by this region (DC5)
  u32   n_regions;            // sub-regions (cases for Cond, body for Loop/Func, members for Module)
  RegionRef regions[n_regions];
  // --- payload tail (Const only) ---
  u32   const_len; u8 const_bytes[const_len];   // literal/type payload
  // --- side table (NOT hashed) ---
  u32   span_file; u32 span_lo; u32 span_hi;     // source map (DC16)
  u32   print_name;                              // interned stable name id (DC16); derived, not hashed
}
```

Key points:

- **`state_in` is the STATE strand.** A pure Op has `state_in = NIL`. An effectful Op has `state_in` set; its *new* state token is the node itself (the result you thread forward) or an explicit `Result` projection for multi-result ops. State edges are thus represented identically to value edges (single `NodeId` slots) — the linearity invariant (each state-producing node has exactly one consumer) is enforced by a **use-count check over `state_in` slots**, not by a separate edge kind. This is the in-memory form of "one rule for both strands" (DC4): there is no second graph for control or memory.
- **`operands[]` is the VALUE strand**, single-origin by construction (DC2): an operand slot is just the `NodeId` of the defining node.
- **`type_id` points at a `Const`** (types are values): `#i32` is itself a row in the table.

### 7.2 Region & port encoding

Regions do not nest by pointer; a region is a contiguous id range plus a port list:

```
struct RegionRef {
  u32 tag;        // for Cond: the case key (0,1,…, or DEFAULT); for Loop: BODY; Func: BODY; Module: MEMBER
  u32 first_node; // NodeId of first node owned by this region (region bodies are id-contiguous)
  u32 node_count; // number of nodes in the region body
  u32 n_results;  // result-port arity (what yield/continue/break/return must supply)
}
```

- **Ports = block parameters** are encoded as special `Op` rows with opcode `port` inside the region's id range; a `yield`/`continue`/`break`/`return` row carries the result `NodeId`s in its `operands[]`. There is no phi row anywhere (DC5).
- Because region bodies are id-contiguous and the whole table is acyclic (DC7), a region is described by an `(first_node, node_count)` slice — traversal and "is X inside loop L" are range checks, not graph walks (one of RVSDG's wins, carried over).
- Loop-carried values and the loop result are ports of the `Loop` row; recursion groups are `RegionRef`s of the `Module` row tagged `MEMBER`, so a recursive call is an ordinary `call` Op pointing at a `Func` id — never a back-edge.

### 7.3 How hash-consing maps to the table

Structural nodes (`Const`, `Op` with `flags.STRUCTURAL`) are interned via a hash map:

```
key   = hash(form, opcode, type_id, state_in, operands[], const_bytes)   ; the HASHED fields only
value = NodeId
```

The **non-hashed side table** (`span_*`, `print_name`) is deliberately excluded from the key so that two nodes identical up to source position still intern to one row — GVN must not be defeated by debug info. Construction goes through the World factory (Tier-1 eager normalization, [Reduction Engine](14-reduction-engine.md)): the factory first applies oriented `=>` rules to fixpoint, then interns. So:

- **structural equality == pointer (id) equality == automatic GVN/CSE** (DC8);
- the id-equality fast path is valid only for **closed** terms — nodes containing free ports (region bodies mid-construction) are *nominal* (`flags.STRUCTURAL = 0`) and compared by identity, not interned. This is the documented α-equality caveat from MimIR (DC8).

Tier-2 equivalence alternatives ([Reduction Engine](14-reduction-engine.md)) are stored as a separate **append-only union table** keyed by `NodeId`, *not* mixed into the interned node rows — the node table stays acyclic and canonical; the overlay records `a ~ b` pairs for ISel/reassociation only (used sparingly, D7).

### 7.4 Memory layout diagram

```
                interned node table (append-only, ids dense)
   id │ form    opcode  type  state_in  operands         span(side)
  ────┼───────────────────────────────────────────────────────────
   1  │ Const   ty.int   -     -         []   payload=32   #i32
   2  │ Func    -        -     -         []   ports=[%x,%y]  demo:..
   3  │ Op(port) port    #i32  -         []                 %x
   4  │ Op(port) port    #i32  -         []                 %y
   5  │ Op      add      #i32  NIL       [3,4]              %r
   6  │ Op      load     #i32  s=12      [11]               (%v,%s1)   <- effectful: state_in set
  ────┴───────────────────────────────────────────────────────────
       hash-cons map:  hash(Op,add,#i32,NIL,[3,4]) -> 5
       union overlay:  5 ~ 5'   (Tier-2 ISel alt, separate table)
       linearity:      every node used as some row's state_in has exactly ONE such use
```

---

## 8. Round-trip: text → nodes → text

The round-trip is the identity, by construction, because (a) the parser builds the interned graph directly (DC12) and (b) the printer is a deterministic pure function of that graph (§6.3). Concretely:

```
                 parse (DC12, direct-to-graph)
   text  ───────────────────────────────────────►  interned node table
    ▲                                                      │
    │            print (deterministic, §6.3)               │
    └──────────────────────────────────────────────────────┘
                          (byte-identical)
```

Worked example — start from text, intern, re-print:

**Input text**

```
func @add(%x: i32, %y: i32) -> i32 {
  %r = add %x, %y
  return %r
}
```

**After parsing (interned table, ids assigned; spans/names in the side table)**

```
1: Const ty.int 32                      -> #i32
2: Op    port   : #i32                  -> %x   (port of Func 6)
3: Op    port   : #i32                  -> %y   (port of Func 6)
4: Op    add    : #i32  ops=[2,3] state=NIL  -> %r   ; hash-cons key -> id 4
6: Func  @add   ports=[2,3] body=[4,return(4)] result-arity=1
```

**Re-printed (byte-identical to input)**

```
func @add(%x: i32, %y: i32) -> i32 {
  %r = add %x, %y
  return %r
}
```

Two stronger guarantees follow from interning:

- **Idempotent GVN on parse.** If the source contained `add %x, %y` twice with identical operands, both parse to *the same id 4*; re-printing emits the shared `%r` once and reuses it — so `text → nodes → text` may produce *fewer* lines than a naively duplicated input. The round-trip is the identity on *graphs*, and the canonical printer is the identity on already-canonical *text*.
- **Order independence of pure lines.** Re-ordering two independent pure defs in the input produces the same graph (pure Ops float, DC3); the canonical printer re-emits them in the fixed post-order, so the re-printed text is the canonical representative of that equivalence class. (State-threaded lines are pinned by the `%sN` chain and cannot be reordered, so effect order is preserved exactly — failure-mode 2.)

This is the practical payoff of DC16 for testing: snapshot tests assert on canonical text, and any non-identity round-trip is a real bug in the parser or printer, not noise.

---

## See also

- [Core Model](11-core-model.md) — the six node forms, the two strands, the structural invariants this format serializes.
- [Types and Effects](13-types-and-effects.md) — `ty.*` values and fine-grained `#state.<class>` tokens (DC4).
- [Reduction Engine](14-reduction-engine.md) — the World factory, Tier-1 interning/normalization, and the Tier-2 union overlay referenced in §7.3.
- [Frontend](18-frontend.md) — direct-to-graph parsing (DC12) that populates the source map.
- [Codegen](17-codegen.md) — how the same node table is lowered in place to target Ops and emitted to bytes (DC13, no secondary IR).
- [Design Rationale](10-design-rationale.md) and [Risks](22-risks-and-open-problems.md) — DC16 vs. the R5 debuggability liability.
- [Glossary](23-glossary.md) — strand, port, state token, hash-consing, interning, and every other Helix term, alphabetized.
