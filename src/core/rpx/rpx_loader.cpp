#include "rpx_loader.hpp"
#include "../elf/elf_types.hpp"
#include <zlib.h>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace rebrewu::rpx {

RpxLoader::RpxLoader(diagnostics::DiagEngine& diag) noexcept
    : m_diag(diag), m_reader(diag) {}

std::optional<RpxModule> RpxLoader::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        m_diag.error("cannot open RPX: " + path.string());
        return {};
    }
    std::streamsize sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) {
        m_diag.error("failed to read RPX: " + path.string());
        return {};
    }
    return load(buf, path.stem().string());
}

std::optional<RpxModule> RpxLoader::load(std::span<const uint8_t> bytes,
                                          std::string module_name) {
    auto img = m_reader.parse(bytes);
    if (!img) return {};

    RpxModule out;
    out.name = std::move(module_name);
    out.entry_point = img->ehdr.entry();

    if (!decode_file_info(*img, out)) return {};
    if (!decode_sections(*img, bytes, out)) return {};
    if (!decode_symbols(*img, out)) return {};
    if (!decode_exports(*img, out)) return {};
    if (!decode_imports(*img, out)) return {};
    if (!decode_relocs(*img, out))  return {};

    return out;
}

bool RpxLoader::decode_file_info(const elf::ElfImage& img, RpxModule& out) {
    const auto* sec = img.first_section_of_type(elf::SHT_RPL_FILEINFO);
    if (!sec || sec->data.size() < sizeof(elf::RplFileInfoRaw)) {
        m_diag.error("missing or truncated SHT_RPL_FILEINFO section");
        return false;
    }
    elf::RplFileInfoRaw raw;
    std::memcpy(&raw, sec->data.data(), sizeof(raw));
    if (!raw.is_valid_magic()) {
        m_diag.error("invalid RplFileInfo magic");
        return false;
    }
    auto& fi = out.file_info;
    fi.text_size          = raw.get_text_size();
    fi.data_size          = raw.get_data_size();
    fi.sda_base           = raw.get_sda_base();
    fi.sda2_base          = raw.get_sda2_base();
    fi.stack_size         = raw.get_stack_size();
    fi.flags              = raw.get_flags();
    fi.heap_size          = raw.get_heap_size();
    fi.min_version        = raw.get_min_version();
    fi.cafe_sdk_version   = raw.get_cafe_sdk_version();
    fi.cafe_sdk_revision  = raw.get_cafe_sdk_revision();
    fi.tls_module_index   = raw.get_tls_module_index();
    fi.tls_align_shift    = raw.get_tls_align_shift();
    return true;
}

bool RpxLoader::decode_sections(const elf::ElfImage& img,
                                 std::span<const uint8_t> raw,
                                 RpxModule& out) {
    for (uint32_t i = 0; i < img.sections.size(); ++i) {
        const auto& s = img.sections[i];
        if (!(s.header.flags() & elf::SHF_ALLOC)) continue;

        RpxSection sec;
        sec.index   = i;
        sec.name    = s.name;
        sec.address = s.header.addr();
        sec.size    = s.header.size();
        sec.flags   = s.header.flags();
        sec.type    = s.header.type();
        sec.alignment = s.header.addralign();

        if (s.header.is_rpl_zlib_compressed()) {
            sec.data = decompress_section(s);
            if (sec.data.empty() && sec.size > 0) return false;
            // sh_size holds the compressed on-disk size; update to actual size.
            sec.size = static_cast<uint32_t>(sec.data.size());
        } else {
            sec.data = s.data;
        }
        out.sections.push_back(std::move(sec));
    }

    // The .shstrtab section may itself be zlib-compressed (SHF_RPL_ZLIB).
    // ElfReader resolved names before decompression, so names may be garbled.
    // Re-resolve using the decompressed string table now.
    uint16_t shstrndx = img.ehdr.shstrndx();
    if (shstrndx < img.sections.size()) {
        const auto& raw_strtab_sec = img.sections[shstrndx];
        std::vector<uint8_t> strtab;
        if (raw_strtab_sec.header.is_rpl_zlib_compressed())
            strtab = decompress_section(raw_strtab_sec);
        else
            strtab = raw_strtab_sec.data;

        if (!strtab.empty()) {
            // Re-resolve names for all output sections
            for (auto& sec : out.sections) {
                uint32_t name_off = img.sections[sec.index].header.name();
                if (name_off < strtab.size()) {
                    const char* p = reinterpret_cast<const char*>(strtab.data() + name_off);
                    // find the null terminator safely
                    size_t max_len = strtab.size() - name_off;
                    sec.name = std::string(p, strnlen(p, max_len));
                }
            }
        }
    }

    return true;
}

