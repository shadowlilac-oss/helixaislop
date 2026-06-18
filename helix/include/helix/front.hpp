// Helix frontend — parses a small structured surface language DIRECTLY into the
// graph (DC12): no AST -> IR lowering stage. Expressions intern into the value
// strand (GVN during parsing); `if`/`loop` build Cond/Loop regions; `comptime`
// calls with constant args fold via the interpreter (comptime = graph reduction).
#pragma once
#include <string>

#include "helix/ir.hpp"

namespace helix {

struct ParseStatus {
    bool ok = true;
    std::string msg;
    int line = 0;
};

// Parse a whole module of `fn` declarations into `w`. Functions are added to the
// module (w.module_funcs). Supports mutual recursion (two-pass over headers).
ParseStatus parse_module(World& w, const std::string& src);

}  // namespace helix
