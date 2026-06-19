// Array writes: a[i] = v through a threaded memory state. Each in-place algorithm
// is run by the interpreter AND both JIT backends on real arrays — all must agree.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

static const char* kProg =
    "fn fill(a: ptr, n: int, v: int) -> int { var i = 0; while i < n { a[i] = v; i = i + 1; } return 0; }\n"
    "fn reverse(a: ptr, n: int) -> int {\n"
    "  var i = 0; var j = n - 1;\n"
    "  while i < j { var t = a[i]; a[i] = a[j]; a[j] = t; i = i + 1; j = j - 1; }\n"
    "  return 0;\n"
    "}\n"
    "fn bubble(a: ptr, n: int) -> int {\n"
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
    "}\n"
    "fn prefix(a: ptr, n: int) -> int { var i = 1; while i < n { a[i] = a[i] + a[i-1]; i = i + 1; } return 0; }\n";

namespace {
struct Engines { World w; JitModule simple, ra; };
void build(Engines& e) {
    auto st = parse_module(e.w, kProg);
    if (!st.ok) std::printf("    parse error (line %d): %s\n", st.line, st.msg.c_str());
    CHECK(st.ok);
    e.simple = jit_compile(e.w);
    e.ra = jit_compile_ra(e.w);
    if (!e.simple.ok) std::printf("    simple jit: %s\n", e.simple.err.c_str());
    if (!e.ra.ok) std::printf("    ra jit: %s\n", e.ra.err.c_str());
    CHECK(e.simple.ok);
    CHECK(e.ra.ok);
}
// run `fn` on a fresh copy of `init` via the given engine; return the resulting array
std::vector<int64_t> run(Engines& e, const char* fn, std::vector<int64_t> init, int extra, int engine) {
    NodeId f = e.w.find_func(fn);
    std::vector<int64_t> arr = init;
    std::vector<int64_t> args = {(int64_t)(uintptr_t)arr.data(), (int64_t)arr.size()};
    if (extra != INT32_MIN) args.push_back(extra);
    if (engine == 0) eval_func(e.w, f, args);
    else if (engine == 1) e.simple.call(f, args);
    else e.ra.call(f, args);
    return arr;
}
void check_all(Engines& e, const char* fn, std::vector<int64_t> init, int extra, std::vector<int64_t> expect) {
    for (int eng = 0; eng < 3; eng++) CHECK(run(e, fn, init, extra, eng) == expect);
}
}  // namespace

TEST("writes: fill (interp == simple == ra)") {
    Engines e; build(e);
    check_all(e, "fill", {1, 2, 3, 4, 5, 6, 7, 8}, 42, {42, 42, 42, 42, 42, 42, 42, 42});
}

TEST("writes: reverse in place (interp == simple == ra)") {
    Engines e; build(e);
    check_all(e, "reverse", {10, 20, 30, 40, 50, 60}, INT32_MIN, {60, 50, 40, 30, 20, 10});
}

TEST("writes: prefix sums in place (read-modify-write, interp == simple == ra)") {
    Engines e; build(e);
    check_all(e, "prefix", {1, 2, 3, 4, 5, 6}, INT32_MIN, {1, 3, 6, 10, 15, 21});
}

TEST("writes: bubble sort sorts a real array natively (interp == simple == ra)") {
    Engines e; build(e);
    std::vector<int64_t> data = {5, 3, 9, 1, 7, 2, 8, 4, 6, 0, -3, 11, -7, 15};
    std::vector<int64_t> sorted = data;
    std::sort(sorted.begin(), sorted.end());
    check_all(e, "bubble", data, INT32_MIN, sorted);
}
