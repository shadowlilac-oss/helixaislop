// Array WRITES: mutable memory through the linear $mem state, validated interp ==
// simple == ra on BOTH the return value and the final array contents. The cross-loop
// read-then-write case is a regression test for an interpreter bug where a sibling
// loop's cached result was wrongly invalidated and re-run against mutated memory.
#include <algorithm>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

namespace {
struct C { World w; JitModule simple, ra; };
static void load(C& c, const char* src) {
    auto st = parse_module(c.w, src);
    if (!st.ok) std::printf("    parse error (line %d): %s\n", st.line, st.msg.c_str());
    CHECK(st.ok);
    c.simple = jit_compile(c.w);
    c.ra = jit_compile_ra(c.w);
    CHECK(c.simple.ok);
    CHECK(c.ra.ok);
}
// Run `fn` on three independent copies of `arr` (interp/simple/ra). Require all three
// agree on the return value AND the resulting array; return the (shared) final array.
static std::vector<int64_t> run3(C& c, const char* fn, std::vector<int64_t> arr, int64_t n) {
    NodeId f = c.w.find_func(fn);
    std::vector<int64_t> a0 = arr, a1 = arr, a2 = arr;
    auto addr = [](std::vector<int64_t>& v) { return (int64_t)(uintptr_t)v.data(); };
    auto ir = eval_func(c.w, f, {addr(a0), n});
    int64_t sv = c.simple.call(f, {addr(a1), n});
    int64_t rv = c.ra.call(f, {addr(a2), n});
    CHECK(ir.ok);
    CHECK_EQ(sv, ir.value);
    CHECK_EQ(rv, ir.value);
    CHECK(a1 == a0);
    CHECK(a2 == a0);
    return a0;
}
}  // namespace

TEST("writes: fill, conditional clamp, in-place reverse (interp==simple==ra + array)") {
    C c;
    load(c,
        "fn fill(a: ptr, n: int) -> int { var i = 0; while i < n { a[i] = i * i; i = i + 1; } return 0; }\n"
        "fn clamp(a: ptr, n: int) -> int { var i = 0; while i < n { if a[i] < 0 { a[i] = 0; } i = i + 1; } return 0; }\n"
        "fn rev(a: ptr, n: int) -> int { var i = 0; var j = n - 1; while i < j { var t = a[i]; a[i] = a[j]; a[j] = t; i = i + 1; j = j - 1; } return 0; }\n");
    std::vector<int64_t> in = {5, -3, 9, -1, 7, -2, 8, 4};
    auto filled = run3(c, "fill", in, 8);
    for (int i = 0; i < 8; i++) CHECK_EQ(filled[i], (int64_t)i * i);
    auto clamped = run3(c, "clamp", in, 8);
    for (int i = 0; i < 8; i++) CHECK_EQ(clamped[i], in[i] < 0 ? 0 : in[i]);
    auto reversed = run3(c, "rev", in, 8);
    for (int i = 0; i < 8; i++) CHECK_EQ(reversed[i], in[7 - i]);
}

TEST("writes: bubble sort produces a sorted array on all three engines") {
    C c;
    load(c,
        "fn sort(a: ptr, n: int) -> int {\n"
        "  var i = 0;\n"
        "  while i < n {\n"
        "    var j = 0;\n"
        "    while j < n - 1 {\n"
        "      if a[j] > a[j + 1] { var t = a[j]; a[j] = a[j + 1]; a[j + 1] = t; }\n"
        "      j = j + 1;\n"
        "    }\n"
        "    i = i + 1;\n"
        "  }\n"
        "  return 0;\n"
        "}\n");
    std::vector<int64_t> in = {5, 3, 9, 1, 7, 2, 8, 4, 6, 0, -3, 11};
    auto out = run3(c, "sort", in, (int64_t)in.size());
    std::vector<int64_t> want = in;
    std::sort(want.begin(), want.end());
    CHECK(out == want);
}

TEST("writes: sibling loop reads original memory before a later loop overwrites it (regress)") {
    // loop1 reads the array to compute acc; loop2 then overwrites the array. acc must be
    // computed from the ORIGINAL contents (program order), not from cells loop2 already
    // wrote. The interpreter previously re-ran loop1 mid-loop2 against mutated memory.
    C c;
    load(c,
        "fn f(a: ptr, n: int) -> int {\n"
        "  var acc = n;\n"
        "  var t0 = 0;\n"
        "  while t0 < n - 1 { acc = (0 - 24 - a[t0]) / 4067; t0 = t0 + 1; }\n"
        "  var t1 = 0;\n"
        "  while t1 < n { a[t1] = 2; a[t1] = acc; t1 = t1 + 1; }\n"
        "  return acc;\n"
        "}\n");
    std::vector<int64_t> in = {10754, 8442485003548017651LL, 91, 8622329357966660728LL, 38823, 38643};
    int64_t n = 6;
    int64_t acc = n;
    for (int64_t t0 = 0; t0 < n - 1; t0++) acc = (0 - 24 - in[(size_t)t0]) / 4067;  // original reads
    auto out = run3(c, "f", in, n);
    for (int64_t i = 0; i < n; i++) CHECK_EQ(out[(size_t)i], acc);  // every cell overwritten with acc
}
