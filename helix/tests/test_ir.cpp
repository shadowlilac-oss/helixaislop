// Tests for the core IR: hash-consing, smart-constructor normalization, identities.
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

TEST("const folding: arithmetic") {
    World w;
    NodeId a = w.konst(20, ty_i64());
    NodeId b = w.konst(22, ty_i64());
    NodeId s = w.add(a, b);
    CHECK(w.is_const(s));
    CHECK_EQ(*w.as_const(s), 42);
    CHECK_EQ(*w.as_const(w.mul(w.konst(6, ty_i64()), w.konst(7, ty_i64()))), 42);
    CHECK_EQ(*w.as_const(w.sub(b, a)), 2);
}

TEST("const folding: width truncation i32") {
    World w;
    // 2^31 - 1 + 1 wraps to -2^31 in i32
    NodeId a = w.konst(2147483647, ty_i32());
    NodeId one = w.konst(1, ty_i32());
    NodeId s = w.add(a, one);
    CHECK(w.is_const(s));
    CHECK_EQ(*w.as_const(s), -2147483648LL);
}

TEST("hash-consing: identical pure nodes are the same node (free GVN)") {
    World w;
    NodeId x = w.param(ty_i64(), 0);
    NodeId y = w.param(ty_i64(), 1);
    NodeId e1 = w.add(x, y);
    NodeId e2 = w.add(x, y);
    CHECK_EQ(e1, e2);  // same NodeId
    // commutativity is canonicalized -> add(y,x) interns to add(x,y)
    NodeId e3 = w.add(y, x);
    CHECK_EQ(e1, e3);
}

TEST("algebraic identities") {
    World w;
    NodeId x = w.param(ty_i64(), 0);
    NodeId zero = w.konst(0, ty_i64());
    NodeId one = w.konst(1, ty_i64());
    CHECK_EQ(w.add(x, zero), x);
    CHECK_EQ(w.sub(x, zero), x);
    CHECK_EQ(w.mul(x, one), x);
    CHECK_EQ(w.mul(x, zero), zero);
    CHECK_EQ(w.sub(x, x), zero);
    CHECK_EQ(w.bit_xor(x, x), zero);
    CHECK_EQ(w.bit_or(x, x), x);
    CHECK_EQ(w.bit_and(x, x), x);
}

TEST("strength reduction: mul by power of two -> shift") {
    World w;
    NodeId x = w.param(ty_i64(), 0);
    NodeId m = w.mul(x, w.konst(8, ty_i64()));
    CHECK_EQ(w.node(m).op, Op::Shl);
    CHECK(w.is_const(w.node(m).ins[1]));
    CHECK_EQ(*w.as_const(w.node(m).ins[1]), 3);  // 8 == 1<<3
}

TEST("comparison folding and reflexivity") {
    World w;
    NodeId a = w.konst(3, ty_i64());
    NodeId b = w.konst(5, ty_i64());
    CHECK_EQ(*w.as_const(w.cmp(Op::CmpLt, a, b)), 1);
    CHECK_EQ(*w.as_const(w.cmp(Op::CmpGt, a, b)), 0);
    NodeId x = w.param(ty_i64(), 0);
    CHECK_EQ(*w.as_const(w.cmp(Op::CmpEq, x, x)), 1);
    CHECK_EQ(*w.as_const(w.cmp(Op::CmpLt, x, x)), 0);
}

TEST("select folding") {
    World w;
    NodeId a = w.param(ty_i64(), 0);
    NodeId b = w.param(ty_i64(), 1);
    CHECK_EQ(w.select(w.konst_bool(true), a, b), a);
    CHECK_EQ(w.select(w.konst_bool(false), a, b), b);
    CHECK_EQ(w.select(w.param(ty_bool(), 2), a, a), a);  // both arms equal
}

TEST("static cond picks the branch") {
    World w;
    NodeId a = w.param(ty_i64(), 0);
    NodeId b = w.param(ty_i64(), 1);
    NodeId c = w.make_cond(w.konst_bool(true), ty_i64(), {b, a});  // pred 1 -> yields[1]=a
    CHECK_EQ(c, a);
}

TEST("nested folding collapses a whole expression") {
    World w;
    // ((2+3) * 4 - 1) == 19, fully folded at construction
    NodeId e = w.sub(w.mul(w.add(w.konst(2, ty_i64()), w.konst(3, ty_i64())),
                           w.konst(4, ty_i64())),
                     w.konst(1, ty_i64()));
    CHECK(w.is_const(e));
    CHECK_EQ(*w.as_const(e), 19);
}
