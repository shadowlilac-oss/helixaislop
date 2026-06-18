// Minimal but correct x86-64 encoder for the Helix backend. Emits real machine
// bytes (Win64 ABI). Supports exactly the instruction forms the code generator
// needs: reg/reg ALU, reg<->[rbp+disp32] moves, movabs, shifts, idiv, setcc,
// cmov, structured jumps with label fixups, and indirect call. See wiki/17-codegen.md.
#pragma once
#include <cstdint>
#include <vector>

namespace helix {

enum Reg : uint8_t {
    RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
    R8 = 8, R9 = 9, R10 = 10, R11 = 11
};

// Condition codes (encode directly into setcc/jcc/cmovcc opcode low nibble).
enum CC : uint8_t { CC_E = 0x4, CC_NE = 0x5, CC_L = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G = 0xF };

enum class Alu : uint8_t { Add, Sub, And, Or, Xor, Cmp };
enum class Shift : uint8_t { Shl, Sar, Shr };

using Label = uint32_t;

class Asm {
public:
    std::vector<uint8_t>& bytes() { return code_; }
    size_t size() const { return code_.size(); }

    // ---- prologue / epilogue ----
    void push_rbp() { emit(0x55); }
    void pop_rbp() { emit(0x5D); }
    void mov_rbp_rsp() { emit3(0x48, 0x89, 0xE5); }
    void mov_rsp_rbp() { emit3(0x48, 0x89, 0xEC); }
    void sub_rsp(int32_t imm) { emit3(0x48, 0x81, 0xEC); emit_i32(imm); }
    void add_rsp(int32_t imm) { emit3(0x48, 0x81, 0xC4); emit_i32(imm); }
    void ret() { emit(0xC3); }

    // ---- moves ----
    void mov_ri(Reg r, int64_t imm) {  // movabs r, imm64
        rex(true, false, r >= 8);
        emit((uint8_t)(0xB8 + (r & 7)));
        for (int i = 0; i < 8; i++) emit((uint8_t)((uint64_t)imm >> (8 * i)));
    }
    void load(Reg dst, int32_t disp) { mem_rbp(0x8B, dst, disp); }   // mov dst,[rbp+disp]
    void store(int32_t disp, Reg src) { mem_rbp(0x89, src, disp); }  // mov [rbp+disp],src
    void mov_rr(Reg dst, Reg src) { rr(0x89, src, dst); }            // 89: rm<-reg

    // ---- ALU reg,reg (dst OP= src); Cmp just sets flags ----
    void alu(Alu k, Reg dst, Reg src) {
        uint8_t op = 0;
        switch (k) {
            case Alu::Add: op = 0x01; break;
            case Alu::Sub: op = 0x29; break;
            case Alu::And: op = 0x21; break;
            case Alu::Or: op = 0x09; break;
            case Alu::Xor: op = 0x31; break;
            case Alu::Cmp: op = 0x39; break;
        }
        rr(op, src, dst);  // these forms are: r/m(dst) OP= reg(src)
    }
    void imul(Reg dst, Reg src) {  // 0F AF /r : reg(dst) *= r/m(src)
        rex(true, dst >= 8, src >= 8);
        emit(0x0F); emit(0xAF);
        emit((uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7)));
    }
    void shift(Shift k, Reg dst /*count in CL*/) {
        uint8_t ext = k == Shift::Shl ? 4 : (k == Shift::Sar ? 7 : 5);
        rex(true, false, dst >= 8);
        emit(0xD3);
        emit((uint8_t)(0xC0 | (ext << 3) | (dst & 7)));
    }
    void neg(Reg r) { unary(0xF7, 3, r); }
    void not_(Reg r) { unary(0xF7, 2, r); }
    void idiv(Reg r) { unary(0xF7, 7, r); }
    void cqo() { emit(0x48); emit(0x99); }

    void test_rr(Reg a, Reg b) { rr(0x85, b, a); }
    void setcc(CC cc) {  // setcc AL
        emit(0x0F); emit((uint8_t)(0x90 + cc)); emit(0xC0);
    }
    void movzx_al(Reg dst) {  // movzx dst, al
        rex(true, dst >= 8, false);
        emit(0x0F); emit(0xB6);
        emit((uint8_t)(0xC0 | ((dst & 7) << 3) | 0 /*al*/));
    }
    void cmovcc(CC cc, Reg dst, Reg src) {  // reg(dst) <- r/m(src) if cc
        rex(true, dst >= 8, src >= 8);
        emit(0x0F); emit((uint8_t)(0x40 + cc));
        emit((uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7)));
    }

    // ---- control flow ----
    Label new_label() { labels_.push_back(-1); return (Label)labels_.size() - 1; }
    void bind(Label l) { labels_[l] = (int64_t)code_.size(); }
    void jmp(Label l) { emit(0xE9); add_fixup(l); }
    void jcc(CC cc, Label l) { emit(0x0F); emit((uint8_t)(0x80 + cc)); add_fixup(l); }
    void call_reg(Reg r) {  // FF /2
        if (r >= 8) emit(0x41);
        emit(0xFF);
        emit((uint8_t)(0xD0 | (r & 7)));
    }

    // Resolve all jmp/jcc rel32 fixups against bound labels. Call once at end.
    void finalize() {
        for (auto& f : fixups_) {
            int64_t target = labels_[f.label];
            int64_t rel = target - (int64_t)(f.pos + 4);
            int32_t r32 = (int32_t)rel;
            for (int i = 0; i < 4; i++) code_[f.pos + i] = (uint8_t)((uint32_t)r32 >> (8 * i));
        }
    }

private:
    struct Fixup { size_t pos; Label label; };
    std::vector<uint8_t> code_;
    std::vector<int64_t> labels_;
    std::vector<Fixup> fixups_;

    void emit(uint8_t b) { code_.push_back(b); }
    void emit3(uint8_t a, uint8_t b, uint8_t c) { emit(a); emit(b); emit(c); }
    void emit_i32(int32_t v) { for (int i = 0; i < 4; i++) emit((uint8_t)((uint32_t)v >> (8 * i))); }
    void rex(bool w, bool r, bool b) {
        uint8_t v = 0x40 | (w ? 8 : 0) | (r ? 4 : 0) | (b ? 1 : 0);
        if (v != 0x40) emit(v);  // omit no-op REX unless needed
        else if (w) emit(0x48);
    }
    void rr(uint8_t op, Reg reg, Reg rm) {  // ModRM mod=11, reg, rm
        rex(true, reg >= 8, rm >= 8);
        emit(op);
        emit((uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
    }
    void unary(uint8_t op, uint8_t ext, Reg rm) {
        rex(true, false, rm >= 8);
        emit(op);
        emit((uint8_t)(0xC0 | (ext << 3) | (rm & 7)));
    }
    void mem_rbp(uint8_t op, Reg reg, int32_t disp) {  // [rbp + disp32], mod=10 rm=101
        rex(true, reg >= 8, false);
        emit(op);
        emit((uint8_t)(0x80 | ((reg & 7) << 3) | 5));
        emit_i32(disp);
    }
    void add_fixup(Label l) { fixups_.push_back({code_.size(), l}); for (int i = 0; i < 4; i++) emit(0); }
};

}  // namespace helix
