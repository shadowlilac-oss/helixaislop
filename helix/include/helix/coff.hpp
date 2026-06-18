// COFF (x86-64) object-file writer contract. Lets Helix emit a real .obj that
// MSVC's link.exe can link into an executable — direct-to-binary, no secondary
// toolchain IR. See wiki/17-codegen.md (DC13).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace helix {

// A symbol defined in (or referenced by) the .text section.
struct CoffSymbol {
    std::string name;     // decorated name as link.exe expects (e.g. plain "fib" for C linkage)
    uint32_t offset = 0;  // byte offset within .text where the symbol is defined
    bool defined = true;  // false => external/undefined reference
    bool is_function = true;
};

// A relocation inside .text. type uses IMAGE_REL_AMD64_* values:
//   REL32 = 4 (rip-relative call/jmp), ADDR64 = 1 (absolute 64-bit).
struct CoffReloc {
    uint32_t offset = 0;          // byte offset in .text of the field to patch
    std::string target_symbol;    // symbol the relocation refers to
    uint16_t type = 4;            // IMAGE_REL_AMD64_REL32
};

struct ObjModule {
    std::vector<uint8_t> text;          // machine code (.text)
    std::vector<CoffSymbol> symbols;    // defined + external symbols
    std::vector<CoffReloc> relocs;      // relocations within .text
};

// Serialize `m` into a COFF object-file image suitable for `link.exe`.
std::vector<uint8_t> write_coff_x64(const ObjModule& m);

}  // namespace helix
