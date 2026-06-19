// fuzz_imp — IMPERATIVE + ARRAY-WRITE differential fuzzer for the Helix backend.
//
// Generates random WELL-FORMED programs in Helix's *imperative* surface language
// (mutable `var`, assignment, `while`, statement-`if`/`else`, array READS `a[i]`
// and array WRITES `a[j] = expr`) and checks that the three engines agree on BOTH
// the function's return value AND the post-execution contents of a REAL int64 array:
//
//     eval_func        — the reference interpreter (the oracle)
//     jit_compile      — simple, always-correct memory-backed codegen
//     jit_compile_ra   — optimizing backend (instruction selection + linear-scan RA)
//
// Each generated function has the shape
//
//     fn f(a: ptr, n: int) -> int {
//         var acc = <expr>;            // a few mutable scalar decls
//         var i = 0;
//         while i < n {                // outer counted loop (counter strictly ++)
//             ... body stmts: assigns, a[i]=expr, nested if/else,
//                 optionally a nested `while j < n-1 { ... }` ...
//             i = i + 1;
//         }
//         return acc;                  // return a live mutable
//     }
//
// All three engines dereference the SAME real addresses, so on any input the
// interpreter runs to completion they MUST agree on the return value and on every
// array cell. A disagreement is a real engine bug (in the interpreter or a backend),
// reported with the smallest repro (program SOURCE, input array, args, and each
// engine's array + return). The driver does NOT stop on the first mismatch: it runs
// the full requested volume, counts every divergence, and at the end prints the
// SMALLEST diverging program (by source length) as the repro. Set env
// HELIX_FUZZ_STOP=1 to abort at the first mismatch instead.
//
// Why interp / simple / ra MUST agree (well-formedness baked into the generator):
//   * Everything is i64 (`int`); every load/store is an i64 load/store. The
//     interpreter truncates per node to the node width; with i64 throughout trunc()
//     is the identity, so the full-64-bit JITs agree.
//   * Comparisons appear ONLY in `if`/`while` conditions (bool context) — never fed
//     into arithmetic — so there is no bool-truncation artifact (a bare bool-typed
//     comparison fed to arithmetic would make the interpreter truncate to 1 bit
//     while the JIT keeps 64 bits). Conditions are pure scalar comparisons of
//     in-scope i64 values; they NEVER read the array (a load in a `while`-condition
//     would dereference a[i] at i == n on the exit check -> OOB).
//   * Division / remainder divisors are nonzero constants in [1,4096]: no div-by-zero
//     (interp defines 0; JIT #DE) and no INT64_MIN/-1. Shift counts are constants
//     in [0,63].
//   * TERMINATION: every loop has a dedicated counter that starts at a fixed value
//     and is incremented by exactly +1 UNCONDITIONALLY at the very end of the body,
//     and the loop runs `while ctr < BOUND` with BOUND a loop-invariant param
//     expression (`n` or `n-1`, possibly minus a small constant) — the counter is
//     never otherwise reassigned, so it strictly increases to BOUND and the loop
//     runs a bounded number of times (<= n <= kMaxN). With a 2-deep loop nest that is
//     <= kMaxN*kMaxN body executions, FAR below the (large) fuel cap, so a real,
//     terminating program never reaches the out_of_fuel path; any out_of_fuel case
//     would be a generator escape and is skipped (never compared) as a backstop.
//   * IN-BOUNDS: every array index is provably in [0, n):
//       - bare counter `i`              — in [0, BOUND) ⊆ [0, n)             (always ok)
//       - `i - 1`  ONLY when the loop counter STARTS at 1 (so i >= 1 always)  -> [0, n)
//       - `i + 1`  ONLY when the loop BOUND is `n - 1` (so i+1 <= n-1 < n)    -> [0, n)
//     The generator tracks, per loop, which of {i, i-1, i+1} are legal and only ever
//     emits those. No index ever mixes the counter with a non-constant value.
//   * `n` is chosen in [1, kMaxN] (>=1 keeps `n-1`-bounded loops sane); the real
//     backing array is allocated with kArrayLen (> kMaxN) cells, so every in-[0,n)
//     index stays well inside the allocation.
//
// Build (ISOLATED, unique obj dir), linking all engine sources:
//   msvc.bat /nologo /EHsc /std:c++20 /O2 /I include tools/fuzz_imp.cpp \
//     src/ir.cpp src/eval.cpp src/front.cpp src/backend.cpp src/backend2.cpp \
//     src/opt.cpp src/coff.cpp src/print.cpp src/verify.cpp /Fe:fuzz_imp.exe \
//     /Fobuild/fuzz_imp/
//
// Run: fuzz_imp.exe [seed] [programs]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"

