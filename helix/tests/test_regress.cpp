// Regression tests for bugs found by adversarial review (all must stay fixed).
#include <climits>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "helix/verify.hpp"
#include "test.hpp"

using namespace helix;

namespace {
struct C { World w; JitModule jit; };
static void load(C& c, const char* src) {
    auto st = parse_module(c.w, src);
    if (!st.ok) std::printf("    parse error (line %d): %s\n", st.line, st.msg.c_str());
    CHECK(st.ok);
    c.jit = jit_compile(c.w);
    CHECK(c.jit.ok);
}
static int64_t diff(C& c, const char* fn, std::vector<int64_t> a) {
    NodeId f = c.w.find_func(fn);
    auto ir = eval_func(c.w, f, a);
    int64_t jv = c.jit.call(f, a);
    CHECK(ir.ok);
    CHECK_EQ(jv, ir.value);
    return jv;
}
}  // namespace

TEST("regress: idiv by zero does not crash and matches interpreter (==0)") {
    C c;
    load(c, "fn dz(a: int, b: int) -> int { a / b }\n"
            "fn rz(a: int, b: int) -> int { a % b }\n");
    CHECK_EQ(diff(c, "dz", {123, 0}), 0);
    CHECK_EQ(diff(c, "rz", {123, 0}), 0);
    CHECK_EQ(diff(c, "dz", {0, 0}), 0);
}

TEST("regress: INT64_MIN / -1 overflow does not trap (#DE)") {
    C c;
    load(c, "fn dv(a: int, b: int) -> int { a / b }\n"
            "fn rv(a: int, b: int) -> int { a % b }\n");
    CHECK_EQ(diff(c, "dv", {INT64_MIN, -1}), INT64_MIN);
    CHECK_EQ(diff(c, "rv", {INT64_MIN, -1}), 0);
    // normal division still correct
    CHECK_EQ(diff(c, "dv", {-100, 7}), -100 / 7);
    CHECK_EQ(diff(c, "rv", {-100, 7}), -100 % 7);
}

TEST("regress: i32 arithmetic wraps to 32 bits in JIT and interpreter") {
    C c;
    load(c, "fn add32(a: i32, b: i32) -> i32 { a + b }\n"
            "fn mul32(a: i32, b: i32) -> i32 { a * b }\n");
    CHECK_EQ(diff(c, "add32", {2000000000, 2000000000}), (int64_t)(int32_t)(2000000000u + 2000000000u));
    CHECK_EQ(diff(c, "mul32", {100000, 100000}), (int64_t)(int32_t)(100000u * 100000u));
    CHECK_EQ(diff(c, "add32", {-1, -2147483648}), (int64_t)(int32_t)((uint32_t)(-1) + (uint32_t)(-2147483648)));
}

TEST("regress: forward-declared comptime call folds correctly (not to 0)") {
    World w;
    auto st = parse_module(w,
        "fn main() -> int { sq(7) }\n"
        "comptime fn sq(x: int) -> int { x * x }\n");
    CHECK(st.ok);
    NodeId f = w.find_func("main");
    NodeId body = w.func_info(f).result;
    CHECK(w.is_const(body));
    CHECK_EQ(*w.as_const(body), 49);
}

TEST("regress: binary cond predicate uses truthiness consistently (interp==jit)") {
    // hand-built cond with a NON-bool i64 predicate; interp and backend must agree.
    C c;
    World& w = c.w;
    NodeId f = w.begin_func("pick", {ty_i64()}, ty_i64());
    NodeId x = w.func_info(f).params[0];
    NodeId cnd = w.make_cond(x, ty_i64(), {w.konst(9, ty_i64()), w.konst(7, ty_i64())});
    w.end_func(f, cnd);
    w.add_func(f);
    c.jit = jit_compile(w);
    CHECK(c.jit.ok);
    for (int64_t v : {0, 1, 2, 5, -3}) CHECK_EQ(diff(c, "pick", {v}), v ? 7 : 9);
}

TEST("regress: comptime i64 overflow folds with defined two's-complement wrap") {
    World w;
    auto st = parse_module(w,
        "comptime fn wrap() -> int { 9223372036854775807 + 1 }\n"
        "fn main() -> int { wrap() }\n");
    CHECK(st.ok);
    NodeId body = w.func_info(w.find_func("main")).result;
    CHECK(w.is_const(body));
    CHECK_EQ(*w.as_const(body), INT64_MIN);  // wraps, no UB
}

