// repro_wr — sanity-check array WRITES across interp / simple-jit / ra-jit, checking
// BOTH the return value AND the final array contents agree.
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

static void run(const char* label, const std::string& src, const char* fn,
                std::vector<int64_t> arr, int64_t n) {
    std::printf("==== %s ====\n", label);
    World w;
    ParseStatus ps = parse_module(w, src);
    if (!ps.ok) { std::printf("PARSE FAIL (line %d): %s\n\n", ps.line, ps.msg.c_str()); return; }
    JitModule simple = jit_compile(w), ra = jit_compile_ra(w);
    if (!simple.ok) { std::printf("simple FAIL: %s\n", simple.err.c_str()); }
    if (!ra.ok) { std::printf("ra FAIL: %s\n", ra.err.c_str()); }
    NodeId f = w.find_func(fn);
    if (f == NONE) { std::printf("no func %s\n\n", fn); return; }

    std::vector<int64_t> a0 = arr, a1 = arr, a2 = arr;
    EvalResult er = eval_func(w, f, {addr_of(a0.data()), n}, 200'000'000);
    int64_t sv = simple.ok ? simple.call(f, {addr_of(a1.data()), n}) : 0;
    int64_t rv = ra.ok ? ra.call(f, {addr_of(a2.data()), n}) : 0;

    bool ret_ok = er.ok && sv == er.value && rv == er.value;
    bool arr_ok = (a1 == a0) && (a2 == a0);
    std::printf("return interp=%lld simple=%lld ra=%lld | array %s | %s\n",
                (long long)er.value, (long long)sv, (long long)rv,
                arr_ok ? "match" : "DIFFER",
                (ret_ok && arr_ok) ? "OK" : "MISMATCH");
    if (!arr_ok) {
        auto dump = [&](const char* t, std::vector<int64_t>& a) {
            std::printf("  %s:", t);
            for (int i = 0; i < (int)n; i++) std::printf(" %lld", (long long)a[i]);
            std::printf("\n");
        };
        dump("interp", a0); dump("simple", a1); dump("ra    ", a2);
    }
    std::printf("\n");
}

int main() {
    std::vector<int64_t> arr = {5, 3, 9, 1, 7, 2, 8, 4, 6, 0, -3, 11};

    run("fill a[i]=i*i", R"(
fn f(a: ptr, n: int) -> int {
  var i = 0;
  while i < n { a[i] = i * i; i = i + 1; }
  return 0;
}
)", "f", arr, 12);

    run("conditional write (clamp negatives to 0)", R"(
fn f(a: ptr, n: int) -> int {
  var i = 0;
  while i < n { if a[i] < 0 { a[i] = 0; } i = i + 1; }
  return 0;
}
)", "f", arr, 12);

    run("read-after-write same cell", R"(
fn f(a: ptr, n: int) -> int {
  var i = 0; var s = 0;
  while i < n { a[i] = a[i] + 1; s = s + a[i]; i = i + 1; }
  return s;
}
)", "f", arr, 12);

    run("reverse in place", R"(
fn f(a: ptr, n: int) -> int {
  var i = 0; var j = n - 1;
  while i < j { var t = a[i]; a[i] = a[j]; a[j] = t; i = i + 1; j = j - 1; }
  return 0;
}
)", "f", arr, 12);

    run("bubble sort", R"(
fn f(a: ptr, n: int) -> int {
  var i = 0;
  while i < n {
    var j = 0;
    while j < n - 1 {
      if a[j] > a[j + 1] {
        var t = a[j]; a[j] = a[j + 1]; a[j + 1] = t;
      }
      j = j + 1;
    }
    i = i + 1;
  }
  return 0;
}
)", "f", arr, 12);

    return 0;
}