using namespace helix;

// Backing-array length (allocation size). The generator always keeps n in
// [1, kMaxN] and every index in [0, n), so this real array is never indexed OOB.
static constexpr int kArrayLen = 64;
// Upper bound on the loop trip-count argument `n`. Kept modest (<< kArrayLen) so a
// 2-deep nest of counted loops performs at most ~kMaxN*kMaxN body executions and the
// interpreter's step count stays FAR below the fuel cap — we must never actually hit
// out_of_fuel during a real (terminating) program, only on a true generator escape.
static constexpr int kMaxN = 24;

static int64_t addr_of(const int64_t* p) { return (int64_t)(uintptr_t)p; }

// ---------------------------------------------------------------------------
// Random program generator. Emits source text consumed by parse_module.
// ---------------------------------------------------------------------------
struct Gen {
    std::mt19937_64 rng;
    int next_tmp = 0;  // unique local-variable suffix within a function

    explicit Gen(uint64_t seed) : rng(seed) {}

    int range(int lo, int hi) {  // inclusive
        std::uniform_int_distribution<int> d(lo, hi);
        return d(rng);
    }
    bool chance(int pct) { return range(1, 100) <= pct; }
    std::string fresh_var() { return "t" + std::to_string(next_tmp++); }

    // Variables in scope. `vars` are READABLE i64 leaves (includes the param `n`
    // and the loop counters); `muts` are the ASSIGNABLE mutable scalars (the `acc*`
    // accumulators) — a STRICT subset of `vars`. Assignment targets are drawn ONLY
    // from `muts`, never `n` (an immutable param the parser rejects) and never a
    // loop counter (which must be reassigned only by its own `ctr = ctr + 1`, so it
    // stays strictly increasing and the loop terminates).
    struct Env {
        std::vector<std::string> vars;     // readable i64 leaves (n, accs, counters)
        std::vector<std::string> muts;     // assignable mutable scalars (accs only)
        std::string ptr = "a";             // the array param
        // The innermost loop's index context, for in-bounds array access:
        std::string idx;                   // current counter name ("" outside loops)
        bool allow_minus1 = false;         // counter starts at 1  -> i-1 in [0,n)
        bool allow_plus1 = false;          // bound is n-1         -> i+1 in [0,n)
    };

    std::string leaf(const Env& env) {
        if (env.vars.empty() || chance(35)) return std::to_string(range(-30, 30));
        return env.vars[range(0, (int)env.vars.size() - 1)];
    }

    std::string safe_divisor() { return std::to_string(range(1, 4096)); }
    std::string shift_amount() { return std::to_string(range(0, 63)); }

    // A provably in-bounds array INDEX expression for the current loop: one of
    // i, i-1 (only if allowed), i+1 (only if allowed). Requires env.idx set.
    std::string index_of(const Env& env) {
        std::vector<int> opts;
        opts.push_back(0);
        if (env.allow_minus1) opts.push_back(-1);
        if (env.allow_plus1) opts.push_back(+1);
        int off = opts[range(0, (int)opts.size() - 1)];
        if (off == 0) return env.idx;
        if (off < 0) return "(" + env.idx + " - 1)";
        return "(" + env.idx + " + 1)";
    }

    // A read-only array load a[<in-bounds index>]. Only valid inside a loop.
    std::string load_expr(const Env& env) { return env.ptr + "[" + index_of(env) + "]"; }

    // --- pure i64 expression of bounded depth. May read the array (in-bounds) when
    // inside a loop; never contains a comparison (those live only in conditions). ---
    std::string expr(const Env& env, int depth) {
        if (depth <= 0) {
            // leaf: a scalar, a constant, or (inside a loop) an in-bounds load
            if (!env.idx.empty() && chance(40)) return load_expr(env);
            return leaf(env);
        }
        int pick = range(0, 8);
        switch (pick) {
            case 0:
            case 1: {  // binary arithmetic / bitwise
                static const char* ops[] = {"+", "-", "*", "&", "|", "^"};
                const char* op = ops[range(0, 5)];
                return "(" + expr(env, depth - 1) + " " + op + " " + expr(env, depth - 1) + ")";
            }
            case 2: {  // division / remainder with a safe nonzero constant divisor
                const char* op = chance(50) ? "/" : "%";
                return "(" + expr(env, depth - 1) + " " + op + " " + safe_divisor() + ")";
            }
            case 3: {  // shift by a constant in [0,63]
                const char* op = chance(50) ? "<<" : ">>";
                return "(" + expr(env, depth - 1) + " " + op + " " + shift_amount() + ")";
            }
            case 4:  // unary negate
                return "(-" + expr(env, depth - 1) + ")";
            case 5:  // in-bounds load (inside loops) or a leaf otherwise
                if (!env.idx.empty()) return load_expr(env);
                return leaf(env);
            default:
                return leaf(env);
        }
    }

