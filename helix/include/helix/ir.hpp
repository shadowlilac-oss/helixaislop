// Helix IR — core graph: six node forms, two strands, hash-consing, smart
// constructors with Tier-1 normalization. See wiki/11-core-model.md.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace helix {

using NodeId = uint32_t;
using RegionId = uint32_t;
constexpr NodeId NONE = 0;

// ---- Types (interned-by-value; small PODs). Types are conceptually values in
// the design; here they are represented as compact descriptors. ----
enum class TyKind : uint8_t { Void, Int, Bool, Ptr, State, Func };

struct Type {
    TyKind kind = TyKind::Void;
    uint16_t bits = 0;  // width for Int
    bool operator==(const Type& o) const { return kind == o.kind && bits == o.bits; }
    bool operator!=(const Type& o) const { return !(*this == o); }
    bool is_int() const { return kind == TyKind::Int; }
};

inline Type ty_int(uint16_t bits) { return {TyKind::Int, bits}; }
inline Type ty_i64() { return {TyKind::Int, 64}; }
inline Type ty_i32() { return {TyKind::Int, 32}; }
inline Type ty_bool() { return {TyKind::Bool, 1}; }
inline Type ty_state() { return {TyKind::State, 0}; }
inline Type ty_ptr() { return {TyKind::Ptr, 64}; }
inline Type ty_void() { return {TyKind::Void, 0}; }

std::string type_str(Type t);

// ---- Opcodes. The single `Op` form (plus region forms) keeps the taxonomy minimal. ----
enum class Op : uint16_t {
    // structural leaves
    ConstInt,
    ConstBool,
    Param,
    // pure integer arithmetic
    Add, Sub, Mul, SDiv, SRem, And, Or, Xor, Shl, AShr, LShr, Neg, Not,
    // comparisons -> bool (signed)
    CmpEq, CmpNe, CmpLt, CmpLe, CmpGt, CmpGe,
    Select,
    // effectful (carry a state edge)
    Load, Store, Call,
    // region / nominal nodes
    Func, Cond, Loop, Module,
    LAST
};

const char* op_name(Op op);
bool op_is_commutative(Op op);
bool op_is_pure_arith(Op op);

// ---- Node: a single uniform record. ----
struct Node {
    Op op;
    Type type;
    std::vector<NodeId> ins;  // value operands (use -> def edges)
    NodeId state_in = NONE;   // NONE => pure (value strand only)
    int64_t imm = 0;          // ConstInt value, Param index, Call target region, ...
    RegionId region = 0;      // index into a per-form side table (Func/Cond/Loop/Module)
};

// Per-form side tables -------------------------------------------------------
struct FuncInfo {
    std::string name;
    std::vector<Type> param_types;
    Type result_type;
    bool has_state = false;
    bool is_comptime = false;
    std::vector<NodeId> params;  // Param nodes (block parameters of the body)
    NodeId state_param = NONE;
    NodeId result = NONE;
    NodeId state_result = NONE;
};

struct CondInfo {
    // Symmetric conditional / switch: predicate selects a case (0..n-1).
    // Each case yields one value (single-result conditionals for now).
    std::vector<NodeId> yields;
};

struct LoopInfo {
    // Tail-controlled loop expressed without a graph cycle.
    std::vector<NodeId> params;     // carried block parameters
    NodeId is_break = NONE;         // bool: stop iterating this round?
    NodeId break_val = NONE;        // value produced on break
    std::vector<NodeId> next_vals;  // carried values when continuing
};

// ---- The World: arena + intern table + smart constructors. The ONLY way to
// create a node, so Tier-1 normalization and hash-consing are unavoidable. ----
class World {
public:
    World();

    const Node& node(NodeId id) const { return nodes_[id]; }
    Node& node_mut(NodeId id) { return nodes_[id]; }
    NodeId node_count() const { return (NodeId)nodes_.size(); }
    const std::string& name(NodeId id) const { return names_[id]; }
    void set_name(NodeId id, std::string n) { names_[id] = std::move(n); }

