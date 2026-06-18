// bench — micro-benchmark of the Helix optimizing backend (jit_compile_ra,
// linear-scan register allocator) against the simple memory-backed backend
// (jit_compile), with the interpreter as a correctness oracle.
//
// For each program we:
//   1. parse the surface source into a World,
//   2. compile the module BOTH ways (simple + RA),
//   3. ASSERT simple == ra == interpreter on a spread of inputs,
//   4. time many calls of the hot function through each backend and report
//      ns/call (simple vs ra) and the speedup ratio,
//   5. report the RA backend's total .text size via compile_module_obj(w).
//
// Build (in isolation): cl bench.cpp + src/{ir,eval,front,backend,backend2,
// opt,coff,print,verify}.cpp.
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"

using namespace helix;
using clock_hr = std::chrono::high_resolution_clock;

namespace {

// One benchmark case: a source module, the hot function to time, the argument
// vectors used both for correctness checking and for the timed loop, and how
// many timed iterations to run (cheap functions need more iterations).
struct Bench {
    const char* name;        // label for the table
    const char* fn;          // hot function to time
    std::string src;         // surface source (may contain helper fns)
    std::vector<std::vector<int64_t>> check_args;  // correctness inputs
    std::vector<int64_t> hot_args;                 // timed-call argument
    long iters;              // timed iterations per backend
};

int g_failures = 0;

// Assert simple-backend, RA-backend, and interpreter all agree on every check
// input. Returns false (and records a failure) on any disagreement.
bool verify_agreement(World& w, const Bench& b, const JitModule& simple,
                      const JitModule& ra) {
    NodeId f = w.find_func(b.fn);
    if (f == NONE) {
        std::printf("  [%s] FAIL: no such function %s\n", b.name, b.fn);
        g_failures++;
        return false;
    }
    bool all_ok = true;
    for (const auto& args : b.check_args) {
        EvalResult ir = eval_func(w, f, args);
        int64_t sv = simple.call(f, args);
        int64_t rv = ra.call(f, args);
        if (!ir.ok || ir.value != sv || ir.value != rv) {
            std::printf("  [%s] MISMATCH args[0]=%lld : interp=%lld simple=%lld ra=%lld%s\n",
                        b.name, (long long)(args.empty() ? 0 : args[0]),
                        (long long)ir.value, (long long)sv, (long long)rv,
                        ir.ok ? "" : " (interp !ok)");
            all_ok = false;
            g_failures++;
        }
    }
    return all_ok;
}

// Time `iters` calls of f(args) through one backend. Returns ns/call.
// `sink` accumulates results so the optimizer can't elide the calls.
double time_calls(const JitModule& jit, NodeId f, const std::vector<int64_t>& args,
                  long iters, volatile int64_t* sink) {
    auto t0 = clock_hr::now();
    int64_t acc = 0;
    for (long i = 0; i < iters; i++) acc += jit.call(f, args);
    auto t1 = clock_hr::now();
    *sink += acc;
    double ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    return ns / (double)iters;
}

}  // namespace

