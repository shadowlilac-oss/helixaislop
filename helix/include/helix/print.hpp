// Helix textual printer — renders the graph in the canonical, diffable textual
// format (DC16). Stable, indented, semantic-name-free-of-noise output.
#pragma once
#include <string>

#include "helix/ir.hpp"

namespace helix {

std::string print_func(World& w, NodeId func);
std::string print_module(World& w);

}  // namespace helix
