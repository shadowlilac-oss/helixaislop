# 25 â€” Path to Production: From Working Prototype to Real Compiler

> **Status of this document.** This is a synthesis of six independent, code-grounded gap
> audits (frontend, type system, IR/memory model, optimizer, backend/regalloc,
> targets/ABI/objects, correctness/testing, and engineering/DX). Every load-bearing claim
> below was re-verified against the actual source in `helix/`. Where the design wiki and the
> implementation disagree, **the code wins** and the disagreement is flagged.

> **UPDATE: Tier 0 is now complete.** Every Tier-0 must-have below has been implemented and
> validated -- the optimizer and verifier are wired into the real pipeline, the optimizer has a
> dedicated differential fuzzer, all recursive walks are guarded, the parser is fuzzed against
> malformed input, the disassembler decodes every emitted form, diagnostics go to stderr with
> stable exit codes, division semantics are pinned, and CI exists. See the "Tier-0 -- DONE"
> section of [24-implementation-status](24-implementation-status.md) for specifics. Tiers 1-3
> (type checker, floats, multi-state memory model, multi-target, etc.) remain as described.

---

## 0. Executive summary

Helix is an impressive, genuinely **correct** teaching/research compiler: a ~4,640-LOC
dependency-free C++20 pipeline that parses a tiny i64-centric language **directly** into an
acyclic, hash-consed SSA graph, normalizes it with construction-time peephole folds + CSE,
schedules with a real Global Code Motion pass, and emits working x86-64 (JIT + a single-section
COFF object) validated by an interpreter oracle and three differential fuzzers. The correctness
discipline is its strongest asset and is largely as advertised.

The gap to a production compiler is nonetheless enormous, and the design wiki materially
over-claims what exists. The four largest, mutually-reinforcing chasms are:

1. **No type system or semantics** â€” zero type checking, signed-i64-only, no floats/unsigned/
   structs/strings, no real pointers, a load-bearing "result type follows the left operand" quirk.
2. **A single global memory state** â€” exactly ONE `$mem` token; the headline
   "multiple independent states / fork-join / alias analysis" differentiator is **0% built**.
3. **A minimal backend** â€” no floats, no aggregates, no addressing-mode tiling, 7-register
   linear scan with no coalescing, one hardcoded target (x86-64/Win64), no debug info/unwind.
4. **No externally-linkable, separately-compiled modules** â€” single-file, single-string
   frontend; no extern/libc calls (hence no I/O); one `.text` section; no IR reader.

Two structural credibility problems compound these: **the shipping driver never runs the
optimizer** (inlining/DCE are test-only dead code) and **the verifier is never invoked on the
code path the fuzzers compile**. The Tier-2 equality-saturation overlay, the SMT-verified
rewrite-rule DSL, and NbE-based comptime are specified in detail but entirely absent.

**Bottom line:** a high-quality ~5k-LOC mid-end prototype that proves a clean core idea,
sitting roughly **8â€“15 person-years** from a production toolchain.

---

## 1. Verified state of the code (ground truth)

These were confirmed by reading the source, not taken on faith:

- **Driver does not optimize.** `tools/helixc.cpp` does parse â†’ verify â†’ (print) â†’ JIT/obj. It
  never calls `inline_into`, `inline_call`, or `reachable_functions`; those live in `src/opt.cpp`
  and are referenced only from `tests/test_opt.cpp`.
- **Verifier is off the codegen path.** `verify_module` is called only in `helixc.cpp`; never in
  `jit_compile`, `jit_compile_ra`, or `compile_module_obj`. The fuzzers never verify.
- **One global memory token.** `front.cpp` threads a single `env["$mem"]` (lines ~257/320/387/467).
  No `fork`/`join`/`borrow` opcode exists anywhere in `src/`.
- **Comptime is a tree-walker.** `front.cpp:565-576` folds a comptime call by running `eval_func`
  (the recursive interpreter in `eval.cpp`) and reifying a single `Const`. No NbE, no neutral
  terms, no residualization.
- **No floats, no unsigned, no SSE** in `src/`/`include/` (only incidental `unsigned char` casts
  and the disassembler).
