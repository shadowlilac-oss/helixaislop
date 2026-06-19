// Helix scheduler — Global Code Motion for the structured graph.
//
// The backends are demand-driven, but a value used from several control-divergent
// positions (e.g. an inner loop's result read in both arms of a following `if`) must
// be MATERIALIZED ONCE at a program point that DOMINATES every use, or one branch
// computes it and another reads a stale/garbage slot. This module computes, for every
// value node, the region in which it must be emitted: the lowest-common-ancestor (in
// the region tree) of all its uses.
//
// Region tree (induced by the structured control nodes):
//   * region 0            — the function body (root).
//   * each `Cond` node    — two child regions (one per arm / yield).
//   * each `Loop` node     — ONE child region (the loop body). Loop-carried params are
//                            available throughout this single region, so a value that
//                            depends on a param is never hoisted above the loop (which
//                            would reference an undefined param). Within the loop region
//                            emission is split into two PHASES around the exit test:
//                              - pre[body]:  the `is_break` (loop condition) cone — emitted
//                                            BEFORE the test, every iteration.
//                              - post[body]: everything else (next-value cone, nested
//                                            structures, body loads) — emitted AFTER the
//                                            test, so it never runs on the exiting round
//                                            (a body load a[i] at i==n would be OOB).
//
// Placement is speculation-safe for our op set: all arithmetic is total (division is
// guarded to never trap), so hoisting a pure value to a dominating region is always
// sound; loop/cond region nodes encapsulate their own control; and body memory loads
// stay in post[] (after the guard), never hoisted across the exit test.
#pragma once
#include <unordered_map>
#include <vector>

#include "helix/ir.hpp"

namespace helix {

struct Schedule {
    int root = 0;
    std::vector<int> parent;                 // region -> parent region (-1 for root)
    std::vector<int> depth;                  // region -> depth (root = 0)
    std::vector<std::vector<NodeId>> pre;    // region -> pre-test nodes (loop cond cone), topo order
    std::vector<std::vector<NodeId>> post;   // region -> remaining nodes, topo order
    std::unordered_map<NodeId, int> region;       // value node -> region it is emitted in
    std::unordered_map<NodeId, int> cond_arm0;    // Cond node -> region of yields[0] (else arm)
    std::unordered_map<NodeId, int> cond_arm1;    // Cond node -> region of yields[1] (then arm)
    std::unordered_map<NodeId, int> loop_body;    // Loop node -> body region

    int region_count() const { return (int)parent.size(); }
};

// Build the schedule for one function. Params are NOT placed (they are bound by the
// function entry / loop machinery); every other reachable value node is.
Schedule build_schedule(World& w, NodeId func);

}  // namespace helix
