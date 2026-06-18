// Helix graph interpreter — the reference semantics. The SAME engine evaluates
// compile-time code (see comptime), and the JIT backend is validated against it.
// Covers the pure + structured-control + call subset (ConstInt/Bool, Param,
// arithmetic, comparisons, select, Cond, Loop, Call). Bounded by fuel so that
// (Turing-complete) comptime evaluation terminates with a diagnostic. R2/DC11.
#pragma once
#include <cstdint>
#include <vector>

#include "helix/ir.hpp"

namespace helix {

struct EvalResult {
    bool ok = false;          // false on error (e.g. ran out of fuel)
    int64_t value = 0;        // the function's result
    bool out_of_fuel = false;
    long fuel_used = 0;
};

// Evaluate `func` with the given integer arguments.
EvalResult eval_func(World& w, NodeId func, const std::vector<int64_t>& args,
                     long fuel = 50'000'000);

}  // namespace helix
