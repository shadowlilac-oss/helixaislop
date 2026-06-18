// Verifies the string-table path for symbol names longer than 8 bytes,
// including an external (undefined) symbol referenced by a reloc.
#include "helix/coff.hpp"
#include <cstdio>
#include <fstream>
using namespace helix;
int main() {
    ObjModule m;
    // helix_return_value (>8 chars) defined; call into an external long name.
    m.text = {0xB8, 0x2A, 0x00, 0x00, 0x00,  // mov eax,42
              0xE8, 0x00, 0x00, 0x00, 0x00,   // call rel32 -> external
              0xC3};                          // ret
    CoffSymbol def; def.name = "helix_return_value"; def.offset = 0;
    def.defined = true; def.is_function = true;
    CoffSymbol ext; ext.name = "external_helper_fn"; ext.offset = 0;
    ext.defined = false; ext.is_function = true;
    m.symbols.push_back(def);
    m.symbols.push_back(ext);
    CoffReloc r; r.offset = 6; r.target_symbol = "external_helper_fn"; r.type = 4;
    m.relocs.push_back(r);
    auto obj = write_coff_x64(m);
    std::ofstream f("E:\\IRGraph\\helix\\scratch_coff\\hxlong.obj", std::ios::binary);
    f.write(reinterpret_cast<const char*>(obj.data()), (std::streamsize)obj.size());
    std::printf("wrote hxlong.obj (%zu bytes)\n", obj.size());
    return 0;
}
