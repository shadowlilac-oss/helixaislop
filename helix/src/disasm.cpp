// Independent x86-64 disassembler for the subset Helix's encoder emits.
// See include/helix/disasm.hpp and include/helix/x64.hpp.
//
// This decoder handles EXACTLY the instruction forms produced by helix::Asm:
//   - push/pop rbp (55 / 5D), ret (C3)
//   - mov rbp,rsp / mov rsp,rbp (48 89 E5 / 48 89 EC)
//   - sub/add rsp, imm32 (48 81 EC/C4 id)
//   - movabs r64, imm64 (REX.W B8+r io)
//   - mov reg, [rbp+disp32] (REX.W 8B /r, mod=10 rm=101)
//   - mov [rbp+disp32], reg (REX.W 89 /r, mod=10 rm=101)
//   - mov reg, reg (REX.W 89 /r, mod=11)
//   - ALU reg/reg: add 01, sub 29, and 21, or 09, xor 31, cmp 39 (mod=11)
//   - imul reg, reg (REX.W 0F AF /r, mod=11)
//   - shift by CL: shl/sar/shr (REX.W D3 /4,/7,/5, mod=11)
//   - neg/not/idiv (REX.W F7 /3,/2,/7, mod=11)
//   - cqo (48 99)
//   - test reg, reg (REX.W 85 /r, mod=11)
//   - setcc al (0F 90+cc C0)
//   - movzx reg, al (REX.W 0F B6 /r, rm=000=al, mod=11)
//   - cmovcc reg, reg (REX.W 0F 40+cc /r, mod=11)
//   - jcc rel32 (0F 80+cc cd), jmp rel32 (E9 cd)
//   - call reg (FF /2, optional 41 prefix, mod=11)

#include "helix/disasm.hpp"

#include <cstdio>

