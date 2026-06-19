// Helix optimizing backend: lower the graph to VCode, run liveness + linear-scan
// register allocation (values live in callee-saved registers so they survive
// calls; stack spills under pressure), then encode x86-64 directly. Validated
// against the interpreter oracle. See wiki/17-codegen.md.
#include <algorithm>
#include <climits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "helix/backend.hpp"
#include "helix/vcode.hpp"
#include "helix/x64.hpp"

namespace helix {

bool mop_is_terminator(MOp op) {
    return op == MOp::Ret || op == MOp::Jmp || op == MOp::Br;
}

namespace {

struct CompileError {
    std::string msg;
};

constexpr Reg kArgRegs[4] = {RCX, RDX, R8, R9};
// Allocatable registers: callee-saved, so any value held in one survives a call.
constexpr Reg kAllocRegs[] = {RBX, RSI, RDI, R12, R13, R14, R15};
constexpr int kNumAlloc = (int)(sizeof(kAllocRegs) / sizeof(kAllocRegs[0]));

// ===================== 1. Lowering: World func -> VFunc =====================
struct Lowerer {
    World& w;
    VFunc vf;
    int cur = 0;
    std::unordered_map<NodeId, VReg> cache;   // per-block value numbering
    std::unordered_map<NodeId, VReg> params;  // func + loop-carried params (persistent)

    Lowerer(World& world, NodeId f) : w(world) {
        const FuncInfo& fi = w.func_info(f);
        vf.name = fi.name;
        vf.node = f;
        vf.nparams = (int)fi.params.size();
        vf.param_types = fi.param_types;
    }

    VReg fresh() { return vf.nvregs++; }
    int new_block() { vf.blocks.emplace_back(); return (int)vf.blocks.size() - 1; }
    void set_block(int b) { cur = b; cache.clear(); }
    void emit(const MInst& m) { vf.blocks[cur].insns.push_back(m); }

    void run() {
        const FuncInfo& fi = w.func_info(vf.node);
        if (fi.params.size() > 4) throw CompileError{"more than 4 params unsupported"};
        cur = new_block();
        for (int i = 0; i < (int)fi.params.size(); i++) {
            VReg pv = fresh();
            params[fi.params[i]] = pv;
            MInst m; m.op = MOp::MovArg; m.dst = pv; m.imm = i; m.type = fi.param_types[i];
            emit(m);
        }
        VReg r = lower(fi.result);
        MInst ret; ret.op = MOp::Ret; ret.a = r; emit(ret);
    }

    VReg lower(NodeId v) {
        const Node& n = w.node(v);
        if (n.op == Op::Param) {
            auto it = params.find(v);
            return it != params.end() ? it->second : fresh();
        }
        auto ci = cache.find(v);
        if (ci != cache.end()) return ci->second;

        VReg r;
        switch (n.op) {
            case Op::ConstInt: case Op::ConstBool: {
                r = fresh();
                MInst m; m.op = MOp::MovImm; m.dst = r; m.imm = n.imm; m.type = n.type; emit(m);
                break;
            }
            case Op::Add: r = bin(MOp::Add, n); break;
            case Op::Sub: r = bin(MOp::Sub, n); break;
            case Op::Mul: r = bin(MOp::Mul, n); break;
            case Op::SDiv: r = bin(MOp::Div, n); break;
            case Op::SRem: r = bin(MOp::Rem, n); break;
            case Op::And: r = bin(MOp::And, n); break;
            case Op::Or: r = bin(MOp::Or, n); break;
            case Op::Xor: r = bin(MOp::Xor, n); break;
            case Op::Shl: r = bin(MOp::Shl, n); break;
            case Op::AShr: r = bin(MOp::Sar, n); break;
            case Op::LShr: r = bin(MOp::Shr, n); break;
            case Op::Neg: r = un(MOp::Neg, n); break;
            case Op::Not: r = un(MOp::Not, n); break;
            case Op::CmpEq: r = cmpv(CC_E, n); break;
            case Op::CmpNe: r = cmpv(CC_NE, n); break;
            case Op::CmpLt: r = cmpv(CC_L, n); break;
            case Op::CmpLe: r = cmpv(CC_LE, n); break;
            case Op::CmpGt: r = cmpv(CC_G, n); break;
            case Op::CmpGe: r = cmpv(CC_GE, n); break;
            case Op::Select: {
                VReg lc = lower(n.ins[0]), la = lower(n.ins[1]), lb = lower(n.ins[2]);
                r = fresh();
                MInst m; m.op = MOp::Sel; m.dst = r; m.a = la; m.b = lb; m.c = lc; m.type = n.type;
                emit(m);
                break;
            }
            case Op::Cond: r = lower_cond(v); break;
            case Op::Loop: r = lower_loop(v); break;
            case Op::Call: r = lower_call(v); break;
            case Op::Load: {
                VReg addr = lower(n.ins[0]);
                r = fresh();
                MInst m; m.op = MOp::Load; m.dst = r; m.a = addr; m.type = n.type; emit(m);
                break;
            }
            default: throw CompileError{std::string("cannot lower ") + op_name(n.op)};
        }
        cache[v] = r;
        return r;
    }