    // A boolean condition over PURE scalars (no array reads): cmp of two i64 exprs.
    // Used only in if/while heads. Never feeds a value into arithmetic.
    std::string cond_expr(const Env& env, int depth) {
        static const char* cmps[] = {"==", "!=", "<", "<=", ">", ">="};
        const char* op = cmps[range(0, 5)];
        // Build operands from scalars only (idx cleared) so conditions never load.
        Env scal = env;
        scal.idx.clear();
        return "(" + expr(scal, depth) + " " + op + " " + expr(scal, depth) + ")";
    }

    // Emit one straight-line body statement (no loops). `ind` is indentation.
    // Appends to `out`. Assignment targets come ONLY from env.muts (so `n` and loop
    // counters are never reassigned). May write the array in-bounds and read it.
    void emit_stmt(std::string& out, const Env& env, const std::string& ind, int depth) {
        int dd = depth - 1 < 0 ? 0 : depth - 1;
        bool can_assign = !env.muts.empty();
        bool can_write = !env.idx.empty();
        int k = range(0, 9);
        if (k <= 4 && can_assign) {
            // assignment to an existing mutable scalar accumulator
            const std::string& v = env.muts[range(0, (int)env.muts.size() - 1)];
            out += ind + v + " = " + expr(env, depth) + ";\n";
        } else if (k <= 6 && can_write) {
            // array write a[<in-bounds>] = expr
            out += ind + env.ptr + "[" + index_of(env) + "] = " + expr(env, depth) + ";\n";
        } else if (k <= 8 && (can_assign || can_write)) {
            // statement-if (optionally with else) that conditionally assigns / writes
            std::string c = cond_expr(env, dd);
            out += ind + "if " + c + " {\n";
            emit_stmt(out, env, ind + "  ", dd);
            out += ind + "}";
            if (chance(50)) {
                out += " else {\n";
                emit_stmt(out, env, ind + "  ", dd);
                out += ind + "}";
            }
            out += "\n";
        } else if (can_assign) {
            const std::string& v = env.muts[range(0, (int)env.muts.size() - 1)];
            out += ind + v + " = " + expr(env, depth) + ";\n";
        } else if (can_write) {
            out += ind + env.ptr + "[" + index_of(env) + "] = " + expr(env, depth) + ";\n";
        }
    }

    // Emit a counted loop `while ctr < BOUND { body; ctr = ctr + 1; }`. The body may
    // contain straight-line statements and (at the outer level) a nested loop. All
    // scalars in env.vars remain assignable (-> loop-carried). The counter starts at
    // `start` (0 or 1) and BOUND is `n` (if start==0/allow i+1 false) or `n - 1`.
    void emit_loop(std::string& out, Env env, const std::string& ind, int depth, bool allow_nested) {
        std::string ctr = fresh_var();
        // Decide loop flavor: start at 0 or 1; bound n or n-1.
        bool start1 = chance(35);
        bool boundNm1 = chance(35);
        std::string start = start1 ? "1" : "0";
        std::string bound = boundNm1 ? "(n - 1)" : "n";

        out += ind + "var " + ctr + " = " + start + ";\n";
        out += ind + "while " + ctr + " < " + bound + " {\n";

        Env lenv = env;
        lenv.idx = ctr;
        lenv.allow_minus1 = start1;     // i-1 >= 0 guaranteed only when i starts at 1
        lenv.allow_plus1 = boundNm1;    // i+1 <= n-1 < n guaranteed only when bound is n-1
        // The counter is a READABLE leaf (vars) but NOT assignable (not in muts), so
        // the body can use it in expressions yet never reassigns it — only the
        // explicit `ctr = ctr + 1;` below mutates it -> strictly increasing.
        lenv.vars.push_back(ctr);

        std::string bind = ind + "  ";
        int nstmt = range(1, 4);
        for (int s = 0; s < nstmt; s++) {
            // Occasionally open a nested loop (only one level; keeps iteration counts
            // and source size bounded). The nested loop gets its own counter context.
            if (allow_nested && s == 0 && chance(35)) {
                emit_loop(out, lenv, bind, depth, /*allow_nested=*/false);
            } else {
                emit_stmt(out, lenv, bind, depth);
            }
        }
        // Unconditional counter increment LAST: guarantees termination.
        out += bind + ctr + " = " + ctr + " + 1;\n";
        out += ind + "}\n";
    }

