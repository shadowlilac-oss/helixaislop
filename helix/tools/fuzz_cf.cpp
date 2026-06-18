// fuzz_cf — control-flow / nested-loop differential fuzzer for the Helix backend.
//
// Generates random WELL-FORMED Helix programs exercising nested if/else, single-
// and multi-variable counted loops (break/next), bounded recursion, calls between
// generated functions, and mixed arithmetic / comparison / select expressions.
// Each generated module is JIT-compiled (jit_compile) and, for many random
// argument vectors per function, the JIT result is compared against the reference
// interpreter (eval_func). Any disagreement on a case where the interpreter ran to
// completion (ok == true) is a real backend bug and is reported with the smallest
// repro we can find (the function source + the arguments).
//
// Well-formedness / definedness guarantees baked into the generator so that the
// interpreter and the JIT MUST agree:
//   * Everything is i64. The interpreter truncates per-node to the node's type
//     width; the JIT works in full 64-bit registers and does not truncate to
//     sub-64-bit widths. With i64 throughout, trunc() is the identity, so they
//     agree. (Comparisons yield 0/1 in both.)
//   * Division / remainder divisors are always nonzero constants in [1,4096].
//     This rules out div-by-zero (interp defines it as 0; JIT faults via idiv)
//     and INT64_MIN / -1 (interp defines it; JIT faults). So div/rem are fully
//     defined and identical in both engines.
//   * Shift counts are constants in [0,63] (both engines mask by 63 anyway).
//   * Loops are counted: a dedicated counter param strictly decreases by 1 each
//     iteration and the loop breaks when it reaches 0. Guaranteed to terminate,
//     so the JIT (which has no fuel) always halts; the interpreter's fuel is a
//     backstop and any out_of_fuel case is skipped.
//   * Recursion is guarded by `if (n <= 0) { base } else { ... f(n-1) ... }` and
//     the recursion argument strictly decreases, with small initial magnitude, so
//     native JIT recursion terminates at shallow depth.
//   * Functions have <= 4 params and calls have <= 4 args (backend ABI limit).
//
// Build (isolated), linking the four sources:
//   msvc.bat /nologo /EHsc /std:c++20 /I include tools/fuzz_cf.cpp \
//     src/ir.cpp src/eval.cpp src/front.cpp src/backend.cpp /Fe:out.exe
//
// Run: out.exe [seed] [modules]

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"