    VReg bin(MOp op, const Node& n) {
        VReg la = lower(n.ins[0]), lb = lower(n.ins[1]);
        VReg r = fresh();
        MInst m; m.op = op; m.dst = r; m.a = la; m.b = lb; m.type = n.type; emit(m);
        return r;
    }
    VReg un(MOp op, const Node& n) {
        VReg la = lower(n.ins[0]);
        VReg r = fresh();
        MInst m; m.op = op; m.dst = r; m.a = la; m.type = n.type; emit(m);
        return r;
    }
    VReg cmpv(CC cc, const Node& n) {
        VReg la = lower(n.ins[0]), lb = lower(n.ins[1]);
        VReg r = fresh();
        MInst m; m.op = MOp::SetCmp; m.dst = r; m.a = la; m.b = lb; m.cc = cc; m.type = ty_bool();
        emit(m);
        return r;
    }

    VReg lower_cond(NodeId v) {
        const Node& n = w.node(v);
        const CondInfo& ci = w.cond_info(v);
        if (ci.yields.size() != 2) throw CompileError{"binary cond only"};
        VReg pred = lower(n.ins[0]);
        int tb = new_block(), eb = new_block(), jb = new_block();
        MInst br; br.op = MOp::Br; br.a = pred; br.target = tb; br.target2 = eb; emit(br);
        VReg r = fresh();
        set_block(tb);
        VReg tv = lower(ci.yields[1]);
        emit_mov(r, tv, n.type);
        emit_jmp(jb);
        set_block(eb);
        VReg ev = lower(ci.yields[0]);
        emit_mov(r, ev, n.type);
        emit_jmp(jb);
        set_block(jb);
        cache[v] = r;
        return r;
    }

    VReg lower_loop(NodeId v) {
        const Node& n = w.node(v);
        const LoopInfo& li = w.loop_info(v);
        const int k = (int)li.params.size();
        std::vector<VReg> cv(k);
        for (int i = 0; i < k; i++) {
            VReg iv = lower(n.ins[i]);
            cv[i] = fresh();
            emit_mov(cv[i], iv, w.node(li.params[i]).type);
            params[li.params[i]] = cv[i];
        }
        int hb = new_block(), bb = new_block(), xb = new_block(), ab = new_block();
        emit_jmp(hb);
        set_block(hb);
        for (int i = 0; i < k; i++) params[li.params[i]] = cv[i];
        VReg bv = lower(li.is_break);
        { MInst br; br.op = MOp::Br; br.a = bv; br.target = xb; br.target2 = bb; emit(br); }
        set_block(bb);
        std::vector<VReg> nv(k);
        for (int i = 0; i < k; i++) nv[i] = lower(li.next_vals[i]);
        std::vector<VReg> tmp(k);
        for (int i = 0; i < k; i++) { tmp[i] = fresh(); emit_mov(tmp[i], nv[i], w.node(li.params[i]).type); }
        for (int i = 0; i < k; i++) emit_mov(cv[i], tmp[i], w.node(li.params[i]).type);
        emit_jmp(hb);
        set_block(xb);
        VReg r = fresh();
        VReg bvv = lower(li.break_val);
        emit_mov(r, bvv, n.type);
        emit_jmp(ab);
        set_block(ab);
        cache[v] = r;
        return r;
    }

