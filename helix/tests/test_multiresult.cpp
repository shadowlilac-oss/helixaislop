// Multi-result loop foundation: a loop producing several values, read via proj().
#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

// build f(n) = (sum 0..n-1) + 1000*(count) via a multi-result loop
static NodeId build_sum_count(World& w) {
    NodeId f = w.begin_func("f", {ty_i64()}, ty_i64());
    NodeId n = w.func_info(f).params[0];
    NodeId s = w.param(ty_i64(), 0, "s"), c = w.param(ty_i64(), 1, "c"), i = w.param(ty_i64(), 2, "i");
    NodeId one = w.konst(1, ty_i64()), zero = w.konst(0, ty_i64());
    NodeId is_break = w.cmp(Op::CmpGe, i, n);
    NodeId lp = w.make_loop_multi({zero, zero, zero}, {s, c, i}, is_break, {s, c},
                                  {w.add(s, i), w.add(c, one), w.add(i, one)});
    NodeId result = w.add(w.proj(lp, 0, ty_i64()), w.mul(w.proj(lp, 1, ty_i64()), w.konst(1000, ty_i64())));
    w.end_func(f, result);
    w.add_func(f);
    return f;
}

TEST("multiresult: loop yielding (sum, count) read via proj") {
    World w;
    NodeId f = w.begin_func("f", {ty_i64()}, ty_i64());
    NodeId n = w.func_info(f).params[0];
    NodeId s = w.param(ty_i64(), 0, "s"), c = w.param(ty_i64(), 1, "c"), i = w.param(ty_i64(), 2, "i");
    NodeId one = w.konst(1, ty_i64()), zero = w.konst(0, ty_i64());
    NodeId is_break = w.cmp(Op::CmpGe, i, n);
    NodeId ns = w.add(s, i), nc = w.add(c, one), ni = w.add(i, one);
    // loop (s=0,c=0,i=0) { if i>=n break (s,c) else next (s+i, c+1, i+1) }
    NodeId lp = w.make_loop_multi({zero, zero, zero}, {s, c, i}, is_break, {s, c}, {ns, nc, ni});
    NodeId sum = w.proj(lp, 0, ty_i64());
    NodeId count = w.proj(lp, 1, ty_i64());
    // result = sum + count*1000
    NodeId result = w.add(sum, w.mul(count, w.konst(1000, ty_i64())));
    w.end_func(f, result);
    w.add_func(f);

    // f(10): sum 0..9 = 45, count = 10 -> 45 + 10000 = 10045
    CHECK_EQ(eval_func(w, f, {10}).value, 10045);
    CHECK_EQ(eval_func(w, f, {5}).value, 10 + 5000);
    CHECK_EQ(eval_func(w, f, {0}).value, 0);
    // proj nodes are interned: proj(lp,0) twice is the same node
    CHECK_EQ(w.proj(lp, 0, ty_i64()), sum);
}

TEST("multiresult: both backends compile a multi-result loop (jit == interp)") {
    World w;
    NodeId f = build_sum_count(w);
    JitModule simple = jit_compile(w);
    JitModule ra = jit_compile_ra(w);
    CHECK(simple.ok);
    CHECK(ra.ok);
    for (int64_t n = 0; n <= 30; n++) {
        int64_t ref = (int64_t)n * (n - 1) / 2 + 1000 * n;
        CHECK_EQ(eval_func(w, f, {n}).value, ref);
        CHECK_EQ(simple.call(f, {n}), ref);
        CHECK_EQ(ra.call(f, {n}), ref);
    }
}
