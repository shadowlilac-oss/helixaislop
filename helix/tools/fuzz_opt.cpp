// fuzz_opt — differential fuzzer for the OPTIMIZER (optimize_module / inlining).
//
// The other fuzzers compare interp/simple/ra on ONE graph, so an optimization that
// changes meaning is invisible (all engines see the same post-optimization graph).
// This fuzzer parses the same random source TWICE — once left unoptimized, once run
// through optimize_module — and checks
//
//     interp(unopt) == interp(opt) == jit_ra(opt) == jit_ra(unopt)
//
// for many argument vectors. The interp(unopt) vs interp(opt) comparison is the new
// signal: it catches a miscompiling middle-end pass (e.g. unsound inlining), which the
// single-graph fuzzers cannot. Programs are PURE multi-function modules (arithmetic,
// if-expressions, counted loops, calls between functions, and bounded self-recursion) —
// exactly the shapes inlining transforms. Watchdog-guarded against a hung JIT.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "fuzz_watchdog.hpp"
#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "helix/opt.hpp"

using namespace helix;

struct Gen {
    std::mt19937_64 rng;
    int tmp = 0;
    explicit Gen(uint64_t s) : rng(s) {}
    int range(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
    bool chance(int p) { return range(1, 100) <= p; }
    std::string fresh() { return "t" + std::to_string(tmp++); }
    std::string divisor() { return std::to_string(range(1, 4096)); }

    struct Sig { std::string name; int nparams; bool recursive; };

    std::string leaf(const std::vector<std::string>& vars) {
        if (vars.empty() || chance(35)) return std::to_string(range(-40, 40));
        return vars[range(0, (int)vars.size() - 1)];
    }
    // pure expression; may call earlier functions and (guarded) recurse.
    std::string expr(std::vector<std::string> vars, int depth, const std::vector<Sig>& fns,
                     const Sig* self, const std::string& guard) {
        if (depth <= 0) return leaf(vars);
        switch (range(0, 9)) {
            case 0: case 1: {
                static const char* ops[] = {"+", "-", "*", "&", "|", "^"};
                return "(" + expr(vars, depth - 1, fns, self, guard) + " " + ops[range(0, 5)] +
                       " " + expr(vars, depth - 1, fns, self, guard) + ")";
            }
            case 2: return "(" + expr(vars, depth - 1, fns, self, guard) +
                           (chance(50) ? " / " : " % ") + divisor() + ")";
            case 3: return "(" + expr(vars, depth - 1, fns, self, guard) +
                           (chance(50) ? " << " : " >> ") + std::to_string(range(0, 63)) + ")";
            case 4: {  // if-expression
                std::string c = "(" + expr(vars, depth - 1, fns, self, guard) +
                                (chance(50) ? " < " : " == ") + expr(vars, depth - 1, fns, self, guard) + ")";
                return "if " + c + " { " + expr(vars, depth - 1, fns, self, guard) + " } else { " +
                       expr(vars, depth - 1, fns, self, guard) + " }";
            }
            case 5: {  // let
                std::string v = fresh();
                std::string val = expr(vars, depth - 1, fns, self, guard);
                std::vector<std::string> v2 = vars; v2.push_back(v);
                return "let " + v + " = " + val + "; " + expr(v2, depth - 1, fns, self, guard);
            }
            case 6: {  // counted loop
                std::string ctr = fresh();
                std::vector<std::string> v2 = vars; v2.push_back(ctr);
                return "loop (" + ctr + " = " + std::to_string(range(0, 20)) + ") {\n" +
                       "  if (" + ctr + " <= 0) { break " + expr(v2, depth - 1, fns, self, guard) +
                       " } else { next (" + ctr + " - 1) }\n}";
            }
            case 7: {  // call an earlier function
                if (!fns.empty()) {
                    const Sig& f = fns[range(0, (int)fns.size() - 1)];
                    std::string s = f.name + "(";
                    for (int i = 0; i < f.nparams; i++) s += (i ? ", " : "") + expr(vars, depth - 1, fns, self, guard);
                    return s + ")";
                }
                return leaf(vars);
            }
            case 8:  // bounded self-recursion
                if (self && self->recursive && !guard.empty()) {
                    std::string s = self->name + "((" + guard + " - 1)";
                    for (int i = 1; i < self->nparams; i++) s += ", " + expr(vars, depth - 1, fns, self, guard);
                    return s + ")";
                }
                return leaf(vars);
            default: return leaf(vars);
        }
    }
    std::string emit_func(const std::string& name, std::vector<Sig>& fns) {
        tmp = 0;
        int np = range(1, 4);
        bool rec = chance(45);
        std::vector<std::string> vars;
        std::string src = "fn " + name + "(";
        for (int i = 0; i < np; i++) { std::string p = "p" + std::to_string(i); src += (i ? ", " : "") + p + ": i64"; vars.push_back(p); }
        src += ") -> i64 {\n";
        Sig self{name, np, rec};
        int depth = range(2, 4);
        if (rec) {
            std::string base = expr(vars, depth, fns, nullptr, "");
            std::string r = expr(vars, depth, fns, &self, "p0");
            src += "  if ((p0 <= 0) | (p0 > 12)) {\n    " + base + "\n  } else {\n    " + r + "\n  }\n";
        } else {
            src += "  " + expr(vars, depth, fns, nullptr, "") + "\n";
        }
        src += "}\n";
        fns.push_back(self);
        return src;
    }
    std::string emit_module(std::vector<Sig>& fns) {
        std::string src;
        int n = range(2, 4);
        for (int i = 0; i < n; i++) src += emit_func("f" + std::to_string(i), fns) + "\n";
        return src;
    }
};

int main(int argc, char** argv) {
    uint64_t seed = 0x0917150FFull;
    int modules = 20000;
    if (argc > 1) seed = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) modules = (int)std::strtol(argv[2], nullptr, 10);
    std::printf("fuzz_opt: seed=%llu modules=%d\n", (unsigned long long)seed, modules);

