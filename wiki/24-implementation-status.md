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
  └─ schedule (Global Code Motion) ...... src/schedule.cpp     (DC13)
       └─ place each value at the region (LCA of its uses) that dominates them
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
| Global Code Motion (dominance-correct scheduling) | `schedule.cpp` | each value emitted once, in the LCA region of its uses; loop bodies split pre/post the exit test. Shared by both backends |
| Direct, in-place lowering to machine code | `backend2.cpp` | VCode is not a separate IR — it is the in-progress machine code (Cranelift-style) |
| Linear-scan register allocation + spilling | `backend2.cpp` | values in callee-saved regs survive calls |
| Direct-to-bytes (JIT) and COFF object output | `backend2.cpp`, `coff.cpp` | no LLVM, no secondary backend IR |

## Validation evidence

- **81 unit/integration tests, ~23,500 assertions** — `./build.ps1` (the verifier also runs
  in-codegen on every test in debug builds).
- **Millions of randomized differential cases, 0 mismatches**, three generators
  (`interp == simple == ra`, return value AND post-run memory):
  - `fuzz_cf` — nested if/else, single/multi-var `loop`/`break`/`next`, bounded
    recursion, calls (up to 8 args, exercising stack-passed args); ~2M cases/seed.
  - `fuzz_imp` — mutable `var`, `while`, statement-`if`/`else`, nested loops, in-bounds
    array reads AND writes; 120k cases/seed.
  - `fuzz_mem` — array-read programs over real backing memory.
  - Each fuzzer runs the interpreter first under a fuel cap and is wrapped by a
    **watchdog** (`tools/fuzz_watchdog.hpp`): since the interpreter proves termination,
    a JIT call that loops forever is a real miscompile — the watchdog reports the repro
    and exits fast instead of hanging.
- **Native end-to-end**: `helixc --emit-obj` → `link.exe` → a real `.exe` that runs
  Helix-compiled `fib`/`gcd`/`sum` correctly.
- **Adversarial review + fuzzing** found and fixed real bugs, each with a regression test:
  the original 14 (idiv traps, cond predicate semantics, comptime forward-refs, i32
  width, UB folds, an arena dangling-reference), plus a **scheduling (GCM) class**: a
  value used from several control-divergent positions (e.g. an inner loop's result read
  in both arms of a following `if`, or a break-value `if`-expression evaluated at the
  loop exit) was emitted in one branch and read as garbage in another. Fixed by the
  dominance-correct scheduler (`schedule.cpp`); both the cross-branch case and the
  break-value-cone case are pinned by regression tests.

## Tier-0 — DONE (the credibility gaps from [25-path-to-production](25-path-to-production.md) are closed)

- **The optimizer is wired into the driver.** `helixc -O[N]` runs `optimize_module` (inlining,
  re-folding via the smart constructors), then **dead-function DCE** (`reachable_functions` +
  `World::keep_funcs`) when a single entry is known, then re-verifies. Previously inlining/DCE
  were test-only library functions the driver never called.
- **The optimizer is validated.** A new differential fuzzer (`tools/fuzz_opt.cpp`) checks
  `interp(unopt) == interp(opt) == jit(opt) == jit(unopt)` — catching a semantics-changing pass,
  which the single-graph fuzzers cannot. 0 mismatches; pinned by a unit test too.
- **The verifier runs on the codegen path.** Debug/CI builds verify every function inside
  `jit_compile` / `jit_compile_ra` / `compile_module_obj`, so the fuzzers exercise it on every
  generated graph (it ran only in the CLI before).
- **All recursive walks hardened.** The verifier DFS is iterative; the interpreter, the printer,
  and the inliner's `Cloner` all have recursion-depth guards that degrade gracefully
  (`out_of_fuel` / elision / leave-unoptimized) instead of overflowing the stack. The parser has
  a depth guard so deeply-nested input is a parse error, not a crash.
- **Parser robustness fuzzed.** `tools/fuzz_parse.cpp` feeds malformed / garbage / deeply-nested
  / truncated input — 60k inputs, **no crash** (graceful parse errors); a UTF-8 BOM is skipped.
- **Disassembler extended** to decode every form the encoder emits (immediate ALU/shift/imul,
  rsp-relative mem, movsx/movsxd); the component test now asserts **all** bytes decode, making the
  "encoder is independently double-checked" claim real.
- **Diagnostics → stderr** with stable exit codes (`0` ok / `1` error / `2` mismatch).
- **div0 / INT_MIN÷-1 pinned**: total signed division, identical across interpreter and both
  backends, documented in [13-types-and-effects](13-types-and-effects.md).
- **CI.** `helix/ci.ps1` + `.github/workflows/ci.yml` build, run the suite, and smoke-fuzz all
  five engines (requires `git init` + a remote to run hosted).

## Recently added (all validated interp == simple == ra)

- Array **writes** (`a[i] = v`, in-place sorts): a linear `$mem` token threads loads/stores;
  the GCM scheduler places each store once, in order, never speculated past a guard. The
  old "ordering miscompile" was actually a sibling-loop interpreter-memoization bug, found
  and fixed under writes fuzzing (`tools/fuzz_imp`, ~1M cases).
- **Win64 ABI up to 8 args**: 4 in registers, the 5th+ on the stack (incoming and outgoing).
- Integer widths **i8 / i16** (alongside i32/i64): narrow arithmetic wraps; narrow args
  sign-extend.
- **Immediate operands** in the optimizing backend (`x + c`, `x << 3`, `x < c` fold the
  constant into the instruction instead of materializing it).

## What is NOT built yet (honest)

- Single target (x86-64 Win64), ≤8 params, no register coalescing / live-range splitting.
- **Unsigned** integer types (division/shift/compare are signed); the only memory cells are
  i64 (`a[i]`), so i8/i16/i32 apply to scalar arithmetic, not loads/stores.
- The full Tier-2 equality overlay and SMT-verified rule DSL (the design's optional
  pieces) are described but not implemented; the shipped optimizer is Tier-1 + structural.

## See also

- [Core Model](11-core-model.md) · [Reduction Engine](14-reduction-engine.md) ·
  [Codegen](17-codegen.md) · [Risks](22-risks-and-open-problems.md)
- The code: [`helix/README.md`](../helix/README.md)
