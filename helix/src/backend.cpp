#include "helix/backend.hpp"

#include <stdexcept>
#include <unordered_set>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "helix/x64.hpp"

namespace helix {
namespace {

constexpr Reg kArgRegs[4] = {RCX, RDX, R8, R9};

struct CompileError {
    std::string msg;
};

// Generates one function's machine code into an Asm, using a memory-backed
// virtual-register model (every value has a home stack slot). Correct for
// arbitrary structured control flow and calls without global register pressure.
struct FuncGen {
    World& w;
    NodeId func;
    Asm a;
    std::unordered_map<NodeId, int32_t> slot_;  // node -> [rbp+disp]
    std::unordered_set<NodeId> computed_;
    std::unordered_set<NodeId> multi_emitted_;                       // multi-result loops emitted
    std::unordered_map<NodeId, std::vector<int32_t>> loop_result_slots_;  // loop -> result slots
    std::vector<std::pair<size_t, NodeId>> call_relocs;  // (imm offset, target func)
    int slot_count = 0;
    size_t sub_rsp_imm_pos = 0;

    FuncGen(World& world, NodeId f) : w(world), func(f) {}

    int32_t slot(NodeId v) {
        auto it = slot_.find(v);
        if (it != slot_.end()) return it->second;
        int32_t d = -8 * (++slot_count);
        slot_[v] = d;
        return d;
    }
    int32_t anon_slot() { return -8 * (++slot_count); }

    // Sign-extend a narrow integer result in `r` to its declared width (so i32
    // wraps like the interpreter's trunc()). i64/bool need no fixup.
    void narrow(Reg r, Type t) {
        if (t.kind == TyKind::Int && t.bits == 32) a.movsxd(r, r);
    }

    void run() {
        const FuncInfo& fi = w.func_info(func);
        if (fi.params.size() > 4) throw CompileError{"more than 4 params unsupported"};

        a.push_rbp();
        a.mov_rbp_rsp();
        a.sub_rsp(0);  // placeholder; patched after we know slot_count
        sub_rsp_imm_pos = a.size() - 4;

        // store incoming arg registers into param slots (sign-extending narrow ints)
        for (size_t i = 0; i < fi.params.size(); i++) {
            if (fi.param_types[i].kind == TyKind::Int && fi.param_types[i].bits == 32)
                a.movsxd(kArgRegs[i], kArgRegs[i]);
            a.store(slot(fi.params[i]), kArgRegs[i]);
            computed_.insert(fi.params[i]);
        }

        emit_value(fi.result);
        a.load(RAX, slot(fi.result));
        a.mov_rsp_rbp();
        a.pop_rbp();
        a.ret();

        // patch frame size = align16(slots*8 + 32 shadow)
        int32_t frame = (int32_t)(((slot_count * 8 + 32) + 15) & ~15);
        auto& b = a.bytes();
        for (int i = 0; i < 4; i++) b[sub_rsp_imm_pos + i] = (uint8_t)((uint32_t)frame >> (8 * i));
        a.finalize();
    }