    VReg lower_call(NodeId v) {
        const Node& n = w.node(v);
        if (n.ins.size() > 4) throw CompileError{"more than 4 call args unsupported"};
        std::vector<VReg> args;
        for (NodeId a : n.ins) args.push_back(lower(a));
        VReg r = fresh();
        MInst m; m.op = MOp::Call; m.dst = r; m.args = args; m.imm = (int64_t)n.imm; m.type = n.type;
        emit(m);
        return r;
    }

    void emit_mov(VReg dst, VReg src, Type t) { MInst m; m.op = MOp::Mov; m.dst = dst; m.a = src; m.type = t; emit(m); }
    void emit_jmp(int b) { MInst m; m.op = MOp::Jmp; m.target = b; emit(m); }
};

// ===================== 2. Liveness + linear-scan allocation =====================
struct Alloc {
    std::vector<int> reg;   // vreg -> alloc reg index (0..kNumAlloc-1) or -1
    std::vector<int> spill; // vreg -> spill slot or -1
    int num_spills = 0;
    std::vector<bool> reg_used;  // which alloc regs are used anywhere
};

void operands(const MInst& in, std::vector<VReg>& uses, VReg& def) {
    def = VNONE;
    switch (in.op) {
        case MOp::MovArg: def = in.dst; break;
        case MOp::MovImm: def = in.dst; break;
        case MOp::Mov: uses = {in.a}; def = in.dst; break;
        case MOp::Add: case MOp::Sub: case MOp::Mul: case MOp::Div: case MOp::Rem:
        case MOp::And: case MOp::Or: case MOp::Xor: case MOp::Shl: case MOp::Sar:
        case MOp::Shr: case MOp::SetCmp:
            uses = {in.a, in.b}; def = in.dst; break;
        case MOp::Neg: case MOp::Not: case MOp::Load: uses = {in.a}; def = in.dst; break;
        case MOp::Sel: uses = {in.a, in.b, in.c}; def = in.dst; break;
        case MOp::Call: uses = in.args; def = in.dst; break;
        case MOp::Ret: uses = {in.a}; break;
        case MOp::Br: uses = {in.a}; break;
        case MOp::Jmp: break;
    }
}

std::vector<int> successors(const VBlock& b) {
    const MInst& t = b.insns.back();
    if (t.op == MOp::Jmp) return {t.target};
    if (t.op == MOp::Br) return {t.target, t.target2};
    return {};
}

Alloc allocate(const VFunc& vf) {
    const int nb = (int)vf.blocks.size();
    std::vector<std::unordered_set<VReg>> gen(nb), kill(nb), live_in(nb), live_out(nb);
    for (int b = 0; b < nb; b++) {
        for (const MInst& in : vf.blocks[b].insns) {
            std::vector<VReg> u; VReg d; operands(in, u, d);
            for (VReg x : u) if (x != VNONE && !kill[b].count(x)) gen[b].insert(x);
            if (d != VNONE) kill[b].insert(d);
        }
    }
    // backward dataflow to fixpoint
    bool changed = true;
    while (changed) {
        changed = false;
        for (int b = nb - 1; b >= 0; b--) {
            std::unordered_set<VReg> out;
            for (int s : successors(vf.blocks[b])) out.insert(live_in[s].begin(), live_in[s].end());
            std::unordered_set<VReg> in = gen[b];
            for (VReg x : out) if (!kill[b].count(x)) in.insert(x);
            if (out.size() != live_out[b].size() || in.size() != live_in[b].size()) changed = true;
            live_out[b] = std::move(out);
            live_in[b] = std::move(in);
        }
    }

    // global instruction numbering + conservative [lo,hi] live intervals
    std::vector<int> lo(vf.nvregs, INT_MAX), hi(vf.nvregs, -1);
    auto touch = [&](VReg v, int i) { if (v == VNONE) return; lo[v] = std::min(lo[v], i); hi[v] = std::max(hi[v], i); };
    int idx = 0;
    std::vector<int> block_lo(nb), block_hi(nb);
    for (int b = 0; b < nb; b++) {
        block_lo[b] = idx;
        for (const MInst& in : vf.blocks[b].insns) {
            std::vector<VReg> u; VReg d; operands(in, u, d);
            for (VReg x : u) touch(x, idx);
            touch(d, idx);
            idx++;
        }
        block_hi[b] = idx;  // one past last
    }
    for (int b = 0; b < nb; b++) {
        for (VReg x : live_in[b]) touch(x, block_lo[b]);
        for (VReg x : live_out[b]) touch(x, block_hi[b]);
    }

    // linear scan
    Alloc al;
    al.reg.assign(vf.nvregs, -1);
    al.spill.assign(vf.nvregs, -1);
    al.reg_used.assign(kNumAlloc, false);
    std::vector<VReg> order;
    for (VReg v = 0; v < vf.nvregs; v++) if (hi[v] >= 0) order.push_back(v);
    std::sort(order.begin(), order.end(), [&](VReg x, VReg y) { return lo[x] < lo[y]; });

    std::vector<bool> free_reg(kNumAlloc, true);
    std::vector<VReg> active;  // sorted by hi ascending
    for (VReg v : order) {
        // expire
        active.erase(std::remove_if(active.begin(), active.end(), [&](VReg a) {
            if (hi[a] < lo[v]) { if (al.reg[a] >= 0) free_reg[al.reg[a]] = true; return true; }
            return false;
        }), active.end());

        int freeIdx = -1;
        for (int r = 0; r < kNumAlloc; r++) if (free_reg[r]) { freeIdx = r; break; }
        if (freeIdx >= 0) {
            al.reg[v] = freeIdx; free_reg[freeIdx] = false; al.reg_used[freeIdx] = true;
            active.push_back(v);
        } else {
            // spill the active interval with the largest hi (or v itself)
            VReg spillCand = v;
            for (VReg a : active) if (hi[a] > hi[spillCand]) spillCand = a;
            if (spillCand == v) {
                al.spill[v] = al.num_spills++;
            } else {
                al.reg[v] = al.reg[spillCand];
                al.reg_used[al.reg[v]] = true;
                al.spill[spillCand] = al.num_spills++;
                al.reg[spillCand] = -1;
                active.erase(std::remove(active.begin(), active.end(), spillCand), active.end());
                active.push_back(v);
            }
        }
        std::sort(active.begin(), active.end(), [&](VReg x, VReg y) { return hi[x] < hi[y]; });
    }
    return al;
}

// ===================== 3. Encoding: VFunc + Alloc -> x86-64 =====================
struct FnEncoder {
    const VFunc& vf;
    const Alloc& al;
    Asm a;
    std::vector<Label> blk;
    std::vector<std::pair<size_t, NodeId>> call_relocs;
    std::vector<int> saved_regs;       // alloc reg indices saved in prologue
    int num_saved = 0;
    size_t sub_rsp_imm_pos = 0;

