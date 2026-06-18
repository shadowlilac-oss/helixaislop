// VCode — the low-level virtual-register machine IR the optimizing backend uses
// between instruction selection and register allocation. A VFunc is a CFG of
// basic blocks over unbounded virtual registers; register allocation rewrites
// vregs to physical registers / spill slots. See wiki/17-codegen.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "helix/ir.hpp"
#include "helix/x64.hpp"  // CC

namespace helix {

using VReg = int32_t;  // -1 == none
constexpr VReg VNONE = -1;

enum class MOp : uint8_t {
    MovArg,   // dst = incoming argument #imm
    MovImm,   // dst = imm
    Mov,      // dst = a
    Add, Sub, Mul, Div, Rem, And, Or, Xor, Shl, Sar, Shr,  // dst = a OP b
    Neg, Not,                                              // dst = OP a
    SetCmp,   // dst = (a cc b) ? 1 : 0
    Sel,      // dst = c ? a : b
    Call,     // dst = call imm(args...)
    // terminators (always last in a block)
    Ret,      // return a
    Jmp,      // -> target
    Br,       // if a != 0 -> target else target2
};

struct MInst {
    MOp op;
    VReg dst = VNONE;
    VReg a = VNONE, b = VNONE, c = VNONE;
    std::vector<VReg> args;          // Call arguments
    int64_t imm = 0;                 // MovImm value / MovArg index / Call target func node
    CC cc = CC_E;                    // SetCmp condition
    Type type;                       // result width (for narrowing)
    int target = -1, target2 = -1;   // successor block indices (Jmp/Br)
};

struct VBlock {
    std::vector<MInst> insns;        // terminator is the last instruction
};

struct VFunc {
    std::string name;
    NodeId node = NONE;
    int nparams = 0;
    std::vector<Type> param_types;
    std::vector<VBlock> blocks;
    VReg nvregs = 0;
};

bool mop_is_terminator(MOp op);

}  // namespace helix