    // Ensure value `v` is materialized in slot(v). Pure values may be recomputed
    // across control-flow boundaries (cleared `computed_`), which is correct.
    void emit_value(NodeId v) {
        if (computed_.count(v)) return;
        const Node& n = w.node(v);
        switch (n.op) {
            case Op::ConstInt:
            case Op::ConstBool:
                a.mov_ri(RAX, n.imm);
                a.store(slot(v), RAX);
                break;
            case Op::Param:
                // populated by entry / loop machinery; nothing to emit
                break;
            case Op::Add: bin_alu(v, Alu::Add); break;
            case Op::Sub: bin_alu(v, Alu::Sub); break;
            case Op::And: bin_alu(v, Alu::And); break;
            case Op::Or: bin_alu(v, Alu::Or); break;
            case Op::Xor: bin_alu(v, Alu::Xor); break;
            case Op::Mul: {
                emit_value(n.ins[0]); emit_value(n.ins[1]);
                a.load(RAX, slot(n.ins[0])); a.load(RCX, slot(n.ins[1]));
                a.imul(RAX, RCX); narrow(RAX, n.type); a.store(slot(v), RAX);
                break;
            }
            case Op::SDiv: case Op::SRem: {
                emit_value(n.ins[0]); emit_value(n.ins[1]);
                a.load(RAX, slot(n.ins[0]));   // dividend
                a.load(RCX, slot(n.ins[1]));   // divisor
                Label Lzero = a.new_label(), Lovf = a.new_label(), Ldiv = a.new_label(), Ldone = a.new_label();
                a.test_rr(RCX, RCX);
                a.jcc(CC_E, Lzero);            // divisor == 0 -> result 0 (matches interpreter)
                a.mov_ri(RDX, -1); a.alu(Alu::Cmp, RCX, RDX); a.jcc(CC_NE, Ldiv);
                a.mov_ri(RDX, INT64_MIN); a.alu(Alu::Cmp, RAX, RDX); a.jcc(CC_NE, Ldiv);
                a.jmp(Lovf);                   // INT64_MIN / -1 -> overflow (would trap #DE)
                a.bind(Ldiv);
                a.cqo(); a.idiv(RCX);
                { Reg r = (n.op == Op::SDiv) ? RAX : RDX; if (r != RAX) a.mov_rr(RAX, r); }
                narrow(RAX, n.type); a.store(slot(v), RAX); a.jmp(Ldone);
                a.bind(Lzero);
                a.mov_ri(RAX, 0); a.store(slot(v), RAX); a.jmp(Ldone);
                a.bind(Lovf);
                if (n.op == Op::SDiv) { narrow(RAX, n.type); a.store(slot(v), RAX); }  // RAX = INT64_MIN
                else { a.mov_ri(RAX, 0); a.store(slot(v), RAX); }
                a.bind(Ldone);
                break;
            }
            case Op::Shl: bin_shift(v, Shift::Shl); break;
            case Op::AShr: bin_shift(v, Shift::Sar); break;
            case Op::LShr: bin_shift(v, Shift::Shr); break;
            case Op::Neg:
                emit_value(n.ins[0]); a.load(RAX, slot(n.ins[0])); a.neg(RAX);
                narrow(RAX, n.type); a.store(slot(v), RAX);
                break;
            case Op::Not:
                emit_value(n.ins[0]); a.load(RAX, slot(n.ins[0])); a.not_(RAX);
                narrow(RAX, n.type); a.store(slot(v), RAX);
                break;
            case Op::CmpEq: cmp(v, CC_E); break;
            case Op::CmpNe: cmp(v, CC_NE); break;
            case Op::CmpLt: cmp(v, CC_L); break;
            case Op::CmpLe: cmp(v, CC_LE); break;
            case Op::CmpGt: cmp(v, CC_G); break;
            case Op::CmpGe: cmp(v, CC_GE); break;
            case Op::Select: {
                emit_value(n.ins[0]); emit_value(n.ins[1]); emit_value(n.ins[2]);
                a.load(RAX, slot(n.ins[2]));  // b (default)
                a.load(RCX, slot(n.ins[1]));  // a
                a.load(RDX, slot(n.ins[0]));  // cond
                a.test_rr(RDX, RDX);
                a.cmovcc(CC_NE, RAX, RCX);    // cond != 0 -> a
                a.store(slot(v), RAX);
                break;
            }
            case Op::Cond: emit_cond(v); break;
            case Op::Loop:
                if (!w.loop_info(v).break_vals.empty()) {
                    emit_loop_multi(v);
                    if (!loop_result_slots_[v].empty()) {
                        a.load(RAX, loop_result_slots_[v][0]);
                        a.store(slot(v), RAX);
                    }
                } else {
                    emit_loop(v);
                }
                break;
            case Op::Proj: {
                NodeId region = n.ins[0];
                emit_loop_multi(region);
                a.load(RAX, loop_result_slots_[region][(size_t)n.imm]);
                a.store(slot(v), RAX);
                break;
            }
            case Op::Call: emit_call(v); break;
            case Op::Load:  // read-only memory load: RAX = *address
                emit_value(n.ins[0]);
                a.load(RAX, slot(n.ins[0]));
                a.mov_from_mem(RAX, RAX);
                narrow(RAX, n.type);
                a.store(slot(v), RAX);
                break;
            default:
                throw CompileError{std::string("cannot lower op ") + op_name(n.op)};
        }
        computed_.insert(v);
    }

