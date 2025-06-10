#pragma once

#include "wiiu_ppc.h"
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>

// RPX File Format Structures
WIIU_PACKED
struct RPX_ElfHeader {
    uint8_t e_ident[16];
    be<uint16_t> e_type;
    be<uint16_t> e_machine;
    be<uint32_t> e_version;
    be<uint32_t> e_entry;
    be<uint32_t> e_phoff;
    be<uint32_t> e_shoff;
    be<uint32_t> e_flags;
    be<uint16_t> e_ehsize;
    be<uint16_t> e_phentsize;
    be<uint16_t> e_phnum;
    be<uint16_t> e_shentsize;
    be<uint16_t> e_shnum;
    be<uint16_t> e_shstrndx;
};
WIIU_PACKED_END

WIIU_PACKED
struct RPX_SectionHeader {
    be<uint32_t> sh_name;
    be<uint32_t> sh_type;
    be<uint32_t> sh_flags;
    be<uint32_t> sh_addr;
    be<uint32_t> sh_offset;
    be<uint32_t> sh_size;
    be<uint32_t> sh_link;
    be<uint32_t> sh_info;
    be<uint32_t> sh_addralign;
    be<uint32_t> sh_entsize;
};
WIIU_PACKED_END

WIIU_PACKED
struct RPX_ProgramHeader {
    be<uint32_t> p_type;
    be<uint32_t> p_offset;
    be<uint32_t> p_vaddr;
    be<uint32_t> p_paddr;
    be<uint32_t> p_filesz;
    be<uint32_t> p_memsz;
    be<uint32_t> p_flags;
    be<uint32_t> p_align;
};
WIIU_PACKED_END

WIIU_PACKED
struct RPX_SymbolEntry {
    be<uint32_t> st_name;
    be<uint32_t> st_value;
    be<uint32_t> st_size;
    uint8_t st_info;
    uint8_t st_other;
    be<uint16_t> st_shndx;
};
WIIU_PACKED_END

WIIU_PACKED
struct RPX_RelocationEntry {
    be<uint32_t> r_offset;
    be<uint32_t> r_info;
};
WIIU_PACKED_END

WIIU_PACKED
struct RPX_RelocationAddendEntry {
    be<uint32_t> r_offset;
    be<uint32_t> r_info;
    be<int32_t> r_addend;
};
WIIU_PACKED_END

// RPX Section types
enum RPX_SectionType : uint32_t {
    SHT_NULL = 0,
    SHT_PROGBITS = 1,
    SHT_SYMTAB = 2,
    SHT_STRTAB = 3,
    SHT_RELA = 4,
    SHT_NOBITS = 8,
    SHT_REL = 9,
    SHT_RPL_EXPORTS = 0x80000001,
    SHT_RPL_IMPORTS = 0x80000002,
    SHT_RPL_CRCS = 0x80000003,
    SHT_RPL_FILEINFO = 0x80000004,
};

// RPX Section flags
enum RPX_SectionFlags : uint32_t {
    SHF_WRITE = 0x1,
    SHF_ALLOC = 0x2,
    SHF_EXECINSTR = 0x4,
    SHF_RPL_ZLIB = 0x08000000,
};

// Section information
struct Section {
    uint32_t base{};
    uint32_t size{};
    uint32_t flags{};
    uint8_t* data{};
    std::string name;
};

// Function information for analysis
struct FunctionInfo {
    uint32_t address;
    uint32_t size;
    std::string name;
    bool analyzed = false;
};

// Import/Export information
struct ImportInfo {
    std::string library;
    std::string name;
    uint32_t address;
};

struct ExportInfo {
    std::string name;
    uint32_t address;
    uint32_t size;
};

class RPXImage {
public:
    // Core data
    std::vector<uint8_t> data;
    uint32_t base = 0;
    uint32_t size = 0;
    uint32_t entry_point = 0;
    uint32_t text_base = 0;
    uint32_t text_size = 0;
    uint32_t data_base = 0;
    uint32_t data_size = 0;

    // Sections and symbols
    std::vector<Section> sections;
    std::unordered_set<Symbol> symbols;
    std::unordered_map<uint32_t, std::string> address_to_symbol;
    
    // Functions, imports, exports
    std::vector<FunctionInfo> functions;
    std::vector<ImportInfo> imports;
    std::vector<ExportInfo> exports;
    
    // String tables
    std::vector<std::string> string_table;
    std::vector<std::string> dynamic_string_table;

    // Parsed headers
    RPX_ElfHeader elf_header;
    std::vector<RPX_SectionHeader> section_headers;
    std::vector<RPX_ProgramHeader> program_headers;

    // Methods
    static RPXImage ParseImage(const void* data, size_t size);
    uint8_t* Find(uint32_t address) const;
    Section* Find(const std::string& name);
    const Section* Find(const std::string& name) const;
    
    bool LoadSymbols();
    bool LoadImports();
    bool LoadExports();
    bool LoadRelocations();
    
    void AnalyzeFunctions();
    void DecompressSection(Section& section, const uint8_t* compressed_data, uint32_t compressed_size);
    
private:
    bool ParseElfHeader(const uint8_t* data, size_t size);
    bool ParseSectionHeaders(const uint8_t* data, size_t size);
    bool ParseProgramHeaders(const uint8_t* data, size_t size);
    bool ParseSections(const uint8_t* data, size_t size);
    
    std::string GetSectionName(uint32_t name_offset) const;
    std::string GetStringFromTable(uint32_t offset, const std::vector<std::string>& table) const;
};

// Helper functions
bool IsValidRPX(const void* data, size_t size);
uint32_t GetRPXEntryPoint(const void* data, size_t size);
std::vector<uint8_t> DecompressZlib(const uint8_t* data, size_t compressed_size, size_t uncompressed_size);

// Memory layout constants for WiiU
constexpr uint32_t WIIU_MEM1_BASE = 0x00800000;
constexpr uint32_t WIIU_MEM1_SIZE = 0x01800000;
constexpr uint32_t WIIU_MEM2_BASE = 0x10000000;
constexpr uint32_t WIIU_MEM2_SIZE = 0x20000000;
constexpr uint32_t WIIU_CODE_BASE = 0x02000000;
constexpr uint32_t WIIU_CODE_SIZE = 0x0E000000;