// Read-only memory tests: array reductions over REAL arrays. The interpreter and
// both backends dereference the same real addresses, so they must all agree.
#include <cstdint>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"
#include "test.hpp"

using namespace helix;

static int64_t addr_of(const int64_t* p) { return (int64_t)(uintptr_t)p; }

TEST("memory: sum / max / dot over a real array (interp == simple == ra)") {
    World w;
    auto st = parse_module(w,
        "fn sum_arr(a: ptr, n: int) -> int {\n"
        "  loop (acc=0, i=0) { if i >= n { break acc } else { next acc + a[i], i+1 } }\n"
        "}\n"
        "fn max_arr(a: ptr, n: int) -> int {\n"
        "  loop (m=a[0], i=1) { if i >= n { break m }\n"
        "    else { let x = a[i]; if x > m { next x, i+1 } else { next m, i+1 } } }\n"
        "}\n"
        "fn dot(a: ptr, b: ptr, n: int) -> int {\n"
        "  loop (acc=0, i=0) { if i >= n { break acc } else { next acc + a[i]*b[i], i+1 } }\n"
        "}\n");
    if (!st.ok) std::printf("    parse error (line %d): %s\n", st.line, st.msg.c_str());
    CHECK(st.ok);

    JitModule simple = jit_compile(w);
    JitModule ra = jit_compile_ra(w);
    CHECK(simple.ok);
    CHECK(ra.ok);

    int64_t arr[] = {5, 3, 9, 1, 7, 2, 8, 4, 6, 0, -4, 11};
    int64_t brr[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const int n = 12;
    int64_t rsum = 0, rmax = arr[0], rdot = 0;
    for (int i = 0; i < n; i++) { rsum += arr[i]; if (arr[i] > rmax) rmax = arr[i]; rdot += arr[i] * brr[i]; }

    NodeId fsum = w.find_func("sum_arr"), fmax = w.find_func("max_arr"), fdot = w.find_func("dot");
    std::vector<int64_t> sa = {addr_of(arr), n};
    CHECK_EQ(eval_func(w, fsum, sa).value, rsum);
    CHECK_EQ(simple.call(fsum, sa), rsum);
    CHECK_EQ(ra.call(fsum, sa), rsum);

    CHECK_EQ(eval_func(w, fmax, sa).value, rmax);
    CHECK_EQ(simple.call(fmax, sa), rmax);
    CHECK_EQ(ra.call(fmax, sa), rmax);

    std::vector<int64_t> da = {addr_of(arr), addr_of(brr), n};
    CHECK_EQ(eval_func(w, fdot, da).value, rdot);
    CHECK_EQ(simple.call(fdot, da), rdot);
    CHECK_EQ(ra.call(fdot, da), rdot);

    // sub-range sums also agree, exercising the index arithmetic
    for (int k = 1; k <= n; k++) {
        int64_t ref = 0; for (int i = 0; i < k; i++) ref += arr[i];
        std::vector<int64_t> ka = {addr_of(arr), k};
        CHECK_EQ(ra.call(fsum, ka), ref);
        CHECK_EQ(simple.call(fsum, ka), ref);
    }
}

TEST("memory: pure load is CSE'd (a[i] read twice is one load node)") {
    World w;
    parse_module(w, "fn f(a: ptr, i: int) -> int { a[i] + a[i] }\n");
    NodeId body = w.func_info(w.find_func("f")).result;
    CHECK_EQ(w.node(body).op, Op::Add);
    CHECK_EQ(w.node(body).ins[0], w.node(body).ins[1]);  // same interned load
}