- **COFF: single `.text` section** (`NumberOfSections=1`), and **relocations are forced to
  `type=1` ADDR64** (`backend2.cpp:769`) even though `coff.hpp:24` documents the default as
  `type=4` REL32. REL32 is documented but never emitted.
- **Recursive pipeline walks**: `verify.cpp` `dfs`, `print.cpp` `ensure`, `opt.cpp` `clone`,
  `eval.cpp` `eval`/`run` all recurse on depth; only `schedule.cpp` was made iterative.
- **No CI**: no `.github/` anywhere; `build.ps1` is MSVC/Windows-only.
- The status wiki (`24-implementation-status.md`) is **honest** about unsigned/Tier-2/SMT gaps;
  the other wiki chapters read as if those pieces are built.

---

## 2. Gap analysis by dimension

### 2.1 Frontend & language surface
A ~700-LOC single-pass recursive-descent parser that builds the SSA graph directly and works for
its tiny language. Missing nearly everything a real language needs:
- **No source spans** (the `Node` struct has no span field; tokens carry only a line number).
  Blocks caret diagnostics, debug line tables, fold provenance, IDE features.
- **First-error-fatal diagnostics**, no columns, no recovery, no "expected X found Y", no codes.
- **No type checker / inference / coercion** (integer literals are *always* i64; the
  `cur_result_ty.is_int() ? ty_i64() : ty_i64()` ternary is dead).
- **Flat per-function name map**, scopes simulated by save/restore; no namespaces, no labels.
- **No modules / imports / separate compilation**; `parse_module` takes one string.
- **Missing surface features**: structs/tuples/enums, strings/chars, floats, unsigned, casts
  (`as`), `for`, mid-block `return`, `continue`, compound assignment, `?:`, function pointers,
  generics, pattern matching, globals/`const`.
- **Lexer gaps**: no block comments, no string/char literals, decimal-only integers (no hex/bin/
  underscores/suffixes), no float literals, overflow-unsafe `stoll`.
- **Robustness**: unbounded recursion in `parse_expr` (stack overflow), OOB-prone forward scans.
- **No-AST technical debt**: while bodies are parsed twice; loop-carried vars detected by textual
  `IDENT =` scanning. `&&`/`||` lower to non-short-circuit bitwise ops (latent once effects exist).

### 2.2 Type system & semantics
There is no type system in the compiler sense â€” only a 3-byte width/kind tag.
- **No type checker** of any kind; `parse_call` checks arg *count*, never types; the verifier
  never inspects `Type`.
- **Result-type-follows-left-operand** (`ir.cpp:167`): mixed-width arithmetic is silently
  mistyped, and commutative canonicalization can swap operands *before* the type is taken,
  making result width depend on hash-cons id ordering â€” a latent miscompile masked only because
  the corpus is uniformly i64.
- **No unsigned** (signed-only SDiv/SRem/AShr and signed compares); **no floats**; **no
  aggregates**; **degenerate untyped pointer** (`ty_ptr` = {Ptr,64}, ignored by every access:
  base treated as i64, scale fixed at 8, element fixed at i64).
- **No casts/conversions** (sext/zext/trunc/bitcast do not exist as ops).
- **div0 / INT_MINÃ·-1 divergence**: the interpreter defines `x/0 = 0`, the folder refuses to fold
  it, and the backend emits a guarded idiv â€” three engines, two stories; a hole in the
  "interp == simple == ra" guarantee.
- **No written semantics**; behavior lives implicitly in `eval.cpp`.

### 2.3 IR completeness & memory model
A clean, minimal acyclic-SSA graph (6 node forms, two strands) that faithfully models i8â€“i64 +
bool + i64-array memory through **one** global linear `$mem` token. Missing:
- **Floats, unsigned, aggregates/GEP, real pointers/allocations.**
- **Multiple state tokens / alias classes** â€” the design's flagship "many independent states from
  day one" is day zero: one token totally orders every memory op against every other.
- **Alias analysis, fork/join/borrow, the state-strand rewrite rules** â€” all specified, none built.
- **Effectful/indirect/external calls** â€” `call()` has a state param but the frontend always
  builds pure calls (`st=NONE`); Call produces no state result; the target is an integer region
  id, so no call-through-a-value and no extern symbols. **A silent correctness hazard, not just a
  missing feature.**