using namespace helix;

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

    // Info about a function already emitted in this module: name + param count.
    struct FnSig {
        std::string name;
        int nparams;
        bool recursive;  // safe to self-call with a strictly smaller first arg
    };

    // A "variable in scope" usable as an i64 expression leaf.
    struct Env {
        std::vector<std::string> vars;  // names of i64 values in scope
    };

    // Pick a random in-scope variable, or a small constant if none.
    std::string leaf(const Env& env) {
        if (env.vars.empty() || chance(30)) {
            // small constant; keep magnitudes modest to avoid huge recursion/loops
            return std::to_string(range(-50, 50));
        }
        return env.vars[range(0, (int)env.vars.size() - 1)];
    }

    // A guaranteed-nonzero constant divisor in [1,4096], never 0 and never -1,
    // so neither div-by-zero nor INT64_MIN/-1 division can occur.
    std::string safe_divisor() { return std::to_string(range(1, 4096)); }

    // A shift amount constant in [0,63].
    std::string shift_amount() { return std::to_string(range(0, 63)); }

    // --- pure expression of bounded depth ---
    // `depth` budgets nesting. `fns` are callable predecessors. `self` is the
    // current function signature (for bounded recursion) or nullptr.
    std::string expr(const Env& env, int depth, const std::vector<FnSig>& fns,
                     const FnSig* self, const std::string& recur_guard_var) {
        if (depth <= 0) return leaf(env);

        int pick = range(0, 11);
        switch (pick) {
            case 0:
            case 1: {  // binary arithmetic / bitwise
                static const char* ops[] = {"+", "-", "*", "&", "|", "^"};
                const char* op = ops[range(0, 5)];
                std::string l = expr(env, depth - 1, fns, self, recur_guard_var);
                std::string r = expr(env, depth - 1, fns, self, recur_guard_var);
                return "(" + l + " " + op + " " + r + ")";
            }
            case 2: {  // division / remainder with a safe constant divisor
                const char* op = chance(50) ? "/" : "%";
                std::string l = expr(env, depth - 1, fns, self, recur_guard_var);
                return "(" + l + " " + op + " " + safe_divisor() + ")";
            }
            case 3: {  // shift by a constant in [0,63]
                const char* op = chance(50) ? "<<" : ">>";
                std::string l = expr(env, depth - 1, fns, self, recur_guard_var);
                return "(" + l + " " + op + " " + shift_amount() + ")";
            }
            case 4: {  // unary negate
                return "(-" + expr(env, depth - 1, fns, self, recur_guard_var) + ")";
            }
            case 5: {  // comparison value used as an i64 0/1.
                // IMPORTANT: a bare comparison node is bool-TYPED. If its value is
                // fed into arithmetic/bitwise/shift, the interpreter truncates the
                // whole expression to 1 bit (trunc to bool width) while the JIT,
                // working in full 64-bit registers, does not -> a *spurious* mismatch
                // that is a generator artifact, not a backend bug. We therefore
                // launder the comparison through an if/else into i64 literals 0/1,
                // yielding an i64-typed value that both engines agree on. This still
                // exercises "comparison mixed into arithmetic".
                std::string c = cond_expr(env, depth - 1, fns, self, recur_guard_var);
                return "if " + c + " { 1 } else { 0 }";
            }
            case 6: {  // if/else expression (Cond region), possibly nested
                std::string c = cond_expr(env, depth - 1, fns, self, recur_guard_var);
                std::string t = expr(env, depth - 1, fns, self, recur_guard_var);
                std::string e = expr(env, depth - 1, fns, self, recur_guard_var);
                return "if " + c + " { " + t + " } else { " + e + " }";
            }
            case 7: {  // let-binding then body
                std::string v = fresh_var();
                std::string val = expr(env, depth - 1, fns, self, recur_guard_var);
                Env e2 = env;
                e2.vars.push_back(v);
                std::string body = expr(e2, depth - 1, fns, self, recur_guard_var);
                return "let " + v + " = " + val + "; " + body;
            }
            case 8: {  // counted loop (single or multi variable)
                return loop_expr(env, depth - 1, fns, self, recur_guard_var);
            }
            case 9: {  // call a previously-defined function
                if (!fns.empty()) {
                    const FnSig& f = fns[range(0, (int)fns.size() - 1)];
                    std::string call = f.name + "(";
                    for (int i = 0; i < f.nparams; i++) {
                        if (i) call += ", ";
                        call += expr(env, depth - 1, fns, self, recur_guard_var);
                    }
                    call += ")";
                    return call;
                }
                return leaf(env);
            }
            case 10: {  // bounded self-recursion (only reachable inside the guarded
                        // else-branch where 0 < recur_guard_var <= kRecurBound).
                if (self && self->recursive && !recur_guard_var.empty()) {
                    // First arg is recur_guard_var - 1: STRICTLY smaller and, because
                    // the entry guard rejects p0 outside [1, kRecurBound], the
                    // recursion depth is bounded by kRecurBound regardless of input.
                    std::string c2 = self->name + "((" + recur_guard_var + " - 1)";
                    for (int i = 1; i < self->nparams; i++) {
                        c2 += ", " + expr(env, depth - 1, fns, self, recur_guard_var);
                    }
                    c2 += ")";
                    return c2;
                }
                return expr(env, depth - 1, fns, self, recur_guard_var);
            }
            default: {  // ternary-ish select via if on a comparison
                std::string c = cond_expr(env, depth - 1, fns, self, recur_guard_var);
                std::string t = leaf(env);
                std::string e = leaf(env);
                return "if " + c + " { " + t + " } else { " + e + " }";
            }
        }
    }

    // A boolean condition expression (the parser accepts any i64; nonzero = true).
    std::string cond_expr(const Env& env, int depth, const std::vector<FnSig>& fns,
                          const FnSig* self, const std::string& recur_guard_var) {
        static const char* cmps[] = {"==", "!=", "<", "<=", ">", ">="};
        const char* op = cmps[range(0, 5)];
        std::string l = expr(env, depth, fns, self, recur_guard_var);
        std::string r = expr(env, depth, fns, self, recur_guard_var);
        return "(" + l + " " + op + " " + r + ")";
    }

    // A counted loop expression. We always include a dedicated counter as the
    // first loop variable, decrement it each iteration, and break when it hits 0.
    // Additional carried variables accumulate arbitrary (defined) values.
    std::string loop_expr(const Env& env, int depth, const std::vector<FnSig>& fns,
                          const FnSig* self, const std::string& recur_guard_var) {
        // counter init in [0, 30] keeps iteration counts bounded for the JIT.
        std::string ctr = fresh_var();
        int ctr_init = range(0, 30);
        int extra = range(0, 2);  // number of additional carried accumulators

        std::vector<std::string> accs;
        std::string header = "loop (" + ctr + " = " + std::to_string(ctr_init);
        Env lenv = env;
        lenv.vars.push_back(ctr);
        for (int i = 0; i < extra; i++) {
            std::string a = fresh_var();
            accs.push_back(a);
            header += ", " + a + " = " + expr(env, depth - 1, fns, self, recur_guard_var);
            lenv.vars.push_back(a);
        }
        header += ") {\n";

        // body: break when counter <= 0, else next with decremented counter and
        // updated accumulators.
        std::string body;
        body += "  if (" + ctr + " <= 0) {\n";
        // break value: function of accumulators / counter
        body += "    break " + expr(lenv, depth - 1, fns, self, recur_guard_var) + "\n";
        body += "  } else {\n";
        body += "    next (" + ctr + " - 1)";
        for (auto& a : accs) {
            body += ", " + expr(lenv, depth - 1, fns, self, recur_guard_var);
        }
        body += "\n  }\n";

        return header + body + "}";
    }

    // Emit one function. Returns its source text and records its signature.
    std::string emit_func(const std::string& name, std::vector<FnSig>& fns) {
        next_tmp = 0;
        int nparams = range(1, 4);
        bool recursive = chance(40);

        Env env;
        std::string src = "fn " + name + "(";
        for (int i = 0; i < nparams; i++) {
            std::string pn = "p" + std::to_string(i);
            if (i) src += ", ";
            src += pn + ": i64";
            env.vars.push_back(pn);
        }
        src += ") -> i64 {\n";

        FnSig self{name, nparams, recursive};
        int depth = range(2, 4);

        if (recursive) {
            // Guarded recursion with a HARD depth bound that is independent of the
            // input magnitude. The base case is taken whenever p0 is outside
            // [1, kRecurBound]; the recursive branch (where 0 < p0 <= kRecurBound)
            // calls self with p0-1, so the native call stack is at most kRecurBound
            // deep no matter how large the original argument is. This keeps both the
            // C++ interpreter and the native JIT from overflowing the stack.
            const int kRecurBound = 12;
            std::string base = expr(env, depth, fns, nullptr, "");
            std::string rec = expr(env, depth, fns, &self, "p0");
            src += "  if ((p0 <= 0) | (p0 > " + std::to_string(kRecurBound) + ")) {\n    " +
                   base + "\n  } else {\n    " + rec + "\n  }\n";
        } else {
            src += "  " + expr(env, depth, fns, nullptr, "") + "\n";
        }
        src += "}\n";

        fns.push_back(self);
        return src;
    }

    // Build a whole module of `nfuncs` functions. Returns source + signatures.
    std::string emit_module(int nfuncs, std::vector<FnSig>& fns) {
        std::string src;
        for (int i = 0; i < nfuncs; i++) {
            std::string name = "f" + std::to_string(i);
            src += emit_func(name, fns);
            src += "\n";
        }
        return src;
    }
};

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------
struct Stats {
    long modules_ok = 0;
    long modules_parse_fail = 0;
    long modules_jit_fail = 0;
    long cases_run = 0;
    long cases_skipped_fuel = 0;
    long mismatches = 0;
};

