// Verify the independent components: the disassembler must decode exactly the
// bytes the encoder emits (second independent check on the code generator), and
// COFF objects must be byte-valid (proven separately by the link+run e2e test).
#include <string>

#include "helix/disasm.hpp"
#include "helix/x64.hpp"
#include "test.hpp"

using namespace helix;

TEST("disasm: decodes the full encoder instruction set with exact lengths") {
    Asm a;
    a.push_rbp(); a.mov_rbp_rsp(); a.sub_rsp(64);
    a.mov_ri(RAX, 42);
    a.mov_ri(R12, -1);
    a.mov_rr(RBX, RAX);
    a.load(RSI, -8);
    a.store(-16, R13);
    a.alu(Alu::Add, RAX, RCX);
    a.alu(Alu::Sub, RBX, RDX);
    a.alu(Alu::Cmp, RAX, RBX);
    a.imul(RAX, RCX);
    a.shift(Shift::Shl, RAX);
    a.neg(RAX); a.not_(RBX);
    a.cqo(); a.idiv(RCX);
    a.test_rr(RAX, RAX);
    a.setcc(CC_L); a.movzx_al(RAX);
    a.movsxd(RAX, RBX);
    a.movsx8(RAX, RCX); a.movsx16(RDX, RBX);            // narrow sign-extends (i8/i16)
    a.alu_ri(Alu::Add, RAX, 100); a.alu_ri(Alu::Cmp, RBX, -7);  // ALU r/m, imm32
    a.shift_ri(Shift::Shl, RAX, 3);                     // shift by constant
    a.imul_rri(RAX, RCX, 9);                            // imul r, r, imm32
    a.store_rsp(32, RAX); a.load_rsp(RDX, 40);          // outgoing/incoming stack args
    a.cmovcc(CC_NE, RAX, RCX);
    Label l = a.new_label();
    a.jcc(CC_E, l);
    a.jmp(l);
    a.bind(l);
    a.call_reg(RAX);
    a.mov_rsp_rbp(); a.pop_rbp(); a.ret();
    a.finalize();

    auto ins = disassemble_x64(a.bytes().data(), a.bytes().size());
    // Key integrity property: the decoder determines every instruction's exact
    // boundary, so the decoded lengths sum to the encoded size (no desync).
    size_t total = 0;
    int known = 0;
    for (auto& i : ins) { total += i.length; if (i.valid) known++; }
    CHECK_EQ((long)total, (long)a.bytes().size());
    CHECK_EQ(known, (int)ins.size());  // EVERY form the encoder emits is now decoded

    // Spot-check mnemonics are recognized (format-agnostic substring checks).
    auto has = [&](const char* m) {
        for (auto& i : ins) if (i.text.find(m) != std::string::npos) return true;
        return false;
    };
    for (const char* m : {"mov", "add", "imul", "ret", "call", "movsx", "shl", "cmp", "[rsp"})
        CHECK(has(m));
}
