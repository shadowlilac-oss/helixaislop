// Frontend tests: parse source DIRECTLY to graph, then evaluate.
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

static World parse_ok(const char* src) {
    World w;
    auto st = parse_module(w, src);
    if (!st.ok) std::printf("    parse error (line %d): %s\n", st.line, st.msg.c_str());
    CHECK(st.ok);
    return w;
}

static int64_t call(World& w, const char* fn, std::vector<int64_t> args) {
    NodeId f = w.find_func(fn);
    auto r = eval_func(w, f, args);
    CHECK(r.ok);
    return r.value;
}

TEST("front: recursive fib from source") {
    World w = parse_ok(
        "fn fib(n: int) -> int {\n"
        "  if n < 2 { n } else { fib(n - 1) + fib(n - 2) }\n"
        "}\n");
    CHECK_EQ(call(w, "fib", {10}), 55);
    CHECK_EQ(call(w, "fib", {20}), 6765);
}

TEST("front: loop sum from source") {
    World w = parse_ok(
        "fn sum(n: int) -> int {\n"
        "  loop (acc = 0, i = 0) {\n"
        "    if i >= n { break acc } else { next acc + i, i + 1 }\n"
        "  }\n"
        "}\n");
    CHECK_EQ(call(w, "sum", {10}), 45);
    CHECK_EQ(call(w, "sum", {100}), 4950);
}

TEST("front: gcd from source") {
    World w = parse_ok(
        "fn gcd(a: int, b: int) -> int {\n"
        "  loop (x = a, y = b) {\n"
        "    if y == 0 { break x } else { next y, x % y }\n"
        "  }\n"
        "}\n");
    CHECK_EQ(call(w, "gcd", {48, 36}), 12);
    CHECK_EQ(call(w, "gcd", {1071, 462}), 21);
}

TEST("front: let bindings and operator precedence") {
    World w = parse_ok(
        "fn f(a: int, b: int) -> int {\n"
        "  let s = a + b; let p = a * b; s * 2 + p\n"
        "}\n");
    // (a+b)*2 + a*b ; for a=3,b=4 -> 14 + 12 = 26
    CHECK_EQ(call(w, "f", {3, 4}), 26);
    // precedence: 2 + 3 * 4 == 14
    World w2 = parse_ok("fn g() -> int { 2 + 3 * 4 }\n");
    CHECK_EQ(call(w2, "g", {}), 14);
}

TEST("front: mutual recursion (is_even/is_odd)") {
    World w = parse_ok(
        "fn is_even(n: int) -> bool { if n == 0 { true } else { is_odd(n - 1) } }\n"
        "fn is_odd(n: int) -> bool { if n == 0 { false } else { is_even(n - 1) } }\n");
    CHECK_EQ(call(w, "is_even", {10}), 1);
    CHECK_EQ(call(w, "is_even", {7}), 0);
    CHECK_EQ(call(w, "is_odd", {7}), 1);
}

TEST("front: comptime call folds at construction (comptime = reduction)") {
    World w = parse_ok(
        "comptime fn sq(x: int) -> int { x * x }\n"
        "fn use_ct(n: int) -> int { sq(7) + n }\n");
    NodeId f = w.find_func("use_ct");
    NodeId body = w.func_info(f).result;
    // body should be add(const 49, n) -- the call was evaluated away
    CHECK_EQ(w.node(body).op, Op::Add);
    bool has49 = false;
    for (NodeId in : w.node(body).ins)
        if (auto c = w.as_const(in)) if (*c == 49) has49 = true;
    CHECK(has49);
    CHECK_EQ(call(w, "use_ct", {1}), 50);
    CHECK_EQ(call(w, "use_ct", {10}), 59);
}

TEST("front: comptime recursive fib folds to a constant") {
    World w = parse_ok(
        "comptime fn cfib(n: int) -> int { if n < 2 { n } else { cfib(n-1) + cfib(n-2) } }\n"
        "fn main() -> int { cfib(15) }\n");
    NodeId f = w.find_func("main");
    NodeId body = w.func_info(f).result;
    CHECK(w.is_const(body));
    CHECK_EQ(*w.as_const(body), 610);
}

TEST("front: parse error reporting") {
    World w;
    auto st = parse_module(w, "fn bad(n: int) -> int { n + }\n");
    CHECK(!st.ok);
}