namespace helix {

namespace {

const char* reg64_name(int idx) {
    static const char* names[16] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    return names[idx & 15];
}

// Condition-code suffix indexed by the low nibble used in jcc/setcc/cmovcc.
// Only the values Helix emits are meaningful (4,5,C,D,E,F); others filled for
// completeness so the table is total over a nibble.
const char* cc_suffix(int cc) {
    static const char* names[16] = {
        "o",  "no", "b",  "ae", "e",  "ne", "be", "a",
        "s",  "ns", "p",  "np", "l",  "ge", "le", "g"};
    return names[cc & 15];
}

std::string hex_u64(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    return std::string(buf);
}

// Signed decimal for immediates (movabs).
std::string dec_i64(int64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return std::string(buf);
}

// Render "[base-8]" / "[base+16]" / "[base+0]".
std::string mem_disp(const char* base, int32_t disp) {
    std::string s = "[";
    s += base;
    if (disp < 0) s += "-" + dec_i64(-(int64_t)disp);  // careful with INT32_MIN
    else s += "+" + dec_i64((int64_t)disp);
    s += "]";
    return s;
}
std::string mem_rbp(int32_t disp) { return mem_disp("rbp", disp); }
std::string mem_rsp(int32_t disp) { return mem_disp("rsp", disp); }

int32_t read_i32(const uint8_t* p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

int64_t read_i64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return (int64_t)v;
}

}  // namespace

std::vector<DisInsn> disassemble_x64(const uint8_t* code, size_t len) {
    std::vector<DisInsn> out;
    size_t pos = 0;

    while (pos < len) {
        DisInsn ins;
        ins.offset = pos;
        ins.valid = true;

        size_t p = pos;  // cursor

        // ---- Parse optional REX prefix (0x40..0x4F) ----
        bool rex = false;
        bool rex_w = false, rex_r = false, rex_x = false, rex_b = false;
        if (p < len && (code[p] & 0xF0) == 0x40) {
            rex = true;
            uint8_t r = code[p];
            rex_w = (r & 0x08) != 0;
            rex_r = (r & 0x04) != 0;
            rex_x = (r & 0x02) != 0;
            rex_b = (r & 0x01) != 0;
            p++;
        }
        (void)rex_x;

        auto fail = [&]() {
            ins.valid = false;
            ins.length = 1;  // skip one byte so we make progress
            ins.text = "(bad)";
            out.push_back(ins);
            pos += 1;
        };

        if (p >= len) {  // dangling REX
            fail();
            continue;
        }

        uint8_t op = code[p];

        // Helper: decode a mod=11 ModRM at code[mp], producing reg & rm with REX
        // extension. Returns false if not present.
        auto modrm_reg = [&](size_t mp) -> int { return ((code[mp] >> 3) & 7) | (rex_r ? 8 : 0); };
        auto modrm_rm = [&](size_t mp) -> int { return (code[mp] & 7) | (rex_b ? 8 : 0); };
        auto modrm_mod = [&](size_t mp) -> int { return (code[mp] >> 6) & 3; };
        auto modrm_ext = [&](size_t mp) -> int { return (code[mp] >> 3) & 7; };

        // ===== Instructions WITHOUT a REX prefix possibility we handle first =====

        // ---- push/pop rbp (single byte, never with REX in Helix) ----
        if (!rex && op == 0x55) {
            ins.text = "push rbp";
            ins.length = 1;
            out.push_back(ins);
            pos += 1;
            continue;
        }
        if (!rex && op == 0x5D) {
            ins.text = "pop rbp";
            ins.length = 1;
            out.push_back(ins);
            pos += 1;
            continue;
        }
        if (!rex && op == 0xC3) {
            ins.text = "ret";
            ins.length = 1;
            out.push_back(ins);
            pos += 1;
            continue;
        }

        // ---- jmp rel32 (E9 cd) ---- (no REX)
        if (!rex && op == 0xE9) {
            if (p + 1 + 4 > len) { fail(); continue; }
            int32_t rel = read_i32(&code[p + 1]);
            ins.length = (p - pos) + 1 + 4;
            uint64_t target = (uint64_t)((int64_t)(pos + ins.length) + rel);
            ins.text = "jmp " + hex_u64(target);
            out.push_back(ins);
            pos += ins.length;
            continue;
        }

        // ---- 0F-prefixed two-byte opcodes (no REX, or REX from Helix forms) ----
        if (op == 0x0F) {
            if (p + 1 >= len) { fail(); continue; }
            uint8_t op2 = code[p + 1];

            // jcc rel32: 0F 80+cc cd  (no REX in Helix)
            if (!rex && op2 >= 0x80 && op2 <= 0x8F) {
                if (p + 2 + 4 > len) { fail(); continue; }
                int cc = op2 - 0x80;
                int32_t rel = read_i32(&code[p + 2]);
                ins.length = (p - pos) + 2 + 4;
                uint64_t target = (uint64_t)((int64_t)(pos + ins.length) + rel);
                ins.text = std::string("j") + cc_suffix(cc) + " " + hex_u64(target);
                out.push_back(ins);
                pos += ins.length;
                continue;
            }

            // setcc al: 0F 90+cc C0  (no REX in Helix)
            if (!rex && op2 >= 0x90 && op2 <= 0x9F) {
                if (p + 2 >= len) { fail(); continue; }
                uint8_t mrm = code[p + 2];
                if (mrm != 0xC0) { fail(); continue; }  // only al form emitted
                int cc = op2 - 0x90;
                ins.length = (p - pos) + 3;
                ins.text = std::string("set") + cc_suffix(cc) + " al";
                out.push_back(ins);
                pos += ins.length;
                continue;
            }

            // movzx reg, r/m8 : 0F B6 /r  (REX.W in Helix; rm=al)
            if (op2 == 0xB6) {
                if (p + 2 >= len) { fail(); continue; }
                size_t mp = p + 2;
                if (modrm_mod(mp) != 3) { fail(); continue; }
                int dst = modrm_reg(mp);
                int rm = code[mp] & 7;  // 8-bit source register (al if 0)
                static const char* reg8[8] = {"al", "cl", "dl", "bl",
                                              "spl", "bpl", "sil", "dil"};
                ins.length = (p - pos) + 3;
                ins.text = std::string("movzx ") + reg64_name(dst) + ", " + reg8[rm];
                out.push_back(ins);
                pos += ins.length;
                continue;
            }

            // movsx reg, r/m8 (0F BE) / r/m16 (0F BF)  (REX.W in Helix, mod=11)
            if (op2 == 0xBE || op2 == 0xBF) {
                if (p + 2 >= len) { fail(); continue; }
                size_t mp = p + 2;
                if (modrm_mod(mp) != 3) { fail(); continue; }
                int dst = modrm_reg(mp), src = modrm_rm(mp);
                ins.length = (p - pos) + 3;
                ins.text = std::string("movsx ") + reg64_name(dst) + ", " + reg64_name(src);
                out.push_back(ins);
                pos += ins.length;
                continue;
            }

            // imul reg, r/m : 0F AF /r  (REX.W in Helix, mod=11)
            if (op2 == 0xAF) {
                if (p + 2 >= len) { fail(); continue; }
                size_t mp = p + 2;
                if (modrm_mod(mp) != 3) { fail(); continue; }
                int dst = modrm_reg(mp);
                int src = modrm_rm(mp);
                ins.length = (p - pos) + 3;
                ins.text = std::string("imul ") + reg64_name(dst) + ", " + reg64_name(src);
                out.push_back(ins);
                pos += ins.length;
                continue;
            }

            // cmovcc reg, r/m : 0F 40+cc /r  (REX.W in Helix, mod=11)
            if (op2 >= 0x40 && op2 <= 0x4F) {
                if (p + 2 >= len) { fail(); continue; }
                size_t mp = p + 2;
                if (modrm_mod(mp) != 3) { fail(); continue; }
                int cc = op2 - 0x40;
                int dst = modrm_reg(mp);
                int src = modrm_rm(mp);
                ins.length = (p - pos) + 3;
                ins.text = std::string("cmov") + cc_suffix(cc) + " " +
                           reg64_name(dst) + ", " + reg64_name(src);
                out.push_back(ins);
                pos += ins.length;
                continue;
            }

            fail();
            continue;
        }

        // ---- cqo : 48 99 (REX.W required) ----
        if (rex && rex_w && !rex_r && !rex_b && op == 0x99) {
            ins.length = (p - pos) + 1;
            ins.text = "cqo";
            out.push_back(ins);
            pos += ins.length;
            continue;
        }

        // ---- movsxd r64, r/m32 : REX.W 63 /r (mod=11) ----
        if (rex && rex_w && op == 0x63) {
            if (p + 1 >= len) { fail(); continue; }
            size_t mp = p + 1;
            if (modrm_mod(mp) != 3) { fail(); continue; }
            int dst = modrm_reg(mp), src = modrm_rm(mp);
            ins.length = (p - pos) + 2;
            ins.text = std::string("movsxd ") + reg64_name(dst) + ", " + reg64_name(src);
            out.push_back(ins);
            pos += ins.length;
            continue;
        }

        // ---- movabs r64, imm64 : REX.W B8+r io ----
        if (rex && rex_w && (op & 0xF8) == 0xB8) {
            if (p + 1 + 8 > len) { fail(); continue; }
            int dst = (op & 7) | (rex_b ? 8 : 0);
            int64_t imm = read_i64(&code[p + 1]);
            ins.length = (p - pos) + 1 + 8;
            ins.text = std::string("movabs ") + reg64_name(dst) + ", " + dec_i64(imm);
            out.push_back(ins);
            pos += ins.length;
            continue;
        }

        // ---- ALU / mov / test with ModRM byte (REX.W in Helix) ----
        // 01 add, 29 sub, 21 and, 09 or, 31 xor, 39 cmp : r/m OP= reg
        // 89 mov : r/m <- reg  (covers mov_rr, store, mov rbp/rsp)
        // 8B mov : reg <- r/m  (covers load)
        // 85 test : r/m, reg
        {
            const char* mnem = nullptr;
            bool reg_is_dst = false;  // 8B-style: reg field is destination
            switch (op) {
                case 0x01: mnem = "add"; break;
                case 0x29: mnem = "sub"; break;
                case 0x21: mnem = "and"; break;
                case 0x09: mnem = "or"; break;
                case 0x31: mnem = "xor"; break;
                case 0x39: mnem = "cmp"; break;
                case 0x85: mnem = "test"; break;
                case 0x89: mnem = "mov"; break;
                case 0x8B: mnem = "mov"; reg_is_dst = true; break;
                default: break;
            }
            if (mnem) {
                if (p + 1 >= len) { fail(); continue; }
                size_t mp = p + 1;
                int mod = modrm_mod(mp);
                int reg = modrm_reg(mp);
                int rmlow = code[mp] & 7;

                if (mod == 3) {
                    // register/register form
                    int rm = modrm_rm(mp);
                    ins.length = (p - pos) + 2;
                    // Intel: dst, src.
                    // For r/m-dst encodings (01/29/.../89/85): "mnem rm, reg".
                    // For reg-dst encoding (8B): "mnem reg, rm".
                    std::string a, b;
                    if (reg_is_dst) {
                        a = reg64_name(reg);
                        b = reg64_name(rm);
                    } else {
                        a = reg64_name(rm);
                        b = reg64_name(reg);
                    }
                    ins.text = std::string(mnem) + " " + a + ", " + b;
                    out.push_back(ins);
                    pos += ins.length;
                    continue;
                } else if (mod == 2 && rmlow == 5) {
                    // [rbp + disp32]
                    if (mp + 1 + 4 > len) { fail(); continue; }
                    int32_t disp = read_i32(&code[mp + 1]);
                    ins.length = (p - pos) + 2 + 4;
                    std::string m = mem_rbp(disp);
                    std::string rn = reg64_name(reg);
                    if (reg_is_dst) {
                        // load: mov reg, [rbp+disp]
                        ins.text = std::string(mnem) + " " + rn + ", " + m;
                    } else {
                        // store: mov [rbp+disp], reg
                        ins.text = std::string(mnem) + " " + m + ", " + rn;
                    }
                    out.push_back(ins);
                    pos += ins.length;
                    continue;
                } else if (mod == 2 && rmlow == 4) {
                    // [rsp + disp32]: SIB (base=rsp=100, index=none=100, scale=0) = 0x24
                    if (mp + 1 >= len || code[mp + 1] != 0x24) { fail(); continue; }
                    if (mp + 2 + 4 > len) { fail(); continue; }
                    int32_t disp = read_i32(&code[mp + 2]);
                    ins.length = (p - pos) + 2 + 1 + 4;
                    std::string m = mem_rsp(disp);
                    std::string rn = reg64_name(reg);
                    ins.text = std::string(mnem) + " " + (reg_is_dst ? rn + ", " + m : m + ", " + rn);
                    out.push_back(ins);
                    pos += ins.length;
                    continue;
                } else {
                    fail();
                    continue;
                }
            }
        }

        // ---- 81 /0 add rsp / /5 sub rsp : REX.W 81 ModRM id ----
        if (op == 0x81) {
            if (p + 1 >= len) { fail(); continue; }
            size_t mp = p + 1;
            if (modrm_mod(mp) != 3) { fail(); continue; }
            int ext = modrm_ext(mp);
            int rm = modrm_rm(mp);
            if (mp + 1 + 4 > len) { fail(); continue; }
            int32_t imm = read_i32(&code[mp + 1]);
            const char* mnem = nullptr;
            switch (ext) {  // 81 /digit id : add/or/and/sub/xor/cmp r/m64, imm32
                case 0: mnem = "add"; break;
                case 1: mnem = "or"; break;
                case 4: mnem = "and"; break;
                case 5: mnem = "sub"; break;
                case 6: mnem = "xor"; break;
                case 7: mnem = "cmp"; break;
                default: break;
            }
            if (!mnem) { fail(); continue; }
            ins.length = (p - pos) + 2 + 4;
            ins.text = std::string(mnem) + " " + reg64_name(rm) + ", " + dec_i64(imm);
            out.push_back(ins);
            pos += ins.length;
            continue;
        }

        // ---- C1 /4 shl, /5 shr, /7 sar : shift r/m64, imm8 ----
        if (op == 0xC1) {
            if (p + 1 >= len) { fail(); continue; }
            size_t mp = p + 1;
            if (modrm_mod(mp) != 3) { fail(); continue; }
            int ext = modrm_ext(mp), rm = modrm_rm(mp);
            if (mp + 1 >= len) { fail(); continue; }
            int imm = code[mp + 1];
            const char* mnem = nullptr;
            switch (ext) { case 4: mnem = "shl"; break; case 5: mnem = "shr"; break; case 7: mnem = "sar"; break; default: break; }
            if (!mnem) { fail(); continue; }
            ins.length = (p - pos) + 3;
            ins.text = std::string(mnem) + " " + reg64_name(rm) + ", " + dec_i64(imm);
            out.push_back(ins);
            pos += ins.length;
            continue;
        }

        // ---- 69 /r id : imul reg, r/m64, imm32 (three-operand) ----
        if (op == 0x69) {
            if (p + 1 >= len) { fail(); continue; }
            size_t mp = p + 1;
            if (modrm_mod(mp) != 3) { fail(); continue; }
            int dst = modrm_reg(mp), src = modrm_rm(mp);
            if (mp + 1 + 4 > len) { fail(); continue; }
            int32_t imm = read_i32(&code[mp + 1]);
            ins.length = (p - pos) + 2 + 4;
            ins.text = std::string("imul ") + reg64_name(dst) + ", " + reg64_name(src) + ", " + dec_i64(imm);
            out.push_back(ins);
            pos += ins.length;
            continue;
        }

        // ---- D3 /4 shl, /5 shr, /7 sar : shift r/m, cl ----
        if (op == 0xD3) {
            if (p + 1 >= len) { fail(); continue; }
            size_t mp = p + 1;
            if (modrm_mod(mp) != 3) { fail(); continue; }
            int ext = modrm_ext(mp);
            int rm = modrm_rm(mp);
            const char* mnem = nullptr;
            switch (ext) {
                case 4: mnem = "shl"; break;
                case 5: mnem = "shr"; break;
                case 7: mnem = "sar"; break;
                default: break;
            }
            if (!mnem) { fail(); continue; }
            ins.length = (p - pos) + 2;
            ins.text = std::string(mnem) + " " + reg64_name(rm) + ", cl";
            out.push_back(ins);
            pos += ins.length;
            continue;
        }

        // ---- F7 /2 not, /3 neg, /7 idiv ----
        if (op == 0xF7) {
            if (p + 1 >= len) { fail(); continue; }
            size_t mp = p + 1;
            if (modrm_mod(mp) != 3) { fail(); continue; }
            int ext = modrm_ext(mp);
            int rm = modrm_rm(mp);
            const char* mnem = nullptr;
            switch (ext) {
                case 2: mnem = "not"; break;
                case 3: mnem = "neg"; break;
                case 7: mnem = "idiv"; break;
                default: break;
            }
            if (!mnem) { fail(); continue; }
            ins.length = (p - pos) + 2;
            ins.text = std::string(mnem) + " " + reg64_name(rm);
            out.push_back(ins);
            pos += ins.length;
            continue;
        }

        // ---- FF /2 call r/m (mod=11) : optional 41 REX.B prefix ----
        if (op == 0xFF) {
            if (p + 1 >= len) { fail(); continue; }
            size_t mp = p + 1;
            if (modrm_mod(mp) != 3) { fail(); continue; }
            int ext = modrm_ext(mp);
            if (ext != 2) { fail(); continue; }
            int rm = modrm_rm(mp);  // honors rex_b
            ins.length = (p - pos) + 2;
            ins.text = std::string("call ") + reg64_name(rm);
            out.push_back(ins);
            pos += ins.length;
            continue;
        }

        // Unknown opcode.
        fail();
    }

    return out;
}

}  // namespace helix
