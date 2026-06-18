// Minimal self-contained test framework for the Helix test suite.
// Tests register themselves via the TEST(name){...} macro; run_tests.cpp runs all.
#pragma once
#include <cstdio>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ht {

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}
inline long& fail_count() { static long f = 0; return f; }
inline long& check_count() { static long c = 0; return c; }

struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

inline void report_fail(const char* file, int line, const char* msg) {
    fail_count()++;
    std::printf("    FAIL %s:%d  %s\n", file, line, msg);
}

}  // namespace ht

#define HELIX_CAT2(a, b) a##b
#define HELIX_CAT(a, b) HELIX_CAT2(a, b)

#define TEST(name)                                                       \
    static void HELIX_CAT(helix_test_fn_, __LINE__)();                   \
    static ht::Reg HELIX_CAT(helix_test_reg_, __LINE__)(                 \
        name, HELIX_CAT(helix_test_fn_, __LINE__));                      \
    static void HELIX_CAT(helix_test_fn_, __LINE__)()

#define CHECK(cond)                                                      \
    do {                                                                 \
        ht::check_count()++;                                             \
        if (!(cond)) ht::report_fail(__FILE__, __LINE__, #cond);         \
    } while (0)

#define CHECK_EQ(a, b)                                                   \
    do {                                                                 \
        ht::check_count()++;                                             \
        auto helix_va = (a);                                             \
        auto helix_vb = (b);                                             \
        if (!(helix_va == helix_vb)) {                                   \
            char buf[256];                                               \
            std::snprintf(buf, sizeof(buf), "%s == %s  (got %lld vs %lld)", \
                          #a, #b, (long long)helix_va, (long long)helix_vb); \
            ht::report_fail(__FILE__, __LINE__, buf);                    \
        }                                                                \
    } while (0)

// For non-integral equality (strings, etc.)
#define CHECK_TRUE(cond) CHECK(cond)
