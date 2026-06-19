# Helix

**A compact, correctness-first compiler IR — and a working compiler built on it.**

Helix is an experiment in how small a *correct* optimizing compiler can be. The core idea:
parse source **directly** into a single acyclic, hash-consed SSA graph, normalize and evaluate
compile-time code *as graph reduction*, schedule with Global Code Motion, and emit real x86-64 —
**no secondary IR at either end**. Every backend is validated against a reference interpreter by
differential fuzzing, so correctness is the headline feature, not an afterthought.

This repository is two things:

| | |
|---|---|
| [**`wiki/`**](wiki/) | The **design**: the IR's rationale, core model, type/effect algebra, reduction engine, codegen, and an honest risk register — grounded in a survey of prior art (sea-of-nodes, RVSDG, Thorin/MimIR, Cranelift, MLIR). Start at [`wiki/00-overview.md`](wiki/00-overview.md). |
| [**`helix/`**](helix/) | The **implementation**: a from-scratch, dependency-free **C++20** compiler for a small imperative+functional integer language. See [`helix/README.md`](helix/README.md). |

**Size.** The compiler itself is **~4,840 LOC** (`src/` + `include/helix/`), or **~4,950** including
the 116-line `helixc` CLI driver. That is the whole pipeline — frontend, IR, verifier, two backends,
GCM scheduler, optimizer, COFF emitter, disassembler. On top of it sit ~1,740 LOC of tests and
~2,450 LOC of differential fuzzers and repro/bench helpers (not counted above).

> ### Honest status
> Helix is a **rigorously-tested research/teaching compiler**, not a production toolchain. The
> *design wiki describes more than is built.* What's real today: a hash-consed SSA core with
> construction-time folding/CSE, a Global Code Motion scheduler, two x86-64 backends (a simple
> oracle-baseline and an optimizing linear-scan one), array reads **and** writes, `i8`–`i64`
> types, a Win64 ABI up to 8 args, function inlining + dead-code elimination behind `-O`, and
> JIT + COFF-object output. **Not built** (designed only): a real type checker, floats, unsigned,
> structs/strings, a multi-state memory model with alias analysis, the Tier-2 equality-saturation
> overlay, and the SMT-verified rewrite-rule DSL. The realistic gap to a production toolchain is
> **~8–15 person-years** — see the code-grounded audit in
> [`wiki/25-path-to-production.md`](wiki/25-path-to-production.md) and the live status in
> [`wiki/24-implementation-status.md`](wiki/24-implementation-status.md).

## Why it's interesting

- **Parser → graph, graph → bytes.** The frontend builds the IR directly (no AST pass); the
  backend emits machine code directly (no LLVM, no secondary backend IR).
- **Optimization is mostly *emergent*.** Hash-consing gives GVN/CSE for free; smart constructors
  fold and strength-reduce at construction; Global Code Motion places each value once in the
  region that dominates its uses.
- **Comptime is the same reduction.** Constant-argument calls fold by running the reference
  interpreter and reifying the result.
- **Correctness is enforced, not hoped for.** A tree-walking interpreter is the oracle; the simple
  and optimizing backends must agree with it (and each other) on the return value *and* final
  memory, over **millions** of randomized programs. A watchdog turns any miscompiled infinite loop
  into a reported bug instead of a hang.

## Quickstart (Windows / MSVC)

```powershell
cd helix
./build.ps1 -Cli          # build + run the test suite (81 tests) and the helixc CLI
./ci.ps1                  # build + test + smoke-fuzz every engine

# interpret / JIT a program (cross-checked against the interpreter), with optimization
./build/helixc.exe examples/demo.hx -O --run fib 30

# emit a real object file and link it into a native .exe
./build/helixc.exe examples/exports.hx --emit-obj build/helix.obj
cl examples/driver.c build/helix.obj /Fe:build/demo.exe   # from an MSVC env
./build/demo.exe
```

Requires VS 2022 Build Tools (`cl`/`link`). **No third-party dependencies** — just the C++
standard library and `kernel32` (`VirtualAlloc`/`VirtualProtect`) for JIT.

## What works (and is tested)

- Functional core (`let`, `loop`/`break`/`next`, recursion, `comptime`) and imperative layer
  (mutable `var`, assignment, `while`, statement-`if`) lowering onto six node forms.
- Array **reads and writes** (`a[i]`, `a[i] = v`) — in-place sorts compile and run correctly.
- `i8`/`i16`/`i32`/`i64` integer widths; Win64 calls with up to 8 arguments (5th+ on the stack).
- `helixc -O`: inlining + dead-function elimination, re-verified after optimization.
- **Validation:** 81 unit tests (~23.5k assertions), the verifier wired into every compile, and
  five differential fuzzers (`fuzz_cf`, `fuzz_imp`, `fuzz_mem`, `fuzz_opt`, `fuzz_parse`) —
  millions of cases, **0 mismatches**, all watchdog-guarded and run by CI.

## Is it "better than sea-of-nodes / RVSDG"?

Honestly: **not yet, and the claim should be read as a design aspiration.** It is genuinely
*shorter*, but partly because the load-bearing pieces those IRs ship (a type checker, alias
analysis, a full optimizer, multi-target codegen, debug info) are *absent*, not more compact. Its
shipped optimizer matches none of them on output quality, and that has never been benchmarked
against LLVM/GCC. What *is* a real, defensible result: **a clean, minimal, rigorously
differential-tested acyclic-SSA core that demonstrates hash-cons-as-GVN and parse-to-graph in
remarkably little code.** See [`wiki/25-path-to-production.md`](wiki/25-path-to-production.md) §5
for the full verdict.

## Layout

```
wiki/    design + research (00 overview, 11 core model, 13 types/effects, 14 reduction engine,
         16 optimizations, 17 codegen, 18 frontend, 22 risks, 24 status, 25 path-to-production)
helix/   the implementation (src/, include/, tests/, tools/, examples/, build.ps1, ci.ps1)
.github/ CI workflow
```

## License

No license file is present yet — until one is added, treat the code as all-rights-reserved.
