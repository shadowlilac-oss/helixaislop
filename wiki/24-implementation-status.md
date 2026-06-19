# 24 — Implementation Status

*What of the Helix design is actually built, where, and how it is validated. The
implementation lives in [`../`](../) (the `helix/` tree); this page is the bridge
from the design wiki to the working code.*

Helix is not just a design wiki — there is a **working, dependency-free C++20
implementation** that takes source to native x86-64. It is a rigorously-tested
compiler over a small (Turing-complete) integer language, not a finished
production toolchain (see [Risks](22-risks-and-open-problems.md), R1/R3).

## The pipeline, implemented

```
source.hx
  └─ parse DIRECTLY to graph ............ src/front.cpp        (DC12)
       └─ Tier-1 normalize at construction (fold/identities/CSE/strength-reduce)
                                          src/ir.cpp           (DC8)
       └─ comptime = the same reduction  src/front.cpp+eval.cpp(DC9)
  └─ verify invariants .................. src/verify.cpp       (DC2/4/7)
  └─ optimize (inline, DCE) ............. src/opt.cpp          (wiki/16)
  └─ lower → VCode → liveness → linear-scan register allocation → x86-64
                                          src/backend2.cpp     (DC13/DC15)
  └─ JIT-execute  OR  emit COFF .obj → link.exe → native .exe
                                          src/backend2.cpp + src/coff.cpp
```

The interpreter (`src/eval.cpp`) is the **reference oracle**: every backend is
validated by differential testing against it.

## Design ↔ code map

| Design element | Where | Notes |
|---|---|---|
| Six node forms (`Const/Op/Cond/Loop/Func/Module`) | `ir.hpp` | exactly as specified (DC17) |
| Two strands (value floats / state linear) | `ir.hpp`, `verify.cpp` | linearity **enforced** (closes MimIR's hole) |
| Block parameters, no phi; acyclic | `ir.hpp` | loops/recursion as regions |
| Hash-consing + smart constructors | `ir.cpp` | GVN/CSE/fold for free at construction |
| Comptime = graph reduction (NbE-style, fuel-bounded) | `front.cpp`, `eval.cpp` | constant calls fold to constants |
| One textual format (printable/diffable) | `print.cpp` | DC16 |
| Tier-1 oriented rewrites | `ir.cpp` (`binop`/`cmp`/`make_cond`) | identities, strength reduction |
| Inlining / DCE (inter-procedural) | `opt.cpp` | clone-and-substitute through the smart constructors |
| Direct, in-place lowering to machine code | `backend2.cpp` | VCode is not a separate IR — it is the in-progress machine code (Cranelift-style) |
| Linear-scan register allocation + spilling | `backend2.cpp` | values in callee-saved regs survive calls |
| Direct-to-bytes (JIT) and COFF object output | `backend2.cpp`, `coff.cpp` | no LLVM, no secondary backend IR |

## Validation evidence

- **68 unit/integration tests, ~21,200 assertions** — `./build.ps1`.
- **~5,400** randomized differential checks for the optimizing backend (`jit_ra == interp == C++ ref`).
- **405,651** randomized control-flow differential checks (nested if/loops/recursion/calls), **0 mismatches**.
- **Native end-to-end**: `helixc --emit-obj` → `link.exe` → a real `.exe` that runs
  Helix-compiled `fib`/`gcd`/`sum` correctly.
- **Adversarial review** of the implementation found 14 real bugs (idiv traps, cond
  predicate semantics, comptime forward-refs, i32 width, UB folds, an arena
  dangling-reference) — all fixed, each with a regression test.

## What is NOT built yet (honest)

- Fine-grained effects: memory uses a single total-order state token (correct but
  conservative — no alias-class states or store-to-load forwarding). Array reads AND
  writes (`a[i]`, `a[i]=v`), mutable `var`/`while`/statement-`if`, and multi-result
  loops ARE implemented and validated (interp == both backends; native bubble sort).
- Single target (x86-64 Win64), ≤4 params, no register coalescing / live-range splitting.
- The full Tier-2 equality overlay and SMT-verified rule DSL (the design's optional
  pieces) are described but not implemented; the shipped optimizer is Tier-1 + structural.

## See also

- [Core Model](11-core-model.md) · [Reduction Engine](14-reduction-engine.md) ·
  [Codegen](17-codegen.md) · [Risks](22-risks-and-open-problems.md)
- The code: [`helix/README.md`](../helix/README.md)