    void bin_alu(NodeId v, Alu k) {
        const Node& n = w.node(v);
        emit_value(n.ins[0]); emit_value(n.ins[1]);
        a.load(RAX, slot(n.ins[0]));
        a.load(RCX, slot(n.ins[1]));
        a.alu(k, RAX, RCX);
        narrow(RAX, n.type);
        a.store(slot(v), RAX);
    }
    void bin_shift(NodeId v, Shift k) {
        const Node& n = w.node(v);
        emit_value(n.ins[0]); emit_value(n.ins[1]);
        a.load(RAX, slot(n.ins[0]));
        a.load(RCX, slot(n.ins[1]));  // count in CL
        a.shift(k, RAX);
        narrow(RAX, n.type);
        a.store(slot(v), RAX);
    }
    void cmp(NodeId v, CC cc) {
        const Node& n = w.node(v);
        emit_value(n.ins[0]); emit_value(n.ins[1]);
        a.load(RAX, slot(n.ins[0]));
        a.load(RCX, slot(n.ins[1]));
        a.alu(Alu::Cmp, RAX, RCX);
        a.setcc(cc);
        a.movzx_al(RAX);
        a.store(slot(v), RAX);
    }

    void emit_cond(NodeId v) {
        const Node& n = w.node(v);
        const CondInfo& ci = w.cond_info(v);
        if (ci.yields.size() != 2) throw CompileError{"backend supports binary cond only"};
        emit_value(n.ins[0]);  // predicate
        a.load(RAX, slot(n.ins[0]));
        a.test_rr(RAX, RAX);
        Label els = a.new_label(), end = a.new_label();
        a.jcc(CC_E, els);  // pred == 0 -> else (yields[0])
        // then = yields[1]
        computed_.clear();
        emit_value(ci.yields[1]);
        a.load(RAX, slot(ci.yields[1]));
        a.store(slot(v), RAX);
        a.jmp(end);
        // else = yields[0]
        a.bind(els);
        computed_.clear();
        emit_value(ci.yields[0]);
        a.load(RAX, slot(ci.yields[0]));
        a.store(slot(v), RAX);
        a.bind(end);
        computed_.clear();
    }

    void emit_loop(NodeId v) {
        const Node& n = w.node(v);
        const LoopInfo& li = w.loop_info(v);
        const size_t k = li.params.size();
        // init carried params
        for (size_t i = 0; i < k; i++) {
            emit_value(n.ins[i]);
            a.load(RAX, slot(n.ins[i]));
            a.store(slot(li.params[i]), RAX);
            computed_.insert(li.params[i]);
        }
        // temp slots for parallel move
        std::vector<int32_t> temps(k);
        for (size_t i = 0; i < k; i++) temps[i] = anon_slot();

        Label top = a.new_label(), brk = a.new_label();
        a.bind(top);
        computed_.clear();
        for (size_t i = 0; i < k; i++) computed_.insert(li.params[i]);  // params live in slots

        emit_value(li.is_break);
        a.load(RAX, slot(li.is_break));
        a.test_rr(RAX, RAX);
        a.jcc(CC_NE, brk);  // is_break != 0 -> exit

        // compute all next values (reading current params), then parallel-move into params
        for (size_t i = 0; i < k; i++) {
            emit_value(li.next_vals[i]);
            a.load(RAX, slot(li.next_vals[i]));
            a.store(temps[i], RAX);
        }
        for (size_t i = 0; i < k; i++) {
            a.load(RAX, temps[i]);
            a.store(slot(li.params[i]), RAX);
        }
        a.jmp(top);

        a.bind(brk);
        computed_.clear();
        for (size_t i = 0; i < k; i++) computed_.insert(li.params[i]);
        emit_value(li.break_val);
        a.load(RAX, slot(li.break_val));
        a.store(slot(v), RAX);
        computed_.clear();
    }

    // Multi-result loop: emit once, leaving each break value in its own result slot.
    void emit_loop_multi(NodeId v) {
        if (multi_emitted_.count(v)) return;
        multi_emitted_.insert(v);
        const Node& n = w.node(v);
        const LoopInfo& li = w.loop_info(v);
        const size_t k = li.params.size();
        for (size_t i = 0; i < k; i++) {
            emit_value(n.ins[i]);
            a.load(RAX, slot(n.ins[i]));
            a.store(slot(li.params[i]), RAX);
            computed_.insert(li.params[i]);
        }
        std::vector<int32_t> temps(k);
        for (size_t i = 0; i < k; i++) temps[i] = anon_slot();

        Label top = a.new_label(), brk = a.new_label();
        a.bind(top);
        computed_.clear();
        for (size_t i = 0; i < k; i++) computed_.insert(li.params[i]);
        emit_value(li.is_break);
        a.load(RAX, slot(li.is_break));
        a.test_rr(RAX, RAX);
        a.jcc(CC_NE, brk);
        for (size_t i = 0; i < k; i++) {
            emit_value(li.next_vals[i]);
            a.load(RAX, slot(li.next_vals[i]));
            a.store(temps[i], RAX);
        }
        for (size_t i = 0; i < k; i++) { a.load(RAX, temps[i]); a.store(slot(li.params[i]), RAX); }
        a.jmp(top);

        a.bind(brk);
        computed_.clear();
        for (size_t i = 0; i < k; i++) computed_.insert(li.params[i]);
        std::vector<int32_t> rslots;
        for (NodeId bvn : li.break_vals) {
            emit_value(bvn);
            a.load(RAX, slot(bvn));
            int32_t rs = anon_slot();
            a.store(rs, RAX);
            rslots.push_back(rs);
        }
        computed_.clear();
        loop_result_slots_[v] = std::move(rslots);
    }

