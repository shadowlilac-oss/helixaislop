#include "helix/x64.hpp"
#include "helix/disasm.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

using namespace helix;

static int g_fail = 0;

static const char* regname(Reg r) {
    static const char* n[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                "r8","r9","r10","r11","r12","r13","r14","r15"};
    return n[(int)r & 15];
}

// One expected (text,length) per encoded instruction, checked in order.
struct Expect { std::string text; size_t len; };

int main() {
    Asm a;
    std::vector<Expect> exp;

    auto add_exp = [&](const std::string& t) {
        // length filled in after we know byte boundaries; we instead record the
        // text and later verify lengths sum correctly via per-instruction sizes.
        exp.push_back({t, 0});
    };

    // We track encoded length of each instruction by snapshotting size().
    std::vector<size_t> bounds;  // cumulative byte offset after each instruction
    auto mark = [&]() { bounds.push_back(a.size()); };

    // ---- prologue ----
    a.push_rbp();            add_exp("push rbp"); mark();
    a.mov_rbp_rsp();         add_exp("mov rbp, rsp"); mark();
    a.sub_rsp(64);           add_exp("sub rsp, 64"); mark();

    // ---- movabs (positive, negative, big) ----
    a.mov_ri(RAX, 42);       add_exp("movabs rax, 42"); mark();
    a.mov_ri(RCX, -1);       add_exp("movabs rcx, -1"); mark();
    a.mov_ri(R8,  0x123456789ABCDEF0LL); add_exp(std::string("movabs r8, ") + std::to_string(0x123456789ABCDEF0LL)); mark();
    a.mov_ri(R11, -9223372036854775807LL - 1); add_exp(std::string("movabs r11, ") + std::to_string((long long)(-9223372036854775807LL - 1))); mark();

    // ---- load / store with rbp displacement (neg, pos, zero) ----
    a.load(RAX, -8);         add_exp("mov rax, [rbp-8]"); mark();
    a.load(R10, 16);         add_exp("mov r10, [rbp+16]"); mark();
    a.store(-24, RDX);       add_exp("mov [rbp-24], rdx"); mark();
    a.store(0, R9);          add_exp("mov [rbp+0], r9"); mark();

    // ---- mov reg, reg ----
    a.mov_rr(RAX, RCX);      add_exp("mov rax, rcx"); mark();
    a.mov_rr(R8, R11);       add_exp("mov r8, r11"); mark();
    a.mov_rr(RBX, R9);       add_exp("mov rbx, r9"); mark();

    // ---- ALU reg, reg (dst OP= src) -> "op dst, src" ----
    a.alu(Alu::Add, RAX, RCX); add_exp("add rax, rcx"); mark();
    a.alu(Alu::Sub, RDX, RBX); add_exp("sub rdx, rbx"); mark();
    a.alu(Alu::And, R8, R9);   add_exp("and r8, r9"); mark();
    a.alu(Alu::Or,  RSI, RDI); add_exp("or rsi, rdi"); mark();
    a.alu(Alu::Xor, R10, RAX); add_exp("xor r10, rax"); mark();
    a.alu(Alu::Cmp, RAX, R11); add_exp("cmp rax, r11"); mark();

    // ---- imul ----
    a.imul(RAX, RCX);        add_exp("imul rax, rcx"); mark();
    a.imul(R8, R9);          add_exp("imul r8, r9"); mark();
    a.imul(RDX, R10);        add_exp("imul rdx, r10"); mark();

    // ---- shifts ----
    a.shift(Shift::Shl, RAX); add_exp("shl rax, cl"); mark();
    a.shift(Shift::Sar, RBX); add_exp("sar rbx, cl"); mark();
    a.shift(Shift::Shr, R8);  add_exp("shr r8, cl"); mark();

    // ---- unary F7 ----
    a.neg(RAX);              add_exp("neg rax"); mark();
    a.not_(R9);              add_exp("not r9"); mark();
    a.idiv(RCX);             add_exp("idiv rcx"); mark();
    a.idiv(R11);             add_exp("idiv r11"); mark();

    // ---- cqo ----
    a.cqo();                 add_exp("cqo"); mark();

    // ---- test ----
    a.test_rr(RAX, RCX);     add_exp("test rax, rcx"); mark();
    a.test_rr(R8, R9);       add_exp("test r8, r9"); mark();

    // ---- setcc al ----
    a.setcc(CC_E);           add_exp("sete al"); mark();
    a.setcc(CC_NE);          add_exp("setne al"); mark();
    a.setcc(CC_L);           add_exp("setl al"); mark();
    a.setcc(CC_GE);          add_exp("setge al"); mark();
    a.setcc(CC_LE);          add_exp("setle al"); mark();
    a.setcc(CC_G);           add_exp("setg al"); mark();

    // ---- movzx reg, al ----
    a.movzx_al(RAX);         add_exp("movzx rax, al"); mark();
    a.movzx_al(R8);          add_exp("movzx r8, al"); mark();

    // ---- cmovcc ----
    a.cmovcc(CC_E,  RAX, RCX); add_exp("cmove rax, rcx"); mark();
    a.cmovcc(CC_NE, R8, R9);   add_exp("cmovne r8, r9"); mark();
    a.cmovcc(CC_L,  RDX, R10); add_exp("cmovl rdx, r10"); mark();
    a.cmovcc(CC_G,  R11, RBX); add_exp("cmovg r11, rbx"); mark();

    // ---- call reg ----
    a.call_reg(RAX);         add_exp("call rax"); mark();
    a.call_reg(R11);         add_exp("call r11"); mark();
    a.call_reg(RBP);         add_exp("call rbp"); mark();

    // ---- control flow with labels (forward + backward jumps) ----
    // Backward target:
    Label back = a.new_label();
    a.bind(back);
    a.alu(Alu::Add, RAX, RAX); add_exp("add rax, rax"); size_t off_addraxrax = a.size() - 0; mark();
    (void)off_addraxrax;

    Label fwd = a.new_label();
    a.jcc(CC_NE, fwd);       add_exp("__JNE_FWD__"); mark();   // target resolved later
    a.jmp(back);             add_exp("__JMP_BACK__"); mark();
    a.jcc(CC_E, fwd);        add_exp("__JE_FWD__"); mark();
    a.bind(fwd);
    a.alu(Alu::Xor, RAX, RAX); add_exp("xor rax, rax"); mark();

    // ---- epilogue ----
    a.add_rsp(64);           add_exp("add rsp, 64"); mark();
    a.mov_rsp_rbp();         add_exp("mov rsp, rbp"); mark();
    a.pop_rbp();             add_exp("pop rbp"); mark();
    a.ret();                 add_exp("ret"); mark();

    a.finalize();

    // Compute label byte offsets for expected jump targets.
    // back label was bound right before "add rax, rax". Find its offset:
    // bounds[i] is cumulative size AFTER instruction i. The instruction index
    // of "add rax, rax" — we need its START offset.
    // Reconstruct start offsets:
    std::vector<size_t> starts;
    size_t prev = 0;
    for (size_t b : bounds) { starts.push_back(prev); prev = b; }

    // Identify indices by scanning exp for our placeholders / known anchors.
    // 'back' target == start offset of the "add rax, rax" instruction.
    // 'fwd' target == start offset of the "xor rax, rax" (after the jumps).
    size_t idx_addraxrax = (size_t)-1, idx_xorraxrax_fwd = (size_t)-1;
    for (size_t i = 0; i < exp.size(); i++) {
        if (exp[i].text == "add rax, rax" && idx_addraxrax == (size_t)-1) idx_addraxrax = i;
    }
    // The fwd 'xor rax, rax' is the one that appears AFTER the jump placeholders.
    bool seen_jump = false;
    for (size_t i = 0; i < exp.size(); i++) {
        if (exp[i].text == "__JE_FWD__") seen_jump = true;
        if (seen_jump && exp[i].text == "xor rax, rax") { idx_xorraxrax_fwd = i; break; }
    }
    size_t back_off = starts[idx_addraxrax];
    size_t fwd_off = starts[idx_xorraxrax_fwd];

    char buf[64];
    for (size_t i = 0; i < exp.size(); i++) {
        if (exp[i].text == "__JNE_FWD__") {
            std::snprintf(buf, sizeof(buf), "jne 0x%llx", (unsigned long long)fwd_off);
            exp[i].text = buf;
        } else if (exp[i].text == "__JE_FWD__") {
            std::snprintf(buf, sizeof(buf), "je 0x%llx", (unsigned long long)fwd_off);
            exp[i].text = buf;
        } else if (exp[i].text == "__JMP_BACK__") {
            std::snprintf(buf, sizeof(buf), "jmp 0x%llx", (unsigned long long)back_off);
            exp[i].text = buf;
        }
    }

    // Fill expected lengths from instruction boundaries.
    for (size_t i = 0; i < exp.size(); i++) exp[i].len = bounds[i] - starts[i];

    // ---- Now disassemble and compare ----
    const std::vector<uint8_t>& code = a.bytes();
    std::vector<DisInsn> dis = disassemble_x64(code.data(), code.size());

    printf("encoded %zu bytes, %zu instructions; disassembled %zu instructions\n",
           code.size(), exp.size(), dis.size());

    if (dis.size() != exp.size()) {
        printf("FAIL: instruction count mismatch (expected %zu, got %zu)\n",
               exp.size(), dis.size());
        g_fail++;
    }

    size_t n = dis.size() < exp.size() ? dis.size() : exp.size();
    size_t running_off = 0;
    for (size_t i = 0; i < n; i++) {
        bool ok = true;
        if (dis[i].text != exp[i].text) ok = false;
        if (dis[i].length != exp[i].len) ok = false;
        if (dis[i].offset != running_off) ok = false;
        if (!dis[i].valid) ok = false;
        if (!ok) {
            printf("FAIL #%zu: expected {\"%s\" len=%zu off=%zu} got {\"%s\" len=%zu off=%zu valid=%d}\n",
                   i, exp[i].text.c_str(), exp[i].len, running_off,
                   dis[i].text.c_str(), dis[i].length, dis[i].offset, (int)dis[i].valid);
            g_fail++;
        }
        running_off += exp[i].len;
    }

    // Verify total decoded length == encoded length (no bytes lost/extra).
    size_t total_decoded = 0;
    for (auto& d : dis) total_decoded += d.length;
    if (total_decoded != code.size()) {
        printf("FAIL: total decoded length %zu != encoded %zu\n", total_decoded, code.size());
        g_fail++;
    }

    if (g_fail == 0) {
        printf("PASS\n");
        return 0;
    } else {
        printf("FAILED: %d error(s)\n", g_fail);
        return 1;
    }
}
