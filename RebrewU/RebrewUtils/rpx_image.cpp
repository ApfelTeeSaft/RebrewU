#include "rpx_image.h"
#include "file.h"
#include <algorithm>
#include <cstring>
#include <zlib.h>

RPXImage RPXImage::ParseImage(const void* data, size_t size) {
    RPXImage image;
    
    if (!ValidateRPXHeader(data, size)) {
        return image; // Return empty image on failure
    }
    
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    
    // Copy data
    image.data.assign(bytes, bytes + size);
    image.size = static_cast<uint32_t>(size);
    
    // Parse ELF header
    if (!image.ParseElfHeader(bytes, size)) {
        return image;
    }
    
    // Parse section headers
    if (!image.ParseSectionHeaders(bytes, size)) {
        return image;
    }
    
    // Parse program headers
    if (!image.ParseProgramHeaders(bytes, size)) {
        return image;
    }
    
    // Parse sections
    if (!image.ParseSections(bytes, size)) {
        return image;
    }
    
    // Load symbols, imports, exports
    image.LoadSymbols();
    image.LoadImports();
    image.LoadExports();
    image.LoadRelocations();
    
    // Analyze functions
    image.AnalyzeFunctions();
    
    return image;
}

bool RPXImage::ParseElfHeader(const uint8_t* data, size_t size) {
    if (size < sizeof(RPX_ElfHeader)) {
        return false;
    }
    
    std::memcpy(&elf_header, data, sizeof(RPX_ElfHeader));
    
    // Extract key information
    entry_point = elf_header.e_entry;
    
    return true;
}

bool RPXImage::ParseSectionHeaders(const uint8_t* data, size_t size) {
    if (elf_header.e_shoff == 0 || elf_header.e_shnum == 0) {
        return false;
    }
    
    if (elf_header.e_shoff + (elf_header.e_shnum * sizeof(RPX_SectionHeader)) > size) {
        return false;
    }
    
    section_headers.resize(elf_header.e_shnum);
    const uint8_t* section_data = data + elf_header.e_shoff;
    
    for (uint16_t i = 0; i < elf_header.e_shnum; i++) {
        std::memcpy(&section_headers[i], section_data + (i * sizeof(RPX_SectionHeader)), 
                   sizeof(RPX_SectionHeader));
    }
    
    return true;
}

bool RPXImage::ParseProgramHeaders(const uint8_t* data, size_t size) {
    if (elf_header.e_phoff == 0 || elf_header.e_phnum == 0) {
        return true; // Not an error if no program headers
    }
    
    if (elf_header.e_phoff + (elf_header.e_phnum * sizeof(RPX_ProgramHeader)) > size) {
        return false;
    }
    
    program_headers.resize(elf_header.e_phnum);
    const uint8_t* program_data = data + elf_header.e_phoff;
    
    for (uint16_t i = 0; i < elf_header.e_phnum; i++) {
        std::memcpy(&program_headers[i], program_data + (i * sizeof(RPX_ProgramHeader)), 
                   sizeof(RPX_ProgramHeader));
    }
    
    return true;
}

bool RPXImage::ParseSections(const uint8_t* data, size_t size) {
    if (section_headers.empty()) {
        return false;
    }
    
    // First, load the string table for section names
    if (elf_header.e_shstrndx < section_headers.size()) {
        const auto& strtab_header = section_headers[elf_header.e_shstrndx];
        if (strtab_header.sh_offset < size && strtab_header.sh_size > 0) {
            const char* string_data = reinterpret_cast<const char*>(data + strtab_header.sh_offset);
            string_table.clear();
            
            // Parse null-terminated strings
            for (uint32_t i = 0; i < strtab_header.sh_size; ) {
                std::string str(string_data + i);
                string_table.push_back(str);
                i += str.length() + 1;
            }
        }
    }
    
    sections.clear();
    sections.reserve(section_headers.size());
    
    for (size_t i = 0; i < section_headers.size(); i++) {
        const auto& header = section_headers[i];
        
        Section section;
        section.base = header.sh_addr;
        section.size = header.sh_size;
        section.flags = header.sh_flags;
        section.name = GetSectionName(header.sh_name);
        
        // Allocate and copy section data
        if (header.sh_type != SHT_NOBITS && header.sh_size > 0 && header.sh_offset < size) {
            if (header.sh_flags & SHF_RPL_ZLIB) {
                // Decompress zlib-compressed section
                section.data = new uint8_t[header.sh_size];
                DecompressSection(section, data + header.sh_offset, 
                                std::min(static_cast<uint32_t>(size - header.sh_offset), header.sh_size));
            } else {
                // Copy uncompressed section
                section.data = new uint8_t[header.sh_size];
                std::memcpy(section.data, data + header.sh_offset, 
                           std::min(static_cast<size_t>(header.sh_size), size - header.sh_offset));
            }
        } else {
            section.data = nullptr;
        }
        
        // Set section type flags for easier identification
        if (header.sh_flags & SHF_EXECINSTR) {
            section.flags |= SectionFlags_Code;
        }
        if (header.sh_flags & SHF_WRITE) {
            section.flags |= SectionFlags_Data;
        }
        if (header.sh_type == SHT_NOBITS) {
            section.flags |= SectionFlags_BSS;
        }
        
        // Track important sections
        if (section.name == ".text" && (section.flags & SectionFlags_Code)) {
            text_base = section.base;
            text_size = section.size;
        }
        if (section.name == ".data" && (section.flags & SectionFlags_Data)) {
            data_base = section.base;
            data_size = section.size;
        }
        
        sections.push_back(std::move(section));
    }
    
    // Calculate base address from the lowest section address
    if (!sections.empty()) {
        base = UINT32_MAX;
        for (const auto& section : sections) {
            if (section.base > 0 && section.base < base) {
                base = section.base;
            }
        }
        if (base == UINT32_MAX) {
            base = 0;
        }
    }
    
    return true;
}

