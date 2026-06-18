// Verifier, printer, and CSE/stats tests.
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "helix/print.hpp"
#include "helix/verify.hpp"
#include "test.hpp"

using namespace helix;

static bool contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

TEST("verify: well-formed functions pass") {
    World w;
    parse_module(w,
        "fn fib(n: int) -> int { if n < 2 { n } else { fib(n-1) + fib(n-2) } }\n"
        "fn sum(n: int) -> int { loop (acc=0, i=0) { if i >= n { break acc } else { next acc+i, i+1 } } }\n");
    auto r = verify_module(w);
    if (!r.ok) for (auto& e : r.errors) std::printf("    %s\n", e.c_str());
    CHECK(r.ok);
}

TEST("verify: linear state discipline accepts correct threading") {
    World w;
    NodeId f = w.begin_func("memfn", {ty_ptr()}, ty_i64(), /*has_state=*/true);
    NodeId p = w.func_info(f).params[0];
    NodeId s0 = w.func_info(f).state_param;
    NodeId l = w.load(p, s0, ty_i64());       // state after load = l
    NodeId v1 = w.add(l, w.konst(1, ty_i64()));
    NodeId s2 = w.store(p, v1, l);            // consumes l, produces s2
    w.end_func(f, l, s2);
    w.add_func(f);
    auto r = verify_func(w, f);
    if (!r.ok) for (auto& e : r.errors) std::printf("    %s\n", e.c_str());
    CHECK(r.ok);
}

TEST("verify: linearity violation (state used twice) is rejected") {
    World w;
    NodeId f = w.begin_func("broken", {ty_ptr()}, ty_i64(), /*has_state=*/true);
    NodeId p = w.func_info(f).params[0];
    NodeId s0 = w.func_info(f).state_param;
    NodeId l = w.load(p, s0, ty_i64());
    NodeId v1 = w.add(l, w.konst(1, ty_i64()));
    NodeId s2 = w.store(p, v1, s0);  // BUG: consumes s0 again (l dropped, s0 doubled)
    w.end_func(f, l, s2);
    w.add_func(f);
    auto r = verify_func(w, f);
    CHECK(!r.ok);  // must catch the linearity violation
}

TEST("printer: produces canonical textual format") {
    World w;
    parse_module(w, "fn fib(n: int) -> int { if n < 2 { n } else { fib(n-1) + fib(n-2) } }\n");
    std::string s = print_func(w, w.find_func("fib"));
    CHECK(contains(s, "fn @fib"));
    CHECK(contains(s, "cond"));
    CHECK(contains(s, "call @fib"));
    CHECK(contains(s, "return"));
}

TEST("stats: hash-consing gives CSE for free (shared subexpressions dedup)") {
    World w;
    NodeId f = w.begin_func("redundant", {ty_i64(), ty_i64()}, ty_i64());
    NodeId a = w.func_info(f).params[0];
    NodeId b = w.func_info(f).params[1];
    NodeId s1 = w.add(a, b);
    NodeId s2 = w.add(a, b);   // same node as s1 (CSE)
    NodeId top = w.add(s1, s2);
    w.end_func(f, top);
    w.add_func(f);
    CHECK_EQ(s1, s2);
    // live nodes: a, b, (a+b), (a+b)+(a+b) == 4
    CHECK_EQ((int)reachable_count(w, f), 4);
}
