#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS  // std::getenv (HELIX_SCHED_DEBUG diagnostic) is fine here
#endif
#include "helix/schedule.hpp"

#include <cstdio>
#include <cstdlib>
#include <unordered_set>

namespace helix {

namespace {

// Iterative post-order DFS over the value graph (operands + structural region fields),
// NOT descending into call targets (a Call's callee is external to this function).
// Post-order = every operand appears before the node that uses it (a valid topo order).
struct PostOrder {
    World& w;
    std::vector<NodeId> order;
    std::unordered_set<NodeId> done;

    explicit PostOrder(World& world) : w(world) {}

    // Push all graph children of `v` (the nodes `v` structurally depends on).
    static void children(World& w, NodeId v, std::vector<NodeId>& out) {
        const Node& n = w.node(v);
        for (NodeId in : n.ins) out.push_back(in);
        if (n.state_in != NONE) out.push_back(n.state_in);
        if (n.op == Op::Cond) {
            for (NodeId y : w.cond_info(v).yields) out.push_back(y);
        } else if (n.op == Op::Loop) {
            const LoopInfo& li = w.loop_info(v);
            for (NodeId p : li.params) out.push_back(p);
            if (li.is_break != NONE) out.push_back(li.is_break);
            if (li.break_val != NONE) out.push_back(li.break_val);
            for (NodeId bv : li.break_vals) out.push_back(bv);
            for (NodeId nv : li.next_vals) out.push_back(nv);
        }
    }