std::string RPXImage::GetSectionName(uint32_t name_offset) const {
    if (name_offset < string_table.size()) {
        return string_table[name_offset];
    }
    return "";
}

uint8_t* RPXImage::Find(uint32_t address) const {
    for (const auto& section : sections) {
        if (address >= section.base && address < section.base + section.size) {
            if (section.data) {
                return section.data + (address - section.base);
            }
        }
    }
    return nullptr;
}

Section* RPXImage::Find(const std::string& name) {
    for (auto& section : sections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

const Section* RPXImage::Find(const std::string& name) const {
    for (const auto& section : sections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

bool RPXImage::LoadSymbols() {
    // Find symbol table section
    const Section* symtab = Find(".symtab");
    const Section* strtab = Find(".strtab");
    
    if (!symtab || !strtab || !symtab->data || !strtab->data) {
        return false;
    }
    
    size_t symbol_count = symtab->size / sizeof(RPX_SymbolEntry);
    const RPX_SymbolEntry* symbol_entries = reinterpret_cast<const RPX_SymbolEntry*>(symtab->data);
    
    symbols.clear();
    address_to_symbol.clear();
    
    for (size_t i = 0; i < symbol_count; i++) {
        const auto& entry = symbol_entries[i];
        
        if (entry.st_name >= strtab->size) {
            continue;
        }
        
        std::string name(reinterpret_cast<const char*>(strtab->data + entry.st_name));
        if (name.empty()) {
            continue;
        }
        
        SymbolType type = Symbol_Function;
        uint8_t symbol_type = entry.st_info & 0xF;
        if (symbol_type == 1) { // STT_OBJECT
            type = Symbol_Data;
        } else if (symbol_type == 2) { // STT_FUNC
            type = Symbol_Function;
        }
        
        Symbol symbol(name, entry.st_value, entry.st_size, type);
        symbols.insert(symbol);
        address_to_symbol[entry.st_value] = name;
    }
    
    return true;
}

bool RPXImage::LoadImports() {
    // Look for import sections
    const Section* imports = Find(".rpl_imports");
    if (!imports || !imports->data) {
        return false;
    }
    
    // RPX import parsing is more complex in a real implementation
    // TODO: ADD
    imports.clear();
    
    return true;
}

bool RPXImage::LoadExports() {
    // Look for export sections  
    const Section* exports_section = Find(".rpl_exports");
    if (!exports_section || !exports_section->data) {
        return false;
    }
    
    // RPX export parsing is more complex in a real implementation
    // TODO: ADD
    exports.clear();
    
    return true;
}

bool RPXImage::LoadRelocations() {
    // Process relocation sections
    for (const auto& section : sections) {
        if (section.name.find(".rela") == 0 || section.name.find(".rel") == 0) {
            // Process relocations - simplified 
            // TODO: Make complex
        }
    }
    
    return true;
}

void RPXImage::AnalyzeFunctions() {
    functions.clear();
    
    // Add functions from symbol table
    for (const auto& symbol : symbols) {
        if (symbol.type == Symbol_Function && symbol.size > 0) {
            FunctionInfo func;
            func.address = symbol.address;
            func.size = symbol.size;
            func.name = symbol.name;
            func.analyzed = true;
            functions.push_back(func);
        }
    }
    
    // Sort functions by address
    std::sort(functions.begin(), functions.end(), 
              [](const FunctionInfo& a, const FunctionInfo& b) {
                  return a.address < b.address;
              });
}

void RPXImage::DecompressSection(Section& section, const uint8_t* compressed_data, uint32_t compressed_size) {
    if (!section.data || section.size == 0) {
        return;
    }
    
    // Use zlib to decompress
    uLongf dest_len = section.size;
    int result = uncompress(section.data, &dest_len, compressed_data, compressed_size);
    
    if (result != Z_OK) {
        // Decompression failed, zero out the section
        std::memset(section.data, 0, section.size);
    }
}

// Helper functions
bool IsValidRPX(const void* data, size_t size) {
    return ValidateRPXHeader(data, size);
}

uint32_t GetRPXEntryPoint(const void* data, size_t size) {
    if (!IsValidRPX(data, size) || size < sizeof(RPX_ElfHeader)) {
        return 0;
    }
    
    const RPX_ElfHeader* header = static_cast<const RPX_ElfHeader*>(data);
    return header->e_entry;
}

std::vector<uint8_t> DecompressZlib(const uint8_t* data, size_t compressed_size, size_t uncompressed_size) {
    std::vector<uint8_t> result(uncompressed_size);
    
    uLongf dest_len = uncompressed_size;
    int ret = uncompress(result.data(), &dest_len, data, compressed_size);
    
    if (ret != Z_OK) {
        result.clear();
    }
    
    return result;
}