    FnEncoder(const VFunc& f, const Alloc& a_) : vf(f), al(a_) {}

    int32_t spill_disp(int slot) const { return -8 * (num_saved + slot + 1); }
    Reg areg(int idx) const { return kAllocRegs[idx]; }

    bool in_reg(VReg v) const { return al.reg[v] >= 0; }
    void get(VReg v, Reg scratch) {  // load v's value into scratch
        if (al.reg[v] >= 0) { if (areg(al.reg[v]) != scratch) a.mov_rr(scratch, areg(al.reg[v])); }
        else a.load(scratch, spill_disp(al.spill[v]));
    }
    void put(VReg v, Reg scratch) {  // store scratch into v's home
        if (al.reg[v] >= 0) { if (areg(al.reg[v]) != scratch) a.mov_rr(areg(al.reg[v]), scratch); }
        else a.store(spill_disp(al.spill[v]), scratch);
    }
    void narrow(Reg r, Type t) { if (t.kind == TyKind::Int && t.bits == 32) a.movsxd(r, r); }

    void run() {
        for (int i = 0; i < (int)al.reg_used.size(); i++) if (al.reg_used[i]) saved_regs.push_back(i);
        num_saved = (int)saved_regs.size();
        for (int i = 0; i < (int)vf.blocks.size(); i++) blk.push_back(a.new_label());

        a.push_rbp();
        a.mov_rbp_rsp();
        a.sub_rsp(0);
        sub_rsp_imm_pos = a.size() - 4;
        for (int i = 0; i < num_saved; i++) a.store(-8 * (i + 1), areg(saved_regs[i]));

        for (int b = 0; b < (int)vf.blocks.size(); b++) {
            a.bind(blk[b]);
            for (const MInst& in : vf.blocks[b].insns) encode(in);
        }

        int32_t frame = (int32_t)(((num_saved + al.num_spills) * 8 + 32 + 15) & ~15);
        auto& by = a.bytes();
        for (int i = 0; i < 4; i++) by[sub_rsp_imm_pos + i] = (uint8_t)((uint32_t)frame >> (8 * i));
        a.finalize();
    }

