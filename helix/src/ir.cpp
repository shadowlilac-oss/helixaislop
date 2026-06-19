#include "helix/ir.hpp"

#include <stdexcept>

namespace helix {

const char* op_name(Op op) {
    switch (op) {
        case Op::ConstInt: return "const.int";
        case Op::ConstBool: return "const.bool";
        case Op::Param: return "param";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Mul: return "mul";
        case Op::SDiv: return "sdiv";
        case Op::SRem: return "srem";
        case Op::And: return "and";
        case Op::Or: return "or";
        case Op::Xor: return "xor";
        case Op::Shl: return "shl";
        case Op::AShr: return "ashr";
        case Op::LShr: return "lshr";
        case Op::Neg: return "neg";
        case Op::Not: return "not";
        case Op::CmpEq: return "cmp.eq";
        case Op::CmpNe: return "cmp.ne";
        case Op::CmpLt: return "cmp.lt";
        case Op::CmpLe: return "cmp.le";
        case Op::CmpGt: return "cmp.gt";
        case Op::CmpGe: return "cmp.ge";
        case Op::Select: return "select";
        case Op::Load: return "load";
        case Op::Store: return "store";
        case Op::Call: return "call";
        case Op::Proj: return "proj";
        case Op::Func: return "func";
        case Op::Cond: return "cond";
        case Op::Loop: return "loop";
        case Op::Module: return "module";
        default: return "?";
    }
}

bool op_is_commutative(Op op) {
    switch (op) {
        case Op::Add: case Op::Mul: case Op::And: case Op::Or: case Op::Xor:
        case Op::CmpEq: case Op::CmpNe:
            return true;
        default:
            return false;
    }
}

bool op_is_pure_arith(Op op) {
    return op >= Op::Add && op <= Op::Select;
}

std::string type_str(Type t) {
    switch (t.kind) {
        case TyKind::Void: return "void";
        case TyKind::Int: return "i" + std::to_string(t.bits);
        case TyKind::Bool: return "bool";
        case TyKind::Ptr: return "ptr";
        case TyKind::State: return "state";
        case TyKind::Func: return "fn";
    }
    return "?";
}

World::World() {
    // Node 0 is the reserved NONE sentinel.
    nodes_.push_back(Node{Op::Module, ty_void(), {}, NONE, 0, 0});
    names_.push_back("<none>");
}

NodeId World::fresh(Node n, std::string name) {
    NodeId id = (NodeId)nodes_.size();
    nodes_.push_back(std::move(n));
    names_.push_back(std::move(name));
    return id;
}

int64_t World::trunc(int64_t v, Type t) const {
    if (t.kind == TyKind::Bool) return v & 1;
    if (t.kind != TyKind::Int) return v;
    if (t.bits >= 64) return v;
    // sign-extend from t.bits
    const uint64_t mask = (uint64_t(1) << t.bits) - 1;
    uint64_t u = (uint64_t)v & mask;
    const uint64_t sign = uint64_t(1) << (t.bits - 1);
    if (u & sign) u |= ~mask;  // sign extend
    return (int64_t)u;
}

uint64_t World::hash_node(const Node& n) {
    uint64_t h = 1469598103934665603ull;  // FNV-1a
    auto mix = [&](uint64_t x) { h ^= x; h *= 1099511628211ull; };
    mix((uint64_t)n.op);
    mix((uint64_t)n.type.kind);
    mix((uint64_t)n.type.bits);
    mix((uint64_t)n.imm);
    mix((uint64_t)n.state_in);
    for (NodeId in : n.ins) mix((uint64_t)in);
    return h;
}

bool World::node_eq(const Node& a, const Node& b) const {
    return a.op == b.op && a.type == b.type && a.imm == b.imm &&
           a.state_in == b.state_in && a.ins == b.ins;
}

NodeId World::intern(Node n) {
    uint64_t h = hash_node(n);
    auto& bucket = intern_[h];
    for (NodeId id : bucket) {
        if (node_eq(nodes_[id], n)) {
            intern_hits_++;
            return id;  // hash-consing: structural equality == identity (free GVN/CSE)
        }
    }
    NodeId id = fresh(std::move(n));
    bucket.push_back(id);
    return id;
}

NodeId World::konst(int64_t v, Type t) {
    Node n{Op::ConstInt, t, {}, NONE, trunc(v, t), 0};
    return intern(std::move(n));
}

NodeId World::konst_bool(bool b) {
    Node n{Op::ConstBool, ty_bool(), {}, NONE, b ? 1 : 0, 0};
    return intern(std::move(n));
}

std::optional<int64_t> World::as_const(NodeId id) const {
    const Node& n = nodes_[id];
    if (n.op == Op::ConstInt || n.op == Op::ConstBool) return n.imm;
    return std::nullopt;
}

std::optional<int64_t> World::fold_binop(Op op, int64_t a, int64_t b, Type t, bool& ok) const {
    ok = true;
    switch (op) {
        case Op::Add: return trunc((int64_t)((uint64_t)a + (uint64_t)b), t);
        case Op::Sub: return trunc((int64_t)((uint64_t)a - (uint64_t)b), t);
        case Op::Mul: return trunc((int64_t)((uint64_t)a * (uint64_t)b), t);
        case Op::SDiv:
            if (b == 0) { ok = false; return std::nullopt; }  // div-by-zero: leave as node
            if (a == INT64_MIN && b == -1) return trunc(a, t);
            return trunc(a / b, t);
        case Op::SRem:
            if (b == 0) { ok = false; return std::nullopt; }
            if (a == INT64_MIN && b == -1) return 0;
            return trunc(a % b, t);
        case Op::And: return trunc(a & b, t);
        case Op::Or: return trunc(a | b, t);
        case Op::Xor: return trunc(a ^ b, t);
        case Op::Shl: return trunc((int64_t)((uint64_t)a << (b & 63)), t);
        case Op::AShr: return trunc(a >> (b & 63), t);
        case Op::LShr: return trunc((int64_t)((uint64_t)a >> (b & 63)), t);
        default: ok = false; return std::nullopt;
    }
}

NodeId World::binop(Op op, NodeId a, NodeId b) {
    const Type t = nodes_[a].type;
    auto ca = as_const(a), cb = as_const(b);

    // Tier-1: constant folding.
    if (ca && cb) {
        bool ok;
        auto r = fold_binop(op, *ca, *cb, t, ok);
        if (ok) return konst(*r, t);
    }

    // Tier-1: canonicalize commutative ops (constant on the right; else order by id)
    // so add(x,y) and add(y,x) intern to the same node (free commutativity-CSE).
    if (op_is_commutative(op)) {
        bool swap = false;
        if (ca && !cb) swap = true;             // move const to the right
        else if (!ca && !cb && a > b) swap = true;  // canonical id order
        if (swap) { std::swap(a, b); std::swap(ca, cb); }
    }

    // Tier-1: algebraic identities.
    switch (op) {
        case Op::Add:
            if (cb && *cb == 0) return a;
            break;
        case Op::Sub:
            if (cb && *cb == 0) return a;
            if (a == b) return konst(0, t);
            break;
        case Op::Mul:
            if (cb && *cb == 0) return konst(0, t);
            if (cb && *cb == 1) return a;
            // strength reduction: x * 2^k -> x << k
            if (cb && *cb > 1 && (*cb & (*cb - 1)) == 0) {
                int64_t k = 0, v = *cb;
                while (v > 1) { v >>= 1; k++; }
                return binop(Op::Shl, a, konst(k, t));
            }
            break;
        case Op::SDiv:
            if (cb && *cb == 1) return a;
            break;
        case Op::And:
            if (cb && *cb == 0) return konst(0, t);
            if (a == b) return a;
            break;
        case Op::Or:
            if (cb && *cb == 0) return a;
            if (a == b) return a;
            break;
        case Op::Xor:
            if (cb && *cb == 0) return a;
            if (a == b) return konst(0, t);
            break;
        case Op::Shl: case Op::AShr: case Op::LShr:
            if (cb && *cb == 0) return a;
            break;
        default:
            break;
    }

    Node n{op, t, {a, b}, NONE, 0, 0};
    return intern(std::move(n));
}

NodeId World::cmp(Op op, NodeId a, NodeId b) {
    auto ca = as_const(a), cb = as_const(b);
    if (ca && cb) {
        bool r = false;
        switch (op) {
            case Op::CmpEq: r = (*ca == *cb); break;
            case Op::CmpNe: r = (*ca != *cb); break;
            case Op::CmpLt: r = (*ca < *cb); break;
            case Op::CmpLe: r = (*ca <= *cb); break;
            case Op::CmpGt: r = (*ca > *cb); break;
            case Op::CmpGe: r = (*ca >= *cb); break;
            default: break;
        }
        return konst_bool(r);
    }
    // x == x / x <= x etc.
    if (a == b) {
        switch (op) {
            case Op::CmpEq: case Op::CmpLe: case Op::CmpGe: return konst_bool(true);
            case Op::CmpNe: case Op::CmpLt: case Op::CmpGt: return konst_bool(false);
            default: break;
        }
    }
    if (op_is_commutative(op) && a > b) std::swap(a, b);
    Node n{op, ty_bool(), {a, b}, NONE, 0, 0};
    return intern(std::move(n));
}

NodeId World::neg(NodeId a) {
    if (auto c = as_const(a))
        return konst(trunc((int64_t)(0ull - (uint64_t)*c), nodes_[a].type), nodes_[a].type);
    Node n{Op::Neg, nodes_[a].type, {a}, NONE, 0, 0};
    return intern(std::move(n));
}

NodeId World::bit_not(NodeId a) {
    if (auto c = as_const(a)) return konst(trunc(~*c, nodes_[a].type), nodes_[a].type);
    Node n{Op::Not, nodes_[a].type, {a}, NONE, 0, 0};
    return intern(std::move(n));
}

NodeId World::select(NodeId c, NodeId a, NodeId b) {
    if (auto cc = as_const(c)) return *cc ? a : b;
    if (a == b) return a;
    Node n{Op::Select, nodes_[a].type, {c, a, b}, NONE, 0, 0};
    return intern(std::move(n));
}

NodeId World::load(NodeId ptr, NodeId st, Type t) {
    // effectful: not interned (carries linear state)
    Node n{Op::Load, t, {ptr}, st, 0, 0};
    return fresh(std::move(n));
}

NodeId World::store(NodeId ptr, NodeId val, NodeId st) {
    Node n{Op::Store, ty_state(), {ptr, val}, st, 0, 0};
    return fresh(std::move(n));
}

NodeId World::pure_load(NodeId ptr, Type t) {
    Node n{Op::Load, t, {ptr}, NONE, 0, 0};
    return intern(std::move(n));  // read-only: loads of the same address are the same node
}

NodeId World::param(Type t, int index, std::string name) {
    Node n{Op::Param, t, {}, NONE, index, 0};
    return fresh(std::move(n), std::move(name));
}

NodeId World::begin_func(const std::string& name, std::vector<Type> param_types,
                         Type result_type, bool has_state, bool is_comptime) {
    FuncInfo fi;
    fi.name = name;
    fi.param_types = param_types;
    fi.result_type = result_type;
    fi.has_state = has_state;
    fi.is_comptime = is_comptime;
    for (size_t i = 0; i < param_types.size(); i++)
        fi.params.push_back(param(param_types[i], (int)i, "arg" + std::to_string(i)));
    if (has_state) fi.state_param = param(ty_state(), (int)param_types.size(), "s");
    RegionId rid = (RegionId)funcs_.size();
    funcs_.push_back(std::move(fi));
    Node n{Op::Func, {TyKind::Func, 0}, {}, NONE, 0, rid};
    return fresh(std::move(n), name);
}

void World::end_func(NodeId func, NodeId result, NodeId state_result) {
    FuncInfo& fi = funcs_[node(func).region];
    fi.result = result;
    fi.state_result = state_result;
}

NodeId World::make_cond(NodeId predicate, Type result_type, std::vector<NodeId> yields) {
    // Fully-static predicate => pick the branch (Tier-1 reduction of control).
    if (auto cp = as_const(predicate)) {
        if (yields.size() == 2) return *cp ? yields[1] : yields[0];  // binary: truthiness
        int64_t idx = *cp;
        if (idx >= 0 && idx < (int64_t)yields.size()) return yields[idx];
    }
    // Tier-1 identities on binary conditionals.
    if (yields.size() == 2) {
        if (yields[0] == yields[1]) return yields[0];  // both arms equal
        auto y0 = as_const(yields[0]), y1 = as_const(yields[1]);
        // cond(p, {false, true}) == p ; cond(p, {true, false}) == !p — only valid when the
        // predicate is a proven bool (in {0,1}); otherwise truthiness != identity.
        const bool pred_bool = node(predicate).type.kind == TyKind::Bool;
        if (result_type.kind == TyKind::Bool && y0 && y1 && pred_bool) {
            if (*y0 == 0 && *y1 == 1) return predicate;
            if (*y0 == 1 && *y1 == 0) return cmp(Op::CmpEq, predicate, konst_bool(false));
        }
    }
    CondInfo ci;
    ci.yields = std::move(yields);
    RegionId rid = (RegionId)conds_.size();
    conds_.push_back(std::move(ci));
    Node n{Op::Cond, result_type, {predicate}, NONE, 0, rid};
    return fresh(std::move(n));
}

NodeId World::make_loop(std::vector<NodeId> inits, Type result_type, std::vector<NodeId> params,
                        NodeId is_break, NodeId break_val, std::vector<NodeId> next_vals) {
    LoopInfo li;
    li.params = std::move(params);
    li.is_break = is_break;
    li.break_val = break_val;
    li.next_vals = std::move(next_vals);
    RegionId rid = (RegionId)loops_.size();
    loops_.push_back(std::move(li));
    Node n{Op::Loop, result_type, std::move(inits), NONE, 0, rid};
    return fresh(std::move(n));
}

NodeId World::make_loop_multi(std::vector<NodeId> inits, std::vector<NodeId> params, NodeId is_break,
                              std::vector<NodeId> break_vals, std::vector<NodeId> next_vals) {
    LoopInfo li;
    li.params = std::move(params);
    li.is_break = is_break;
    li.break_vals = std::move(break_vals);
    li.next_vals = std::move(next_vals);
    RegionId rid = (RegionId)loops_.size();
    loops_.push_back(std::move(li));
    // A multi-result region node is void-typed; results are read out via proj().
    Node n{Op::Loop, ty_void(), std::move(inits), NONE, 0, rid};
    return fresh(std::move(n));
}

NodeId World::proj(NodeId region, int idx, Type t) {
    Node n{Op::Proj, t, {region}, NONE, (int64_t)idx, 0};
    return intern(std::move(n));  // proj(r,k) is structural: same (r,k) -> same node
}

NodeId World::call(NodeId func, std::vector<NodeId> args, NodeId st) {
    const FuncInfo& fi = func_info(func);
    Node n{Op::Call, fi.result_type, std::move(args), st, (int64_t)func, 0};
    return fresh(std::move(n));
}

NodeId World::find_func(const std::string& name) const {
    for (NodeId f : module_funcs_)
        if (funcs_[node(f).region].name == name) return f;
    return NONE;
}

}  // namespace helix
