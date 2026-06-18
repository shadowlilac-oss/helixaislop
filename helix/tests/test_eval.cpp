// Interpreter tests: hand-built graphs for recursion (Cond+Call) and loops.
#include "helix/eval.hpp"
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

static int64_t run1(World& w, NodeId f, int64_t a) {
    auto r = eval_func(w, f, {a});
    CHECK(r.ok);
    return r.value;
}

// fib(n) = n<2 ? n : fib(n-1)+fib(n-2)  -- recursion via Cond + Call
static NodeId build_fib(World& w) {
    NodeId f = w.begin_func("fib", {ty_i64()}, ty_i64());
    NodeId n = w.func_info(f).params[0];
    NodeId pred = w.cmp(Op::CmpLt, n, w.konst(2, ty_i64()));
    NodeId n1 = w.sub(n, w.konst(1, ty_i64()));
    NodeId n2 = w.sub(n, w.konst(2, ty_i64()));
    NodeId rec = w.add(w.call(f, {n1}), w.call(f, {n2}));
    // yields[0] = predicate false -> recurse; yields[1] = predicate true -> n
    NodeId body = w.make_cond(pred, ty_i64(), {rec, n});
    w.end_func(f, body);
    w.add_func(f);
    return f;
}

// loop accumulator helper: loop(acc=ai, i=ii) { if cmp(i,n) break acc; next(acc OP i, i+1) }
static NodeId build_loop_fn(World& w, const char* name, int64_t acc0, int64_t i0,
                            Op stop_cmp, Op step_op) {
    NodeId f = w.begin_func(name, {ty_i64()}, ty_i64());
    NodeId n = w.func_info(f).params[0];
    NodeId acc = w.param(ty_i64(), 0, "acc");
    NodeId i = w.param(ty_i64(), 1, "i");
    NodeId is_break = w.cmp(stop_cmp, i, n);
    NodeId next_acc = w.binop(step_op, acc, i);
    NodeId next_i = w.add(i, w.konst(1, ty_i64()));
    NodeId lp = w.make_loop({w.konst(acc0, ty_i64()), w.konst(i0, ty_i64())}, ty_i64(),
                            {acc, i}, is_break, acc, {next_acc, next_i});
    w.end_func(f, lp);
    w.add_func(f);
    return f;
}

TEST("interp: recursive fib") {
    World w;
    NodeId f = build_fib(w);
    CHECK_EQ(run1(w, f, 0), 0);
    CHECK_EQ(run1(w, f, 1), 1);
    CHECK_EQ(run1(w, f, 10), 55);
    CHECK_EQ(run1(w, f, 20), 6765);
    CHECK_EQ(run1(w, f, 30), 832040);
}

TEST("interp: loop sum 0..n-1") {
    World w;
    NodeId f = build_loop_fn(w, "sum", 0, 0, Op::CmpGe, Op::Add);
    CHECK_EQ(run1(w, f, 10), 45);
    CHECK_EQ(run1(w, f, 100), 4950);
    CHECK_EQ(run1(w, f, 1), 0);
}

TEST("interp: factorial via loop") {
    World w;
    // loop(acc=1, i=1){ if i>n break acc; next(acc*i, i+1) }
    NodeId f = build_loop_fn(w, "fact", 1, 1, Op::CmpGt, Op::Mul);
    CHECK_EQ(run1(w, f, 5), 120);
    CHECK_EQ(run1(w, f, 10), 3628800);
    CHECK_EQ(run1(w, f, 0), 1);
}

TEST("interp: gcd via loop with two params") {
    World w;
    // gcd(a,b): loop(x=a, y=b){ if y==0 break x; next(y, x%y) }
    NodeId f = w.begin_func("gcd", {ty_i64(), ty_i64()}, ty_i64());
    NodeId a = w.func_info(f).params[0];
    NodeId b = w.func_info(f).params[1];
    NodeId x = w.param(ty_i64(), 0, "x");
    NodeId y = w.param(ty_i64(), 1, "y");
    NodeId is_break = w.cmp(Op::CmpEq, y, w.konst(0, ty_i64()));
    NodeId next_x = y;
    NodeId next_y = w.srem(x, y);
    NodeId lp = w.make_loop({a, b}, ty_i64(), {x, y}, is_break, x, {next_x, next_y});
    w.end_func(f, lp);
    w.add_func(f);
    auto r1 = eval_func(w, f, {48, 36}); CHECK(r1.ok); CHECK_EQ(r1.value, 12);
    auto r2 = eval_func(w, f, {1071, 462}); CHECK(r2.ok); CHECK_EQ(r2.value, 21);
    auto r3 = eval_func(w, f, {17, 5}); CHECK(r3.ok); CHECK_EQ(r3.value, 1);
}

TEST("interp: fuel bounds non-termination (R2)") {
    World w;
    // loop that never breaks: is_break = false always
    NodeId f = w.begin_func("spin", {ty_i64()}, ty_i64());
    NodeId acc = w.param(ty_i64(), 0, "acc");
    NodeId i = w.param(ty_i64(), 1, "i");
    NodeId is_break = w.konst_bool(false);
    NodeId lp = w.make_loop({w.konst(0, ty_i64()), w.konst(0, ty_i64())}, ty_i64(),
                            {acc, i}, is_break, acc, {acc, w.add(i, w.konst(1, ty_i64()))});
    w.end_func(f, lp);
    w.add_func(f);
    auto r = eval_func(w, f, {0}, 100000);
    CHECK(!r.ok);
    CHECK(r.out_of_fuel);
}