int main() {
    std::vector<Bench> benches;

    // 1. Recursive fib — deep call-chain, exercises call/return + recursion.
    benches.push_back({
        "fib(28) recursive", "fib",
        "fn fib(n: int) -> int { if n < 2 { n } else { fib(n-1) + fib(n-2) } }\n",
        {{0},{1},{5},{10},{20},{25}}, {28}, 200});

    // 2. Ackermann — heavy nested recursion.
    benches.push_back({
        "ack(2,7)", "ack",
        "fn ack(m:int,n:int)->int { if m==0 {n+1} else { if n==0 {ack(m-1,1)} else {ack(m-1,ack(m,n-1))} } }\n",
        {{0,0},{1,1},{2,3},{3,3},{2,5}}, {2,7}, 20000});

    // 3. Counted sum loop with many iterations — tight loop body.
    benches.push_back({
        "sum(100000) loop", "sum",
        "fn sum(n: int) -> int { loop (acc=0,i=0){ if i>=n {break acc} else {next acc+i,i+1} } }\n",
        {{0},{1},{10},{100},{1000}}, {100000}, 2000});

    // 4. gcd — short loop with div/rem.
    benches.push_back({
        "gcd(1071,462)", "gcd",
        "fn gcd(a: int, b: int) -> int { loop (x=a,y=b){ if y==0 {break x} else {next y, x%y} } }\n",
        {{1071,462},{48,36},{17,5},{100,0},{0,7}}, {1071,462}, 500000});

    // 5. Register-pressure "spill" — ~12 simultaneously-live temporaries.
    benches.push_back({
        "spill (12 temps)", "spill",
        "fn spill(a: int, b: int, c: int, d: int) -> int {\n"
        "  let t0=a*a+1; let t1=b*b+2; let t2=c*c+3; let t3=d*d+4;\n"
        "  let t4=a*b+5; let t5=b*c+6; let t6=c*d+7; let t7=a*d+8;\n"
        "  let t8=a+b+9; let t9=c+d+10; let t10=a*c+11; let t11=b*d+12;\n"
        "  t0+t1+t2+t3+t4+t5+t6+t7+t8+t9+t10+t11\n"
        "}\n",
        {{1,2,3,4},{-5,6,-7,8},{1000,-1000,500,-500},{0,0,0,0}}, {13,17,19,23}, 1000000});

    std::printf("Helix backend benchmark: simple (jit_compile) vs RA (jit_compile_ra)\n");
    std::printf("interpreter is the correctness oracle; both backends must agree.\n\n");

    // Column layout for the results table.
    std::printf("%-20s %14s %14s %9s %12s\n",
                "program", "simple ns/call", "ra ns/call", "speedup", "ra .text B");
    std::printf("%-20s %14s %14s %9s %12s\n",
                "--------------------", "--------------",
                "--------------", "---------", "------------");

    volatile int64_t sink = 0;

    for (auto& b : benches) {
        World w;
        ParseStatus st = parse_module(w, b.src);
        if (!st.ok) {
            std::printf("  [%s] parse error (line %d): %s\n", b.name, st.line, st.msg.c_str());
            g_failures++;
            continue;
        }

        JitModule simple = jit_compile(w);
        JitModule ra = jit_compile_ra(w);
        if (!simple.ok) { std::printf("  [%s] jit_compile error: %s\n", b.name, simple.err.c_str()); g_failures++; continue; }
        if (!ra.ok)     { std::printf("  [%s] jit_compile_ra error: %s\n", b.name, ra.err.c_str());   g_failures++; continue; }

        // Step 3: correctness must hold before we trust the timings.
        if (!verify_agreement(w, b, simple, ra)) continue;

        NodeId f = w.find_func(b.fn);

        // Warm up (page-in code, prime caches) before timing.
        sink += simple.call(f, b.hot_args);
        sink += ra.call(f, b.hot_args);

        double ns_simple = time_calls(simple, f, b.hot_args, b.iters, &sink);
        double ns_ra     = time_calls(ra,     f, b.hot_args, b.iters, &sink);

        // Step 5: RA backend total emitted code size (whole module .text).
        ObjModule obj = compile_module_obj(w);
        size_t text_bytes = obj.text.size();

        double speedup = ns_ra > 0.0 ? ns_simple / ns_ra : 0.0;
        std::printf("%-20s %14.1f %14.1f %8.2fx %12zu\n",
                    b.name, ns_simple, ns_ra, speedup, text_bytes);
    }

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("RESULT: all backends agree with the interpreter; timings above.\n");
    } else {
        std::printf("RESULT: %d failure(s) — see messages above.\n", g_failures);
    }
    (void)sink;
    return g_failures == 0 ? 0 : 1;
}
