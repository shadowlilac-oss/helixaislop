// repro_imp — run a single Helix source through interp / simple-jit / ra-jit and
// print each engine's return value and final array, for minimizing a fuzzer mismatch.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "helix/backend.hpp"
#include "helix/eval.hpp"
#include "helix/front.hpp"
#include "helix/ir.hpp"

using namespace helix;

static int64_t addr_of(const int64_t* p) { return (int64_t)(uintptr_t)p; }

static void run_one(const char* label, const std::string& src,
                    const std::vector<int64_t>& input, int64_t n) {
    std::printf("==== %s ====\n%s\n", label, src.c_str());
    World w;
    ParseStatus ps = parse_module(w, src);
    if (!ps.ok) { std::printf("PARSE FAIL\n\n"); return; }
    JitModule simple = jit_compile(w);
    JitModule ra = jit_compile_ra(w);
    if (!simple.ok) { std::printf("simple jit FAIL\n"); }
    if (!ra.ok) { std::printf("ra jit FAIL\n"); }
    NodeId f = w.find_func("f");
    if (f == NONE) { std::printf("no func f\n\n"); return; }

    std::vector<int64_t> a0 = input, a1 = input, a2 = input;
    std::vector<int64_t> args0 = {addr_of(a0.data()), n};
    std::vector<int64_t> args1 = {addr_of(a1.data()), n};
    std::vector<int64_t> args2 = {addr_of(a2.data()), n};

    EvalResult er = eval_func(w, f, args0, 200'000'000);
    int64_t sv = simple.ok ? simple.call(f, args1) : 0;
    int64_t rv = ra.ok ? ra.call(f, args2) : 0;
    std::printf("interp ok=%d oof=%d value=%lld\n", er.ok, er.out_of_fuel,
                (long long)er.value);
    std::printf("simple value=%lld\n", (long long)sv);
    std::printf("ra     value=%lld\n", (long long)rv);
    bool agree = er.ok && !er.out_of_fuel && sv == er.value && rv == er.value;
    std::printf("RETURN AGREE: %s\n\n", agree ? "yes" : "NO");
}