    void emit_call(NodeId v) {
        const Node& n = w.node(v);
        if (n.ins.size() > 4) throw CompileError{"more than 4 call args unsupported"};
        for (NodeId arg : n.ins) emit_value(arg);
        for (size_t i = 0; i < n.ins.size(); i++) a.load(kArgRegs[i], slot(n.ins[i]));
        // mov rax, <abs target placeholder>; record reloc; call rax
        size_t pos0 = a.size();
        a.mov_ri(RAX, 0);
        size_t imm_pos = pos0 + 1;  // REX(1) + opcode(1)? RAX needs REX.W=0x48 then B8 -> imm at +2
        // mov_ri for RAX emits 0x48,0xB8,imm8.. so imm starts at pos0+2
        imm_pos = pos0 + 2;
        call_relocs.push_back({imm_pos, (NodeId)n.imm});
        a.call_reg(RAX);
        a.store(slot(v), RAX);
    }
};

}  // namespace

JitModule::~JitModule() {
    if (mem_) VirtualFree(mem_, 0, MEM_RELEASE);
}
JitModule::JitModule(JitModule&& o) noexcept { *this = std::move(o); }
JitModule& JitModule::operator=(JitModule&& o) noexcept {
    if (this != &o) {
        if (mem_) VirtualFree(mem_, 0, MEM_RELEASE);
        mem_ = o.mem_; size_ = o.size_; entries_ = std::move(o.entries_);
        ok = o.ok; err = std::move(o.err);
        o.mem_ = nullptr; o.size_ = 0; o.ok = false;
    }
    return *this;
}

int64_t JitModule::call(NodeId func, const std::vector<int64_t>& args) const {
    auto it = entries_.find(func);
    if (it == entries_.end()) return 0;
    using Fn = int64_t (*)(int64_t, int64_t, int64_t, int64_t);
    Fn f = reinterpret_cast<Fn>((uint8_t*)mem_ + it->second);
    int64_t a0 = args.size() > 0 ? args[0] : 0;
    int64_t a1 = args.size() > 1 ? args[1] : 0;
    int64_t a2 = args.size() > 2 ? args[2] : 0;
    int64_t a3 = args.size() > 3 ? args[3] : 0;
    return f(a0, a1, a2, a3);
}

JitModule jit_compile(World& w) {
    JitModule mod;
    std::vector<FuncGen> gens;
    std::unordered_map<NodeId, size_t> func_index;  // func node -> index in gens

    try {
        for (NodeId f : w.module_funcs()) {
            func_index[f] = gens.size();
            gens.emplace_back(w, f);
            gens.back().run();
        }
    } catch (const CompileError& e) {
        mod.ok = false;
        mod.err = e.msg;
        return mod;
    }

    // lay out functions contiguously (16-byte aligned)
    std::vector<size_t> offsets(gens.size());
    size_t total = 0;
    for (size_t i = 0; i < gens.size(); i++) {
        total = (total + 15) & ~size_t(15);
        offsets[i] = total;
        total += gens[i].a.size();
    }
    if (total == 0) total = 16;

    void* mem = VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { mod.ok = false; mod.err = "VirtualAlloc failed"; return mod; }
    uint8_t* base = (uint8_t*)mem;

    for (size_t i = 0; i < gens.size(); i++) {
        const auto& b = gens[i].a.bytes();
        memcpy(base + offsets[i], b.data(), b.size());
    }
    // patch internal call relocations with absolute target addresses
    for (size_t i = 0; i < gens.size(); i++) {
        for (auto& rel : gens[i].call_relocs) {
            size_t tgt_idx = func_index[rel.second];
            uint64_t abs = (uint64_t)(base + offsets[tgt_idx]);
            uint8_t* site = base + offsets[i] + rel.first;
            for (int k = 0; k < 8; k++) site[k] = (uint8_t)(abs >> (8 * k));
        }
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

}  // namespace helix
