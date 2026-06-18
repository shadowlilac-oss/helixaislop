# Helix — a working implementation of the Helix IR

A from-scratch, dependency-free **C++20** implementation of the Helix compiler IR
designed in [`../wiki`](../wiki). It is a real, end-to-end optimizing compiler for
a small language: it **parses source directly into the graph, normalizes and
evaluates compile-time code as graph reduction, and JIT-compiles functions to
real x86-64 machine code that it then executes** — with no secondary IR at either
end, exactly as the design specifies.

> **Honest scope.** This is a rigorously-tested *vertical slice*, not a finished
> production compiler (that is years of work — see the design's risk register
> R1/R3). What is implemented is implemented for real and tested hard; what is
> out of scope is listed below rather than stubbed and oversold.

## What works (and is tested)

| Layer | Status | Where |
|---|---|---|
| Core graph: 6 node forms, two strands, hash-consing | ✅ | `src/ir.cpp` |
| Tier-1 smart-constructor normalization (fold, identities, strength reduction, commutativity-CSE, cond identities) | ✅ | `src/ir.cpp` |
| Reference **interpreter** (recursion, loops, fuel-bounded) | ✅ | `src/eval.cpp` |
| **Comptime = graph reduction** (constant calls fold via the interpreter) | ✅ | `src/front.cpp` |
| **Frontend**: parse a structured language *directly* to the graph (on-the-fly SSA) | ✅ | `src/front.cpp` |
| **Verifier**: acyclicity, single-origin SSA, well-formed regions, **enforced linear state** | ✅ | `src/verify.cpp` |
| Textual **printer** (diffable IR dump) | ✅ | `src/print.cpp` |
| **x86-64 JIT backend**: lower → encode → execute, no secondary IR | ✅ | `src/backend.cpp`, `include/helix/x64.hpp` |
| Differential test: **JIT == interpreter == independent reference** | ✅ | `tests/test_fuzz.cpp` |

**38 tests, ~12.8k assertions, all passing**, including 9.6k randomized
differential checks (random expression graphs compiled to x64 and cross-checked
against the interpreter and a C++ reference).

## Build & run (Windows / MSVC)

```powershell
# builds the test runner and the helixc CLI, runs the suite
./build.ps1 -Cli

# compile + run a program (cross-checked against the interpreter)
./build/helixc.exe examples/demo.hx --print
./build/helixc.exe examples/demo.hx --run fib 30
./build/helixc.exe examples/demo.hx --run main
```

Requires MSVC (`cl`, VS 2022 Build Tools). No third-party dependencies — just the
C++ standard library and `kernel32` (`VirtualAlloc`/`VirtualProtect`) for the JIT.

## The language

Expression-oriented, immutable bindings, structured control. Maps 1:1 onto the
six Helix node forms (see [`examples/demo.hx`](examples/demo.hx)):

```
fn fib(n: int) -> int { if n < 2 { n } else { fib(n-1) + fib(n-2) } }

fn gcd(a: int, b: int) -> int {
    loop (x = a, y = b) { if y == 0 { break x } else { next y, x % y } }
}

comptime fn fact(n: int) -> int {      // evaluated at compile time
    loop (acc = 1, i = 1) { if i > n { break acc } else { next acc*i, i+1 } }
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
  `backend.cpp` emits x86-64 bytes straight from the graph (`wiki/17`, `wiki/18`).

## Out of scope / known limitations (honest)

- **Backend is i64-centric** and uses a correctness-first **memory-backed
  virtual-register** model (every value gets a stack slot). It does *not* yet do
  real register allocation — that is the next step (`wiki/17` describes the linear-scan
  plan). Generated code is correct but not yet fast.
- **`load`/`store`/`call`-with-state** are modeled in the IR and checked by the
  verifier, but the JIT currently lowers the **pure + control + call** subset (no
  memory ops in codegen yet).
- **`idiv` edge cases** (`x / 0`, `INT64_MIN / -1`) trap on hardware like C UB; the
  interpreter defines them as `0`/`INT64_MIN`. Treat them as undefined in compiled code.
- Single target (**x86-64, Win64 ABI**), `≤ 4` parameters/args per function.
- The textual printer shows a *first-use scheduling view*; a pure value shared by
  sibling branches is printed at its first occurrence (the backend recomputes per
  branch). Codegen correctness is established by the differential tests, not the dump.

## Source layout

```
include/helix/   ir, eval, front, verify, print, x64, backend  (public headers)
src/             ir, eval, front, verify, print, backend        (implementation)
tests/           test framework + 7 test files (ir, eval, front, backend, verify, fuzz)
tools/helixc.cpp the CLI driver
examples/demo.hx a sample program
build.ps1        MSVC build + test runner
```
