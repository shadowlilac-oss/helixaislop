// Win64 ABI: functions and calls with more than 4 arguments (5th+ passed on the stack).
// Validated interp == simple == ra. Covers parameter reads from the incoming stack area,
// outgoing stack args at a call site, and recursion that threads >4 args.
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

namespace {
struct C { World w; JitModule simple, ra; };
static void load(C& c, const char* src) {
    auto st = parse_module(c.w, src);
    if (!st.ok) std::printf("    parse error (line %d): %s\n", st.line, st.msg.c_str());
    CHECK(st.ok);
    c.simple = jit_compile(c.w);
    c.ra = jit_compile_ra(c.w);
    CHECK(c.simple.ok);
    CHECK(c.ra.ok);
}
static int64_t diff(C& c, const char* fn, std::vector<int64_t> a) {
    NodeId f = c.w.find_func(fn);
    auto ir = eval_func(c.w, f, a);
    CHECK(ir.ok);
    CHECK_EQ(c.simple.call(f, a), ir.value);
    CHECK_EQ(c.ra.call(f, a), ir.value);
    return ir.value;
}
}  // namespace

TEST("abi: 6- and 8-parameter functions read stack args correctly") {
    C c;
    load(c,
        "fn add6(a: int, b: int, c: int, d: int, e: int, f: int) -> int { a + b + c + d + e + f }\n"
        "fn add8(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int) -> int {\n"
        "  a + b + c + d + e + f + g + h\n}\n");
    CHECK_EQ(diff(c, "add6", {1, 2, 3, 4, 5, 6}), 21);
    CHECK_EQ(diff(c, "add6", {10, -20, 30, -40, 50, -60}), -30);
    CHECK_EQ(diff(c, "add8", {1, 2, 3, 4, 5, 6, 7, 8}), 36);
    CHECK_EQ(diff(c, "add8", {-1, -2, -3, -4, -5, -6, -7, -8}), -36);
    // order sensitivity: a non-commutative combination so a wrong arg slot is caught
    load(c, "fn mix(a: int, b: int, c: int, d: int, e: int, f: int) -> int { ((a - b) * c) - ((d - e) * f) }\n");
    CHECK_EQ(diff(c, "mix", {9, 2, 3, 8, 1, 4}), (9 - 2) * 3 - (8 - 1) * 4);
}

TEST("abi: a call site passes 6 outgoing args (4 reg + 2 stack)") {
    C c;
    load(c,
        "fn add6(a: int, b: int, c: int, d: int, e: int, f: int) -> int { a + b + c + d + e + f }\n"
        "fn caller(x: int) -> int { add6(x, x + 1, x + 2, x + 3, x + 4, x + 5) }\n");
    for (int64_t x = -5; x <= 5; x++) CHECK_EQ(diff(c, "caller", {x}), 6 * x + 15);
}

TEST("abi: recursion threading 5 args (stack arg in a loop of calls)") {
    C c;
    load(c,
        "fn r(n: int, a: int, b: int, c: int, d: int) -> int {\n"
        "  if n <= 0 { a + b + c + d } else { r(n - 1, a + 1, b + 2, c + 3, d + 4) }\n"
        "}\n");
    // after n steps: (a+n) + (b+2n) + (c+3n) + (d+4n) = a+b+c+d + 10n
    for (int64_t n = 0; n <= 12; n++) CHECK_EQ(diff(c, "r", {n, 0, 0, 0, 0}), 10 * n);
    CHECK_EQ(diff(c, "r", {7, 1, 2, 3, 4}), 1 + 2 + 3 + 4 + 70);
}

TEST("abi: 8-arg call from a 4-arg caller, mixed with arithmetic") {
    C c;
    load(c,
        "fn add8(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int) -> int {\n"
        "  a + b + c + d + e + f + g + h\n}\n"
        "fn caller(p: int, q: int, r: int, s: int) -> int {\n"
        "  add8(p, q, r, s, p * 2, q * 2, r * 2, s * 2)\n}\n");
    CHECK_EQ(diff(c, "caller", {1, 2, 3, 4}), (1 + 2 + 3 + 4) * 3);
    CHECK_EQ(diff(c, "caller", {-3, 5, -7, 11}), (-3 + 5 - 7 + 11) * 3);
}
