#include "helix/verify.hpp"

#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace helix {
namespace {

// All node references reachable from `v` WITHIN one function body (does not
// descend into callees).
void successors(World& w, NodeId v, std::vector<NodeId>& out) {
    const Node& n = w.node(v);
    for (NodeId in : n.ins) out.push_back(in);
    if (n.state_in != NONE) out.push_back(n.state_in);
    if (n.op == Op::Cond) {
        for (NodeId y : w.cond_info(v).yields) out.push_back(y);
    } else if (n.op == Op::Loop) {
        const LoopInfo& li = w.loop_info(v);
        for (NodeId p : li.params) out.push_back(p);
        out.push_back(li.is_break);
        if (li.break_val != NONE) out.push_back(li.break_val);
        for (NodeId bv : li.break_vals) out.push_back(bv);
        for (NodeId nv : li.next_vals) out.push_back(nv);
    }
}

struct Verifier {
    World& w;
    VerifyResult res;
    std::unordered_set<NodeId> reached;
    std::unordered_set<NodeId> on_stack;

    explicit Verifier(World& world) : w(world) {}

    void fail(const std::string& m) { res.ok = false; res.errors.push_back(m); }

    // DFS: collect reachable set + detect cycles (Invariant A).
    void dfs(NodeId v) {
        if (reached.count(v)) return;
        if (on_stack.count(v)) { fail("cycle detected at node " + std::to_string(v)); return; }
        on_stack.insert(v);
        std::vector<NodeId> succ;
        successors(w, v, succ);
        for (NodeId s : succ) {
            if (s == NONE || s >= w.node_count()) { fail("dangling operand at node " + std::to_string(v)); continue; }
            dfs(s);
        }
        on_stack.erase(v);
        reached.insert(v);
    }

    void check_regions() {
        for (NodeId v : reached) {
            const Node& n = w.node(v);
            if (n.op == Op::Cond) {
                if (w.cond_info(v).yields.size() < 1) fail("cond with no cases");
            } else if (n.op == Op::Loop) {
                const LoopInfo& li = w.loop_info(v);
                if (li.params.size() != li.next_vals.size())
                    fail("loop carried/next arity mismatch");
                if (li.params.size() != n.ins.size())
                    fail("loop init/param arity mismatch");
            }
        }
    }

    // Linear state discipline (Invariant D): every state producer is consumed
    // exactly once (state_in edges), modulo the function's state_result.
    void check_linearity(NodeId func) {
        const FuncInfo& fi = w.func_info(func);
        std::unordered_map<NodeId, int> consumers;  // producer -> #consumers
        std::unordered_set<NodeId> producers;
        if (fi.state_param != NONE) producers.insert(fi.state_param);
        for (NodeId v : reached) {
            const Node& n = w.node(v);
            // Only *effectful* Load/Store (state_in set) produce a state token; a pure
            // read-only load has no state. A Call may consume state but yields none.
            if ((n.op == Op::Load || n.op == Op::Store) && n.state_in != NONE) producers.insert(v);
            if (n.state_in != NONE) consumers[n.state_in]++;
        }
        if (fi.state_result != NONE) consumers[fi.state_result]++;
        for (NodeId p : producers) {
            int c = consumers.count(p) ? consumers[p] : 0;
            if (c == 0) fail("state token " + std::to_string(p) + " is dropped (linearity)");
            else if (c > 1) fail("state token " + std::to_string(p) + " used " + std::to_string(c) +
                                 " times (linearity)");
        }
    }

    VerifyResult run(NodeId func) {
        const FuncInfo& fi = w.func_info(func);
        if (fi.result == NONE) { fail("function '" + fi.name + "' has no result"); return res; }
        dfs(fi.result);
        if (fi.state_result != NONE) dfs(fi.state_result);
        check_regions();
        check_linearity(func);
        return res;
    }
};

}  // namespace

VerifyResult verify_func(World& w, NodeId func) {
    Verifier v(w);
    return v.run(func);
}

VerifyResult verify_module(World& w) {
    VerifyResult all;
    for (NodeId f : w.module_funcs()) {
        VerifyResult r = verify_func(w, f);
        if (!r.ok) {
            all.ok = false;
            for (auto& e : r.errors) all.errors.push_back(w.func_info(f).name + ": " + e);
        }
    }
    return all;
}

size_t reachable_count(World& w, NodeId func) {
    Verifier v(w);
    const FuncInfo& fi = w.func_info(func);
    if (fi.result != NONE) v.dfs(fi.result);
    return v.reached.size();
}

}  // namespace helix