- **Verifier coverage holes**: no operand-type checks, Call invisible to linearity, can't detect
  a zero-use (leaked) state token through ports, recursive DFS overflows on deep graphs.
- **Exceptions/unwinding, atomics/volatile/ordering, varargs, debug metadata, checked/saturating
  arithmetic** â€” all absent.
- **`pure_load` soundness rests on a frontend syntactic heuristic** (`scan_has_stores`): it will
  miscompile the moment effectful/external calls or aliasing writes exist.

### 2.4 Optimizer & the "optimizes better" claim
What actually runs on a real compile: **construction-time peephole folds** (constant folding,
~15 algebraic identities, **exactly one** strength-reduction rule `x*2^k â†’ shl`, commutative
canonicalization) plus **hash-cons CSE** (local, not cross-control-flow GVN) and an **LCA
scheduler**. Everything else is unwired, latent, or unimplemented:
- Inlining/DCE exist only as test-only functions; **no pass manager**, no fixpoint.
- **No real DCE** of dead functions; **no dead-store elimination** (single token pins every store).
- **No SCCP/range/known-bits**, so only literal-constant facts fold.
- **No LICM of loads** (memory pinned), **no unroll/peel/rotation/TRE** (recursion blows the
  native stack), **no cost-driven ISel / LEA tiling / reassociation**.
- **No SMT-verified rule DSL** â€” every rule is a hand-written, unverified C++ branch (exactly what
  the design claims to have eliminated).
- **No alias analysis / fine-grained state** â€” so store-to-load forwarding, dead-store elimination,
  redundant-load elimination, and effect reordering are *not even representable*.
- **Comptime â‰  optimizer engine**: it's a separate tree-walking interpreter handling all-constant
  scalar args â†’ single Const; no partial evaluation/specialization.

### 2.5 Backend codegen & register allocation
Correct but deliberately minimal.
- **No floats/SSE/XMM.**
- **64-bit-only at the machine level**; i8/i16/i32 emulated by computing in 64 bits + sign-extend
  (narrow memory is impossible).
- **No unsigned codegen** (no udiv/urem, no CC_A/CC_B, sar-only shifts).
- **One-node-at-a-time ISel**: no tiling, **no LEA at all**, no `[base+idx*scale+disp]` addressing,
  no fused compare-and-branch (every conditional materializes a 0/1 byte then test+jcc).
- **Linear scan over only 7 callee-saved registers**; no coalescing (every port/loop/cond Mov is a
  real copy), no live-range splitting/holes, naive furthest-endpoint spilling, no rematerialization.
  Caller-saved RAX/RCX/RDX/R8â€“R11 are permanent scratch â€” root cause of register pressure *and* the
  documented deep-recursion slowdown.
- **No peephole, no block layout/fall-through, no branch relaxation/short forms.**
- **No shrink-wrapping / leaf frame omission**; every function builds a full rbp frame.
- **Latent landmine**: `mov_from/to_mem` "must not" use rbp/rsp/r12/r13 as base, but the allocator
  *does* allocate r12/r13 â€” safe today only because addresses are funneled through RAX scratch.
- **Disassembler** (the "second check on the encoder") does not decode several forms `backend2`
  actually emits, so the validation claim is weaker than stated.

### 2.6 Targets, ABI, objects, linking & debug info
One hardcoded target (x86-64/Win64), a proof-of-concept COFF writer. Missing:
- **SysV ABI / ELF / Mach-O**; **second ISA** (AArch64/RISC-V) â€” no target abstraction exists.
- **Data/rodata/bss sections + globals**; **RIP-relative (REL32) relocations + PIC** (documented,
  never emitted â€” every call is a 10-byte `movabs;call rax` + ADDR64).
- **External symbols / dynamic linking / libc imports** â€” cannot call printf/malloc/OS APIs at all.
- **Unwind tables** (.pdata/.xdata on Win64 â€” legally required for non-leaf functions; .eh_frame on
  SysV). Today "works" only because nothing unwinds through a Helix frame.
