#include "helix/eval.hpp"

#include <unordered_map>

namespace helix {

namespace {

struct Interp {
    World& w;
    long fuel;
    bool out_of_fuel = false;

    explicit Interp(World& world, long f) : w(world), fuel(f) {}

    struct Frame {
        std::unordered_map<NodeId, int64_t> bind;  // Param -> value (incl. loop carried)
        std::unordered_map<NodeId, int64_t> memo;  // pure-node cache (valid while bind fixed)
    };

    bool spend() {
        if (--fuel < 0) { out_of_fuel = true; return false; }
        return true;
    }

    int64_t eval(NodeId id, Frame& fr) {
        if (out_of_fuel) return 0;
        const Node& n = w.node(id);
        switch (n.op) {
            case Op::ConstInt:
            case Op::ConstBool:
                return n.imm;
            case Op::Param: {
                auto it = fr.bind.find(id);
                return it == fr.bind.end() ? 0 : it->second;
            }
            default:
                break;
        }

        // memoize pure structural nodes within the current (fixed-binding) frame
        const bool memoizable = (n.state_in == NONE) &&
                                (n.op >= Op::Add && n.op <= Op::Select);
        if (memoizable) {
            auto it = fr.memo.find(id);
            if (it != fr.memo.end()) return it->second;
        }

        int64_t result = 0;
        switch (n.op) {
            case Op::Add: case Op::Sub: case Op::Mul: case Op::SDiv: case Op::SRem:
            case Op::And: case Op::Or: case Op::Xor: case Op::Shl: case Op::AShr:
            case Op::LShr: {
                int64_t a = eval(n.ins[0], fr), b = eval(n.ins[1], fr);
                result = arith(n.op, a, b, n.type);
                break;
            }
            case Op::Neg: result = trunc((int64_t)(0ull - (uint64_t)eval(n.ins[0], fr)), n.type); break;
            case Op::Not: result = trunc(~eval(n.ins[0], fr), n.type); break;
            case Op::CmpEq: case Op::CmpNe: case Op::CmpLt: case Op::CmpLe:
            case Op::CmpGt: case Op::CmpGe: {
                int64_t a = eval(n.ins[0], fr), b = eval(n.ins[1], fr);
                result = compare(n.op, a, b) ? 1 : 0;
                break;
            }
            case Op::Select: {
                int64_t c = eval(n.ins[0], fr);
                result = c ? eval(n.ins[1], fr) : eval(n.ins[2], fr);
                break;
            }
            case Op::Cond: {
                int64_t p = eval(n.ins[0], fr);
                const CondInfo& ci = w.cond_info(id);
                if (ci.yields.size() == 2) {  // binary cond: truthiness (matches backend/folder)
                    result = eval(ci.yields[p ? 1 : 0], fr);
                } else if (p < 0 || p >= (int64_t)ci.yields.size()) {
                    result = 0;
                } else {
                    result = eval(ci.yields[(size_t)p], fr);  // n-ary: case index
                }
                break;
            }
            case Op::Loop:
                result = eval_loop(id, fr);
                break;
            case Op::Call:
                result = eval_call(id, fr);
                break;
            case Op::Load: {  // read-only load via a real pointer
                int64_t addr = eval(n.ins[0], fr);
                result = *reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(addr));
                break;
            }
            default:
                result = 0;
                break;
        }

        if (out_of_fuel) return 0;
        if (memoizable) fr.memo[id] = result;
        return result;
    }

