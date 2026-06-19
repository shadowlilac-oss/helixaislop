#include "helix/opt.hpp"

#include <unordered_map>
#include <unordered_set>

namespace helix {
namespace {

// Thrown when a clone recurses deeper than the C++ stack can safely take: the caller
// catches it and leaves that function unoptimized (correct, just not inlined).
struct CloneTooDeep {};

// Clones a value subgraph through the smart constructors (so the clone is folded
// / CSE'd), substituting per a node->node map, and inlining Call nodes up to a
// bounded depth.
struct Cloner {
    World& w;
    int rec_ = 0;                            // current clone recursion depth
    static constexpr int kMaxRec = 3000;     // < the C++ stack; trips CloneTooDeep
    Cloner(World& world) : w(world) {}

    NodeId clone(NodeId v, std::unordered_map<NodeId, NodeId>& map, int depth) {
        auto it = map.find(v);
        if (it != map.end()) return it->second;
        if (++rec_ > kMaxRec) throw CloneTooDeep{};
        // COPY by value: building new nodes below reallocates the World's arenas,
        // which would dangle any reference into them.
        const Node n = w.node(v);
        NodeId r;
        switch (n.op) {
            case Op::ConstInt: case Op::ConstBool:
                r = v;  // interned constants are shared
                break;
            case Op::Param:
                r = v;  // a param not in the map is free in this scope: keep it
                break;
            case Op::Add: case Op::Sub: case Op::Mul: case Op::SDiv: case Op::SRem:
            case Op::And: case Op::Or: case Op::Xor: case Op::Shl: case Op::AShr: case Op::LShr:
                r = w.binop(n.op, clone(n.ins[0], map, depth), clone(n.ins[1], map, depth));
                break;
            case Op::Neg: r = w.neg(clone(n.ins[0], map, depth)); break;
            case Op::Not: r = w.bit_not(clone(n.ins[0], map, depth)); break;
            case Op::CmpEq: case Op::CmpNe: case Op::CmpLt: case Op::CmpLe:
            case Op::CmpGt: case Op::CmpGe:
                r = w.cmp(n.op, clone(n.ins[0], map, depth), clone(n.ins[1], map, depth));
                break;
            case Op::Select:
                r = w.select(clone(n.ins[0], map, depth), clone(n.ins[1], map, depth),
                             clone(n.ins[2], map, depth));
                break;
            case Op::Cond: {
                const CondInfo ci = w.cond_info(v);  // copy: arenas will reallocate below
                std::vector<NodeId> ys;
                for (NodeId y : ci.yields) ys.push_back(clone(y, map, depth));
                r = w.make_cond(clone(n.ins[0], map, depth), n.type, std::move(ys));
                break;
            }
            case Op::Loop: {
                const LoopInfo li = w.loop_info(v);  // copy: arenas will reallocate below
                std::vector<NodeId> inits;
                for (NodeId in : n.ins) inits.push_back(clone(in, map, depth));
                // fresh carried params, mapped from the originals
                std::vector<NodeId> nps;
                for (size_t k = 0; k < li.params.size(); k++) {
                    NodeId np = w.param(w.node(li.params[k]).type, (int)k, w.name(li.params[k]));
                    map[li.params[k]] = np;
                    nps.push_back(np);
                }
                NodeId isb = clone(li.is_break, map, depth);
                std::vector<NodeId> nxt;
                for (NodeId nv : li.next_vals) nxt.push_back(clone(nv, map, depth));
                if (!li.break_vals.empty()) {
                    std::vector<NodeId> bvs;
                    for (NodeId bv : li.break_vals) bvs.push_back(clone(bv, map, depth));
                    r = w.make_loop_multi(std::move(inits), std::move(nps), isb, std::move(bvs), std::move(nxt));
                } else {
                    NodeId bvl = clone(li.break_val, map, depth);
                    r = w.make_loop(std::move(inits), n.type, std::move(nps), isb, bvl, std::move(nxt));
                }
                break;
            }
            case Op::Proj:
                r = w.proj(clone(n.ins[0], map, depth), (int)n.imm, n.type);
                break;
            case Op::Call: {
                std::vector<NodeId> args;
                for (NodeId a : n.ins) args.push_back(clone(a, map, depth));
                NodeId target = (NodeId)n.imm;
                const FuncInfo tfi = w.func_info(target);  // copy
                if (depth > 0 && tfi.result != NONE && !tfi.has_state && tfi.state_result == NONE) {
                    // inline: clone the callee body with its params bound to args
                    std::unordered_map<NodeId, NodeId> sub;
                    for (size_t i = 0; i < tfi.params.size() && i < args.size(); i++)
                        sub[tfi.params[i]] = args[i];
                    r = clone(tfi.result, sub, depth - 1);
                } else {
                    r = w.call(target, std::move(args));
                }
                break;
            }
            default:
                r = v;
                break;
        }
        map[v] = r;
        --rec_;
        return r;
    }
};

}  // namespace

NodeId inline_call(World& w, NodeId call_node, int depth) {
    const Node& n = w.node(call_node);
    if (n.op != Op::Call) return call_node;
    NodeId target = (NodeId)n.imm;
    const FuncInfo& tfi = w.func_info(target);
    if (tfi.result == NONE || tfi.has_state || tfi.state_result != NONE) return call_node;
    Cloner c(w);
    std::unordered_map<NodeId, NodeId> sub;
    for (size_t i = 0; i < tfi.params.size() && i < n.ins.size(); i++)
        sub[tfi.params[i]] = n.ins[i];
    try { return c.clone(tfi.result, sub, depth - 1); }
    catch (const CloneTooDeep&) { return call_node; }  // too deep: leave the call
}

void inline_into(World& w, NodeId func, int max_depth) {
    FuncInfo& fi = w.func_info(func);
    if (fi.result == NONE) return;
    Cloner c(w);
    std::unordered_map<NodeId, NodeId> map;
    for (NodeId p : fi.params) map[p] = p;  // keep the function's own params
    NodeId nr;
    try { nr = c.clone(fi.result, map, max_depth); }
    catch (const CloneTooDeep&) { return; }  // too deep: leave the function unoptimized
    w.func_info(func).result = nr;
}

void optimize_module(World& w, int inline_depth) {
    // Copy the function list: inline_into grows the World's arenas (but not this list).
    std::vector<NodeId> funcs(w.module_funcs().begin(), w.module_funcs().end());
    for (NodeId f : funcs) {
        const FuncInfo& fi = w.func_info(f);
        // Pure functions only: the Cloner does not clone Load/Store nodes (value strand
        // only) and inline_into does not update state_result, so skip stateful functions.
        if (fi.result != NONE && fi.state_result == NONE && !fi.has_state)
            inline_into(w, f, inline_depth);
    }
}

std::vector<NodeId> reachable_functions(World& w, const std::vector<NodeId>& roots) {
    std::unordered_set<NodeId> seen;
    std::vector<NodeId> order;
    std::vector<NodeId> stack;
    for (NodeId r : roots) if (seen.insert(r).second) { order.push_back(r); stack.push_back(r); }

    while (!stack.empty()) {
        NodeId f = stack.back(); stack.pop_back();
        const FuncInfo& fi = w.func_info(f);
        if (fi.result == NONE) continue;
        // walk the body collecting Call targets
        std::unordered_set<NodeId> vis;
        std::vector<NodeId> ns = {fi.result};
        if (fi.state_result != NONE) ns.push_back(fi.state_result);
        while (!ns.empty()) {
            NodeId v = ns.back(); ns.pop_back();
            if (!vis.insert(v).second) continue;
            const Node& n = w.node(v);
            if (n.op == Op::Call) {
                NodeId t = (NodeId)n.imm;
                if (seen.insert(t).second) { order.push_back(t); stack.push_back(t); }
            }
            for (NodeId in : n.ins) ns.push_back(in);
            if (n.state_in != NONE) ns.push_back(n.state_in);
            if (n.op == Op::Cond) for (NodeId y : w.cond_info(v).yields) ns.push_back(y);
            if (n.op == Op::Loop) {
                const LoopInfo& li = w.loop_info(v);
                for (NodeId p : li.params) ns.push_back(p);
                ns.push_back(li.is_break); ns.push_back(li.break_val);
                for (NodeId nv : li.next_vals) ns.push_back(nv);
            }
        }
    }
    return order;
}

}  // namespace helix
