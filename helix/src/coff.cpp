// COFF (x86-64) object-file writer. Serializes an ObjModule into a valid
// IMAGE_FILE_MACHINE_AMD64 (0x8664) object that MSVC's link.exe can consume.
//
// Layout produced (in file order):
//   [0]                      COFF file header              (20 bytes)
//   [20]                     .text section header          (40 bytes)
//   [60]                     .text raw data                (m.text.size() bytes)
//   [...]                    .text relocations             (10 bytes each)
//   [...]                    symbol table                  (18 bytes each)
//   [...]                    string table                  (4-byte size + blobs)
//
// All multi-byte integer fields are little-endian, matching x86-64 / PE-COFF.
#include "helix/coff.hpp"

#include <cstring>
#include <unordered_map>

namespace helix {

namespace {

// ---- PE/COFF constants -----------------------------------------------------
constexpr uint16_t kMachineAmd64 = 0x8664;

// Section characteristics for a typical code section.
constexpr uint32_t kSCN_CNT_CODE = 0x00000020;
constexpr uint32_t kSCN_MEM_EXECUTE = 0x20000000;
constexpr uint32_t kSCN_MEM_READ = 0x40000000;
// 16-byte alignment for code (IMAGE_SCN_ALIGN_16BYTES).
constexpr uint32_t kSCN_ALIGN_16 = 0x00500000;

// Symbol storage classes / section numbers.
constexpr uint8_t kIMAGE_SYM_CLASS_EXTERNAL = 2;
constexpr int16_t kIMAGE_SYM_UNDEFINED = 0;  // section number 0 => undefined/external
// Symbol type: 0x20 in the high byte marks a function (DT_FUNCTION << 4).
constexpr uint16_t kSYM_TYPE_FUNCTION = 0x20;
constexpr uint16_t kSYM_TYPE_NULL = 0x00;

// Each on-disk record size.
constexpr size_t kFileHeaderSize = 20;
constexpr size_t kSectionHeaderSize = 40;
constexpr size_t kRelocSize = 10;
constexpr size_t kSymbolSize = 18;

// ---- little-endian append helpers -----------------------------------------
void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}
void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

}  // namespace

