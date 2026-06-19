// repro_cf — run a single Helix module through interp / simple-jit / ra-jit for a
// named function and argument vector, to minimize a fuzz_cf mismatch.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "helix/print.hpp"

using namespace helix;

static void run(const char* label, const std::string& src, const char* fn,
                std::vector<int64_t> args) {
    std::printf("==== %s ==== (fn %s)\n", label, fn);
    World w;
    ParseStatus ps = parse_module(w, src);
    if (!ps.ok) { std::printf("PARSE FAIL: %s\n\n", ps.msg.c_str()); return; }
    JitModule simple = jit_compile(w);
    JitModule ra = jit_compile_ra(w);
    NodeId f = w.find_func(fn);
    if (f == NONE) { std::printf("no func %s\n\n", fn); return; }
    EvalResult er = eval_func(w, f, args, 200'000'000);
    int64_t sv = simple.ok ? simple.call(f, args) : 0;
    int64_t rv = ra.ok ? ra.call(f, args) : 0;
    std::printf("interp=%lld  simple=%lld  ra=%lld  -> %s\n\n",
                (long long)er.value, (long long)sv, (long long)rv,
                (er.ok && sv == er.value && rv == er.value) ? "AGREE" : "MISMATCH");
}

int main() {
    const char* mod = R"(
fn g(p2: i64) -> i64 {
  loop (t0 = 22) {
    if (t0 <= 0) {
      break if ((t0 | p2) < t0) { 1 } else { 0 }
    } else {
      next (t0 - 1)
    }
  }
}
)";
    run("loop break_val if-expr, p2<0", mod, "g", {-7});
    run("loop break_val if-expr, p2>=0", mod, "g", {5});

    // Even smaller: break value is just an if-expr on the counter at break (t0==0).
    const char* mod2 = R"(
fn g(p2: i64) -> i64 {
  loop (t0 = 3) {
    if (t0 <= 0) { break if (p2 < t0) { 100 } else { 200 } }
    else { next (t0 - 1) }
  }
}
)";
    run("min break if(p2<t0), p2=-7 -> 100", mod2, "g", {-7});
    run("min break if(p2<t0), p2=5  -> 200", mod2, "g", {5});

    // Control: break a plain expression of the counter (no inner if).
    const char* mod3 = R"(
fn g(p2: i64) -> i64 {
  loop (t0 = 3) {
    if (t0 <= 0) { break (t0 + p2) } else { next (t0 - 1) }
  }
}
)";
    run("control break (t0+p2), p2=-7 -> -7", mod3, "g", {-7});

    // Localize: inner-if predicate is a BINOP of the counter and param (t0==0 at break).
    auto mk = [](const char* pred) {
        return std::string("fn g(p2: i64) -> i64 {\n  loop (t0 = 3) {\n"
                           "    if (t0 <= 0) { break if (") + pred +
               ") { 100 } else { 200 } }\n    else { next (t0 - 1) }\n  }\n}\n";
    };
    run("pred (t0|p2)<t0  p2=-7 -> 100", mk("(t0 | p2) < t0"), "g", {-7});
    run("pred (p2|t0)<t0  p2=-7 -> 100", mk("(p2 | t0) < t0"), "g", {-7});
    run("pred (t0+p2)<t0  p2=-7 -> 100", mk("(t0 + p2) < t0"), "g", {-7});
    run("pred (t0|p2)<5   p2=-7 -> 100", mk("(t0 | p2) < 5"), "g", {-7});
    run("pred (t0|p2)<0   p2=-7 -> 100", mk("(t0 | p2) < 0"), "g", {-7});
    run("break (t0|p2) raw p2=-7 -> -7", std::string(
        "fn g(p2: i64) -> i64 {\n  loop (t0 = 3) {\n"
        "    if (t0 <= 0) { break (t0 | p2) } else { next (t0 - 1) }\n  }\n}\n"), "g", {-7});

    // The actual failing shape: inner `if P {1} else {0}` (folds to the bare cmp),
    // at small init to rule out the counter magnitude.
    const char* modF = R"(
fn g(p2: i64) -> i64 {
  loop (t0 = 3) {
    if (t0 <= 0) { break if ((t0 | p2) < t0) { 1 } else { 0 } }
    else { next (t0 - 1) }
  }
}
)";
    run("FAIL shape {1}/{0} init=3 p2=-7 -> 1", modF, "g", {-7});
    {
        World w; parse_module(w, modF);
        std::printf("---- IR of g ----\n%s\n", print_func(w, w.find_func("g")).c_str());
    }
    return 0;
}