    // Emit one whole function. Always a memory function: fn f(a: ptr, n: int) -> int.
    std::string emit_func(const std::string& name) {
        next_tmp = 0;
        Env env;
        env.ptr = "a";
        // n is in scope as an i64 leaf (used in arithmetic / conditions); never
        // reassigned, so it stays a loop-invariant bound.
        env.vars.push_back("n");

        std::string src = "fn " + name + "(a: ptr, n: int) -> int {\n";

        // A couple of mutable scalar accumulators, returned at the end.
        int naccs = range(1, 3);
        std::vector<std::string> accs;
        for (int i = 0; i < naccs; i++) {
            std::string v = "acc" + std::to_string(i);
            accs.push_back(v);
            // Init from pure scalars only (no loads: we are outside any loop).
            src += "  var " + v + " = " + expr(env, range(0, 2)) + ";\n";
            env.vars.push_back(v);   // readable as a leaf
            env.muts.push_back(v);   // and assignable (the only assignable scalars)
        }

        int depth = range(2, 3);
        int nloops = range(1, 2);
        for (int l = 0; l < nloops; l++) emit_loop(src, env, "  ", depth, /*allow_nested=*/true);

        // Return a live mutable accumulator (its post-loop value).
        const std::string& ret = accs[range(0, (int)accs.size() - 1)];
        src += "  return " + ret + ";\n";
        src += "}\n";
        return src;
    }
};

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------
struct Stats {
    long programs_ok = 0;
    long programs_parse_fail = 0;
    long programs_jit_fail = 0;
    long cases_run = 0;       // 3-way comparisons actually performed
    long cases_skipped = 0;   // interpreter out_of_fuel / not-ok
    long mismatches = 0;
};

// Fill an array with a mix of small, medium, full-64-bit, and boundary values.
static void fill_array(std::vector<int64_t>& arr, std::mt19937_64& r) {
    for (auto& x : arr) {
        int mode = (int)(r() % 4);
        if (mode == 0) x = (int64_t)(r() % 201) - 100;
        else if (mode == 1) x = (int64_t)(r() % 200001) - 100000;
        else if (mode == 2) x = (int64_t)r();
        else {
            static const int64_t bs[] = {0, 1, -1, INT64_MAX, INT64_MIN,
                                         INT32_MAX, INT32_MIN, 2, -2};
            x = bs[r() % 9];
        }
    }
}

