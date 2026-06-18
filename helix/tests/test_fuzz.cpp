// Randomized differential testing: build random expression graphs, then check
// that the JIT, the interpreter, and an independent C++ reference all agree.
#include <functional>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

namespace {

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return s >> 33;
    }
    uint64_t upto(uint64_t n) { return next() % n; }
};

using RefFn = std::function<int64_t(const int64_t*)>;
struct Expr {
    NodeId node;
    RefFn ref;
};

struct Gen {
    World& w;
    Rng& rng;
    std::vector<NodeId> params;

    Expr leaf() {
        if (rng.upto(2) == 0 && !params.empty()) {
            int idx = (int)rng.upto(params.size());
            return {params[idx], [idx](const int64_t* a) { return a[idx]; }};
        }
        int64_t c = (int64_t)(rng.upto(201)) - 100;
        return {w.konst(c, ty_i64()), [c](const int64_t*) { return c; }};
    }

    Expr gen(int depth) {
        if (depth <= 0 || rng.upto(4) == 0) return leaf();
        int op = (int)rng.upto(8);
        Expr x = gen(depth - 1), y = gen(depth - 1);
        RefFn fx = x.ref, fy = y.ref;
        switch (op) {
            case 0: return {w.add(x.node, y.node),
                            [fx, fy](const int64_t* a) { return (int64_t)((uint64_t)fx(a) + (uint64_t)fy(a)); }};
            case 1: return {w.sub(x.node, y.node),
                            [fx, fy](const int64_t* a) { return (int64_t)((uint64_t)fx(a) - (uint64_t)fy(a)); }};
            case 2: return {w.mul(x.node, y.node),
                            [fx, fy](const int64_t* a) { return (int64_t)((uint64_t)fx(a) * (uint64_t)fy(a)); }};
            case 3: return {w.bit_and(x.node, y.node), [fx, fy](const int64_t* a) { return fx(a) & fy(a); }};
            case 4: return {w.bit_or(x.node, y.node), [fx, fy](const int64_t* a) { return fx(a) | fy(a); }};
            case 5: return {w.bit_xor(x.node, y.node), [fx, fy](const int64_t* a) { return fx(a) ^ fy(a); }};
            case 6: {  // shift left by a small amount
                int64_t sh = (int64_t)rng.upto(63);
                NodeId k = w.konst(sh, ty_i64());
                return {w.shl(x.node, k),
                        [fx, sh](const int64_t* a) { return (int64_t)((uint64_t)fx(a) << (sh & 63)); }};
            }
            default: {  // select(x < y, t, e)
                Expr t = gen(depth - 1), e = gen(depth - 1);
                RefFn ft = t.ref, fe = e.ref;
                NodeId c = w.cmp(Op::CmpLt, x.node, y.node);
                return {w.select(c, t.node, e.node),
                        [fx, fy, ft, fe](const int64_t* a) { return fx(a) < fy(a) ? ft(a) : fe(a); }};
            }
        }
    }
};

}  // namespace

TEST("fuzz: jit == interp == reference over random expression graphs") {
    constexpr int kFuncs = 200;
    constexpr int kTrials = 16;
    World w;
    Rng rng(0x9E3779B97F4A7C15ull);
    std::vector<NodeId> funcs;
    std::vector<RefFn> refs;

    for (int i = 0; i < kFuncs; i++) {
        NodeId f = w.begin_func("fuzz" + std::to_string(i), {ty_i64(), ty_i64(), ty_i64()}, ty_i64());
        Gen g{w, rng, w.func_info(f).params};
        Expr e = g.gen(4);
        w.end_func(f, e.node);
        w.add_func(f);
        funcs.push_back(f);
        refs.push_back(e.ref);
    }

    JitModule jit = jit_compile(w);
    if (!jit.ok) std::printf("    jit error: %s\n", jit.err.c_str());
    CHECK(jit.ok);

    for (int i = 0; i < kFuncs; i++) {
        for (int t = 0; t < kTrials; t++) {
            int64_t a[3];
            for (int k = 0; k < 3; k++) a[k] = (int64_t)(rng.next() % 200000) - 100000;
            std::vector<int64_t> args = {a[0], a[1], a[2]};
            int64_t expect = refs[i](a);
            auto ir = eval_func(w, funcs[i], args);
            int64_t jv = jit.call(funcs[i], args);
            CHECK(ir.ok);
            CHECK_EQ(ir.value, expect);
            CHECK_EQ(jv, expect);
        }
    }
}