- **Debug info** (DWARF/CodeView/PDB) â€” none; no source-location propagation to bytes.
- **FP ABI, struct/aggregate passing (sret, multi-reg returns), varargs, >8 args, TLS.**
- **Linker driver** â€” `helixc` only writes the `.obj` and prints "Link with: â€¦".
- **Symbol attributes** â€” every symbol is EXTERNAL, no aux/section symbols, no static/weak/COMDAT.

### 2.7 Correctness, verification & test infrastructure
A genuinely strong differential harness â€” and a clear-eyed view of its limits:
- **Strong**: fuel-bounded interpreter oracle; three differential fuzzers (interp == simple == ra)
  over millions of cases; a watchdog turning native hangs into reportable bugs; smallest-repro
  reduction; ~80 unit tests / ~23k assertions; a regression corpus; an independent reference in
  `test_fuzz.cpp`; honest fuzzer headers.
- **Gaps**: no **formal semantics** (differential agreement can't catch a *shared* misconception);
  the **oracle is unverified recursive C++** that stack-overflows before fuel trips (capping the
  hardest inputs exactly); the **verifier is off the codegen path**; **no SMT-verified rules**;
  fuzzers test only a **UB-free, i64-only, well-formed slice** (narrow ints are unit-tested but
  **never fuzzed**); **no optimized-vs-unoptimized differential** (all engines share the reduced
  graph, so fold/inline/comptime bugs are invisible); **no translation validation / regalloc
  checker**; **no CI**, **no sanitizers**, **no coverage**, **no parser/error-path fuzzing**;
  **comptime is checked only against itself** (same engine both sides).

### 2.8 Engineering: scale, compile-time perf, tooling & DX
A ~4.6k-LOC research prototype with one demo CLI.
- **No incremental/separate compilation** (whole module re-lowered/-allocated/-encoded every call).
- **No IR reader / binary serialization** (printer is one-way; the wiki's "round-trip identity" is
  unimplemented).
- **Stack-overflow-prone recursive walks** reachable from the normal path.
- **Line-only, single-error diagnostics**; verifier errors print internal node numbers.
- **No type-checker diagnostics.**
- **Monotonic arena growth** (no eviction/GC; inlining multiplies nodes into the same arena).
- **No pass manager / analysis caching**; repeated full DFS sweeps; NodeId-keyed hash maps where
  dense vectors belong.
- **No debugger/debug-info, no profiler, no -O levels, no LSP, no formatter, no package manager,
  no cross-platform build** (MSVC/Windows-hardwired), **no CI**, **no spec/versioning policy**,
  **no machine-readable diagnostics/exit-code contract**.
- **Comptime fuel** is a global hardcoded 50M with an unlocatable error â€” contradicting the design's
  "localizable budget reported at the offending site."

---

## 3. Maturity tiers (the roadmap)

### Tier 0 â€” Make the prototype honest and load-bearing  *(~1â€“2 person-months)*
No new language features; make the existing pipeline trustworthy and self-consistent.
- Wire the **verifier into** `jit_compile`/`jit_compile_ra`/`compile_module_obj` (debug/CI builds).
- Wire the **optimizer into the driver** behind `-O`; add a minimal **pass-manager/fixpoint** shim
  and a real DCE that prunes dead functions.
- Add an **optimized-vs-unoptimized differential fuzzer** (catches fold/inline/comptime bugs).
- **Harden recursive walks** (verify/print/clone/eval) against stack overflow.
- **Pin div0 / INT_MINÃ·-1 semantics** consistently across all three engines and document it.
- Stand up **CI** (build + tests + N fuzz seeds + ASan/UBSan), ideally a non-MSVC compiler too.
- Minimal **diagnostic/IO/exit-code** hygiene (errors â†’ stderr; stable codes).
- **Reconcile the wiki with the code** (mark multi-state, SMT DSL, Tier-2 overlay, NbE comptime,
  source maps, IR round-trip as DESIGN-NOT-BUILT).
- **Fuzz the parser** with malformed input; extend the **disassembler** to cover what `backend2`
  emits.

### Tier 1 â€” A real (if small) language  *(~12â€“18 person-months)*
A typed language you could write non-trivial single-binary programs in.
- **Type checker + written semantics**; kill result-follows-left and dead literal typing; add
  explicit conversion nodes; make the verifier check operand-type well-formedness.
- **Source spans + recoverable caret diagnostics.**
- **Unsigned** types end to end; **floats** end to end (types, ops, conversions, XMM, SSE, FP ABI).
- **Aggregates** (structs/tuples/arrays) + native 8/16/32-bit loads/stores; **strings** as i8 arrays.
- **Real pointer-to-T**; **data/rodata/bss + REL32 relocations + globals/string literals.**
- **extern declarations + import relocations + effectful (state-threading) calls + varargs** â†’
  libc/I/O becomes possible.
- **Lexical scopes/namespaces**; richer lexer (hex/bin/float/suffix literals, strings, block
  comments).
- **Debug-info skeleton** (line tables) + **Win64 .pdata/.xdata** for every non-leaf function.
- **Linker-driving driver** (one-command sourceâ†’exe) with `-o`, `-O`, `--version`, help.
- **Re-establish `pure_load` soundness** once effects exist.

### Tier 2 â€” A serious, self-hosting-capable compiler  *(~3â€“5 person-years)*
The design's actual differentiators + the quality that makes "optimizes well" testable.
- **Multi-state memory model**: fork/join/borrow + multiple tokens + **alias analysis** that splits
  `$mem`. (Prerequisite for all serious memory optimization and the central design claim.)
- **Memory opts**: store-to-load forwarding, dead-store elimination, redundant-load elimination,
  LICM of loads, disjoint-effect scheduling freedom.
- **Real optimizer**: pass manager + analyses (**SCCP/range/known-bits**), cross-control-flow
  **GVN/PRE**, **unroll/peel/rotation/TRE**, **inliner with a cost model**.
- **Cost-driven ISel**: addressing-mode/LEA tiling, fused compare-and-branch, peephole + block
  layout.
- **Production register allocator**: all 16 GPRs, coalescing, live-range splitting, spill weights,
  rematerialization.
- **Modules / imports / separate compilation / incremental + function/IR cache**; proper symbol
  attributes (internal/static/weak/COMDAT).
- **Binary + textual IR serialization with a real reader** (round-trip becomes testable).
- **Second target/ABI**: at least x86-64 SysV + ELF; ideally AArch64 (forces the target
  abstraction + branch relaxation/veneers).
- **Aggregate/struct ABI passing + >8 args.**
- **The SMT-verified rule DSL + Tier-2 overlay** *if pursued* (else explicitly retire those claims);
  either way verify the existing folds (Alive2-style) and add **translation validation / a regalloc
  checker**; **benchmark vs LLVM/GCC** for the first time.

### Tier 3 â€” Production toolchain  *(~+4â€“8 person-years; a small team for years)*
Everything a team and ecosystem need beyond a good compiler binary.
- **Debugger-grade DWARF/CodeView** (variable locations, inlined frames) + correct unwind/CFI on
  all targets.
- **LSP** (incremental parse, go-to-def/hover/completion/live diagnostics).
- **PGO, branch-probability layout, hot/cold splitting, devirtualization**; per-pass timing.
- **Exceptions/unwinding, atomics/volatile/ordering + concurrency memory model, TLS.**
- **Mature multi-target** (x86-64 + AArch64 + ideally RISC-V), full ABIs, cross-compilation,
  ELF/Mach-O/COFF, dynamic linking/PLT/GOT, PIC/PIE.
- **Ecosystem & stability**: package manager / build manifest, formatter, versioned spec +
  semver/compat policy, changelog, **spec-driven conformance suite**.
- **Scale engineering**: parallel + incremental codegen, bounded/streaming memory, coverage-guided
  fuzzing, continuous benchmarking.

---

## 4. Top 10 priorities (highest leverage first)

| # | Item | Effort | Why |
|---|------|--------|-----|
| 1 | Wire the **verifier into codegen** + the **optimizer into the driver** (+ fixpoint + opt-vs-unopt fuzzer) | M | The two worst credibility gaps: the verifier never runs on what's compiled, and the optimizer is dead code. Cheap; unblocks trust in everything. |
| 2 | **Type checker + written semantics** (kill result-follows-left & dead literal typing) | XL | Zero type checking today; ill-typed programs silently miscompile. Foundation for unsigned/float/struct/pointer. |
| 3 | **Source spans + recoverable caret diagnostics** | L | Line-only/first-error-fatal is teaching-toy level; prerequisite for debug info, IDE, fold provenance. |
| 4 | **Multi-state memory model** (fork/join + tokens + alias analysis) | XL | THE central differentiator, 0% built; without it no memory optimization is even representable. |
| 5 | **extern + import relocs + effectful calls** (libc/I/O) | L | No I/O / real programs possible today; also fixes a latent pure-call miscompile. |
| 6 | **Data/rodata/bss + REL32 relocations + globals/strings** | M | One `.text` section, ADDR64-only; blocks strings, constants, switch tables, PIC. |
| 7 | **Floats end to end** (types/ops/XMM/SSE/FP ABI) | XL | No floats â‡’ not general-purpose; large orthogonal domain many features assume. |
| 8 | **Register allocator upgrade** (16 GPRs, coalescing, splitting, spill weights) | L | Dominant code-quality gap and the documented recursion regression. |
| 9 | **Harden recursive walks + CI + sanitizers + many fuzz seeds** | M | Pipeline-reachable stack overflows; headline test numbers are unreproducible without CI. |
| 10 | **Structs/aggregates + native narrow loads/stores + pointer-to-T** | L | Memory is i64-only; unblocks strings, data structures, struct ABI, SROA later. |

---

## 5. Verdict on the "shorter AND optimizes better" claim

**Shorter:** true but misleading. ~4,640 LOC is small largely because the load-bearing parts the
named IRs ship â€” a type checker, alias analysis, multiple memory states, a wired optimizer with a
pass manager, ISel tiling, a rule DSL, serialization, multi-target codegen, debug info â€” are
**absent**, not implemented more compactly. The wiki's own budget (~6.6kâ€“10.7k LOC for a *first
mature* single-ISA implementation) implicitly concedes this.

