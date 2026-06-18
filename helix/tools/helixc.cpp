// helixc — the Helix command-line driver. Demonstrates the whole pipeline:
// parse .hx source directly to the graph, verify invariants, print the IR,
// JIT to x86-64, and run a function (cross-checked against the interpreter).
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "helix/print.hpp"
#include "helix/verify.hpp"

using namespace helix;

static void usage() {
    std::printf(
        "usage: helixc <file.hx> [--print] [--run FN ARG...]\n"
        "  --print        print the Helix IR for every function\n"
        "  --run FN ARGS  JIT-compile and run FN(ARGS), cross-checked vs the interpreter\n");
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    std::string path = argv[1];
    std::ifstream in(path, std::ios::binary);
    if (!in) { std::printf("error: cannot open %s\n", path.c_str()); return 1; }
    std::stringstream ss; ss << in.rdbuf();
    std::string src = ss.str();

    World w;
    ParseStatus st = parse_module(w, src);
    if (!st.ok) { std::printf("parse error (line %d): %s\n", st.line, st.msg.c_str()); return 1; }

    VerifyResult vr = verify_module(w);
    if (!vr.ok) {
        std::printf("verification FAILED:\n");
        for (auto& e : vr.errors) std::printf("  %s\n", e.c_str());
        return 1;
    }
    std::printf("parsed %zu function(s); invariants verified.\n", w.module_funcs().size());

    bool do_print = false, simple = false;
    std::string run_fn, obj_path;
    std::vector<int64_t> args;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--print") do_print = true;
        else if (a == "--simple") simple = true;  // use the memory-slot backend
        else if (a == "--run") { if (i + 1 < argc) run_fn = argv[++i]; }
        else if (a == "--emit-obj") { if (i + 1 < argc) obj_path = argv[++i]; }
        else args.push_back(std::strtoll(a.c_str(), nullptr, 10));
    }

    if (do_print) {
        std::printf("\n%s", print_module(w).c_str());
    }

    if (!obj_path.empty()) {
        ObjModule obj = compile_module_obj(w);
        if (obj.text.empty()) { std::printf("codegen failed (unsupported construct)\n"); return 1; }
        std::vector<uint8_t> bytes = write_coff_x64(obj);
        std::ofstream out(obj_path, std::ios::binary);
        out.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        std::printf("wrote %s (%zu bytes, %zu functions). Link with: link /entry:... or with a C driver.\n",
                    obj_path.c_str(), bytes.size(), obj.symbols.size());
    }

    if (!run_fn.empty()) {
        NodeId f = w.find_func(run_fn);
        if (f == NONE) { std::printf("no such function: %s\n", run_fn.c_str()); return 1; }
        std::printf("\nlive nodes in %s: %zu\n", run_fn.c_str(), reachable_count(w, f));

        JitModule jit = simple ? jit_compile(w) : jit_compile_ra(w);
        if (!jit.ok) { std::printf("jit error: %s\n", jit.err.c_str()); return 1; }

        auto ir = eval_func(w, f, args);
        int64_t jv = jit.call(f, args);
        std::printf("interp(%s) = %lld\n", run_fn.c_str(), (long long)ir.value);
        std::printf("  jit(%s) = %lld\n", run_fn.c_str(), (long long)jv);
        std::printf("%s\n", (ir.ok && ir.value == jv) ? "OK: jit matches interpreter" : "MISMATCH!");
        return (ir.ok && ir.value == jv) ? 0 : 2;
    }
    return 0;
}