    void epilogue() {
        for (int i = 0; i < num_saved; i++) a.load(areg(saved_regs[i]), -8 * (i + 1));
        a.mov_rsp_rbp();
        a.pop_rbp();
        a.ret();
    }

    // Where a vreg lives: a physical register, or a stack spill slot (as [rbp+disp]).
    struct Loc { bool inreg; Reg r; int32_t disp; };
    Loc loc(VReg v) const {
        if (al.reg[v] >= 0) return {true, areg(al.reg[v]), 0};
        return {false, RAX, spill_disp(al.spill[v])};
    }
    // dst OP= b, reading b directly as a register or [memory] operand (no scratch).
    void alu_v(Alu k, Reg dst, VReg b) {
        Loc l = loc(b);
        if (l.inreg) a.alu(k, dst, l.r); else a.alu_rm(k, dst, l.disp);
    }

    void encode(const MInst& in) {
        switch (in.op) {
            case MOp::MovArg: {
                Reg ar = kArgRegs[in.imm];
                if (in.type.kind == TyKind::Int && in.type.bits == 32) a.movsxd(ar, ar);
                put(in.dst, ar);
                break;
            }
            case MOp::MovImm: {
                Loc d = loc(in.dst);
                if (d.inreg) a.mov_ri(d.r, in.imm);
                else { a.mov_ri(RAX, in.imm); a.store(d.disp, RAX); }
                break;
            }
            case MOp::Mov: {
                Loc d = loc(in.dst);
                if (d.inreg) get(in.a, d.r);  // mov dst, a (elided when already coalesced)
                else { get(in.a, RAX); a.store(d.disp, RAX); }
                break;
            }
            case MOp::Add: bin2(in, Alu::Add); break;
            case MOp::Sub: bin2(in, Alu::Sub); break;
            case MOp::And: bin2(in, Alu::And); break;
            case MOp::Or: bin2(in, Alu::Or); break;
            case MOp::Xor: bin2(in, Alu::Xor); break;
            case MOp::Mul: {
                Loc d = loc(in.dst);
                Reg t = d.inreg ? d.r : RAX;
                get(in.a, t);
                Loc lb = loc(in.b);
                if (lb.inreg) a.imul(t, lb.r); else a.imul_rm(t, lb.disp);
                narrow(t, in.type);
                if (!d.inreg) a.store(d.disp, t);
                break;
            }
            case MOp::Shl: shift2(in, Shift::Shl); break;
            case MOp::Sar: shift2(in, Shift::Sar); break;
            case MOp::Shr: shift2(in, Shift::Shr); break;
            case MOp::Neg: un2(in, true); break;
            case MOp::Not: un2(in, false); break;
            case MOp::Div: case MOp::Rem: encode_div(in); break;
            case MOp::SetCmp:
                get(in.a, RAX); alu_v(Alu::Cmp, RAX, in.b);
                a.setcc(in.cc); a.movzx_al(RAX); put(in.dst, RAX);
                break;
            case MOp::Sel:
                get(in.b, RAX); get(in.a, RCX); get(in.c, RDX);
                a.test_rr(RDX, RDX); a.cmovcc(CC_NE, RAX, RCX); narrow(RAX, in.type); put(in.dst, RAX);
                break;
            case MOp::Load: {
                get(in.a, RAX);            // address into RAX
                Loc d = loc(in.dst);
                Reg t = d.inreg ? d.r : RAX;
                a.mov_from_mem(t, RAX);     // t = *address
                narrow(t, in.type);
                if (!d.inreg) a.store(d.disp, t);
                break;
            }
            case MOp::Call: encode_call(in); break;
            case MOp::Ret: get(in.a, RAX); epilogue(); break;
            case MOp::Jmp: a.jmp(blk[in.target]); break;
            case MOp::Br:
                get(in.a, RAX); a.test_rr(RAX, RAX); a.jcc(CC_NE, blk[in.target]); a.jmp(blk[in.target2]);
                break;
        }
    }