**Optimizes better:** false as stated. The shipping optimizer is construction-time peephole folding
(~15 identities, **one** strength-reduction rule), hash-cons CSE (local, not cross-control-flow
GVN), and an LCA scheduler â€” and the driver doesn't even invoke inlining/DCE. There is no
SCCP/range analysis, no LICM of loads, no unroll/peel/TRE, no cost-driven ISel, and â€” decisively â€”
**no alias analysis or fine-grained state**, so no memory reordering / store-to-load forwarding /
dead-store elimination is representable. The most-repeated differentiator ("multiple independent
state tokens from day one") is contradicted by the single `$mem` token. The verification thesis
("SMT-verified rules â‡’ fewer Alive-class bugs") is unbuilt; every rule is a hand-written,
unverified C++ branch.

To the project's credit, the README and status wiki (24) are notably honest, and `wiki/16 Â§10`
already walks the claim back to "match, not beat, -O3; the win is code size and unification." But
even that retreat over-claims: the **unification is not realized** â€” folding is C++ peephole, ISel
is a separate C++ switch, comptime is a separate tree-walking interpreter.

**Defensible claim today:** *"A clean, minimal, rigorously differential-tested acyclic-SSA core that
demonstrates hash-cons-as-GVN and parser-as-IR-builder in remarkably little code."* That is a real,
interesting result. The competitive-optimization and verified-rules claims are aspirational design,
not implemented fact, and should be labeled as such until alias analysis + multi-state + a wired
optimizer + the rule DSL exist **and have been benchmarked against LLVM/GCC** (which has never been
measured).

---

## 6. Effort summary

| Tier | Goal | Rough person-time |
|------|------|-------------------|
| 0 | Make the prototype honest & load-bearing | 1â€“2 person-months |
| 1 | A real (if small) language | 12â€“18 person-months |
| 2 | A serious, self-hosting-capable compiler | 3â€“5 person-years |
| 3 | Production toolchain | +4â€“8 person-years (small team) |

**Total from here to a credible production toolchain: ~8â€“15 person-years**, dominated by the type
system, the alias/memory model, optimizer quality, and the multi-target/ABI/debug-info work the
wiki currently only describes. These estimates assume the existing differential-testing discipline
is maintained throughout â€” which roughly doubles raw coding time but is non-negotiable for a
compiler.
