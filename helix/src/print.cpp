#include "helix/print.hpp"

#include <unordered_map>
#include <unordered_set>

namespace helix {
namespace {

struct Printer {
    World& w;
    std::string out;
    std::unordered_map<NodeId, std::string> id_;
    std::unordered_set<NodeId> printed_;
    int ctr_ = 0;
    int depth_ = 0;                          // ensure() recursion depth
    static constexpr int kMaxDepth = 4000;   // beyond this, elide rather than overflow

    explicit Printer(World& world) : w(world) {}

    struct Dep { int& d; explicit Dep(int& x) : d(x) { ++x; } ~Dep() { --d; } };

    std::string name(NodeId v) {
        auto it = id_.find(v);
        if (it != id_.end()) return it->second;
        std::string s = "%" + std::to_string(ctr_++);
        id_[v] = s;
        return s;
    }
    void preassign(NodeId v, const std::string& s) { id_[v] = s; }
    void ind(int n) { out.append((size_t)n * 2, ' '); }

    std::string ref(NodeId v) {
        const Node& n = w.node(v);
        if (n.op == Op::ConstInt) return std::to_string(n.imm);
        if (n.op == Op::ConstBool) return n.imm ? "true" : "false";
        return name(v);
    }

    void ensure(NodeId v, int level) {
        if (printed_.count(v)) return;
        Dep dp(depth_);
        if (depth_ > kMaxDepth) {  // diagnostic printer: elide instead of overflowing
            printed_.insert(v);
            ind(level);
            out += "/* ...elided (nesting too deep) */\n";
            return;
        }
        const Node& n = w.node(v);
        if (n.op == Op::ConstInt || n.op == Op::ConstBool || n.op == Op::Param) {
            printed_.insert(v);
            return;
        }
        switch (n.op) {
            case Op::Cond: {
                ensure(n.ins[0], level);
                const CondInfo& ci = w.cond_info(v);
                printed_.insert(v);
                ind(level);
                out += name(v) + " = cond " + ref(n.ins[0]) + " -> " + type_str(n.type) + " {\n";
                for (size_t k = 0; k < ci.yields.size(); k++) {
                    ind(level + 1);
                    out += "case " + std::to_string(k) + ": {\n";
                    ensure(ci.yields[k], level + 2);
                    ind(level + 2);
                    out += "yield " + ref(ci.yields[k]) + "\n";
                    ind(level + 1);
                    out += "}\n";
                }
                ind(level);
                out += "}\n";
                return;
            }
            case Op::Loop: {
                const LoopInfo& li = w.loop_info(v);
                for (NodeId in : n.ins) ensure(in, level);
                printed_.insert(v);
                // name the carried params
                ind(level);
                out += name(v) + " = loop (";
                for (size_t k = 0; k < li.params.size(); k++) {
                    if (k) out += ", ";
                    out += name(li.params[k]) + " = " + ref(n.ins[k]);
                }
                out += ") -> " + type_str(n.type) + " {\n";
                for (NodeId p : li.params) printed_.insert(p);
                ensure(li.is_break, level + 1);
                ensure(li.break_val, level + 1);
                ind(level + 1);
                out += "break if " + ref(li.is_break) + " -> " + ref(li.break_val) + "\n";
                for (NodeId nv : li.next_vals) ensure(nv, level + 1);
                ind(level + 1);
                out += "continue (";
                for (size_t k = 0; k < li.next_vals.size(); k++) {
                    if (k) out += ", ";
                    out += ref(li.next_vals[k]);
                }
                out += ")\n";
                ind(level);
                out += "}\n";
                return;
            }
            case Op::Call: {
                for (NodeId in : n.ins) ensure(in, level);
                printed_.insert(v);
                NodeId target = (NodeId)n.imm;
                ind(level);
                out += name(v) + " = call @" + w.func_info(target).name + "(";
                for (size_t k = 0; k < n.ins.size(); k++) {
                    if (k) out += ", ";
                    out += ref(n.ins[k]);
                }
                out += ") : " + type_str(n.type) + "\n";
                return;
            }
            default: {  // pure op
                for (NodeId in : n.ins) ensure(in, level);
                printed_.insert(v);
                ind(level);
                out += name(v) + " = " + op_name(n.op);
                for (size_t k = 0; k < n.ins.size(); k++) out += (k ? ", " : " ") + ref(n.ins[k]);
                out += " : " + type_str(n.type) + "\n";
                return;
            }
        }
    }

    void func(NodeId f) {
        const FuncInfo& fi = w.func_info(f);
        out += (fi.is_comptime ? "comptime fn @" : "fn @") + fi.name + "(";
        for (size_t i = 0; i < fi.params.size(); i++) {
            std::string pn = "%a" + std::to_string(i);
            preassign(fi.params[i], pn);
            if (i) out += ", ";
            out += pn + ": " + type_str(fi.param_types[i]);
        }
        out += ") -> " + type_str(fi.result_type) + " {\n";
        ensure(fi.result, 1);
        ind(1);
        out += "return " + ref(fi.result) + "\n";
        out += "}\n";
    }
};

}  // namespace

std::string print_func(World& w, NodeId func) {
    Printer p(w);
    p.func(func);
    return p.out;
}

std::string print_module(World& w) {
    std::string out;
    for (NodeId f : w.module_funcs()) {
        Printer p(w);
        p.func(f);
        out += p.out + "\n";
    }
    return out;
}

}  // namespace helix