    // dst = a OP b  ->  compute into dst's register directly: mov dst,a ; op dst,b.
    void bin2(const MInst& in, Alu k) {
        Loc d = loc(in.dst);
        Reg t = d.inreg ? d.r : RAX;
        get(in.a, t);            // mov t, a  (elided if a is already in t)
        alu_v(k, t, in.b);       // t OP= b   (b is never in t: both are live here)
        narrow(t, in.type);
        if (!d.inreg) a.store(d.disp, t);
    }
    void shift2(const MInst& in, Shift k) {
        Loc d = loc(in.dst);
        Reg t = d.inreg ? d.r : RAX;
        get(in.a, t);
        get(in.b, RCX);          // count in CL (RCX is scratch, never an alloc reg)
        a.shift(k, t);
        narrow(t, in.type);
        if (!d.inreg) a.store(d.disp, t);
    }
    void un2(const MInst& in, bool isneg) {
        Loc d = loc(in.dst);
        Reg t = d.inreg ? d.r : RAX;
        get(in.a, t);
        if (isneg) a.neg(t); else a.not_(t);
        narrow(t, in.type);
        if (!d.inreg) a.store(d.disp, t);
    }
    void encode_div(const MInst& in) {
        get(in.a, RAX); get(in.b, RCX);
        Label Lz = a.new_label(), Lo = a.new_label(), Ld = a.new_label(), Ldone = a.new_label();
        a.test_rr(RCX, RCX); a.jcc(CC_E, Lz);
        a.mov_ri(RDX, -1); a.alu(Alu::Cmp, RCX, RDX); a.jcc(CC_NE, Ld);
        a.mov_ri(RDX, INT64_MIN); a.alu(Alu::Cmp, RAX, RDX); a.jcc(CC_NE, Ld);
        a.jmp(Lo);
        a.bind(Ld);
        a.cqo(); a.idiv(RCX);
        { Reg r = (in.op == MOp::Div) ? RAX : RDX; if (r != RAX) a.mov_rr(RAX, r); }
        narrow(RAX, in.type); put(in.dst, RAX); a.jmp(Ldone);
        a.bind(Lz);
        a.mov_ri(RAX, 0); put(in.dst, RAX); a.jmp(Ldone);
        a.bind(Lo);
        if (in.op == MOp::Div) { narrow(RAX, in.type); put(in.dst, RAX); }
        else { a.mov_ri(RAX, 0); put(in.dst, RAX); }
        a.bind(Ldone);
    }
    void encode_call(const MInst& in) {
        for (size_t i = 0; i < in.args.size(); i++) get(in.args[i], kArgRegs[i]);
        size_t pos0 = a.size();
        a.mov_ri(RAX, 0);
        call_relocs.push_back({pos0 + 2, (NodeId)in.imm});
        a.call_reg(RAX);
        put(in.dst, RAX);
    }
};

}  // namespace

struct EncodedFn {
    NodeId node;
    std::vector<uint8_t> code;
    std::vector<std::pair<size_t, NodeId>> relocs;
};

JitModule jit_compile_ra(World& w) {
    JitModule mod;
    std::vector<EncodedFn> outs;
    std::unordered_map<NodeId, size_t> func_index;

    try {
        for (NodeId f : w.module_funcs()) {
            Lowerer lo(w, f);
            lo.run();
            Alloc al = allocate(lo.vf);
            FnEncoder enc(lo.vf, al);
            enc.run();
            func_index[f] = outs.size();
            outs.push_back({f, std::move(enc.a.bytes()), std::move(enc.call_relocs)});
        }
    } catch (const CompileError& e) {
        mod.ok = false; mod.err = e.msg; return mod;
    }

    std::vector<size_t> offsets(outs.size());
    size_t total = 0;
    for (size_t i = 0; i < outs.size(); i++) {
        total = (total + 15) & ~size_t(15);
        offsets[i] = total;
        total += outs[i].code.size();
    }
    if (total == 0) total = 16;
    void* mem = VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { mod.ok = false; mod.err = "VirtualAlloc failed"; return mod; }
    uint8_t* base = (uint8_t*)mem;
    for (size_t i = 0; i < outs.size(); i++)
        memcpy(base + offsets[i], outs[i].code.data(), outs[i].code.size());
    for (size_t i = 0; i < outs.size(); i++)
        for (auto& rel : outs[i].relocs) {
            size_t ti = func_index[rel.second];
            uint64_t abs = (uint64_t)(base + offsets[ti]);
            uint8_t* site = base + offsets[i] + rel.first;
            for (int k = 0; k < 8; k++) site[k] = (uint8_t)(abs >> (8 * k));
        }
    DWORD old = 0;
    VirtualProtect(mem, total, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem, total);

    std::unordered_map<NodeId, size_t> entries;
    for (NodeId f : w.module_funcs()) entries[f] = offsets[func_index[f]];
    mod.adopt(mem, total, std::move(entries));
    mod.ok = true;
    return mod;
}

ObjModule compile_module_obj(World& w) {
    ObjModule obj;
    std::vector<EncodedFn> outs;
    std::unordered_map<NodeId, size_t> idx;
    try {
        for (NodeId f : w.module_funcs()) {
            Lowerer lo(w, f);
            lo.run();
            Alloc al = allocate(lo.vf);
            FnEncoder enc(lo.vf, al);
            enc.run();
            idx[f] = outs.size();
            outs.push_back({f, std::move(enc.a.bytes()), std::move(enc.call_relocs)});
        }
    } catch (const CompileError&) {
        return obj;  // empty text signals failure
    }
    std::vector<size_t> off(outs.size());
    size_t total = 0;
    for (size_t i = 0; i < outs.size(); i++) {
        total = (total + 15) & ~size_t(15);
        off[i] = total;
        total += outs[i].code.size();
    }
    obj.text.assign(total, 0);
    for (size_t i = 0; i < outs.size(); i++)
        memcpy(obj.text.data() + off[i], outs[i].code.data(), outs[i].code.size());
    for (NodeId f : w.module_funcs()) {
        CoffSymbol s;
        s.name = w.func_info(f).name;
        s.offset = (uint32_t)off[idx[f]];
        s.defined = true;
        s.is_function = true;
        obj.symbols.push_back(s);
    }
    for (size_t i = 0; i < outs.size(); i++)
        for (auto& rel : outs[i].relocs) {
            CoffReloc r;
            r.offset = (uint32_t)(off[i] + rel.first);
            r.target_symbol = w.func_info(rel.second).name;
            r.type = 1;  // IMAGE_REL_AMD64_ADDR64 (mov rax, <abs callee>)
            obj.relocs.push_back(r);
        }
    return obj;
}

}  // namespace helix
