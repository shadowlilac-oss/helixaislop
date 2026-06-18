// Independent x86-64 disassembler for the subset Helix's encoder emits. Used as
// a SECOND, independent verification path for the code generator: encode an
// instruction, disassemble the bytes, and check the mnemonic round-trips. This
// catches encoder bugs that differential execution alone might miss.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace helix {

struct DisInsn {
    size_t offset = 0;   // byte offset of this instruction
    size_t length = 0;   // encoded length in bytes
    std::string text;    // AT&T-free, Intel-style mnemonic, e.g. "add rax, rcx"
    bool valid = true;
};

// Decode `len` bytes of x86-64 machine code into a list of instructions.
// Must handle exactly the forms produced by helix::Asm (x64.hpp): movabs,
// mov reg/[rbp+disp32], reg/reg ALU, imul, shifts, idiv/neg/not, cqo, cmp,
// setcc, movzx, cmov, test, jcc/jmp rel32, call reg, push/pop rbp, ret,
// sub/add rsp imm32. REX prefixes and r8-r15 must be decoded.
std::vector<DisInsn> disassemble_x64(const uint8_t* code, size_t len);

}  // namespace helix
