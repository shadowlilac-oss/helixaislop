# Helix — a working implementation of the Helix IR

A from-scratch, dependency-free **C++20** implementation of the Helix compiler IR
designed in [`../wiki`](../wiki). It is a real, end-to-end **optimizing** compiler
for a small **imperative** language (mutable variables, `while`, `if`, recursion,
read/write arrays): it **parses source directly into the graph, normalizes and
evaluates compile-time code as graph reduction, optimizes (GVN/fold/CSE +
inlining + DCE), register-allocates, and emits real x86-64** — either JIT-executed
or written to a **COFF object file** that `link.exe` turns into a native `.exe`.
No secondary IR at either end. It compiles an imperative **bubble sort** to a native
object that sorts a real C array in place.

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

- **68 unit/integration tests, ~21,200 assertions** (`./build.ps1`)
- **~5,400** randomized differential checks for the optimizing backend
- **405,651** randomized control-flow differential checks (separate fuzzer), 0 mismatches
- **336,000** randomized memory/array differential checks (3-way), 0 mismatches
- in-place algorithms (bubble sort, reverse, prefix-sums) validated **interp == simple == ra** on real arrays
- **native link + run** of an emitted `.obj` is part of the suite (incl. native bubble sort)

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
| **Array memory**: read `a[i]` (pure, CSE'd) and write `a[i]=v` (threaded state) | ✅ | `src/front.cpp`, both backends |
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

fn bubble(a: ptr, n: int) -> int {      // imperative: mutable vars, while, array writes
    var i = 0;
    while i < n {
        var j = 0;
        while j < n - 1 {
            if a[j] > a[j + 1] { var t = a[j]; a[j] = a[j + 1]; a[j + 1] = t; }
            j = j + 1;
        }
        i = i + 1;
    }
    return 0;
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

- **Effect granularity.** Memory uses a single threaded state token (a total order of
  writes — correct but conservative); no fine-grained alias-class states or
  store-to-load forwarding yet. Read-only loads are CSE'd; effectful loads (inside
  write-containing functions) are not.
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
                 memory,imperative,writes,multiresult,components,regress,fuzz}.cpp
tools/helixc.cpp the CLI driver (--run / --print / --emit-obj / --simple)
examples/        demo.hx, exports.hx, driver.c
build.ps1        MSVC build + test runner
```