int main(int argc, char** argv) {
    uint64_t seed = 0xC0FFEEull;
    int modules = 4000;
    if (argc > 1) seed = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) modules = (int)std::strtol(argv[2], nullptr, 10);

    std::printf("fuzz_cf: seed=%llu modules=%d\n", (unsigned long long)seed, modules);

    Stats st;
    std::mt19937_64 argrng(seed ^ 0x9E3779B97F4A7C15ull);

    // Fuel: generous but finite. Loops are bounded (<=30 iters) and recursion is
    // shallow, so well-formed cases finish far under this. out_of_fuel => skip.
    const long kFuel = 20'000'000;

    bool found_mismatch = false;
    std::string repro_src;
    std::string repro_detail;

    for (int m = 0; m < modules && !found_mismatch; m++) {
        Gen g(seed + (uint64_t)m * 0x100000001B3ull);
        std::vector<Gen::FnSig> fns;
        int nfuncs = g.range(1, 4);
        std::string src = g.emit_module(nfuncs, fns);

        World w;
        ParseStatus ps = parse_module(w, src);
        if (!ps.ok) {
            st.modules_parse_fail++;
            continue;  // generator bug if frequent; counted and reported.
        }

        JitModule jit = jit_compile(w);
        if (!jit.ok) {
            st.modules_jit_fail++;
            continue;
        }
        st.modules_ok++;

        // For each function, run many random argument vectors.
        for (const auto& fs : fns) {
            NodeId f = w.find_func(fs.name);
            if (f == NONE) continue;
            if (!jit.has(f)) continue;

            const int kVectors = 40;
            for (int v = 0; v < kVectors && !found_mismatch; v++) {
                std::vector<int64_t> args;
                args.reserve(fs.nparams);
                for (int i = 0; i < fs.nparams; i++) {
                    // Mix of small (drives recursion/loops) and large magnitudes.
                    int mode = (int)(argrng() % 4);
                    int64_t a;
                    if (mode == 0) {
                        a = (int64_t)(argrng() % 41) - 20;  // [-20,20]
                    } else if (mode == 1) {
                        a = (int64_t)(argrng() % 2001) - 1000;  // [-1000,1000]
                    } else if (mode == 2) {
                        a = (int64_t)argrng();  // full 64-bit
                    } else {
                        // near-boundary values
                        static const int64_t bs[] = {0, 1, -1, INT64_MAX, INT64_MIN,
                                                      INT32_MAX, INT32_MIN, 2, -2};
                        a = bs[argrng() % 9];
                    }
                    args.push_back(a);
                }

                EvalResult er = eval_func(w, f, args, kFuel);
                if (er.out_of_fuel || !er.ok) {
                    st.cases_skipped_fuel++;
                    continue;  // skip cases the interpreter couldn't finish
                }

                int64_t jv = jit.call(f, args);
                st.cases_run++;

                if (jv != er.value) {
                    st.mismatches++;
                    found_mismatch = true;
                    repro_src = src;
                    repro_detail = "function " + fs.name + " args=[";
                    for (size_t i = 0; i < args.size(); i++) {
                        if (i) repro_detail += ", ";
                        repro_detail += std::to_string(args[i]);
                    }
                    repro_detail += "]  interp=" + std::to_string(er.value) +
                                    "  jit=" + std::to_string(jv);
                }
            }
        }
    }

    std::printf("\n==== results ====\n");
    std::printf("modules ok           : %ld\n", st.modules_ok);
    std::printf("modules parse-failed : %ld\n", st.modules_parse_fail);
    std::printf("modules jit-failed   : %ld\n", st.modules_jit_fail);
    std::printf("cases run (compared) : %ld\n", st.cases_run);
    std::printf("cases skipped (fuel) : %ld\n", st.cases_skipped_fuel);
    std::printf("mismatches           : %ld\n", st.mismatches);

    if (found_mismatch) {
        std::printf("\n!!!! MISMATCH FOUND !!!!\n");
        std::printf("%s\n\n", repro_detail.c_str());
        std::printf("---- module source ----\n%s\n", repro_src.c_str());
        return 2;
    }

    std::printf("\nALL %ld COMPARED CASES PASSED (jit == interp).\n", st.cases_run);
    return 0;
}
