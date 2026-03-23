#include "elf_reader.hpp"
#include <fstream>
#include <cstring>

namespace rebrewu::elf {

ElfReader::ElfReader(diagnostics::DiagEngine& diag) noexcept
    : m_diag(diag) {}

std::optional<ElfImage> ElfReader::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        m_diag.error("cannot open file: " + path.string());
        return {};
    }
    std::streamsize sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (!f.read(reinterpret_cast<char*>(buf.data()), sz)) {
        m_diag.error("failed to read file: " + path.string());
        return {};
    }
    return parse(buf);
}

std::optional<ElfImage> ElfReader::parse(std::span<const uint8_t> bytes) {
    if (!validate_ident(bytes))
        return {};

    ElfImage img;
    // Copy the ELF header (it is at offset 0 in a well-formed ELF file)
    std::memcpy(&img.ehdr, bytes.data(), sizeof(Elf32_Ehdr));

    if (!parse_phdrs(bytes, img.ehdr, img))
        return {};
    if (!parse_shdrs(bytes, img.ehdr, img))
        return {};
    if (!resolve_section_names(bytes, img))
        return {};

    return img;
}

bool ElfReader::validate_ident(std::span<const uint8_t> bytes) {
    if (bytes.size() < sizeof(Elf32_Ehdr)) {
        m_diag.error("file too small to be an ELF");
        return false;
    }
    if (bytes[EI_MAG0] != ELFMAG0 || bytes[EI_MAG1] != ELFMAG1 ||
        bytes[EI_MAG2] != ELFMAG2 || bytes[EI_MAG3] != ELFMAG3) {
        m_diag.error("not an ELF file (bad magic)");
        return false;
    }
    if (bytes[EI_CLASS] != ELFCLASS32) {
        m_diag.error("only ELF32 is supported");
        return false;
    }
    if (bytes[EI_DATA] != ELFDATA2MSB) {
        m_diag.error("only big-endian ELF is supported (Wii U is BE)");
        return false;
    }
    return true;
}

bool ElfReader::parse_phdrs(std::span<const uint8_t> bytes,
                             const Elf32_Ehdr& ehdr,
                             ElfImage& out) {
    uint32_t phoff  = ehdr.phoff();
    uint16_t phnum  = ehdr.phnum();
    uint16_t phentsize = ehdr.phentsize();

    if (phnum == 0) return true;

    if (phoff + static_cast<uint64_t>(phnum) * phentsize > bytes.size()) {
        m_diag.error("program header table extends beyond end of file");
        return false;
    }

    out.phdrs.resize(phnum);
    for (uint16_t i = 0; i < phnum; ++i) {
        std::memcpy(&out.phdrs[i],
                    bytes.data() + phoff + i * phentsize,
                    sizeof(Elf32_Phdr));
    }
    return true;
}

bool ElfReader::parse_shdrs(std::span<const uint8_t> bytes,
                             const Elf32_Ehdr& ehdr,
                             ElfImage& out) {
    uint32_t shoff    = ehdr.shoff();
    uint16_t shnum    = ehdr.shnum();
    uint16_t shentsize= ehdr.shentsize();

    if (shnum == 0) return true;

    if (shoff + static_cast<uint64_t>(shnum) * shentsize > bytes.size()) {
        m_diag.error("section header table extends beyond end of file");
        return false;
    }

    out.sections.resize(shnum);
    for (uint16_t i = 0; i < shnum; ++i) {
        Elf32_Shdr& sh = out.sections[i].header;
        std::memcpy(&sh, bytes.data() + shoff + i * shentsize, sizeof(Elf32_Shdr));

        uint32_t off  = sh.offset();
        uint32_t size = sh.size();
        if (sh.type() != SHT_NOBITS && size > 0) {
            if (off + size > bytes.size()) {
                m_diag.error("section " + std::to_string(i) + " data extends beyond file");
                return false;
            }
            out.sections[i].data.assign(bytes.data() + off,
                                        bytes.data() + off + size);
        }
    }
    return true;
}

bool ElfReader::resolve_section_names(std::span<const uint8_t> /*bytes*/,
                                      ElfImage& out) {
    uint16_t shstrndx = out.ehdr.shstrndx();
    if (shstrndx == SHN_UNDEF || shstrndx >= out.sections.size())
        return true; // no string table

    const auto& strtab = out.sections[shstrndx].data;
    for (auto& sec : out.sections) {
        uint32_t name_off = sec.header.name();
        if (name_off < strtab.size()) {
            sec.name = reinterpret_cast<const char*>(strtab.data() + name_off);
        }
    }
    return true;
}

}