bool RpxLoader::decode_symbols(const elf::ElfImage& img, RpxModule& out) {
    const auto* symtab = img.section_by_name(".symtab");
    if (!symtab) return true; // no symbol table is valid

    const auto* strtab = img.section_by_name(".strtab");
    const char* strings = strtab ? reinterpret_cast<const char*>(strtab->data.data()) : nullptr;
    size_t str_size = strtab ? strtab->data.size() : 0;

    const size_t sym_count = symtab->data.size() / sizeof(elf::Elf32_Sym);
    out.symbols.reserve(sym_count);

    for (size_t i = 0; i < sym_count; ++i) {
        elf::Elf32_Sym raw_sym;
        std::memcpy(&raw_sym, symtab->data.data() + i * sizeof(elf::Elf32_Sym),
                    sizeof(raw_sym));

        RpxSymbol sym;
        sym.address       = raw_sym.value();
        sym.size          = raw_sym.size();
        sym.binding       = raw_sym.bind();
        sym.type          = raw_sym.type();
        sym.section_index = raw_sym.shndx();

        uint32_t name_off = raw_sym.name();
        if (strings && name_off < str_size)
            sym.name = strings + name_off;

        out.symbols.push_back(std::move(sym));
    }
    return true;
}

bool RpxLoader::decode_exports(const elf::ElfImage& img, RpxModule& out) {
    for (const auto& sec : img.sections) {
        if (sec.header.type() != elf::SHT_RPL_EXPORTS) continue;
        // Export table layout: uint32_t count, uint32_t signature,
        // then count * { uint32_t tls_offset, uint32_t addr, uint32_t name_off }
        if (sec.data.size() < 8) continue;
        uint32_t count = elf::from_be(*reinterpret_cast<const uint32_t*>(sec.data.data()));
        const uint8_t* p = sec.data.data() + 8;
        for (uint32_t i = 0; i < count && (p + 12) <= sec.data.data() + sec.data.size(); ++i, p += 12) {
            RpxExport exp;
            exp.tls_offset = elf::from_be(*reinterpret_cast<const uint32_t*>(p));
            exp.address    = elf::from_be(*reinterpret_cast<const uint32_t*>(p + 4));
            uint32_t name_off = elf::from_be(*reinterpret_cast<const uint32_t*>(p + 8));
            if (name_off < sec.data.size())
                exp.name = reinterpret_cast<const char*>(sec.data.data() + name_off);
            exp.is_data = (exp.tls_offset & 1) != 0;
            exp.tls_offset &= ~1u;
            out.exports.push_back(std::move(exp));
        }
    }
    return true;
}

bool RpxLoader::decode_imports(const elf::ElfImage& img, RpxModule& out) {
    for (const auto& sec : img.sections) {
        if (sec.header.type() != elf::SHT_RPL_IMPORTS) continue;
        // Import section name is .fimport_<module>
        std::string mod_name;
        if (sec.name.size() > 9) // ".fimport_"
            mod_name = sec.name.substr(9);

        if (sec.data.size() < 8) continue;
        uint32_t count = elf::from_be(*reinterpret_cast<const uint32_t*>(sec.data.data()));
        const uint8_t* p = sec.data.data() + 8;
        for (uint32_t i = 0; i < count && (p + 12) <= sec.data.data() + sec.data.size(); ++i, p += 12) {
            RpxImport imp;
            imp.tls_offset   = elf::from_be(*reinterpret_cast<const uint32_t*>(p));
            imp.stub_address = elf::from_be(*reinterpret_cast<const uint32_t*>(p + 4));
            uint32_t name_off= elf::from_be(*reinterpret_cast<const uint32_t*>(p + 8));
            if (name_off < sec.data.size())
                imp.name = reinterpret_cast<const char*>(sec.data.data() + name_off);
            imp.from_module = mod_name;
            imp.is_data = (imp.tls_offset & 1) != 0;
            imp.tls_offset &= ~1u;
            out.imports.push_back(std::move(imp));
        }
    }
    return true;
}

bool RpxLoader::decode_relocs(const elf::ElfImage& img, RpxModule& out) {
    for (const auto& sec : img.sections) {
        if (sec.header.type() != elf::SHT_RELA) continue;
        const size_t entry_count = sec.data.size() / sizeof(elf::Elf32_Rela);
        for (size_t i = 0; i < entry_count; ++i) {
            elf::Elf32_Rela raw;
            std::memcpy(&raw, sec.data.data() + i * sizeof(raw), sizeof(raw));
            RpxReloc r;
            r.offset    = raw.offset();
            r.sym_index = raw.sym_index();
            r.type      = raw.reloc_type();
            r.addend    = raw.addend();
            if (r.sym_index < out.symbols.size())
                r.sym_name = out.symbols[r.sym_index].name;
            out.relocations.push_back(std::move(r));
        }
    }
    return true;
}

std::vector<uint8_t> RpxLoader::decompress_section(const elf::RawSection& sec) {
    if (sec.data.size() < 4) return {};
    uint32_t uncompressed_size = elf::from_be(
        *reinterpret_cast<const uint32_t*>(sec.data.data()));

    std::vector<uint8_t> out(uncompressed_size);
    uLongf dest_len = uncompressed_size;
    int ret = uncompress(out.data(), &dest_len,
                         sec.data.data() + 4,
                         static_cast<uLong>(sec.data.size() - 4));
    if (ret != Z_OK) {
        m_diag.error("zlib decompress failed for section '" + sec.name + "'");
        return {};
    }
    out.resize(dest_len);
    return out;
}

}