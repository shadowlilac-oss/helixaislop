// Narrow integer types i8 / i16 (and i32): arithmetic wraps to the type width and a
// narrow-typed incoming argument is sign-extended, identically in interp / simple / ra.
#include <climits>
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
static int64_t diff(C& c, const char* fn, std::vector<int64_t> a) {
    NodeId f = c.w.find_func(fn);
    auto ir = eval_func(c.w, f, a);
    CHECK(ir.ok);
    CHECK_EQ(c.simple.call(f, a), ir.value);
    CHECK_EQ(c.ra.call(f, a), ir.value);
    return ir.value;
}
// reference: wrap v to a signed `bits`-wide integer.
static int64_t wrap(int64_t v, int bits) {
    uint64_t m = (bits >= 64) ? ~0ull : ((1ull << bits) - 1);
    uint64_t u = (uint64_t)v & m;
    if (bits < 64 && (u & (1ull << (bits - 1)))) u |= ~m;  // sign-extend
    return (int64_t)u;
}
}  // namespace

TEST("types: i8 arithmetic wraps to 8 bits on all three engines") {
    C c;
    load(c,
        "fn add8(a: i8, b: i8) -> int { a + b }\n"
        "fn sub8(a: i8, b: i8) -> int { a - b }\n"
        "fn mul8(a: i8, b: i8) -> int { a * b }\n");
    for (int64_t a : {0, 1, 100, 127, -128, -1, 50}) {
        for (int64_t b : {0, 1, 100, 127, -128, -1, 50}) {
            CHECK_EQ(diff(c, "add8", {a, b}), wrap(wrap(a, 8) + wrap(b, 8), 8));
            CHECK_EQ(diff(c, "sub8", {a, b}), wrap(wrap(a, 8) - wrap(b, 8), 8));
            CHECK_EQ(diff(c, "mul8", {a, b}), wrap(wrap(a, 8) * wrap(b, 8), 8));
        }
    }
}

TEST("types: i16 arithmetic wraps to 16 bits on all three engines") {
    C c;
    load(c,
        "fn add16(a: i16, b: i16) -> int { a + b }\n"
        "fn mul16(a: i16, b: i16) -> int { a * b }\n");
    for (int64_t a : {0, 1, 20000, 32767, -32768, -1, 200}) {
        for (int64_t b : {0, 1, 20000, 32767, -32768, -1, 200}) {
            CHECK_EQ(diff(c, "add16", {a, b}), wrap(wrap(a, 16) + wrap(b, 16), 16));
            CHECK_EQ(diff(c, "mul16", {a, b}), wrap(wrap(a, 16) * wrap(b, 16), 16));
        }
    }
}

TEST("types: narrow-typed incoming arguments are sign-extended (interp==simple==ra)") {
    C c;
    load(c,
        "fn id8(a: i8) -> int { a }\n"
        "fn id16(a: i16) -> int { a }\n"
        "fn id32(a: i32) -> int { a }\n");
    for (int64_t v : std::vector<int64_t>{0, 200, 255, 256, -1, 128, 32768, 65535, 70000,
                                          2147483648LL, INT64_MAX, INT64_MIN}) {
        CHECK_EQ(diff(c, "id8", {v}), wrap(v, 8));
        CHECK_EQ(diff(c, "id16", {v}), wrap(v, 16));
        CHECK_EQ(diff(c, "id32", {v}), wrap(v, 32));
    }
}

TEST("types: narrow types thread through a loop (i8 accumulator wraps each step)") {
    C c;
    load(c, "fn s(a: i8, n: int) -> int { var acc = a; var i = 0; while i < n { acc = acc + a; i = i + 1; } return acc; }\n");
    for (int64_t a : {1, 7, 50, 100, -100}) {
        for (int64_t n = 0; n <= 10; n++) {
            int64_t acc = wrap(a, 8);
            for (int64_t i = 0; i < n; i++) acc = wrap(acc + wrap(a, 8), 8);
            CHECK_EQ(diff(c, "s", {a, n}), acc);
        }
    }
}
