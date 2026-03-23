#include <catch2/catch_test_macros.hpp>
#include "core/elf/elf_types.hpp"
#include "core/elf/elf_reader.hpp"
#include "core/rpx/rpx_types.hpp"
#include "diagnostics/diagnostics.hpp"

using namespace rebrewu;

// ============================================================================
// Helpers: construct a minimal valid RPX ELF32 image in memory
// ============================================================================

static std::vector<uint8_t> make_minimal_elf() {
    std::vector<uint8_t> buf(sizeof(elf::Elf32_Ehdr), 0);

    auto* ehdr = reinterpret_cast<elf::Elf32_Ehdr*>(buf.data());
    ehdr->e_ident[elf::EI_MAG0]  = elf::ELFMAG0;
    ehdr->e_ident[elf::EI_MAG1]  = elf::ELFMAG1;
    ehdr->e_ident[elf::EI_MAG2]  = elf::ELFMAG2;
    ehdr->e_ident[elf::EI_MAG3]  = elf::ELFMAG3;
    ehdr->e_ident[elf::EI_CLASS] = elf::ELFCLASS32;
    ehdr->e_ident[elf::EI_DATA]  = elf::ELFDATA2MSB;
    ehdr->e_ident[elf::EI_VERSION] = elf::EV_CURRENT;

    // Store big-endian fields
    auto be16 = [](uint16_t v) -> uint16_t {
        return static_cast<uint16_t>((v >> 8) | (v << 8));
    };
    auto be32 = [](uint32_t v) -> uint32_t {
        return ((v & 0xFF) << 24) | (((v >> 8) & 0xFF) << 16) |
               (((v >> 16) & 0xFF) << 8) | (v >> 24);
    };

    ehdr->e_type      = be16(elf::ET_EXEC);
    ehdr->e_machine   = be16(elf::EM_PPC);
    ehdr->e_version   = be32(1);
    ehdr->e_ehsize    = be16(sizeof(elf::Elf32_Ehdr));
    ehdr->e_phentsize = be16(sizeof(elf::Elf32_Phdr));
    ehdr->e_shentsize = be16(sizeof(elf::Elf32_Shdr));

    return buf;
}

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("ELF magic constants are correct", "[elf_types]") {
    REQUIRE(elf::ELFMAG0 == 0x7F);
    REQUIRE(elf::ELFMAG1 == 'E');
    REQUIRE(elf::ELFMAG2 == 'L');
    REQUIRE(elf::ELFMAG3 == 'F');
}

TEST_CASE("ELF struct sizes match ABI", "[elf_types]") {
    REQUIRE(sizeof(elf::Elf32_Ehdr) == 52);
    REQUIRE(sizeof(elf::Elf32_Shdr) == 40);
    REQUIRE(sizeof(elf::Elf32_Phdr) == 32);
    REQUIRE(sizeof(elf::Elf32_Sym)  == 16);
    REQUIRE(sizeof(elf::Elf32_Rel)  ==  8);
    REQUIRE(sizeof(elf::Elf32_Rela) == 12);
    REQUIRE(sizeof(elf::RplFileInfoRaw) == 0x60);
}

TEST_CASE("from_be performs correct byte swap on all sizes", "[elf_types]") {
    REQUIRE(elf::from_be(uint16_t{0x0102u}) == 0x0201u);
    REQUIRE(elf::from_be(uint32_t{0x01020304u}) == 0x04030201u);
    REQUIRE(elf::from_be(int32_t{0x01020304}) == static_cast<int32_t>(0x04030201u));
}

TEST_CASE("RPL section type constants are distinct", "[elf_types]") {
    REQUIRE(elf::SHT_RPL_EXPORTS  != elf::SHT_RPL_IMPORTS);
    REQUIRE(elf::SHT_RPL_CRCS     != elf::SHT_RPL_FILEINFO);
    REQUIRE(elf::SHT_RPL_EXPORTS  >= elf::SHT_LOUSER);
}

TEST_CASE("SHF_RPL_ZLIB flag does not overlap standard SHF flags", "[elf_types]") {
    constexpr uint32_t std_flags = elf::SHF_WRITE | elf::SHF_ALLOC | elf::SHF_EXECINSTR;
    REQUIRE((elf::SHF_RPL_ZLIB & std_flags) == 0);
}

TEST_CASE("cafe_version encoding round-trips correctly", "[elf_types]") {
    uint32_t v = elf::cafe_version(5, 5, 2);
    REQUIRE(elf::cafe_version_major(v) == 5);
    REQUIRE(elf::cafe_version_minor(v) == 5);
    REQUIRE(elf::cafe_version_patch(v) == 2);
}

TEST_CASE("ElfReader rejects empty buffer", "[elf_reader]") {
    diagnostics::DiagEngine diag;
    elf::ElfReader reader(diag);
    auto result = reader.parse({});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(diag.has_errors());
}

TEST_CASE("ElfReader rejects non-ELF magic", "[elf_reader]") {
    diagnostics::DiagEngine diag;
    elf::ElfReader reader(diag);
    std::vector<uint8_t> bad(64, 0xAA);
    auto result = reader.parse(bad);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(diag.has_errors());
}

TEST_CASE("ElfReader rejects little-endian ELF", "[elf_reader]") {
    diagnostics::DiagEngine diag;
    elf::ElfReader reader(diag);
    auto buf = make_minimal_elf();
    // Change to little-endian
    buf[elf::EI_DATA] = elf::ELFDATA2LSB;
    auto result = reader.parse(buf);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(diag.has_errors());
}

TEST_CASE("ElfReader accepts minimal valid big-endian ELF32", "[elf_reader]") {
    diagnostics::DiagEngine diag;
    elf::ElfReader reader(diag);
    auto buf = make_minimal_elf();
    auto result = reader.parse(buf);
    REQUIRE(result.has_value());
    REQUIRE_FALSE(diag.has_errors());
    REQUIRE(result->ehdr.machine() == elf::EM_PPC);
}

TEST_CASE("RpxSection::contains works correctly", "[rpx_types]") {
    rpx::RpxSection sec;
    sec.address = 0x0200'0000;
    sec.size    = 0x1000;
    REQUIRE(sec.contains(0x0200'0000));
    REQUIRE(sec.contains(0x0200'0FFF));
    REQUIRE_FALSE(sec.contains(0x0200'1000));
    REQUIRE_FALSE(sec.contains(0x01FF'FFFF));
}

TEST_CASE("RpxModule::read_word returns nullopt for unmapped address", "[rpx_types]") {
    rpx::RpxModule mod;
    REQUIRE_FALSE(mod.read_word(0xDEAD'BEEF).has_value());
}