int main() {
    // Seed-1 smallest repro, exact input prefix (n=21).
    std::vector<int64_t> in = {
        66030, -95297, 31, 28038, 7473194123193036865LL, 2, -8109390180464281517LL,
        2, 74, 541221264386535116LL, 22, 29, -34825, -4770737320281964086LL,
        -5684125287886785097LL, -75, 2, 84700, -96295, 2147483647, -8086575979006384633LL};
    while ((int)in.size() < 64) in.push_back(0);

    run_one("ORIG seed1 repro", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = n;
  var t0 = 1;
  while t0 < n {
    var t1 = 1;
    while t1 < (n - 1) {
      acc0 = (a[(t1 + 1)] ^ acc0);
      t1 = t1 + 1;
    }
    if (t0 <= (-14)) {
      acc0 = (acc0 << 44);
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 21);

    // M1: drop the dead if (condition t0 <= -14 is never true for t0 in [1,n)).
    run_one("M1 drop dead if", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = n;
  var t0 = 1;
  while t0 < n {
    var t1 = 1;
    while t1 < (n - 1) {
      acc0 = (a[(t1 + 1)] ^ acc0);
      t1 = t1 + 1;
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 21);

    // M2: single loop, accumulate xor of a[t1+1].
    run_one("M2 single loop xor", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = n;
  var t1 = 1;
  while t1 < (n - 1) {
    acc0 = (a[(t1 + 1)] ^ acc0);
    t1 = t1 + 1;
  }
  return acc0;
}
)", in, 21);

    // M3: single loop, just read a[t1] (index without offset) and add.
    run_one("M3 single loop add a[t1]", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = 0;
  var t1 = 0;
  while t1 < n {
    acc0 = (a[t1] + acc0);
    t1 = t1 + 1;
  }
  return acc0;
}
)", in, 21);

    // M4: single loop, read a[t1+1] and add (offset index).
    run_one("M4 single loop add a[t1+1]", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = 0;
  var t1 = 1;
  while t1 < (n - 1) {
    acc0 = (a[(t1 + 1)] + acc0);
    t1 = t1 + 1;
  }
  return acc0;
}
)", in, 21);

    // M5: nested loops, inner accumulates via read; outer has an if that is ALWAYS
    // TRUE (t0 >= 0). The if body modifies acc0.
    run_one("M5 nested + always-true if", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = n;
  var t0 = 1;
  while t0 < n {
    var t1 = 1;
    while t1 < (n - 1) {
      acc0 = (a[(t1 + 1)] ^ acc0);
      t1 = t1 + 1;
    }
    if (t0 >= 0) {
      acc0 = (acc0 << 44);
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 21);

    // M6: nested loops, NO array read in inner (pure scalar), dead if after.
    run_one("M6 nested no-read + dead if", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = n;
  var t0 = 1;
  while t0 < n {
    var t1 = 1;
    while t1 < (n - 1) {
      acc0 = (acc0 + t1);
      t1 = t1 + 1;
    }
    if (t0 <= (-14)) {
      acc0 = (acc0 << 44);
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 21);

    // M7: single OUTER loop (no inner), read a[t0], dead if after the read.
    run_one("M7 single loop + dead if", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = n;
  var t0 = 1;
  while t0 < n {
    acc0 = (a[t0] ^ acc0);
    if (t0 <= (-14)) {
      acc0 = (acc0 << 44);
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 21);

    // M8: nested loops + read, but if has NO shift (just an add) — isolate shift-by-44.
    run_one("M8 nested + dead if (add not shift)", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = n;
  var t0 = 1;
  while t0 < n {
    var t1 = 1;
    while t1 < (n - 1) {
      acc0 = (a[(t1 + 1)] ^ acc0);
      t1 = t1 + 1;
    }
    if (t0 <= (-14)) {
      acc0 = (acc0 + 7);
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 21);

    // MIN: fully minimized, no arrays, no inner-loop body work beyond the counter.
    // Two nested counted loops; a never-taken if in the outer body after the inner loop.
    run_one("MIN minimal no-array repro", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = 5;
  var t0 = 0;
  while t0 < n {
    var t1 = 0;
    while t1 < n {
      t1 = t1 + 1;
    }
    if (acc0 > 1000000) {
      acc0 = 9;
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 4);

    // MIN2: same but if condition references the loop counter (always false).
    run_one("MIN2 minimal, cond on counter", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = 5;
  var t0 = 0;
  while t0 < n {
    var t1 = 0;
    while t1 < n {
      t1 = t1 + 1;
    }
    if (t0 < 0) {
      acc0 = 9;
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 4);

    // MIN3: control — NO inner loop, just the dead if. Expect agreement (matches M7).
    run_one("MIN3 control no inner loop", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = 5;
  var t0 = 0;
  while t0 < n {
    if (t0 < 0) {
      acc0 = 9;
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 4);

    // MIN4: inner loop, but if is ALWAYS TAKEN. Expect agreement (matches M5).
    run_one("MIN4 inner loop + always-taken if", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = 5;
  var t0 = 0;
  while t0 < n {
    var t1 = 0;
    while t1 < n {
      t1 = t1 + 1;
    }
    if (t0 >= 0) {
      acc0 = 9;
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 4);

    // MIN5: acc0 is mutated INSIDE the inner loop, then a never-taken if mutates it
    // after the inner loop. No arrays. This is the suspected minimal trigger.
    run_one("MIN5 acc mutated in inner + dead if", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = 5;
  var t0 = 0;
  while t0 < n {
    var t1 = 0;
    while t1 < n {
      acc0 = (acc0 + 1);
      t1 = t1 + 1;
    }
    if (t0 < 0) {
      acc0 = 9;
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 4);

    // MIN6: same as MIN5 but WITHOUT the dead if (control). Expect agreement.
    run_one("MIN6 acc mutated in inner, NO if", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = 5;
  var t0 = 0;
  while t0 < n {
    var t1 = 0;
    while t1 < n {
      acc0 = (acc0 + 1);
      t1 = t1 + 1;
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 4);

    // MIN5b: MIN5 but inner loop starts at 1 with bound (n-1), like M6, n=21.
    run_one("MIN5b inner t1=1 bound n-1", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = 5;
  var t0 = 0;
  while t0 < n {
    var t1 = 1;
    while t1 < (n - 1) {
      acc0 = (acc0 + 1);
      t1 = t1 + 1;
    }
    if (t0 < 0) {
      acc0 = 9;
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 21);

    // MIN5c: MIN5 but outer t0 starts at 1 (bound n), inner t1=0 bound n, n=21.
    run_one("MIN5c outer t0=1", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = 5;
  var t0 = 1;
  while t0 < n {
    var t1 = 0;
    while t1 < n {
      acc0 = (acc0 + 1);
      t1 = t1 + 1;
    }
    if (t0 < 0) {
      acc0 = 9;
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 21);

    // MIN5d: exactly M6's shape but inner body uses constant (+1) not t1, n=21.
    run_one("MIN5d M6-shape const inner", R"(fn f(a: ptr, n: int) -> int {
  var acc0 = n;
  var t0 = 1;
  while t0 < n {
    var t1 = 1;
    while t1 < (n - 1) {
      acc0 = (acc0 + 1);
      t1 = t1 + 1;
    }
    if (t0 <= (-14)) {
      acc0 = (acc0 + 7);
    }
    t0 = t0 + 1;
  }
  return acc0;
}
)", in, 21);

    return 0;
}