TEST("regress: argument-count mismatch is a parse error") {
    World w1; CHECK(!parse_module(w1, "fn f(a: int) -> int { a }\nfn g() -> int { f(1, 2) }\n").ok);
    World w2; CHECK(!parse_module(w2, "fn f(a: int, b: int) -> int { a }\nfn g() -> int { f(1) }\n").ok);
}

TEST("regress: GCM schedules break-value cond cone before the loop exit test") {
    // fuzz_cf seed-1 repro. A single-result `loop` whose break value is an if-expr
    // reading the loop counter at break (counter == 0). The break-value Cond is
    // evaluated BEFORE the exit test; its arm references a body-region constant that
    // the scheduler must also place pre-test (else it is read before being defined on
    // the breaking iteration). Both backends AND the interpreter must agree.
    World w;
    auto st = parse_module(w,
        "fn g(p2: i64) -> i64 {\n"
        "  loop (t0 = 3) {\n"
        "    if (t0 <= 0) { break if ((t0 | p2) < t0) { 1 } else { 0 } }\n"
        "    else { next (t0 - 1) }\n"
        "  }\n"
        "}\n");
    CHECK(st.ok);
    JitModule simple = jit_compile(w), ra = jit_compile_ra(w);
    CHECK(simple.ok);
    CHECK(ra.ok);
    NodeId g = w.find_func("g");
    for (int64_t p2 : std::vector<int64_t>{-7, -1, 0, 1, 5, INT64_MIN, INT64_MAX}) {
        int64_t expect = ((0 | p2) < 0) ? 1 : 0;  // at break t0==0
        auto ir = eval_func(w, g, {p2});
        CHECK(ir.ok);
        CHECK_EQ(ir.value, expect);
        CHECK_EQ(simple.call(g, {p2}), expect);
        CHECK_EQ(ra.call(g, {p2}), expect);
    }
}

TEST("regress: inner-loop result used in both arms of a following if (GCM domination)") {
    // A value (inner loop's result) consumed from several control-divergent positions
    // must be materialized once at a point dominating every use. Both backends + interp.
    World w;
    auto st = parse_module(w,
        "fn f(n: int) -> int {\n"
        "  var acc = 0; var i = 0;\n"
        "  while i < n {\n"
        "    var j = 0; var s = 0;\n"
        "    while j < i { s = s + 1; j = j + 1; }\n"
        "    if s > 2 { acc = acc + s; } else { acc = acc + 1; }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return acc;\n"
        "}\n");
    CHECK(st.ok);
    JitModule simple = jit_compile(w), ra = jit_compile_ra(w);
    CHECK(simple.ok);
    CHECK(ra.ok);
    NodeId f = w.find_func("f");
    for (int64_t n = 0; n <= 20; n++) {
        int64_t acc = 0;
        for (int64_t i = 0; i < n; i++) { int64_t s = i; acc += (s > 2 ? s : 1); }
        auto ir = eval_func(w, f, {n});
        CHECK(ir.ok);
        CHECK_EQ(ir.value, acc);
        CHECK_EQ(simple.call(f, {n}), acc);
        CHECK_EQ(ra.call(f, {n}), acc);
    }
}

TEST("regress: verifier does not flag a state-consuming Call as a dropped producer") {
    World w;
    NodeId g = w.begin_func("g", {ty_ptr()}, ty_i64(), /*has_state=*/true);
    w.end_func(g, w.func_info(g).params[0], w.func_info(g).state_param);
    w.add_func(g);

    NodeId f = w.begin_func("f", {ty_ptr()}, ty_i64(), /*has_state=*/true);
    NodeId fp = w.func_info(f).params[0];
    NodeId fs = w.func_info(f).state_param;            // the only state producer
    NodeId call = w.call(g, {fp}, fs);                 // Call consumes state fs (state_in = fs)
    w.end_func(f, call, call);                         // result value = call
    w.add_func(f);
    // fs is consumed exactly once (by the call); the Call itself must NOT be treated as a
    // state producer (the fix). Before the fix this raised a spurious linearity error.
    auto r = verify_func(w, f);
    if (!r.ok) for (auto& e : r.errors) std::printf("    %s\n", e.c_str());
    CHECK(r.ok);
}