std::vector<uint8_t> write_coff_x64(const ObjModule& m) {
    // -----------------------------------------------------------------------
    // 1. Build the string table first so symbol records can reference it.
    //    COFF symbol names <= 8 bytes are stored inline; longer names go in a
    //    string table and the symbol record holds a 4-byte offset (with the
    //    first 4 name bytes zero). The string table is prefixed by its own
    //    total size (4 bytes), and offsets are measured from the start of that
    //    size field, so the first string lands at offset 4.
    // -----------------------------------------------------------------------
    std::vector<uint8_t> string_table;
    put_u32(string_table, 0);  // placeholder for total size; patched at the end.

    // Map a symbol name -> its 4-byte string-table offset (only for long names).
    auto intern_name = [&](const std::string& name) -> uint32_t {
        uint32_t off = static_cast<uint32_t>(string_table.size());
        string_table.insert(string_table.end(), name.begin(), name.end());
        string_table.push_back(0);  // NUL terminator
        return off;
    };

    // -----------------------------------------------------------------------
    // 2. Assign a symbol-table index to every symbol, in declaration order.
    //    Relocations reference symbols by their index in this table.
    // -----------------------------------------------------------------------
    std::unordered_map<std::string, uint32_t> sym_index;
    sym_index.reserve(m.symbols.size() * 2 + 1);
    for (uint32_t i = 0; i < m.symbols.size(); ++i) {
        sym_index.emplace(m.symbols[i].name, i);
    }

    // -----------------------------------------------------------------------
    // 3. Compute file offsets of each region.
    // -----------------------------------------------------------------------
    const uint32_t text_size = static_cast<uint32_t>(m.text.size());
    const uint32_t num_relocs = static_cast<uint32_t>(m.relocs.size());
    const uint32_t num_symbols = static_cast<uint32_t>(m.symbols.size());

    const uint32_t raw_data_offset =
        static_cast<uint32_t>(kFileHeaderSize + kSectionHeaderSize);  // 60
    const uint32_t reloc_offset = raw_data_offset + text_size;
    const uint32_t symtab_offset =
        reloc_offset + num_relocs * static_cast<uint32_t>(kRelocSize);

    // -----------------------------------------------------------------------
    // 4. COFF file header (20 bytes).
    // -----------------------------------------------------------------------
    std::vector<uint8_t> out;
    out.reserve(symtab_offset + num_symbols * kSymbolSize + 64);

    put_u16(out, kMachineAmd64);                  // Machine
    put_u16(out, 1);                              // NumberOfSections
    put_u32(out, 0);                              // TimeDateStamp (0 = deterministic)
    put_u32(out, num_symbols ? symtab_offset : 0);// PointerToSymbolTable
    put_u32(out, num_symbols);                    // NumberOfSymbols
    put_u16(out, 0);                              // SizeOfOptionalHeader (0 for objects)
    put_u16(out, 0);                              // Characteristics

    // -----------------------------------------------------------------------
    // 5. .text section header (40 bytes).
    // -----------------------------------------------------------------------
    {
        char name[8] = {'.', 't', 'e', 'x', 't', 0, 0, 0};
        out.insert(out.end(), name, name + 8);    // Name
    }
    put_u32(out, 0);                              // VirtualSize (0 in objects)
    put_u32(out, 0);                              // VirtualAddress (0 in objects)
    put_u32(out, text_size);                      // SizeOfRawData
    put_u32(out, text_size ? raw_data_offset : 0);// PointerToRawData
    put_u32(out, num_relocs ? reloc_offset : 0);  // PointerToRelocations
    put_u32(out, 0);                              // PointerToLinenumbers
    put_u16(out, static_cast<uint16_t>(num_relocs));  // NumberOfRelocations
    put_u16(out, 0);                              // NumberOfLinenumbers
    put_u32(out, kSCN_CNT_CODE | kSCN_MEM_EXECUTE | kSCN_MEM_READ |
                     kSCN_ALIGN_16);              // Characteristics

    // -----------------------------------------------------------------------
    // 6. .text raw data.
    // -----------------------------------------------------------------------
    out.insert(out.end(), m.text.begin(), m.text.end());

    // -----------------------------------------------------------------------
    // 7. Relocation table (10 bytes each):
    //    VirtualAddress (u32), SymbolTableIndex (u32), Type (u16).
    // -----------------------------------------------------------------------
    for (const CoffReloc& r : m.relocs) {
        auto it = sym_index.find(r.target_symbol);
        uint32_t idx = (it != sym_index.end()) ? it->second : 0;
        put_u32(out, r.offset);
        put_u32(out, idx);
        put_u16(out, r.type);
    }

    // -----------------------------------------------------------------------
    // 8. Symbol table (18 bytes each). No aux records are emitted.
    //    Name field: inline if <= 8 bytes, else zero-zero-offset into strtab.
    // -----------------------------------------------------------------------
    for (const CoffSymbol& s : m.symbols) {
        if (s.name.size() <= 8) {
            char name8[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            std::memcpy(name8, s.name.data(), s.name.size());
            out.insert(out.end(), name8, name8 + 8);
        } else {
            uint32_t str_off = intern_name(s.name);
            put_u32(out, 0);         // zeroes => name is in string table
            put_u32(out, str_off);   // offset into string table
        }
        put_u32(out, s.offset);      // Value (offset within section for defined)
        // SectionNumber: 1 (.text) if defined, 0 (UNDEFINED) if external.
        int16_t section_number =
            s.defined ? static_cast<int16_t>(1) : kIMAGE_SYM_UNDEFINED;
        put_u16(out, static_cast<uint16_t>(section_number));
        // Type: function symbols set the DT_FUNCTION nibble in the high byte.
        put_u16(out, s.is_function ? kSYM_TYPE_FUNCTION : kSYM_TYPE_NULL);
        out.push_back(kIMAGE_SYM_CLASS_EXTERNAL);  // StorageClass
        out.push_back(0);                          // NumberOfAuxSymbols
    }

    // -----------------------------------------------------------------------
    // 9. String table: patch its total size (includes the 4-byte size field
    //    itself) and append to the output.
    // -----------------------------------------------------------------------
    uint32_t strtab_size = static_cast<uint32_t>(string_table.size());
    string_table[0] = static_cast<uint8_t>(strtab_size & 0xFF);
    string_table[1] = static_cast<uint8_t>((strtab_size >> 8) & 0xFF);
    string_table[2] = static_cast<uint8_t>((strtab_size >> 16) & 0xFF);
    string_table[3] = static_cast<uint8_t>((strtab_size >> 24) & 0xFF);
    out.insert(out.end(), string_table.begin(), string_table.end());

    return out;
}

}  // namespace helix
