// Imperative frontend: mutable variables, assignment, `while`, statement `if`.
// Each program is checked against the interpreter via BOTH backends.
#include <climits>
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
    int64_t s = c.simple.call(f, a), r = c.ra.call(f, a);
    CHECK(ir.ok);
    CHECK_EQ(s, ir.value);
    CHECK_EQ(r, ir.value);
    return ir.value;
}
}  // namespace

TEST("imperative: while-loop sum and factorial") {
    C c;
    load(c,
        "fn sum(n: int) -> int { var s = 0; var i = 0; while i < n { s = s + i; i = i + 1; } return s; }\n"
        "fn fact(n: int) -> int { var acc = 1; var i = 1; while i <= n { acc = acc * i; i = i + 1; } return acc; }\n");
    for (int n = 0; n <= 50; n++) CHECK_EQ(diff(c, "sum", {n}), (int64_t)n * (n - 1) / 2);
    int64_t f[] = {1, 1, 2, 6, 24, 120, 720, 5040, 40320, 362880, 3628800};
    for (int n = 0; n <= 10; n++) CHECK_EQ(diff(c, "fact", {n}), f[n]);
}

TEST("imperative: gcd with simultaneous update (parallel carried vars)") {
    C c;
    load(c,
        "fn gcd(a: int, b: int) -> int {\n"
        "  var x = a; var y = b;\n"
        "  while y != 0 { var t = x % y; x = y; y = t; }\n"
        "  return x;\n"
        "}\n");
    CHECK_EQ(diff(c, "gcd", {1071, 462}), 21);
    CHECK_EQ(diff(c, "gcd", {48, 36}), 12);
    CHECK_EQ(diff(c, "gcd", {270, 192}), 6);
    CHECK_EQ(diff(c, "gcd", {17, 5}), 1);
}

TEST("imperative: statement-if mutates a variable conditionally") {
    C c;
    load(c,
        "fn absdiff(a: int, b: int) -> int { var d = a - b; if d < 0 { d = 0 - d; } return d; }\n"
        "fn clamp(x: int) -> int { var r = x; if r < 0 { r = 0; } if r > 100 { r = 100; } return r; }\n");
    for (int64_t a = -10; a <= 10; a++)
        for (int64_t b = -10; b <= 10; b++)
            CHECK_EQ(diff(c, "absdiff", {a, b}), a > b ? a - b : b - a);
    for (int64_t x = -50; x <= 150; x += 10)
        CHECK_EQ(diff(c, "clamp", {x}), x < 0 ? 0 : (x > 100 ? 100 : x));
}

TEST("imperative: nested if inside while (collatz)") {
    C c;
    load(c,
        "fn collatz(n: int) -> int {\n"
        "  var x = n; var steps = 0;\n"
        "  while x != 1 {\n"
        "    if x % 2 == 0 { x = x / 2; } else { x = 3 * x + 1; }\n"
        "    steps = steps + 1;\n"
        "  }\n"
        "  return steps;\n"
        "}\n");
    CHECK_EQ(diff(c, "collatz", {1}), 0);
    CHECK_EQ(diff(c, "collatz", {6}), 8);
    CHECK_EQ(diff(c, "collatz", {27}), 111);
}

TEST("imperative: while over an array (mutable accumulator + loads)") {
    C c;
    load(c,
        "fn asum(a: ptr, n: int) -> int { var s = 0; var i = 0; while i < n { s = s + a[i]; i = i + 1; } return s; }\n"
        "fn amax(a: ptr, n: int) -> int {\n"
        "  var m = a[0]; var i = 1;\n"
        "  while i < n { if a[i] > m { m = a[i]; } i = i + 1; }\n"
        "  return m;\n"
        "}\n");
    int64_t arr[] = {5, 3, 9, 1, 7, 2, 8, 4, 6, 0, -3, 11};
    int64_t addr = (int64_t)(uintptr_t)arr;
    for (int n = 1; n <= 12; n++) {
        int64_t rs = 0, rm = arr[0];
        for (int i = 0; i < n; i++) { rs += arr[i]; if (arr[i] > rm) rm = arr[i]; }
        CHECK_EQ(diff(c, "asum", {addr, n}), rs);
        CHECK_EQ(diff(c, "amax", {addr, n}), rm);
    }
}

TEST("imperative: while condition containing braces parses correctly (regression BUG4)") {
    C c;
    load(c, "fn f(n: int) -> int { var i = 0; while if i < n { 1 } else { 0 } { i = i + 1; } return i; }\n");
    for (int n = 0; n <= 10; n++) CHECK_EQ(diff(c, "f", {n}), n);
}

TEST("imperative: body-local var does not hijack an outer var (regression BUG5)") {
    C c;
    load(c, "fn f(n: int) -> int { var x = 100; var i = 0; while i < n { var x = i; x = x + 1; i = i + 1; } return x; }\n");
    for (int n = 0; n <= 10; n++) CHECK_EQ(diff(c, "f", {n}), 100);  // outer x stays 100
}

TEST("imperative: nested while with accumulator used in both if branches (regression)") {
    // Inner-loop result feeds BOTH branches of a following if -> must be hoisted to
    // dominate the branch (else the backends read a cross-branch garbage register).
    C c;
    load(c,
        "fn f(n: int) -> int {\n"
        "  var acc = n; var i = 1;\n"
        "  while i < n {\n"
        "    var j = 1;\n"
        "    while j < n - 1 { acc = acc + 1; j = j + 1; }\n"
        "    if i <= 0 - 14 { acc = acc + 7; }\n"   // never taken; acc used in both branches
        "    i = i + 1;\n"
        "  }\n"
        "  return acc;\n"
        "}\n");
    for (int n = 0; n <= 25; n++) {
        int64_t expect = (int64_t)n + (n >= 2 ? (int64_t)(n - 1) * (n - 2) : 0);
        CHECK_EQ(diff(c, "f", {n}), expect);
    }
    // a genuinely-taken branch variant too
    C c2;
    load(c2,
        "fn g(n: int) -> int {\n"
        "  var acc = 0; var i = 0;\n"
        "  while i < n {\n"
        "    var j = 0; var s = 0;\n"
        "    while j < i { s = s + 1; j = j + 1; }\n"
        "    if s > 2 { acc = acc + s; } else { acc = acc + 1; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return acc;\n"
        "}\n");
    for (int n = 0; n <= 15; n++) {
        int64_t acc = 0;
        for (int i = 0; i < n; i++) { int s = i; acc += (s > 2 ? s : 1); }
        CHECK_EQ(diff(c2, "g", {n}), acc);
    }
}
