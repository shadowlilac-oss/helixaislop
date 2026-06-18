#include <cstdio>
#include <string>
#include <vector>
#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "helix/print.hpp"
using namespace helix;

static void test(const char* label, const std::string& src, const char* fn,
                 std::vector<int64_t> args, bool show=false) {
    World w;
    ParseStatus ps = parse_module(w, src);
    if (!ps.ok) { std::printf("[%s] PARSE FAIL line %d: %s\n", label, ps.line, ps.msg.c_str()); return; }
    JitModule jit = jit_compile(w);
    if (!jit.ok) { std::printf("[%s] JIT FAIL: %s\n", label, jit.err.c_str()); return; }
    NodeId f = w.find_func(fn);
    EvalResult er = eval_func(w, f, args, 50'000'000);
    int64_t jv = jit.call(f, args);
    std::printf("[%s] interp=%lld jit=%lld  %s\n", label,
                (long long)er.value, (long long)jv,
                (er.ok && er.value == jv) ? "MATCH" : "*** MISMATCH ***");
    if (show) std::printf("%s\n", print_func(w, f).c_str());
}

int main() {
    std::vector<int64_t> a = {2147483647, -6348381632447941519LL};

    // Bare: a comparison (bool) XORed with an i64 param. Hypothesis: result is
    // bool-typed, interp truncates to 1 bit, jit does full 64-bit -> mismatch.
    test("xor-bool-i64",
        "fn g(p0: i64, p1: i64) -> i64 { ((p0 <= p1) ^ p1) }\n", "g", a, true);

    // Same with the >>20 in the middle (original shape).
    test("shr-xor",
        "fn g(p0: i64, p1: i64) -> i64 { (((p0 <= p1) >> 20) ^ p1) }\n", "g", a);

    // Control: p1 ^ (comparison) -- left operand is i64 param, so result i64.
    test("xor-i64-bool",
        "fn g(p0: i64, p1: i64) -> i64 { (p1 ^ (p0 <= p1)) }\n", "g", a);

    // Control: plain i64 ^ i64, no bool involved.
    test("xor-i64-i64",
        "fn g(p0: i64, p1: i64) -> i64 { (p0 ^ p1) }\n", "g", a);

    // Add instead of xor: (bool + i64), result bool-typed -> truncated by interp.
    test("add-bool-i64",
        "fn g(p0: i64, p1: i64) -> i64 { ((p0 <= p1) + p1) }\n", "g", {1, 10});

    return 0;
}