    int64_t eval_loop(NodeId id, Frame& fr) {
        const Node& n = w.node(id);
        const LoopInfo& li = w.loop_info(id);
        std::vector<int64_t> cur;
        cur.reserve(n.ins.size());
        for (NodeId init : n.ins) cur.push_back(eval(init, fr));
        if (out_of_fuel) return 0;

        for (;;) {
            if (!spend()) return 0;
            for (size_t k = 0; k < li.params.size(); k++) fr.bind[li.params[k]] = cur[k];
            fr.memo.clear();  // carried values changed -> invalidate pure cache

            int64_t brk = eval(li.is_break, fr);
            if (out_of_fuel) return 0;
            if (brk) return eval(li.break_val, fr);

            std::vector<int64_t> nxt;
            nxt.reserve(li.next_vals.size());
            for (NodeId nv : li.next_vals) nxt.push_back(eval(nv, fr));
            if (out_of_fuel) return 0;
            cur = std::move(nxt);
        }
    }

    int64_t eval_call(NodeId id, Frame& fr) {
        const Node& n = w.node(id);
        NodeId target = (NodeId)n.imm;
        std::vector<int64_t> args;
        args.reserve(n.ins.size());
        for (NodeId a : n.ins) args.push_back(eval(a, fr));
        if (out_of_fuel) return 0;
        return run(target, args);
    }

    int64_t run(NodeId func, const std::vector<int64_t>& args) {
        if (!spend()) return 0;  // bound recursion depth/count
        const FuncInfo& fi = w.func_info(func);
        if (fi.result == NONE) { out_of_fuel = true; return 0; }  // unfinalized body: error, not 0
        Frame fr;
        for (size_t i = 0; i < fi.params.size() && i < args.size(); i++)
            fr.bind[fi.params[i]] = trunc(args[i], fi.param_types[i]);
        return eval(fi.result, fr);
    }

    int64_t trunc(int64_t v, Type t) const {
        if (t.kind == TyKind::Bool) return v & 1;
        if (t.kind != TyKind::Int || t.bits >= 64) return v;
        const uint64_t mask = (uint64_t(1) << t.bits) - 1;
        uint64_t u = (uint64_t)v & mask;
        const uint64_t sign = uint64_t(1) << (t.bits - 1);
        if (u & sign) u |= ~mask;
        return (int64_t)u;
    }

    int64_t arith(Op op, int64_t a, int64_t b, Type t) const {
        switch (op) {
            case Op::Add: return trunc((int64_t)((uint64_t)a + (uint64_t)b), t);
            case Op::Sub: return trunc((int64_t)((uint64_t)a - (uint64_t)b), t);
            case Op::Mul: return trunc((int64_t)((uint64_t)a * (uint64_t)b), t);
            case Op::SDiv:
                if (b == 0) return 0;  // div-by-zero: defined as 0 in the reference
                if (a == INT64_MIN && b == -1) return trunc(a, t);
                return trunc(a / b, t);
            case Op::SRem:
                if (b == 0) return 0;
                if (a == INT64_MIN && b == -1) return 0;
                return trunc(a % b, t);
            case Op::And: return trunc(a & b, t);
            case Op::Or: return trunc(a | b, t);
            case Op::Xor: return trunc(a ^ b, t);
            case Op::Shl: return trunc((int64_t)((uint64_t)a << (b & 63)), t);
            case Op::AShr: return trunc(a >> (b & 63), t);
            case Op::LShr: return trunc((int64_t)((uint64_t)a >> (b & 63)), t);
            default: return 0;
        }
    }

    bool compare(Op op, int64_t a, int64_t b) const {
        switch (op) {
            case Op::CmpEq: return a == b;
            case Op::CmpNe: return a != b;
            case Op::CmpLt: return a < b;
            case Op::CmpLe: return a <= b;
            case Op::CmpGt: return a > b;
            case Op::CmpGe: return a >= b;
            default: return false;
        }
    }
};

}  // namespace

EvalResult eval_func(World& w, NodeId func, const std::vector<int64_t>& args, long fuel) {
    Interp ip(w, fuel);
    int64_t v = ip.run(func, args);
    EvalResult r;
    r.out_of_fuel = ip.out_of_fuel;
    r.ok = !ip.out_of_fuel;
    r.value = v;
    r.fuel_used = fuel - ip.fuel;
    return r;
}

}  // namespace helix
