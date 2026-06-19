// fuzz_mem — memory-aware DIFFERENTIAL fuzzer across BOTH Helix backends.
//
// Generates random WELL-FORMED Helix programs that reduce over a REAL i64 array
// passed as a `ptr` param: random counted loops folding an accumulator `acc`
// over `a[i]` with mixed arithmetic, comparisons, and select (if/else). It also
// emits a fraction of pure-scalar (no-memory) functions. For each generated
// function we allocate a real int64 array filled with random values and, for
// many random (array, n) inputs, compare THREE engines:
//
//     eval_func        — the reference interpreter (the oracle)
//     jit_compile      — simple, always-correct memory-backed codegen
//     jit_compile_ra   — optimizing backend (instruction selection + linear-scan RA)
//
// All three dereference the SAME real addresses, so on any input the interpreter
// runs to completion they MUST agree. Any disagreement is a real backend bug and
// is reported with the smallest repro we can find (program source + array + args).
//
// Why interp/simple/ra MUST agree (well-formedness baked into the generator):
//   * Everything is i64 (`int`) and every load is an i64 load. The interpreter
//     truncates per-node to the node's width; with i64 throughout, trunc() is the
//     identity and the JITs (full 64-bit registers) agree. Comparisons yield 0/1
//     in all three.
//   * A bare comparison node is bool-TYPED. Feeding it straight into arithmetic
//     would make the interpreter truncate the whole subexpression to 1 bit while
//     the JIT keeps 64 bits -> a SPURIOUS, generator-induced mismatch. So every
//     comparison is laundered through `if c { 1 } else { 0 }` into an i64-typed
//     0/1 before it enters arithmetic. (Still exercises "comparison mixed in".)
//   * Division / remainder divisors are nonzero constants in [1,4096]: no
//     div-by-zero (interp defines 0; JIT faults via idiv) and no INT64_MIN / -1
//     (interp defines it; JIT would #DE). Shift counts are constants in [0,63].
//   * Loops are COUNTED by the index `i`: the body is
//         if i >= n { break acc } else { let x = a[i]; next <fold(acc,i,x)>, i+1 }
//     so `i` runs 0,1,...,n-1 and the load a[i] is LEXICALLY confined to the
//     else-arm where i < n. The Cond region lowers to a real branch in BOTH
//     backends and is lazy in the interpreter, so a[i] is NEVER dereferenced at
//     i == n. Every index is therefore in [0, n) — no out-of-bounds read.
//   * `n` is always chosen in [0, ARRAY_LEN], so the whole index range stays
//     inside the allocated array.
//   * The loop runs at most ARRAY_LEN iterations, so it always terminates; the
//     interpreter's fuel is a backstop and any (rare) out_of_fuel case is skipped
//     and never compared.
//
// Build (ISOLATED, unique obj dir), linking all backend sources:
//   msvc.bat /nologo /EHsc /std:c++20 /O2 /I include tools/fuzz_mem.cpp \
//     src/ir.cpp src/eval.cpp src/front.cpp src/backend.cpp src/backend2.cpp \
//     src/opt.cpp src/coff.cpp src/print.cpp src/verify.cpp /Fe:fuzz_mem.exe \
//     /Fobuild/fuzz_mem/
//
// Run: fuzz_mem.exe [seed] [programs]

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "fuzz_watchdog.hpp"
#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"

using namespace helix;

// Real backing array: every generated program indexes into THIS, and the
// interpreter and both JITs dereference its real address. Sized generously; the
// generator always keeps n in [0, kArrayLen] and indices in [0, n).
static constexpr int kArrayLen = 256;

static int64_t addr_of(const int64_t* p) { return (int64_t)(uintptr_t)p; }

// ---------------------------------------------------------------------------
// Random program generator. Emits source text consumed by parse_module.
// ---------------------------------------------------------------------------
struct Gen {
    std::mt19937_64 rng;
    int next_tmp = 0;  // unique local-variable suffix within a function

    explicit Gen(uint64_t seed) : rng(seed) {}

