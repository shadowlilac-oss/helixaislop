#include "test.hpp"

int main() {
    int pass = 0, fail = 0;
    long checks_before_total = 0;
    std::printf("running %zu tests\n\n", ht::registry().size());
    for (auto& t : ht::registry()) {
        long before = ht::fail_count();
        long checks_before = ht::check_count();
        t.fn();
        long new_fails = ht::fail_count() - before;
        long new_checks = ht::check_count() - checks_before;
        if (new_fails == 0) {
            std::printf("[  ok  ] %-40s (%ld checks)\n", t.name, new_checks);
            pass++;
        } else {
            std::printf("[ FAIL ] %-40s (%ld checks, %ld failed)\n", t.name, new_checks, new_fails);
            fail++;
        }
        checks_before_total += new_checks;
    }
    std::printf("\n========================================\n");
    std::printf("%d tests: %d passed, %d failed | %ld checks total\n",
                (int)ht::registry().size(), pass, fail, ht::check_count());
    std::printf("========================================\n");
    return fail == 0 ? 0 : 1;
}
