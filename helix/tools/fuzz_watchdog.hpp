// Shared watchdog for the differential fuzzers.
//
// Every fuzzer runs the reference interpreter FIRST under a fuel cap and only compares
// a case if the interpreter ran it to completion. So if a JIT-compiled call then runs
// FOREVER, that is a real miscompile (typically a mis-scheduled loop exit) — not a slow
// test. Native code stuck in an infinite loop cannot be interrupted safely, so a single
// persistent monitor thread watches a deadline the main thread arms around each call;
// if the deadline passes it prints the repro and exits the process fast (each seed runs
// as its own process, so the outer driver simply moves on). O(1) threads — the main
// thread runs the calls directly, with no per-call thread churn.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

struct FuzzWatchdog {
    std::atomic<bool> in_call{false};
    std::atomic<long long> deadline_ms{0};
    const std::string* src = nullptr;  // current program source (read only on a hang)
    std::string context;               // caller-formatted repro context (fn, args, input)

    static long long now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    void start() {
        std::thread([this] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (in_call.load() && now_ms() > deadline_ms.load()) {
                    std::printf("\n!!!! JIT NON-TERMINATION (interp terminated; a backend "
                                "looped forever) !!!!\n%s\n", context.c_str());
                    if (src) std::printf("---- program source ----\n%s\n", src->c_str());
                    std::fflush(stdout);
                    std::_Exit(3);
                }
            }
        }).detach();
    }
    void arm(const std::string& s, std::string ctx, int timeout_ms) {
        src = &s;
        context = std::move(ctx);
        deadline_ms.store(now_ms() + timeout_ms);
        in_call.store(true);
    }
    void disarm() { in_call.store(false); }
};