    uint64_t u() { return rng(); }
    int range(int lo, int hi) {  // inclusive
        std::uniform_int_distribution<int> d(lo, hi);
        return d(rng);
    }
    bool chance(int pct) { return range(1, 100) <= pct; }
    std::string fresh_var() { return "t" + std::to_string(next_tmp++); }

    // Signature of the function we are currently building.
    struct Sig {
        std::string name;
        bool has_mem = false;          // first param is `a: ptr`
        std::vector<bool> param_is_ptr;  // per-param: is it the pointer?
        int nparams = 0;
    };

    // Variables in scope usable as i64 expression leaves (NOT the pointer param,
    // which is only ever used via a[...] indexing, never as a bare i64).
    struct Env {
        std::vector<std::string> vars;   // i64 values in scope
        std::string ptr_name;            // name of the `ptr` param, or "" if none
        std::string index_var;           // a loop index known to be in [0, n), or ""
    };

    // A small i64 leaf: an in-scope scalar or a modest constant.
    std::string leaf(const Env& env) {
        if (env.vars.empty() || chance(35)) return std::to_string(range(-40, 40));
        return env.vars[range(0, (int)env.vars.size() - 1)];
    }

    std::string safe_divisor() { return std::to_string(range(1, 4096)); }
    std::string shift_amount() { return std::to_string(range(0, 63)); }

    // A memory read a[idx] where idx is GUARANTEED in [0, n): we only ever index
    // with the in-bounds loop index (optionally with a small NON-NEGATIVE offset
    // that keeps it in range only when safe). To stay strictly in-bounds we ONLY
    // use the bare index var; mixing offsets risks i+off >= n, so we avoid it.
    std::string load_expr(const Env& env) {
        // Precondition: caller guarantees env.ptr_name and env.index_var are set
        // and that index_var is in [0, n).
        return env.ptr_name + "[" + env.index_var + "]";
    }

