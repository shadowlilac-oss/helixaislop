// Helix backend — JIT-compile the module's functions to real x86-64 machine code
// and run them. No secondary IR: we walk the graph and emit bytes directly
// (DC13). Validated differentially against the interpreter. See wiki/17-codegen.md.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "helix/ir.hpp"

namespace helix {

class JitModule {
public:
    JitModule() = default;
    ~JitModule();
    JitModule(JitModule&&) noexcept;
    JitModule& operator=(JitModule&&) noexcept;
    JitModule(const JitModule&) = delete;
    JitModule& operator=(const JitModule&) = delete;

    bool ok = false;
    std::string err;

    bool has(NodeId func) const { return entries_.count(func) != 0; }
    // Call a JIT-compiled function with up to 4 integer arguments.
    int64_t call(NodeId func, const std::vector<int64_t>& args) const;

    void* base() const { return mem_; }
    void adopt(void* mem, size_t size, std::unordered_map<NodeId, size_t> entries) {
        mem_ = mem; size_ = size; entries_ = std::move(entries);
    }

private:
    void* mem_ = nullptr;
    size_t size_ = 0;
    std::unordered_map<NodeId, size_t> entries_;  // func node -> byte offset
};

// Compile every function in the module. The returned module owns executable memory.
// jit_compile      — simple, always-correct memory-backed codegen (the oracle baseline).
// jit_compile_ra   — optimizing backend: instruction selection + liveness +
//                    linear-scan register allocation (callee-saved homes, stack spills).
JitModule jit_compile(World& w);
JitModule jit_compile_ra(World& w);

}  // namespace helix
