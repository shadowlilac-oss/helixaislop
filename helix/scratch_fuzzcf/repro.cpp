#include <cstdio>
#include <string>
#include <vector>
#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "helix/print.hpp"
using namespace helix;

static int test(const char* label, const std::string& src, const char* fn,
                std::vector<int64_t> args) {
    World w;
    ParseStatus ps = parse_module(w, src);
    if (!ps.ok) { std::printf("[%s] PARSE FAIL line %d: %s\n", label, ps.line, ps.msg.c_str()); return 1; }
    JitModule jit = jit_compile(w);
    if (!jit.ok) { std::printf("[%s] JIT FAIL: %s\n", label, jit.err.c_str()); return 1; }
    NodeId f = w.find_func(fn);
    if (f == NONE) { std::printf("[%s] no func %s\n", label, fn); return 1; }
    EvalResult er = eval_func(w, f, args, 50'000'000);
    int64_t jv = jit.call(f, args);
    std::printf("[%s] interp=%lld jit=%lld  ok=%d oof=%d  %s\n", label,
                (long long)er.value, (long long)jv, er.ok, er.out_of_fuel,
                (er.ok && er.value == jv) ? "MATCH" : "*** MISMATCH ***");
    return (er.ok && er.value == jv) ? 0 : 2;
}

int main() {
    // Original full module from the fuzzer.
    std::string full =
        "fn f0(p0: i64, p1: i64, p2: i64) -> i64 {\n  p1\n}\n"
        "fn f1(p0: i64, p1: i64, p2: i64, p3: i64) -> i64 {\n"
        "  f0(let t0 = ((p0 + -33) == (-26 + p2)); (let t1 = 4; -35 != if (p0 == p0) { p3 } else { t0 }), (((p0 <= p1) >> 20) ^ if ((-27) == f0(-13, -15, p0)) { (27 & -50) } else { if (p3 > p3) { p0 } else { p1 } }), ((-if (p3 <= p2) { p2 } else { 34 }) / 2021))\n}\n";
    std::vector<int64_t> a = {2147483647, -6348381632447941519LL, -3, -2};
    test("full-f1", full, "f1", a);

    // f0 simply returns p1. f1 returns f0(A, B, C) = B. So f1 == B.
    // B = ((p0<=p1)>>20) ^ ELSE  where ELSE (since f0(-13,-15,p0)=-15 != -27) =
    //   if (p3 > p3) {p0} else {p1} = p1.
    // So B = ((p0<=p1)>>20) ^ p1. With p0=2147483647, p1 huge negative: p0<=p1 false=0,
    // 0>>20=0, 0^p1 = p1.  Expected = p1 = -6348381632447941519.

    // Minimal: does the FIRST argument expression to f0 (which f0 ignores) matter?
    std::string m1 =
        "fn f0(p0: i64, p1: i64, p2: i64) -> i64 {\n  p1\n}\n"
        "fn g(p0: i64, p1: i64, p2: i64, p3: i64) -> i64 {\n"
        "  f0(0, (((p0 <= p1) >> 20) ^ p1), 0)\n}\n";
    test("min-noarg1", m1, "g", a);

    // Reintroduce the complicated, side-effect-free first argument:
    std::string m2 =
        "fn f0(p0: i64, p1: i64, p2: i64) -> i64 {\n  p1\n}\n"
        "fn g(p0: i64, p1: i64, p2: i64, p3: i64) -> i64 {\n"
        "  f0((let t1 = 4; -35 != if (p0 == p0) { p3 } else { 0 }), (((p0 <= p1) >> 20) ^ p1), 0)\n}\n";
    test("min-arg1complex", m2, "g", a);

    // Strip to: f0(COMPLEX, p1, 0) -- does an arg-expr before p1 corrupt p1?
    std::string m3 =
        "fn f0(p0: i64, p1: i64, p2: i64) -> i64 {\n  p1\n}\n"
        "fn g(p0: i64, p1: i64, p2: i64, p3: i64) -> i64 {\n"
        "  f0((-35 != if (p0 == p0) { p3 } else { 0 }), p1, 0)\n}\n";
    test("min-f0complex-p1", m3, "g", a);

    // Even simpler: arg0 contains an if; arg1 is p1.
    std::string m4 =
        "fn f0(p0: i64, p1: i64, p2: i64) -> i64 {\n  p1\n}\n"
        "fn g(p0: i64, p1: i64) -> i64 {\n"
        "  f0(if (p0 == p0) { p0 } else { 0 }, p1, 0)\n}\n";
    test("min-if-then-p1", m4, "g", {7, 123456789});

    // The if alone as a call arg, returning param:
    std::string m5 =
        "fn h(p0: i64, p1: i64) -> i64 {\n"
        "  if (p0 == p0) { p1 } else { 0 }\n}\n";
    test("if-returns-p1", m5, "h", {7, 999});

    return 0;
}
