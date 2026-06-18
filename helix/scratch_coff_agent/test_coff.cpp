// Standalone end-to-end test for write_coff_x64.
// Builds ObjModule(s), serializes to .obj, and the driver compiles/links/runs.
#include "helix/coff.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace helix;

static bool write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

int main(int argc, char** argv) {
    // argv[1] selects which object to emit: "single" or "two".
    std::string which = (argc > 1) ? argv[1] : "single";
    std::string out_path = (argc > 2) ? argv[2] : "hx.obj";

    ObjModule m;

    if (which == "single") {
        // hxret42: mov eax,42 ; ret  -> returns 42.
        m.text = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
        CoffSymbol s;
        s.name = "hxret42";
        s.offset = 0;
        s.defined = true;
        s.is_function = true;
        m.symbols.push_back(s);
    } else if (which == "two") {
        // Two functions:
        //   hxret42 @ offset 0:  mov eax,42 ; ret        (6 bytes)
        //   hxcall  @ offset 6:  call hxret42 ; ret      (e8 rel32; c3)
        // hxcall tail-returns whatever hxret42 returns => 42.
        m.text = {
            // hxret42 (offset 0)
            0xB8, 0x2A, 0x00, 0x00, 0x00,  // mov eax, 42
            0xC3,                          // ret
            // hxcall (offset 6)
            0xE8, 0x00, 0x00, 0x00, 0x00,  // call rel32 (-> hxret42), patched by reloc
            0xC3,                          // ret
        };
        CoffSymbol a;
        a.name = "hxret42";
        a.offset = 0;
        a.defined = true;
        a.is_function = true;
        m.symbols.push_back(a);

        CoffSymbol b;
        b.name = "hxcall";
        b.offset = 6;
        b.defined = true;
        b.is_function = true;
        m.symbols.push_back(b);

        // Relocation: the call rel32 field is at text offset 7 (after 0xE8),
        // referencing hxret42. REL32 = 4. link.exe computes the displacement.
        CoffReloc r;
        r.offset = 7;
        r.target_symbol = "hxret42";
        r.type = 4;  // IMAGE_REL_AMD64_REL32
        m.relocs.push_back(r);
    } else {
        std::fprintf(stderr, "unknown mode: %s\n", which.c_str());
        return 2;
    }

    std::vector<uint8_t> obj = write_coff_x64(m);
    if (!write_file(out_path, obj)) {
        std::fprintf(stderr, "failed to write %s\n", out_path.c_str());
        return 3;
    }
    std::printf("wrote %s (%zu bytes), mode=%s\n", out_path.c_str(), obj.size(),
                which.c_str());
    return 0;
}
