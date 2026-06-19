// Helix middle-end optimization passes operating directly on the graph. GVN,
// constant folding, algebraic identities and CSE already happen at construction
// (smart constructors); these are the inter-node / inter-procedural passes.
// See wiki/16-optimizations.md.
#pragma once
#include <vector>

#include "helix/ir.hpp"

namespace helix {

// Inline a single Call node: return a node equal to the callee's body with its
// parameters substituted by the call's arguments. `depth` bounds nested inlining
// (so recursive callees unroll a bounded number of times, then stay as calls).
NodeId inline_call(World& w, NodeId call_node, int depth = 1);

// Rewrite a function's body, inlining the calls it makes up to `max_depth`.
// Re-runs the smart constructors, so inlined constant-argument calls fold.
void inline_into(World& w, NodeId func, int max_depth = 1);

// The set of functions reachable from `roots` through the call graph (for dead-
// function elimination). Order is deterministic (roots first, then discovery).
std::vector<NodeId> reachable_functions(World& w, const std::vector<NodeId>& roots);

// Run the middle-end on every function in the module: inline calls (re-folding via
// the smart constructors). Only PURE functions are transformed — the Cloner is sound
// only on the value strand, so functions that thread memory state are left untouched.
// This is what `helixc -O` invokes; validated by the opt-vs-unopt differential fuzzer.
void optimize_module(World& w, int inline_depth = 1);

}  // namespace helix