int main(int argc, char** argv) {
    uint64_t seed = 0xF002B17EC0DEull;  // fixed default seed
    int programs = 40000;
    if (argc > 1) seed = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) programs = (int)std::strtol(argv[2], nullptr, 10);

    std::printf("fuzz_imp: seed=%llu programs=%d array_len=%d\n",
                (unsigned long long)seed, programs, kArrayLen);

    std::mt19937_64 datarng(seed ^ 0xD1CE5EEDF00Dull);
    std::mt19937_64 argrng(seed ^ 0x9E3779B97F4A7C15ull);

    // Each loop runs <= n <= kMaxN iterations; loops nest at most 2 deep, so the
    // worst-case interpreter step count per case is bounded and small. Fuel is set
    // FAR above that worst case so a real (always-terminating) program never reaches
    // the out_of_fuel path; any out_of_fuel case would indicate a generator escape
    // and is skipped (never compared) as a backstop.
    const long kFuel = 200'000'000;

    Stats st;
    bool found = false;
    std::string repro_src, repro_detail;
    std::vector<int64_t> repro_in;
    size_t repro_len = SIZE_MAX;   // length of the smallest-source repro kept so far
    // Whether to stop at the first mismatch (default: keep going to run the full
    // requested volume and then report the SMALLEST repro). Set env HELIX_FUZZ_STOP=1
    // to abort on the first mismatch instead.
    const bool stop_on_first = []{ const char* e = std::getenv("HELIX_FUZZ_STOP");
                                   return e && e[0] == '1'; }();

    // Scratch arrays: the input and one mutated copy per engine.
    std::vector<int64_t> in(kArrayLen), a0(kArrayLen), a1(kArrayLen), a2(kArrayLen);

    for (int prog = 0; prog < programs; prog++) {
        if (stop_on_first && found) break;
        if (prog > 0 && prog % 2000 == 0) {
            std::printf("  ... %d/%d programs, %ld cases compared, %ld skipped, %ld mismatches\n",
                        prog, programs, st.cases_run, st.cases_skipped, st.mismatches);
            std::fflush(stdout);
        }

        Gen g(seed + (uint64_t)prog * 0x100000001B3ull);
        std::string name = "f";
        std::string src = g.emit_func(name);

        World w;
        ParseStatus ps = parse_module(w, src);
        if (!ps.ok) { st.programs_parse_fail++; continue; }

        JitModule simple = jit_compile(w);
        JitModule ra = jit_compile_ra(w);
        if (!simple.ok || !ra.ok) { st.programs_jit_fail++; continue; }
        st.programs_ok++;

        NodeId f = w.find_func(name);
        if (f == NONE || !simple.has(f) || !ra.has(f)) continue;

        // Several random (array, n) inputs per program.
        const int kVectors = 12;
        for (int v = 0; v < kVectors; v++) {
            if (stop_on_first && found) break;
            fill_array(in, datarng);
            int64_t n = (int64_t)(argrng() % kMaxN) + 1;  // n in [1, kMaxN], << kArrayLen

            // Reference: run the interpreter on a fresh copy first. If it can't
            // finish within fuel, skip this case entirely (never compare).
            a0 = in;
            std::vector<int64_t> args0 = {addr_of(a0.data()), n};
            EvalResult er = eval_func(w, f, args0, kFuel);
            if (er.out_of_fuel || !er.ok) { st.cases_skipped++; continue; }

            // Both JITs on their own fresh copies of the SAME input.
            a1 = in;
            a2 = in;
            std::vector<int64_t> args1 = {addr_of(a1.data()), n};
            std::vector<int64_t> args2 = {addr_of(a2.data()), n};
            int64_t sv = simple.call(f, args1);
            int64_t rv = ra.call(f, args2);
            st.cases_run++;

            bool ret_ok = (sv == er.value) && (rv == er.value);
            bool arr_ok = (a1 == a0) && (a2 == a0);
            if (!ret_ok || !arr_ok) {
                st.mismatches++;
                found = true;
                // Keep the SMALLEST repro by source length, so the final report shows
                // the most reduced program among everything that diverged.
                if (src.size() < repro_len) {
                    repro_len = src.size();
                    repro_src = src;
                    repro_in.assign(in.begin(), in.begin() + (size_t)n);  // only the used prefix
                    repro_detail = "function " + name + " n=" + std::to_string(n) + "\n";
                    repro_detail += "  return: interp=" + std::to_string(er.value) +
                                    " simple=" + std::to_string(sv) +
                                    " ra=" + std::to_string(rv) + "\n";
                    // Show the first array cell that differs (and how) for each backend.
                    auto diffline = [&](const char* tag, const std::vector<int64_t>& got) {
                        for (int i = 0; i < (int)n; i++) {
                            if (got[i] != a0[i]) {
                                repro_detail += std::string("  array ") + tag + ": first diff at [" +
                                                std::to_string(i) + "] interp=" + std::to_string(a0[i]) +
                                                " " + tag + "=" + std::to_string(got[i]) + "\n";
                                return;
                            }
                        }
                        repro_detail += std::string("  array ") + tag + ": matches interp\n";
                    };
                    diffline("simple", a1);
                    diffline("ra", a2);
                }
            }
        }
    }

    std::printf("\n==== results ====\n");
    std::printf("programs ok           : %ld\n", st.programs_ok);
    std::printf("programs parse-failed : %ld\n", st.programs_parse_fail);
    std::printf("programs jit-failed   : %ld\n", st.programs_jit_fail);
    std::printf("3-way cases compared  : %ld\n", st.cases_run);
    std::printf("cases skipped (fuel)  : %ld\n", st.cases_skipped);
    std::printf("mismatches            : %ld\n", st.mismatches);

    if (found) {
        std::printf("\n!!!! MISMATCH FOUND (smallest repro of %ld total) !!!!\n%s\n",
                    st.mismatches, repro_detail.c_str());
        std::printf("---- program source ----\n%s\n", repro_src.c_str());
        std::printf("---- input array (int64, first n cells; rest are random) ----\n");
        for (size_t i = 0; i < repro_in.size(); i++)
            std::printf("  in[%zu] = %lld\n", i, (long long)repro_in[i]);
        return 2;
    }

    std::printf("\nALL %ld 3-WAY CASES PASSED (interp == simple == ra; return + array).\n",
                st.cases_run);
    return 0;
}
