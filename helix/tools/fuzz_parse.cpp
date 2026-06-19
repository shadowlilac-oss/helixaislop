// fuzz_parse — robustness fuzzer for the FRONT END. Feeds malformed, garbage, deeply
// nested, and truncated input to parse_module and checks it never crashes, hangs, or
// reads out of bounds: it must always return a ParseStatus (ok or a located error).
// When a program does parse, it is also verified + compiled, so those paths are
// exercised on weird-but-well-formed graphs too. A crash (segfault / stack overflow)
// kills the process and is caught by the harness; a hang is caught by the watchdog.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "fuzz_watchdog.hpp"
#include "helix/backend.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"

using namespace helix;

// A grab-bag of tokens the lexer recognizes, plus garbage, to assemble random soup.
static const char* kToks[] = {
    "fn", "comptime", "let", "var", "if", "else", "loop", "while", "break", "next",
    "return", "true", "false", "int", "i64", "i32", "i16", "i8", "bool", "ptr",
    "f", "g", "x", "n", "a", "0", "1", "42", "-7", "999999999999",
    "(", ")", "{", "}", "[", "]", ",", ";", ":", "->", "=",
    "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "&&", "||", "!",
    "==", "!=", "<", "<=", ">", ">=", " ", "\n", "@", "$", "#", "\\", "\"", ".",
};

struct Gen {
    std::mt19937_64 rng;
    explicit Gen(uint64_t s) : rng(s) {}
    int range(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }

    std::string token_soup(int n) {
        std::string s;
        for (int i = 0; i < n; i++) { s += kToks[range(0, (int)(sizeof(kToks) / sizeof(*kToks)) - 1)]; if (range(0, 2)) s += ' '; }
        return s;
    }
    std::string random_bytes(int n) {
        std::string s;
        for (int i = 0; i < n; i++) s += (char)range(1, 126);
        return s;
    }
    std::string deep_parens(int n) {
        std::string s = "fn f() -> int {\n  ";
        for (int i = 0; i < n; i++) s += "(";
        s += "1";
        for (int i = 0; i < range(0, n); i++) s += ")";  // often unbalanced
        return s + "\n}\n";
    }
    std::string deep_blocks(int n) {
        std::string s = "fn f(n: int) -> int {\n";
        for (int i = 0; i < n; i++) s += "if n { ";
        s += "n";
        for (int i = 0; i < range(0, n); i++) s += " } else { 0 }";
        return s + "\n}\n";
    }
    std::string deep_unary(int n) {
        std::string s = "fn f(n: int) -> int { ";
        for (int i = 0; i < n; i++) s += "-";
        return s + "n }\n";
    }
    std::string truncate_valid(int cut) {
        std::string v = "fn fib(n: int) -> int { if n < 2 { n } else { fib(n-1) + fib(n-2) } }\n"
                        "fn sum(a: ptr, m: int) -> int { var s = 0; var i = 0; while i < m { s = s + a[i]; i = i + 1; } return s; }\n";
        return v.substr(0, (size_t)cut % (v.size() + 1));
    }
    std::string make(int kind) {
        switch (kind % 6) {
            case 0: return token_soup(range(1, 200));
            case 1: return random_bytes(range(0, 300));
            case 2: return deep_parens(range(1, 5000));   // exercises the parser depth guard
            case 3: return deep_blocks(range(1, 3000));
            case 4: return deep_unary(range(1, 5000));
            default: return truncate_valid(range(0, 400));
        }
    }
};

int main(int argc, char** argv) {
    uint64_t seed = 0xBADC0DE;
    int iters = 200000;
    if (argc > 1) seed = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) iters = (int)std::strtol(argv[2], nullptr, 10);
    std::printf("fuzz_parse: seed=%llu iters=%d\n", (unsigned long long)seed, iters);

    FuzzWatchdog wd; wd.start();
    long ok = 0, errs = 0, compiled = 0;

    for (int i = 0; i < iters; i++) {
        if (i && i % 20000 == 0) { std::printf("  ... %d/%d  parsed_ok=%ld parse_errors=%ld\n", i, iters, ok, errs); std::fflush(stdout); }
        Gen g(seed + (uint64_t)i * 0x100000001B3ull);
        std::string src = g.make(i);

        World w;
        wd.arm(src, "parse iter " + std::to_string(i), 3000);  // parse must terminate
        ParseStatus ps = parse_module(w, src);                 // must NOT crash on any input
        wd.disarm();
        if (!ps.ok) { errs++; continue; }
        ok++;
        // A program that happens to parse must also verify/compile without crashing.
        wd.arm(src, "compile iter " + std::to_string(i), 3000);
        JitModule jit = jit_compile_ra(w);
        wd.disarm();
        if (jit.ok) compiled++;
    }

    std::printf("\n==== results ====\nparsed ok      : %ld\nparse errors   : %ld\ncompiled ok    : %ld\n", ok, errs, compiled);
    std::printf("\nNO CRASH over %d malformed inputs (parser is robust).\n", iters);
    return 0;
}
