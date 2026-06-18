// Helix verifier — checks the four structural invariants (wiki/11-core-model.md):
// acyclic DAG, single-origin SSA operands, well-formed regions, and the LINEAR
// state discipline (each state token consumed exactly once) that Helix enforces
// where MimIR left it unchecked (DC4).
#pragma once
#include <string>
#include <vector>

#include "helix/ir.hpp"

namespace helix {

struct VerifyResult {
    bool ok = true;
    std::vector<std::string> errors;
};

// Verify a single function's body.
VerifyResult verify_func(World& w, NodeId func);
// Verify every function in the module.
VerifyResult verify_module(World& w);

// Number of distinct nodes reachable from a function's result (live nodes).
size_t reachable_count(World& w, NodeId func);

}  // namespace helix
