// Backend / JIT tests: compile to real x86-64 and execute, validated against
// the interpreter (differential testing) and against known values.
#include <string>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

namespace {

struct Compiled {
    World w;
    JitModule jit;
};

// parse + jit; assert success
static void load(Compiled& c, const char* src) {
    auto st = parse_module(c.w, src);
    if (!st.ok) std::printf("    parse error (line %d): %s\n", st.line, st.msg.c_str());
    CHECK(st.ok);
    c.jit = jit_compile(c.w);
    if (!c.jit.ok) std::printf("    jit error: %s\n", c.jit.err.c_str());
    CHECK(c.jit.ok);
}

// differential: jit result == interp result; return the (agreed) value
static int64_t diff(Compiled& c, const char* fn, std::vector<int64_t> args) {
    NodeId f = c.w.find_func(fn);
    CHECK(f != NONE);
    auto ir = eval_func(c.w, f, args);
    CHECK(ir.ok);
    int64_t jv = c.jit.call(f, args);
    CHECK_EQ(jv, ir.value);
    return jv;
}

}  // namespace

TEST("jit: arithmetic expression matches and is correct") {
    Compiled c;
    load(c, "fn f(a: int, b: int) -> int { (a + b) * (a - b) + a * 2 }\n");
    for (int64_t a = -5; a <= 5; a++)
        for (int64_t b = -5; b <= 5; b++) {
            int64_t expect = (a + b) * (a - b) + a * 2;
            CHECK_EQ(diff(c, "f", {a, b}), expect);
        }
}

TEST("jit: recursive fib executes on real x64") {
    Compiled c;
    load(c, "fn fib(n: int) -> int { if n < 2 { n } else { fib(n-1) + fib(n-2) } }\n");
    int64_t fibs[] = {0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55};
    for (int n = 0; n <= 10; n++) CHECK_EQ(diff(c, "fib", {n}), fibs[n]);
    CHECK_EQ(diff(c, "fib", {25}), 75025);
}

TEST("jit: loop sum and factorial") {
    Compiled c;
    load(c,
         "fn sum(n: int) -> int { loop (acc=0, i=0) { if i >= n { break acc } else { next acc+i, i+1 } } }\n"
         "fn fact(n: int) -> int { loop (acc=1, i=1) { if i > n { break acc } else { next acc*i, i+1 } } }\n");
    for (int n = 0; n <= 20; n++) CHECK_EQ(diff(c, "sum", {n}), (int64_t)n * (n - 1) / 2);
    int64_t f[] = {1, 1, 2, 6, 24, 120, 720, 5040, 40320, 362880, 3628800};
    for (int n = 0; n <= 10; n++) CHECK_EQ(diff(c, "fact", {n}), f[n]);
}

TEST("jit: gcd (parallel loop-carried update / swap correctness)") {
    Compiled c;
    load(c, "fn gcd(a: int, b: int) -> int { loop (x=a, y=b) { if y == 0 { break x } else { next y, x % y } } }\n");
    CHECK_EQ(diff(c, "gcd", {48, 36}), 12);
    CHECK_EQ(diff(c, "gcd", {1071, 462}), 21);
    CHECK_EQ(diff(c, "gcd", {270, 192}), 6);
    CHECK_EQ(diff(c, "gcd", {17, 13}), 1);
}

TEST("jit: mutual recursion across functions") {
    Compiled c;
    load(c,
         "fn is_even(n: int) -> bool { if n == 0 { true } else { is_odd(n-1) } }\n"
         "fn is_odd(n: int) -> bool { if n == 0 { false } else { is_even(n-1) } }\n");
    for (int n = 0; n <= 12; n++) {
        CHECK_EQ(diff(c, "is_even", {n}), (n % 2 == 0) ? 1 : 0);
        CHECK_EQ(diff(c, "is_odd", {n}), (n % 2 == 1) ? 1 : 0);
    }
}

TEST("jit: ackermann (deeply nested recursion + conditionals)") {
    Compiled c;
    load(c,
         "fn ack(m: int, n: int) -> int {\n"
         "  if m == 0 { n + 1 } else { if n == 0 { ack(m-1, 1) } else { ack(m-1, ack(m, n-1)) } }\n"
         "}\n");
    CHECK_EQ(diff(c, "ack", {2, 3}), 9);
    CHECK_EQ(diff(c, "ack", {3, 3}), 61);
    CHECK_EQ(diff(c, "ack", {3, 4}), 125);
}

TEST("jit: power with loop-invariant value used in body") {
    Compiled c;
    load(c, "fn pow(base: int, e: int) -> int { loop (acc=1, k=e) { if k == 0 { break acc } else { next acc*base, k-1 } } }\n");
    CHECK_EQ(diff(c, "pow", {2, 10}), 1024);
    CHECK_EQ(diff(c, "pow", {3, 5}), 243);
    CHECK_EQ(diff(c, "pow", {5, 0}), 1);
    CHECK_EQ(diff(c, "pow", {7, 3}), 343);
}

TEST("jit: collatz steps (nested if inside loop)") {
    Compiled c;
    load(c,
         "fn collatz(n: int) -> int {\n"
         "  loop (x=n, steps=0) {\n"
         "    if x == 1 { break steps } else { if x % 2 == 0 { next x/2, steps+1 } else { next 3*x+1, steps+1 } }\n"
         "  }\n"
         "}\n");
    CHECK_EQ(diff(c, "collatz", {1}), 0);
    CHECK_EQ(diff(c, "collatz", {6}), 8);
    CHECK_EQ(diff(c, "collatz", {27}), 111);
}

TEST("jit: comptime-folded constant compiles to a constant return") {
    Compiled c;
    load(c,
         "comptime fn cfib(n: int) -> int { if n < 2 { n } else { cfib(n-1) + cfib(n-2) } }\n"
         "fn main() -> int { cfib(20) }\n");
    CHECK_EQ(diff(c, "main", {}), 6765);
}

TEST("jit: signed division and remainder semantics") {
    Compiled c;
    load(c, "fn dm(a: int, b: int) -> int { (a / b) * 100 + (a % b) }\n");
    for (int64_t a = -20; a <= 20; a++)
        for (int64_t b = -7; b <= 7; b++) {
            if (b == 0) continue;
            CHECK_EQ(diff(c, "dm", {a, b}), (a / b) * 100 + (a % b));
        }
}