    // --- pure i64 expression of bounded depth. Never reads memory itself; any
    // memory value must be passed in via an in-scope `let x = a[i]` binding. ---
    std::string expr(const Env& env, int depth) {
        if (depth <= 0) return leaf(env);
        int pick = range(0, 9);
        switch (pick) {
            case 0:
            case 1: {  // binary arithmetic / bitwise
                static const char* ops[] = {"+", "-", "*", "&", "|", "^"};
                const char* op = ops[range(0, 5)];
                return "(" + expr(env, depth - 1) + " " + op + " " +
                       expr(env, depth - 1) + ")";
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
            case 5: {  // comparison laundered to an i64 0/1 (see header rationale)
                return "if " + cond_expr(env, depth - 1) + " { 1 } else { 0 }";
            }
            case 6: {  // select via if/else over a comparison, both arms i64
                std::string c = cond_expr(env, depth - 1);
                return "if " + c + " { " + expr(env, depth - 1) + " } else { " +
                       expr(env, depth - 1) + " }";
            }
            case 7: {  // let-binding then body
                std::string v = fresh_var();
                std::string val = expr(env, depth - 1);
                Env e2 = env;
                e2.vars.push_back(v);
                return "let " + v + " = " + val + "; " + expr(e2, depth - 1);
            }
            default:  // plain leaf to keep some subtrees shallow
                return leaf(env);
        }
    }

    // A boolean condition (any i64; nonzero = true), built from i64 expressions.
    std::string cond_expr(const Env& env, int depth) {
        static const char* cmps[] = {"==", "!=", "<", "<=", ">", ">="};
        const char* op = cmps[range(0, 5)];
        return "(" + expr(env, depth) + " " + op + " " + expr(env, depth) + ")";
    }

    // Emit a counted reduction loop over a[0..n) for a memory function. `acc` is
    // folded; the loaded element a[i] is bound as `x` ONLY inside the in-bounds
    // else-arm, so it is never read at i == n.
    std::string reduction_loop(Env env, int depth) {
        std::string acc = fresh_var();
        std::string idx = fresh_var();
        std::string init_acc = expr(env, depth - 1);  // init may reference params

        Env lenv = env;
        lenv.vars.push_back(acc);
        lenv.index_var = idx;  // i, known in [0, n) inside the else-arm

        // Inside the else-arm, idx is in [0, n): bind x = a[idx] and fold acc.
        Env body_env = lenv;
        body_env.vars.push_back("x");  // the loaded element
        std::string fold = expr(body_env, depth);  // new acc using acc, i, x, params

        std::string s;
        s += "loop (" + acc + " = " + init_acc + ", " + idx + " = 0) {\n";
        s += "    if " + idx + " >= n {\n";
        s += "      break " + acc + "\n";
        s += "    } else {\n";
        s += "      let x = " + lenv.ptr_name + "[" + idx + "];\n";
        s += "      next " + fold + ", " + idx + " + 1\n";
        s += "    }\n";
        s += "  }";
        return s;
    }

    // Emit one function. ~75% are memory functions: `fn name(a: ptr, n: int, ...)`.
    // The rest are pure-scalar (no memory) to exercise the non-memory path too.
    std::string emit_func(const std::string& name, Sig& sig) {
        next_tmp = 0;
        bool has_mem = chance(75);
        sig.name = name;
        sig.has_mem = has_mem;

        Env env;
        std::string src = "fn " + name + "(";
        int idxp = 0;
        if (has_mem) {
            src += "a: ptr, n: int";
            env.ptr_name = "a";
            sig.param_is_ptr.push_back(true);   // a
            sig.param_is_ptr.push_back(false);  // n
            // n is in scope as an i64 too (used in comparisons / arithmetic).
            env.vars.push_back("n");
            idxp = 2;
        }
        int extra = range(has_mem ? 0 : 1, has_mem ? 2 : 3);
        for (int i = 0; i < extra; i++) {
            std::string pn = "p" + std::to_string(i);
            if (idxp || i) src += ", ";
            src += pn + ": int";
            env.vars.push_back(pn);
            sig.param_is_ptr.push_back(false);
        }
        sig.nparams = (int)sig.param_is_ptr.size();
        src += ") -> int {\n";

        int depth = range(2, 4);
        if (has_mem) {
            // Optionally wrap the reduction in some surrounding scalar arithmetic.
            std::string red = reduction_loop(env, depth);
            if (chance(40)) {
                std::string v = fresh_var();
                src += "  let " + v + " = " + red + ";\n";
                Env e2 = env;
                e2.vars.push_back(v);
                src += "  " + expr(e2, depth) + "\n";
            } else {
                src += "  " + red + "\n";
            }
        } else {
            src += "  " + expr(env, depth) + "\n";
        }
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

int main(int argc, char** argv) {
    uint64_t seed = 0xA5310BEEFull;
    int programs = 6000;
    if (argc > 1) seed = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) programs = (int)std::strtol(argv[2], nullptr, 10);

    std::printf("fuzz_mem: seed=%llu programs=%d array_len=%d\n",
                (unsigned long long)seed, programs, kArrayLen);

    // One real backing array, refilled with fresh random values per program.
    std::vector<int64_t> arr(kArrayLen);
    std::mt19937_64 datarng(seed ^ 0xD1CE5EEDF00Dull);
    std::mt19937_64 argrng(seed ^ 0x9E3779B97F4A7C15ull);

    // Bound interpreter work: each loop does <= kArrayLen iterations and bodies
    // are shallow, so this fuel is ample; out_of_fuel cases are simply skipped.
    const long kFuel = 5'000'000;

    FuzzWatchdog wd;
    wd.start();

    Stats st;
    bool found = false;
    std::string repro_src, repro_detail;
    std::vector<int64_t> repro_arr;  // snapshot of the array contents for the repro

    for (int prog = 0; prog < programs && !found; prog++) {
        if (prog > 0 && prog % 500 == 0) {
            std::printf("  ... %d/%d programs, %ld cases compared, %ld skipped\n",
                        prog, programs, st.cases_run, st.cases_skipped);
            std::fflush(stdout);
        }

        // Fresh random array contents for this program (mix of small + large +
        // boundary values to exercise overflow paths in the fold).
        for (int i = 0; i < kArrayLen; i++) {
            int mode = (int)(datarng() % 4);
            if (mode == 0) arr[i] = (int64_t)(datarng() % 201) - 100;       // [-100,100]
            else if (mode == 1) arr[i] = (int64_t)(datarng() % 200001) - 100000;
            else if (mode == 2) arr[i] = (int64_t)datarng();                 // full 64-bit
            else {
                static const int64_t bs[] = {0, 1, -1, INT64_MAX, INT64_MIN,
                                             INT32_MAX, INT32_MIN, 2, -2};
                arr[i] = bs[datarng() % 9];
            }
        }

        Gen g(seed + (uint64_t)prog * 0x100000001B3ull);
        Gen::Sig sig;
        std::string name = "f0";
        std::string src = g.emit_func(name, sig);

        World w;
        ParseStatus ps = parse_module(w, src);
        if (!ps.ok) { st.programs_parse_fail++; continue; }

        JitModule simple = jit_compile(w);
        JitModule ra = jit_compile_ra(w);
        if (!simple.ok || !ra.ok) { st.programs_jit_fail++; continue; }
        st.programs_ok++;

        NodeId f = w.find_func(name);
        if (f == NONE || !simple.has(f) || !ra.has(f)) continue;

        // Many random (array, n) inputs per program.
        const int kVectors = 24;
        for (int v = 0; v < kVectors && !found; v++) {
            std::vector<int64_t> args;
            args.reserve(sig.nparams);
            for (int i = 0; i < sig.nparams; i++) {
                if (sig.param_is_ptr[i]) {
                    args.push_back(addr_of(arr.data()));        // the real array
                } else if (i == 1 && sig.has_mem) {
                    args.push_back((int64_t)(argrng() % (kArrayLen + 1)));  // n in [0,len]
                } else {
                    // scalar arg: mix of magnitudes / boundary values
                    int mode = (int)(argrng() % 4);
                    int64_t a;
                    if (mode == 0) a = (int64_t)(argrng() % 41) - 20;
                    else if (mode == 1) a = (int64_t)(argrng() % 2001) - 1000;
                    else if (mode == 2) a = (int64_t)argrng();
                    else {
                        static const int64_t bs[] = {0, 1, -1, INT64_MAX, INT64_MIN,
                                                     INT32_MAX, INT32_MIN, 2, -2};
                        a = bs[argrng() % 9];
                    }
                    args.push_back(a);
                }
            }

            EvalResult er = eval_func(w, f, args, kFuel);
            if (er.out_of_fuel || !er.ok) { st.cases_skipped++; continue; }

            {  // watchdog: interp already terminated, so a hung JIT call is a real bug
                std::string ctx = "function f args=[";
                for (size_t i = 0; i < args.size(); i++)
                    ctx += (i ? ", " : "") + std::to_string((long long)args[i]);
                ctx += "]";
                wd.arm(src, std::move(ctx), 3000);
            }
            int64_t sv = simple.call(f, args);
            int64_t rv = ra.call(f, args);
            wd.disarm();
            st.cases_run++;

            if (sv != er.value || rv != er.value) {
                st.mismatches++;
                found = true;
                repro_src = src;
                repro_arr = arr;
                repro_detail = "function " + name + " args=[";
                for (size_t i = 0; i < args.size(); i++) {
                    if (i) repro_detail += ", ";
                    repro_detail += std::to_string(args[i]);
                }
                repro_detail += "]\n  interp =" + std::to_string(er.value) +
                                "\n  simple =" + std::to_string(sv) +
                                "\n  ra     =" + std::to_string(rv);
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
        std::printf("\n!!!! MISMATCH FOUND !!!!\n%s\n", repro_detail.c_str());
        std::printf("\n---- program source ----\n%s\n", repro_src.c_str());
        std::printf("---- array contents (int64) ----\n");
        for (size_t i = 0; i < repro_arr.size(); i++)
            std::printf("  a[%zu] = %lld\n", i, (long long)repro_arr[i]);
        return 2;
    }

    std::printf("\nALL %ld 3-WAY CASES PASSED (interp == simple == ra).\n",
                st.cases_run);
    return 0;
}