    // constants
    NodeId konst(int64_t v, Type t);
    NodeId konst_bool(bool b);
    std::optional<int64_t> as_const(NodeId id) const;
    bool is_const(NodeId id) const { return as_const(id).has_value(); }

    // pure ops (smart constructors)
    NodeId binop(Op op, NodeId a, NodeId b);
    NodeId add(NodeId a, NodeId b) { return binop(Op::Add, a, b); }
    NodeId sub(NodeId a, NodeId b) { return binop(Op::Sub, a, b); }
    NodeId mul(NodeId a, NodeId b) { return binop(Op::Mul, a, b); }
    NodeId sdiv(NodeId a, NodeId b) { return binop(Op::SDiv, a, b); }
    NodeId srem(NodeId a, NodeId b) { return binop(Op::SRem, a, b); }
    NodeId bit_and(NodeId a, NodeId b) { return binop(Op::And, a, b); }
    NodeId bit_or(NodeId a, NodeId b) { return binop(Op::Or, a, b); }
    NodeId bit_xor(NodeId a, NodeId b) { return binop(Op::Xor, a, b); }
    NodeId shl(NodeId a, NodeId b) { return binop(Op::Shl, a, b); }
    NodeId ashr(NodeId a, NodeId b) { return binop(Op::AShr, a, b); }
    NodeId lshr(NodeId a, NodeId b) { return binop(Op::LShr, a, b); }
    NodeId cmp(Op op, NodeId a, NodeId b);
    NodeId neg(NodeId a);
    NodeId bit_not(NodeId a);
    NodeId select(NodeId c, NodeId a, NodeId b);

    // effectful ops (state strand)
    NodeId load(NodeId ptr, NodeId st, Type t);
    NodeId store(NodeId ptr, NodeId val, NodeId st);
    // read-only load (no state token): pure, hash-consed (loads of the same address
    // CSE). Sound for memory that does not change during the call (e.g. input arrays).
    NodeId pure_load(NodeId ptr, Type t);

    // params and regions
    NodeId param(Type t, int index, std::string name = "");

    // functions
    NodeId begin_func(const std::string& name, std::vector<Type> param_types,
                      Type result_type, bool has_state = false, bool is_comptime = false);
    void end_func(NodeId func, NodeId result, NodeId state_result = NONE);
    FuncInfo& func_info(NodeId f) { return funcs_[node(f).region]; }
    const FuncInfo& func_info(NodeId f) const { return funcs_[node(f).region]; }

    // conditionals (predicate selects case; yields one value per case)
    NodeId make_cond(NodeId predicate, Type result_type, std::vector<NodeId> yields);
    const CondInfo& cond_info(NodeId c) const { return conds_[node(c).region]; }

    // loops
    NodeId make_loop(std::vector<NodeId> inits, Type result_type, std::vector<NodeId> params,
                     NodeId is_break, NodeId break_val, std::vector<NodeId> next_vals);
    const LoopInfo& loop_info(NodeId l) const { return loops_[node(l).region]; }

    // calls (direct call of a Func node)
    NodeId call(NodeId func, std::vector<NodeId> args, NodeId st = NONE);

    // module
    void add_func(NodeId f) { module_funcs_.push_back(f); }
    const std::vector<NodeId>& module_funcs() const { return module_funcs_; }
    NodeId find_func(const std::string& name) const;

    // stats
    NodeId interned_hits() const { return intern_hits_; }

private:
    std::vector<Node> nodes_;
    std::vector<std::string> names_;
    std::vector<FuncInfo> funcs_;
    std::vector<CondInfo> conds_;
    std::vector<LoopInfo> loops_;
    std::vector<NodeId> module_funcs_;
    std::unordered_map<uint64_t, std::vector<NodeId>> intern_;
    NodeId intern_hits_ = 0;

    NodeId fresh(Node n, std::string name = "");
    NodeId intern(Node n);  // hash-cons structural nodes
    static uint64_t hash_node(const Node& n);
    bool node_eq(const Node& a, const Node& b) const;
    int64_t trunc(int64_t v, Type t) const;  // normalize to type width
    std::optional<int64_t> fold_binop(Op op, int64_t a, int64_t b, Type t, bool& ok) const;
};

}  // namespace helix
