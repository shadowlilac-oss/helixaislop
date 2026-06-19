// Optimization-pass tests: inlining and dead-function elimination, validated
// against the interpreter oracle (an inlined program must behave identically).
#include <algorithm>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "helix/opt.hpp"
#include "helix/verify.hpp"
#include "test.hpp"

using namespace helix;

namespace {
bool body_has_call(World& w, NodeId func) {
    std::vector<NodeId> ns = {w.func_info(func).result};
    std::vector<bool> seen(w.node_count(), false);
    while (!ns.empty()) {
        NodeId v = ns.back(); ns.pop_back();
        if (v == NONE || seen[v]) continue;
        seen[v] = true;
        const Node& n = w.node(v);
        if (n.op == Op::Call) return true;
        for (NodeId in : n.ins) ns.push_back(in);
        if (n.op == Op::Cond) for (NodeId y : w.cond_info(v).yields) ns.push_back(y);
        if (n.op == Op::Loop) {
            const LoopInfo& li = w.loop_info(v);
            for (NodeId p : li.params) ns.push_back(p);
            ns.push_back(li.is_break); ns.push_back(li.break_val);
            for (NodeId nv : li.next_vals) ns.push_back(nv);
        }
    }
    return false;
}
}  // namespace

TEST("opt: inlining removes the call and preserves semantics") {
    World w;
    parse_module(w,
        "fn sq(x: int) -> int { x * x }\n"
        "fn f(a: int) -> int { sq(a + 1) }\n");
    NodeId f = w.find_func("f");
    CHECK(body_has_call(w, f));
    inline_into(w, f, 1);
    CHECK(!body_has_call(w, f));  // sq call inlined away
    for (int64_t a = -5; a <= 5; a++) {
        auto r = eval_func(w, f, {a});
        CHECK(r.ok);
        CHECK_EQ(r.value, (a + 1) * (a + 1));
    }
}

TEST("opt: inlining a constant-argument call folds to a constant") {
    World w;
    parse_module(w,
        "fn sq(x: int) -> int { x * x }\n"
        "fn g() -> int { sq(7) }\n");
    NodeId g = w.find_func("g");
    inline_into(w, g, 1);
    NodeId body = w.func_info(g).result;
    CHECK(w.is_const(body));
    CHECK_EQ(*w.as_const(body), 49);
}

TEST("opt: inlining a function containing a loop stays correct (jit == interp)") {
    World w;
    parse_module(w,
        "fn sum(n: int) -> int { loop (acc=0,i=0){ if i>=n {break acc} else {next acc+i,i+1} } }\n"
        "fn h(n: int) -> int { sum(n) + sum(n) }\n");
    NodeId h = w.find_func("h");
    inline_into(w, h, 1);
    CHECK(!body_has_call(w, h));
    JitModule jit = jit_compile_ra(w);
    CHECK(jit.ok);
    for (int64_t n = 0; n <= 20; n++) {
        auto ir = eval_func(w, h, {n});
        int64_t jv = jit.call(h, {n});
        CHECK(ir.ok);
        CHECK_EQ(ir.value, (int64_t)n * (n - 1));  // 2 * sum(0..n-1)
        CHECK_EQ(jv, ir.value);
    }
}

TEST("opt: optimize_module (the -O path) preserves semantics and re-verifies") {
    const char* src =
        "fn inc(x: int) -> int { x + 1 }\n"
        "fn sq(x: int) -> int { x * x }\n"
        "fn poly(a: int, b: int) -> int { sq(inc(a)) + inc(sq(b)) }\n"
        "fn rec(n: int) -> int { if (n <= 0) | (n > 12) { n } else { rec(n - 1) + inc(n) } }\n";
    World wu, wo;  // unoptimized vs optimized copies of the same source
    CHECK(parse_module(wu, src).ok);
    CHECK(parse_module(wo, src).ok);
    optimize_module(wo, 2);
    CHECK(verify_module(wo).ok);  // the optimized graph must still pass the verifier
    JitModule ju = jit_compile_ra(wu), jo = jit_compile_ra(wo);
    CHECK(ju.ok);
    CHECK(jo.ok);
    for (const char* fn : {"poly", "rec", "inc", "sq"}) {
        NodeId fu = wu.find_func(fn), fo = wo.find_func(fn);
        int np = (std::string(fn) == "poly") ? 2 : 1;
        for (int64_t a = -6; a <= 13; a++) {
            std::vector<int64_t> args = np == 2 ? std::vector<int64_t>{a, a + 2} : std::vector<int64_t>{a};
            auto ir = eval_func(wu, fu, args);
            CHECK(ir.ok);
            CHECK_EQ(eval_func(wo, fo, args).value, ir.value);  // opt == unopt (interp)
            CHECK_EQ(jo.call(fo, args), ir.value);              // opt jit == interp
            CHECK_EQ(ju.call(fu, args), ir.value);              // unopt jit == interp
        }
    }
}

TEST("opt: reachable_functions finds the live set and excludes dead code") {
    World w;
    parse_module(w,
        "fn b(n: int) -> int { n + 1 }\n"
        "fn a(n: int) -> int { b(n) * 2 }\n"
        "fn main() -> int { a(5) }\n"
        "fn dead(n: int) -> int { n * n }\n");
    auto live = reachable_functions(w, {w.find_func("main")});
    CHECK_EQ((int)live.size(), 3);
    NodeId dead = w.find_func("dead");
    CHECK(std::find(live.begin(), live.end(), dead) == live.end());
    NodeId b = w.find_func("b");
    CHECK(std::find(live.begin(), live.end(), b) != live.end());
}