    std::mt19937_64 argrng(seed ^ 0x9E3779B97F4A7C15ull);
    const long kFuel = 2'000'000;
    FuzzWatchdog wd; wd.start();

    long cases = 0, skipped = 0, mismatches = 0, parsefail = 0;
    std::string repro_src, repro_detail; size_t repro_len = SIZE_MAX;

    for (int m = 0; m < modules; m++) {
        if (m && m % 2000 == 0) { std::printf("  ... %d/%d, %ld cases, %ld mismatches\n", m, modules, cases, mismatches); std::fflush(stdout); }
        Gen g(seed + (uint64_t)m * 0x100000001B3ull);
        std::vector<Gen::Sig> fns;
        std::string src = g.emit_module(fns);

        World wu, wo;  // unoptimized and optimized copies of the SAME source
        if (!parse_module(wu, src).ok || !parse_module(wo, src).ok) { parsefail++; continue; }
        optimize_module(wo, /*inline_depth=*/2);

        JitModule jo = jit_compile_ra(wo), ju = jit_compile_ra(wu);
        if (!jo.ok || !ju.ok) continue;

        for (const auto& fs : fns) {
            NodeId fu = wu.find_func(fs.name), fo = wo.find_func(fs.name);
            if (fu == NONE || fo == NONE || !jo.has(fo) || !ju.has(fu)) continue;
            for (int v = 0; v < 25; v++) {
                std::vector<int64_t> args(fs.nparams);
                for (auto& a : args) {
                    int mode = (int)(argrng() % 4);
                    if (mode == 0) a = (int64_t)(argrng() % 41) - 20;
                    else if (mode == 1) a = (int64_t)(argrng() % 2001) - 1000;
                    else if (mode == 2) a = (int64_t)argrng();
                    else { static const int64_t bs[] = {0,1,-1,INT64_MAX,INT64_MIN,INT32_MAX,INT32_MIN}; a = bs[argrng() % 7]; }
                }
                EvalResult eu = eval_func(wu, fu, args, kFuel);
                if (eu.out_of_fuel || !eu.ok) { skipped++; continue; }
                EvalResult eo = eval_func(wo, fo, args, kFuel);
                if (eo.out_of_fuel || !eo.ok) { skipped++; continue; }

                wd.arm(src, "fn " + fs.name, 3000);
                int64_t jov = jo.call(fo, args), juv = ju.call(fu, args);
                wd.disarm();
                cases++;

                if (eo.value != eu.value || jov != eu.value || juv != eu.value) {
                    mismatches++;
                    if (src.size() < repro_len) {
                        repro_len = src.size(); repro_src = src;
                        repro_detail = "fn " + fs.name + "  interp_unopt=" + std::to_string(eu.value) +
                                       " interp_opt=" + std::to_string(eo.value) + " jit_opt=" + std::to_string(jov) +
                                       " jit_unopt=" + std::to_string(juv) + "  args=[";
                        for (size_t i = 0; i < args.size(); i++) repro_detail += (i ? "," : "") + std::to_string((long long)args[i]);
                        repro_detail += "]";
                    }
                }
            }
        }
    }

    std::printf("\n==== results ====\nparse-failed : %ld\ncases        : %ld\nskipped      : %ld\nmismatches   : %ld\n",
                parsefail, cases, skipped, mismatches);
    if (mismatches) {
        std::printf("\n!!!! OPT MISMATCH (smallest of %ld) !!!!\n%s\n---- source ----\n%s\n", mismatches, repro_detail.c_str(), repro_src.c_str());
        return 2;
    }
    std::printf("\nALL %ld CASES PASSED (interp_unopt == interp_opt == jit_opt == jit_unopt).\n", cases);
    return 0;
}
