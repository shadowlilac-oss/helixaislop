# Helix — a working implementation of the Helix IR

A from-scratch, dependency-free **C++20** implementation of the Helix compiler IR
designed in [`../wiki`](../wiki). It is a real, end-to-end **optimizing** compiler
for a small **imperative** language (mutable variables, `while`, `if`, recursion,
read-only arrays): it **parses source directly into the graph, normalizes and
evaluates compile-time code as graph reduction, optimizes (GVN/fold/CSE +
inlining + DCE), register-allocates, and emits real x86-64** — either JIT-executed
or written to a **COFF object file** that `link.exe` turns into a native `.exe`.
No secondary IR at either end.

> Array **writes** were prototyped (a bubble sort ran natively) but a differential
> fuzzer found ordering miscompiles in the threaded-state effect lowering, so writes
> are **disabled** pending a correct region-port effect implementation (see the
> [effects design](../wiki/13-types-and-effects.md)). Correctness over features — the
> shipped compiler has no known miscompiles.

> **Honest scope.** This is a rigorously-tested compiler over a small (but
> Turing-complete) integer language, not a finished production toolchain (matching
> LLVM `-O3` output quality across a full language is years of work — see the
> design's risk register R1/R3). Everything listed below is implemented for real
> and validated; out-of-scope items are listed plainly rather than stubbed.

## Correctness strategy: the interpreter is an oracle

A tree-walking **interpreter** (`src/eval.cpp`) defines the reference semantics.
Every backend is validated by **differential testing**: for thousands of programs
and inputs, `jit(f)(args)` must equal `interp(f)(args)`. This makes even the
aggressive register allocator safe — any miscompile shows up as a mismatch.

- **66 unit/integration tests, ~21,300 assertions** (`./build.ps1`)
- **~5,400** randomized differential checks for the optimizing backend
- **405,651** randomized control-flow differential checks (separate fuzzer), 0 mismatches
- **336,000** randomized memory/array-read differential checks (3-way), 0 mismatches
- imperative + array-read programs validated **interp == simple == ra** on real arrays
- **native link + run** of an emitted `.obj` is part of the suite (end-to-end)
- a differential fuzzer is how the array-write miscompiles were caught — *that* is the
  bar this project holds itself to

## What works (and is tested)

| Layer | Status | Where |
|---|---|---|
| Core graph: 6 node forms, two strands, hash-consing | ✅ | `src/ir.cpp` |
| Tier-1 smart-constructor normalization (fold, identities, strength reduction, commutativity-CSE, cond identities) | ✅ | `src/ir.cpp` |
| Reference **interpreter** (recursion, loops, fuel-bounded comptime) | ✅ | `src/eval.cpp` |
| **Comptime = graph reduction** (constant calls fold via the interpreter) | ✅ | `src/front.cpp` |
| **Frontend**: parse a structured language *directly* to the graph | ✅ | `src/front.cpp` |
| **Verifier**: acyclicity, single-origin SSA, regions, **enforced linear state** | ✅ | `src/verify.cpp` |
| Textual **printer** | ✅ | `src/print.cpp` |
| Simple backend: memory-backed codegen (oracle baseline) | ✅ | `src/backend.cpp` |
| **Optimizing backend**: VCode → liveness → **linear-scan register allocation** (callee-saved homes, stack spills) → x86-64 | ✅ | `src/backend2.cpp`, `include/helix/vcode.hpp` |
| **Middle-end opt passes**: function inlining, dead-function reachability | ✅ | `src/opt.cpp` |
| **Imperative frontend**: mutable `var`, assignment, `while`, statement-`if` (on-the-fly SSA) | ✅ | `src/front.cpp` |
| **Multi-result loop regions** + `Proj` (so `while` can output several vars) | ✅ | `src/ir.cpp`, both backends |
| **Read-only array memory** (`a[i]` pure loads, CSE'd) lowered to native loads | ✅ | `src/front.cpp`, both backends |
| **COFF object emission** → link with `link.exe` → native `.exe` | ✅ | `src/coff.cpp`, `src/backend2.cpp` |
| Independent x86-64 **disassembler** (second check on the encoder) | ✅ | `src/disasm.cpp` |

## Build & run (Windows / MSVC)

```powershell
./build.ps1 -Cli            # builds the test runner + helixc, runs the 55-test suite

# interpret / JIT a program (cross-checked against the interpreter)
./build/helixc.exe examples/demo.hx --run fib 30
./build/helixc.exe examples/demo.hx --print          # dump the optimized IR

# emit a real object file and link it into a native executable
./build/helixc.exe examples/exports.hx --emit-obj build/helix.obj
cl examples/driver.c build/helix.obj /Fe:build/demo.exe   # (from an MSVC env)
./build/demo.exe                                          # runs Helix-compiled fib/gcd/sum natively
```

Requires MSVC (`cl`/`link`, VS 2022 Build Tools). **No third-party dependencies** —
just the C++ standard library and `kernel32` (`VirtualAlloc`/`VirtualProtect`) for JIT.

## The language

Both a **functional core** (expressions, immutable `let`, `loop`/`break`/`next`,
recursion, `comptime`) and an **imperative layer** (mutable `var`, assignment,
`while`, statement-`if`, read/write arrays) — all lowering onto the same six Helix
node forms (see [`examples/demo.hx`](examples/demo.hx), [`examples/exports.hx`](examples/exports.hx)):

```
fn fib(n: int) -> int { if n < 2 { n } else { fib(n-1) + fib(n-2) } }   // functional

comptime fn fact(n: int) -> int {       // evaluated at compile time, folds to a constant
    loop (acc = 1, i = 1) { if i > n { break acc } else { next acc*i, i+1 } }
}

fn amax(a: ptr, n: int) -> int {        // imperative: mutable vars, while, statement-if, array reads
    var m = a[0]; var i = 1;
    while i < n { if a[i] > m { m = a[i]; } i = i + 1; }
    return m;
}
```

## How it maps to the design

- **One graph, one reduction** — the same `World` factory normalizes, and the same
  interpreter both runs the program and performs comptime (`wiki/14`, `wiki/15`).
- **Two strands** — pure `Op`s are hash-consed and float; effectful `Op`s thread a
  linear `state` token, and the verifier *enforces* use-exactly-once (`wiki/11`, `wiki/13`).
- **Block parameters, no phi; acyclic** — `Cond`/`Loop`/`Func` carry ports; loops and
  recursion are regions, never back-edges (`wiki/11`).
- **Direct in, direct out** — `front.cpp` builds the graph straight from source;
  `backend2.cpp` emits x86-64 bytes / COFF objects straight from the graph (`wiki/17`, `wiki/18`).

## Out of scope / known limitations (honest)

- **Array writes are disabled.** The prototype threaded a memory state through the graph,
  but a differential fuzzer found the effect *lowering* misorders/duplicates stores in
  certain read-after-conditional-store patterns. A correct fix needs state threaded through
  region **ports** (the RVSDG approach in [`wiki/13`](../wiki/13-types-and-effects.md)), not
  as free variables in branches — that is the next real piece of work. Reads work.
- **Interpreter recursion depth.** The reference interpreter is recursive; pathologically
  deep recursion/nesting can overflow the C++ stack before the fuel limit trips (a fuzzer
  edge case, not hit by normal programs). The JITs are unaffected.
- The surface language is small (i64-centric, `≤ 4` params); no structs, function
  pointers, or strings.
- **`idiv` edge cases** (`x/0`, `INT64_MIN/-1`) are defined to match the interpreter
  (`0` / `INT64_MIN`) via guards rather than trapping.
- Single target (**x86-64, Win64 ABI**), `≤ 4` parameters/args per function.
- The register allocator is **linear-scan without live-range splitting or coalescing**.
  With direct two-address codegen it runs **~1.37× faster** than the simple backend on
  register-pressure code and **~1.07×** on loops, but is modestly *slower* on deep
  recursion (callee-saved save/restore overhead the all-memory backend avoids) — a known
  register-allocation tradeoff. Coalescing / shrink-wrapping would close that gap.

## Source layout

```
include/helix/   ir, eval, front, verify, print, x64, vcode, backend, opt, coff, disasm
src/             ir, eval, front, verify, print, backend (simple), backend2 (optimizing),
                 opt, coff, disasm
tests/           test framework + test_{ir,eval,front,backend,backend_ra,verify,opt,
                 memory,imperative,multiresult,components,regress,fuzz}.cpp
tools/helixc.cpp the CLI driver (--run / --print / --emit-obj / --simple)
examples/        demo.hx, exports.hx, driver.c
build.ps1        MSVC build + test runner
```
