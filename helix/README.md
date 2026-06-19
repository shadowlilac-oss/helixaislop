# Helix — a working implementation of the Helix IR

A from-scratch, dependency-free **C++20** implementation of the Helix compiler IR
designed in [`../wiki`](../wiki). It is a real, end-to-end **optimizing** compiler
for a small **imperative** language (mutable variables, `while`, `if`, recursion,
read-only arrays): it **parses source directly into the graph, normalizes and
evaluates compile-time code as graph reduction, optimizes (GVN/fold/CSE +
inlining + DCE), register-allocates, and emits real x86-64** — either JIT-executed
or written to a **COFF object file** that `link.exe` turns into a native `.exe`.
No secondary IR at either end.

> Array **writes** (`a[i] = v`, including in-place sorts) are supported: a linear
> memory-state token is threaded through loads/stores and the Global Code Motion
> scheduler places each store once, in order, never speculated past a guard. Validated
> by ~1M differential cases (interp == simple == ra, return value AND final array).
> The compiler has no known miscompiles.

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

- **81 unit/integration tests, ~23,500 assertions** (`./build.ps1`); the verifier also runs
  in-codegen on every test (debug builds), and `helixc -O` (inlining + dead-function DCE) is
  wired into the driver and re-verifies
- **millions of randomized differential cases, 0 mismatches**, across five generators:
  `tools/fuzz_cf` (control flow / recursion / up-to-8-arg calls), `tools/fuzz_imp` (imperative
  `while`/`if` + array reads **and writes**), `tools/fuzz_mem` (array reads) — each comparing
  **interp == simple == ra** on the return value AND the final array; `tools/fuzz_opt`
  (`interp(unopt) == interp(opt) == jit(opt) == jit(unopt)` — validates the optimizer); and
  `tools/fuzz_parse` (60k malformed/garbage/deeply-nested inputs, **no crash**)
- **CI**: `ci.ps1` + `.github/workflows/ci.yml` build, run the suite, and smoke-fuzz all engines
- every fuzzer is wrapped by a **watchdog** (`tools/fuzz_watchdog.hpp`): the interpreter
  proves each compared case terminates, so a JIT call that loops forever is a real
  miscompile — the watchdog prints the repro and exits fast rather than hanging
- **native link + run** of an emitted `.obj` is part of the suite (end-to-end)
- differential fuzzing is how the array-write, the **scheduling (GCM)**, and a cross-loop
  interpreter-memoization miscompile were all caught — *that* is the bar this project
  holds itself to

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
| **Optimizing backend**: VCode → liveness → **linear-scan register allocation** (callee-saved homes, stack spills) + **immediate operands** (`x+c`, `x<<3`, `x<c` fold the constant into the instruction) → x86-64 | ✅ | `src/backend2.cpp`, `include/helix/vcode.hpp` |
| **Middle-end opt passes**: function inlining, dead-function reachability | ✅ | `src/opt.cpp` |
| **Global Code Motion**: place each value once in the region (LCA of uses) that dominates them; loop bodies split around the exit test. Shared by both backends | ✅ | `src/schedule.cpp` |
| **Imperative frontend**: mutable `var`, assignment, `while`, statement-`if` (on-the-fly SSA) | ✅ | `src/front.cpp` |
| **Multi-result loop regions** + `Proj` (so `while` can output several vars) | ✅ | `src/ir.cpp`, both backends |
| **Array memory**: `a[i]` reads (CSE'd pure loads) and `a[i] = v` **writes** (linear $mem state, GCM-ordered) → native loads/stores; in-place sorts work | ✅ | `src/front.cpp`, both backends |
| **Win64 ABI ≤ 8 args**: 4 in registers, 5th+ on the stack (in and out) | ✅ | both backends, `src/x64.hpp` |
| **Integer widths** i8 / i16 / i32 / i64 (narrow arithmetic wraps; args sign-extend) | ✅ | `src/front.cpp`, both backends |
| **COFF object emission** → link with `link.exe` → native `.exe` | ✅ | `src/coff.cpp`, `src/backend2.cpp` |
| Independent x86-64 **disassembler** (second check on the encoder) | ✅ | `src/disasm.cpp` |

## Build & run (Windows / MSVC)

```powershell
./build.ps1 -Cli            # builds the test runner + helixc, runs the full test suite

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

- **Interpreter recursion depth.** The reference interpreter is recursive; pathologically
  deep recursion/nesting can overflow the C++ stack before the fuel limit trips (a fuzzer
  edge case, not hit by normal programs). The JITs are unaffected.
- The surface language is small (i64-centric with i8/i16/i32; `≤ 8` params); no structs,
  function pointers, strings, or **unsigned** types (division/shift/compare are signed).
- **`idiv` edge cases** (`x/0`, `INT64_MIN/-1`) are defined to match the interpreter
  (`0` / `INT64_MIN`) via guards rather than trapping.
- Single target (**x86-64, Win64 ABI**); the only loads/stores are i64 array cells
  (`a[i]` is `*(i64*)(a + 8*i)`), so i8/i16/i32 apply to scalar arithmetic, not memory.
- The register allocator is **linear-scan without live-range splitting or coalescing**.
  With direct two-address codegen and immediate operands it runs faster than the simple
  backend on register-pressure code and loops, but is modestly *slower* on deep recursion
  (callee-saved save/restore overhead the all-memory backend avoids) — a known tradeoff
  coalescing / shrink-wrapping would close.

## Source layout

```
include/helix/   ir, eval, front, verify, print, x64, vcode, backend, opt, coff, disasm,
                 schedule
src/             ir, eval, front, verify, print, backend (simple), backend2 (optimizing),
                 opt, coff, disasm, schedule (Global Code Motion, shared by both backends)
tests/           test framework + test_{ir,eval,front,backend,backend_ra,verify,opt,
                 memory,imperative,multiresult,components,regress,fuzz,writes,abi,types}.cpp
tools/           helixc.cpp (CLI: --run / --print / --emit-obj / --simple);
                 fuzz_{cf,imp,mem}.cpp differential fuzzers + fuzz_watchdog.hpp; bench.cpp
examples/        demo.hx, exports.hx, driver.c
build.ps1        MSVC build + test runner
```