    void visit(NodeId root) {
        if (root == NONE || done.count(root)) return;
        // (node, child-cursor) explicit stack; emit node to `order` once all children done.
        std::vector<std::pair<NodeId, size_t>> stk;
        std::vector<std::vector<NodeId>> kids;
        std::unordered_set<NodeId> on_stack;
        auto push = [&](NodeId v) {
            if (v == NONE || done.count(v) || on_stack.count(v)) return;
            std::vector<NodeId> c;
            children(w, v, c);
            stk.push_back({v, 0});
            kids.push_back(std::move(c));
            on_stack.insert(v);
        };
        push(root);
        while (!stk.empty()) {
            auto& [v, ci] = stk.back();
            std::vector<NodeId>& c = kids.back();
            if (ci < c.size()) {
                NodeId child = c[ci++];
                push(child);
                continue;
            }
            order.push_back(v);
            done.insert(v);
            on_stack.erase(v);
            stk.pop_back();
            kids.pop_back();
        }
    }
};

}  // namespace

Schedule build_schedule(World& w, NodeId func) {
    const FuncInfo& fi = w.func_info(func);
    Schedule s;
    s.parent.push_back(-1);
    s.depth.push_back(0);  // region 0 = root

    auto new_region = [&](int par) -> int {
        int id = (int)s.parent.size();
        s.parent.push_back(par);
        s.depth.push_back(s.depth[par] + 1);
        return id;
    };
    auto lca = [&](int a, int b) -> int {
        if (a < 0) return b;
        if (b < 0) return a;
        while (s.depth[a] > s.depth[b]) a = s.parent[a];
        while (s.depth[b] > s.depth[a]) b = s.parent[b];
        while (a != b) { a = s.parent[a]; b = s.parent[b]; }
        return a;
    };

    // 1. Topological order (operands before users) over everything reachable.
    PostOrder po(w);
    po.visit(fi.result);
    if (fi.state_result != NONE) po.visit(fi.state_result);
    const std::vector<NodeId>& postorder = po.order;

    // 2. Assign a region to every node = LCA of its use sites. Process users before
    //    the nodes they use (reverse topo): then when a node is processed, every use
    //    has already folded its demanded region in, so the node's region is final.
    s.region[fi.result] = s.root;
    if (fi.state_result != NONE) s.region[fi.state_result] = s.root;

    auto demand = [&](NodeId d, int r) {
        if (d == NONE || w.node(d).op == Op::Param) return;  // params are bound, not placed
        auto it = s.region.find(d);
        if (it == s.region.end()) s.region[d] = r;
        else it->second = lca(it->second, r);
    };

    for (auto it = postorder.rbegin(); it != postorder.rend(); ++it) {
        NodeId u = *it;
        const Node& n = w.node(u);
        if (n.op == Op::Param) continue;
        int ru;
        {
            auto f = s.region.find(u);
            ru = (f != s.region.end()) ? f->second : s.root;  // unreached-as-operand => root
            s.region[u] = ru;
        }
        if (n.op == Op::Cond) {
            s.cond_arm0[u] = new_region(ru);
            s.cond_arm1[u] = new_region(ru);
            demand(n.ins[0], ru);  // predicate evaluated in the parent region
            const CondInfo& ci = w.cond_info(u);
            demand(ci.yields[0], s.cond_arm0[u]);
            demand(ci.yields[1], s.cond_arm1[u]);
        } else if (n.op == Op::Loop) {
            int body = new_region(ru);
            s.loop_body[u] = body;
            for (NodeId in : n.ins) demand(in, ru);  // inits computed in the parent region
            const LoopInfo& li = w.loop_info(u);
            demand(li.is_break, body);
            demand(li.break_val, body);
            for (NodeId bv : li.break_vals) demand(bv, body);
            for (NodeId nv : li.next_vals) demand(nv, body);
            // params are loop-bound defs, available throughout `body` — not placed.
        } else {
            for (NodeId in : n.ins) demand(in, ru);
            demand(n.state_in, ru);
        }
    }

    // 3. Within each loop body, every value needed to evaluate `is_break` and the
    //    break value(s) must be emitted BEFORE the exit test. Emitting the break_val
    //    Cond pre-test also emits its ARM contents, which may reference other body-region
    //    nodes (e.g. a shared constant) — so the cone must follow ALL dependency edges
    //    (operands AND cond yields / loop fields), not just operands, or such a node is
    //    bucketed into post[] and read before it is defined on the breaking iteration.
    //    Of the reachable nodes we mark only those assigned to THIS loop body region;
    //    nodes living in nested arm/body regions emit with their own region node.
    std::unordered_set<NodeId> is_pre;
    for (auto& kv : s.loop_body) {
        NodeId loop = kv.first;
        int body = kv.second;
        const LoopInfo& li = w.loop_info(loop);
        std::vector<NodeId> stk;
        auto push = [&](NodeId x) { if (x != NONE) stk.push_back(x); };
        push(li.is_break);
        push(li.break_val);
        for (NodeId bv : li.break_vals) push(bv);
        std::unordered_set<NodeId> seen;
        std::vector<NodeId> kids;
        while (!stk.empty()) {
            NodeId d = stk.back();
            stk.pop_back();
            if (!seen.insert(d).second) continue;
            auto rit = s.region.find(d);
            if (rit != s.region.end() && rit->second == body) is_pre.insert(d);
            kids.clear();
            PostOrder::children(w, d, kids);
            for (NodeId c : kids) push(c);
        }
    }

    // 4. Bucket nodes into pre[]/post[] per region, in topological order.
    s.pre.assign(s.region_count(), {});
    s.post.assign(s.region_count(), {});
    for (NodeId d : postorder) {
        const Node& nd = w.node(d);
        if (nd.op == Op::Param) continue;
        auto it = s.region.find(d);
        if (it == s.region.end()) continue;
        int r = it->second;
        if (is_pre.count(d)) s.pre[r].push_back(d);
        else s.post[r].push_back(d);
    }

    if (std::getenv("HELIX_SCHED_DEBUG")) {
        std::printf("[sched] func=%u regions=%d\n", func, s.region_count());
        for (int r = 0; r < s.region_count(); r++) {
            std::printf("  region %d (parent=%d depth=%d): pre=[", r, s.parent[r], s.depth[r]);
            for (NodeId d : s.pre[r]) std::printf("%u(%s) ", d, op_name(w.node(d).op));
            std::printf("] post=[");
            for (NodeId d : s.post[r]) std::printf("%u(%s) ", d, op_name(w.node(d).op));
            std::printf("]\n");
        }
        for (auto& kv : s.cond_arm0)
            std::printf("  cond %u: arm0=%d arm1=%d\n", kv.first, kv.second, s.cond_arm1[kv.first]);
        for (auto& kv : s.loop_body) std::printf("  loop %u: body=%d\n", kv.first, kv.second);
    }
    return s;
}

}  // namespace helix
