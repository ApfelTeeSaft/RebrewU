#pragma once

#include <cstdint>
#include <bit>
#include <type_traits>
#include <array>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// elf_types.hpp — ELF32 type definitions, constants, and byte-swap helpers
//
// Wii U executables (RPX) and libraries (RPL) are big-endian ELF32 binaries
// targeting PowerPC (EM_PPC = 20).  Sections may be zlib-compressed and carry
// Wii U-specific section types and flags documented below.
// ============================================================================

namespace rebrewu::elf {

// ============================================================================
// Fundamental ELF scalar typedefs (ELF32 naming convention)
// ============================================================================

using Elf32_Addr  = uint32_t;
using Elf32_Off   = uint32_t;
using Elf32_Half  = uint16_t;
using Elf32_Word  = uint32_t;
using Elf32_Sword = int32_t;
using Elf32_Byte  = uint8_t;

// ============================================================================
// ELF identification (e_ident indices)
// ============================================================================

inline constexpr std::size_t EI_NIDENT    = 16;
inline constexpr std::size_t EI_MAG0      = 0;
inline constexpr std::size_t EI_MAG1      = 1;
inline constexpr std::size_t EI_MAG2      = 2;
inline constexpr std::size_t EI_MAG3      = 3;
inline constexpr std::size_t EI_CLASS     = 4;
inline constexpr std::size_t EI_DATA      = 5;
inline constexpr std::size_t EI_VERSION   = 6;
inline constexpr std::size_t EI_OSABI     = 7;
inline constexpr std::size_t EI_ABIVERSION= 8;
inline constexpr std::size_t EI_PAD       = 9;

// Magic bytes
inline constexpr uint8_t ELFMAG0 = 0x7F;
inline constexpr uint8_t ELFMAG1 = 'E';
inline constexpr uint8_t ELFMAG2 = 'L';
inline constexpr uint8_t ELFMAG3 = 'F';

// EI_CLASS values
inline constexpr uint8_t ELFCLASSNONE = 0;
inline constexpr uint8_t ELFCLASS32   = 1;
inline constexpr uint8_t ELFCLASS64   = 2;

// EI_DATA values
inline constexpr uint8_t ELFDATANONE = 0;
inline constexpr uint8_t ELFDATA2LSB = 1; // Little-endian
inline constexpr uint8_t ELFDATA2MSB = 2; // Big-endian (Wii U)

// EI_VERSION / e_version
inline constexpr uint8_t EV_NONE    = 0;
inline constexpr uint8_t EV_CURRENT = 1;

// EI_OSABI
inline constexpr uint8_t ELFOSABI_NONE = 0;
inline constexpr uint8_t ELFOSABI_CAFE = 0xCA; // Wii U / Cafe OS (unofficial)

// ============================================================================
// Object file types (e_type)
// ============================================================================

inline constexpr Elf32_Half ET_NONE   = 0;      // No file type
inline constexpr Elf32_Half ET_REL    = 1;      // Relocatable
inline constexpr Elf32_Half ET_EXEC   = 2;      // Executable
inline constexpr Elf32_Half ET_DYN    = 3;      // Shared object
inline constexpr Elf32_Half ET_CORE   = 4;      // Core dump
inline constexpr Elf32_Half ET_LOPROC = 0xFF00; // Processor-specific range start
inline constexpr Elf32_Half ET_HIPROC = 0xFFFF; // Processor-specific range end

// ============================================================================
// Machine architecture (e_machine)
// ============================================================================

inline constexpr Elf32_Half EM_NONE = 0;
inline constexpr Elf32_Half EM_PPC  = 20; // PowerPC — used by Wii U

// ============================================================================
// Special section indices
// ============================================================================

inline constexpr Elf32_Half SHN_UNDEF     = 0;
inline constexpr Elf32_Half SHN_LORESERVE = 0xFF00;
inline constexpr Elf32_Half SHN_LOPROC    = 0xFF00;
inline constexpr Elf32_Half SHN_HIPROC    = 0xFF1F;
inline constexpr Elf32_Half SHN_ABS       = 0xFFF1;
inline constexpr Elf32_Half SHN_COMMON    = 0xFFF2;
inline constexpr Elf32_Half SHN_XINDEX    = 0xFFFF;
inline constexpr Elf32_Half SHN_HIRESERVE = 0xFFFF;

// ============================================================================
// Section types (sh_type)
// ============================================================================

inline constexpr Elf32_Word SHT_NULL          = 0;
inline constexpr Elf32_Word SHT_PROGBITS      = 1;
inline constexpr Elf32_Word SHT_SYMTAB        = 2;
inline constexpr Elf32_Word SHT_STRTAB        = 3;
inline constexpr Elf32_Word SHT_RELA          = 4;
inline constexpr Elf32_Word SHT_HASH          = 5;
inline constexpr Elf32_Word SHT_DYNAMIC       = 6;
inline constexpr Elf32_Word SHT_NOTE          = 7;
inline constexpr Elf32_Word SHT_NOBITS        = 8;
inline constexpr Elf32_Word SHT_REL           = 9;
inline constexpr Elf32_Word SHT_SHLIB         = 10;
inline constexpr Elf32_Word SHT_DYNSYM        = 11;
inline constexpr Elf32_Word SHT_INIT_ARRAY    = 14;
inline constexpr Elf32_Word SHT_FINI_ARRAY    = 15;
inline constexpr Elf32_Word SHT_PREINIT_ARRAY = 16;
inline constexpr Elf32_Word SHT_GROUP         = 17;
inline constexpr Elf32_Word SHT_SYMTAB_SHNDX  = 18;
inline constexpr Elf32_Word SHT_LOPROC        = 0x70000000;
inline constexpr Elf32_Word SHT_HIPROC        = 0x7FFFFFFF;
inline constexpr Elf32_Word SHT_LOUSER        = 0x80000000;
inline constexpr Elf32_Word SHT_HIUSER        = 0xFFFFFFFF;

// Wii U RPX/RPL-specific section types
inline constexpr Elf32_Word SHT_RPL_EXPORTS  = 0x80000001; // .fexport section
inline constexpr Elf32_Word SHT_RPL_IMPORTS  = 0x80000002; // .fimport_<modname> section
inline constexpr Elf32_Word SHT_RPL_CRCS     = 0x80000003; // Section CRC table
inline constexpr Elf32_Word SHT_RPL_FILEINFO = 0x80000004; // RplFileInfo binary blob

// ============================================================================
// Section flags (sh_flags)
// ============================================================================

inline constexpr Elf32_Word SHF_WRITE            = 0x00000001; // Writable
inline constexpr Elf32_Word SHF_ALLOC            = 0x00000002; // Occupies memory during execution
inline constexpr Elf32_Word SHF_EXECINSTR        = 0x00000004; // Executable
inline constexpr Elf32_Word SHF_MERGE            = 0x00000010; // Might be merged
inline constexpr Elf32_Word SHF_STRINGS          = 0x00000020; // Contains nul-terminated strings
inline constexpr Elf32_Word SHF_INFO_LINK        = 0x00000040; // sh_info holds section index
inline constexpr Elf32_Word SHF_LINK_ORDER       = 0x00000080; // Ordering requirement
inline constexpr Elf32_Word SHF_OS_NONCONFORMING = 0x00000100; // Non-standard OS-specific handling
inline constexpr Elf32_Word SHF_GROUP            = 0x00000200; // Member of section group
inline constexpr Elf32_Word SHF_TLS              = 0x00000400; // Thread-local storage
inline constexpr Elf32_Word SHF_MASKOS           = 0x0FF00000; // OS-specific mask
inline constexpr Elf32_Word SHF_MASKPROC         = 0xF0000000; // Processor-specific mask

// Wii U RPX/RPL-specific section flag
// When set, the section data is zlib-compressed; the first 4 bytes of the
// raw section data give the uncompressed size as a big-endian uint32_t.
inline constexpr Elf32_Word SHF_RPL_ZLIB = 0x08000000;

// ============================================================================
// Segment / program header types (p_type)
// ============================================================================

inline constexpr Elf32_Word PT_NULL    = 0;
inline constexpr Elf32_Word PT_LOAD    = 1;
inline constexpr Elf32_Word PT_DYNAMIC = 2;
inline constexpr Elf32_Word PT_INTERP  = 3;
inline constexpr Elf32_Word PT_NOTE    = 4;
inline constexpr Elf32_Word PT_SHLIB   = 5;
inline constexpr Elf32_Word PT_PHDR    = 6;
inline constexpr Elf32_Word PT_TLS     = 7;
inline constexpr Elf32_Word PT_LOPROC  = 0x70000000;
inline constexpr Elf32_Word PT_HIPROC  = 0x7FFFFFFF;

// ============================================================================
// Segment flags (p_flags)
// ============================================================================

inline constexpr Elf32_Word PF_X        = 0x1; // Execute
inline constexpr Elf32_Word PF_W        = 0x2; // Write
inline constexpr Elf32_Word PF_R        = 0x4; // Read
inline constexpr Elf32_Word PF_MASKOS   = 0x0FF00000;
inline constexpr Elf32_Word PF_MASKPROC = 0xF0000000;

// ============================================================================
// Symbol binding (ELF32_ST_BIND)
// ============================================================================

inline constexpr uint8_t STB_LOCAL  = 0;
inline constexpr uint8_t STB_GLOBAL = 1;
inline constexpr uint8_t STB_WEAK   = 2;
inline constexpr uint8_t STB_LOPROC = 13;
inline constexpr uint8_t STB_HIPROC = 15;

// ============================================================================
// Symbol type (ELF32_ST_TYPE)
// ============================================================================

inline constexpr uint8_t STT_NOTYPE  = 0;
inline constexpr uint8_t STT_OBJECT  = 1;
inline constexpr uint8_t STT_FUNC    = 2;
inline constexpr uint8_t STT_SECTION = 3;
inline constexpr uint8_t STT_FILE    = 4;
inline constexpr uint8_t STT_COMMON  = 5;
inline constexpr uint8_t STT_TLS     = 6;
inline constexpr uint8_t STT_LOPROC  = 13;
inline constexpr uint8_t STT_HIPROC  = 15;

// ============================================================================
// Symbol visibility (ELF32_ST_VISIBILITY)
// ============================================================================

inline constexpr uint8_t STV_DEFAULT   = 0;
inline constexpr uint8_t STV_INTERNAL  = 1;
inline constexpr uint8_t STV_HIDDEN    = 2;
inline constexpr uint8_t STV_PROTECTED = 3;

// ELF32_ST_BIND / ELF32_ST_TYPE / ELF32_ST_VISIBILITY helper macros
// expressed as constexpr functions to avoid macro pollution.
[[nodiscard]] constexpr uint8_t elf32_st_bind(uint8_t info) noexcept {
    return static_cast<uint8_t>(info >> 4);
}
[[nodiscard]] constexpr uint8_t elf32_st_type(uint8_t info) noexcept {
    return static_cast<uint8_t>(info & 0x0F);
}
[[nodiscard]] constexpr uint8_t elf32_st_info(uint8_t bind, uint8_t type) noexcept {
    return static_cast<uint8_t>((bind << 4) | (type & 0x0F));
}
[[nodiscard]] constexpr uint8_t elf32_st_visibility(uint8_t other) noexcept {
    return static_cast<uint8_t>(other & 0x3);
}

// ============================================================================
// Relocation helpers
// ============================================================================

[[nodiscard]] constexpr Elf32_Word elf32_r_sym(Elf32_Word info) noexcept {
    return info >> 8;
}
[[nodiscard]] constexpr Elf32_Word elf32_r_type(Elf32_Word info) noexcept {
    return info & 0xFF;
}
[[nodiscard]] constexpr Elf32_Word elf32_r_info(Elf32_Word sym, Elf32_Word type) noexcept {
    return (sym << 8) | (type & 0xFF);
}

// ============================================================================
// PowerPC relocation types (R_PPC_*)
// Values are the 8-bit type field encoded in r_info.
// ============================================================================

inline constexpr uint32_t R_PPC_NONE            = 0;
inline constexpr uint32_t R_PPC_ADDR32          = 1;   // S + A
inline constexpr uint32_t R_PPC_ADDR24          = 2;   // (S + A) >> 2
inline constexpr uint32_t R_PPC_ADDR16          = 3;   // S + A
inline constexpr uint32_t R_PPC_ADDR16_LO       = 4;   // #lo(S + A)
inline constexpr uint32_t R_PPC_ADDR16_HI       = 5;   // #hi(S + A)
inline constexpr uint32_t R_PPC_ADDR16_HA       = 6;   // #ha(S + A)
inline constexpr uint32_t R_PPC_ADDR14          = 7;   // (S + A) >> 2, 14-bit
inline constexpr uint32_t R_PPC_ADDR14_BRTAKEN  = 8;   // branch taken hint
inline constexpr uint32_t R_PPC_ADDR14_BRNTAKEN = 9;   // branch not-taken hint
inline constexpr uint32_t R_PPC_REL24           = 10;  // (S + A - P) >> 2
inline constexpr uint32_t R_PPC_REL14           = 11;  // (S + A - P) >> 2, 14-bit
inline constexpr uint32_t R_PPC_REL14_BRTAKEN   = 12;
inline constexpr uint32_t R_PPC_REL14_BRNTAKEN  = 13;
inline constexpr uint32_t R_PPC_GOT16           = 14;  // G + A
inline constexpr uint32_t R_PPC_GOT16_LO        = 15;
inline constexpr uint32_t R_PPC_GOT16_HI        = 16;
inline constexpr uint32_t R_PPC_GOT16_HA        = 17;
inline constexpr uint32_t R_PPC_PLTREL24        = 18;
inline constexpr uint32_t R_PPC_COPY            = 19;
inline constexpr uint32_t R_PPC_GLOB_DAT        = 20;
inline constexpr uint32_t R_PPC_JMP_SLOT        = 21;
inline constexpr uint32_t R_PPC_RELATIVE        = 22;
inline constexpr uint32_t R_PPC_LOCAL24PC        = 23;
inline constexpr uint32_t R_PPC_UADDR32         = 24;
inline constexpr uint32_t R_PPC_UADDR16         = 25;
inline constexpr uint32_t R_PPC_REL32           = 26;
inline constexpr uint32_t R_PPC_PLT32           = 27;
inline constexpr uint32_t R_PPC_PLTREL32        = 28;
inline constexpr uint32_t R_PPC_PLT16_LO        = 29;
inline constexpr uint32_t R_PPC_PLT16_HI        = 30;
inline constexpr uint32_t R_PPC_PLT16_HA        = 31;
inline constexpr uint32_t R_PPC_SDAREL16        = 32;
inline constexpr uint32_t R_PPC_SECTOFF         = 33;
inline constexpr uint32_t R_PPC_SECTOFF_LO      = 34;
inline constexpr uint32_t R_PPC_SECTOFF_HI      = 35;
inline constexpr uint32_t R_PPC_SECTOFF_HA      = 36;
inline constexpr uint32_t R_PPC_ADDR30          = 37;
// TLS relocations
inline constexpr uint32_t R_PPC_TLS             = 67;
inline constexpr uint32_t R_PPC_DTPMOD32        = 68;
inline constexpr uint32_t R_PPC_TPREL16         = 69;
inline constexpr uint32_t R_PPC_TPREL16_LO      = 70;
inline constexpr uint32_t R_PPC_TPREL16_HI      = 71;
inline constexpr uint32_t R_PPC_TPREL16_HA      = 72;
inline constexpr uint32_t R_PPC_TPREL32         = 73;
inline constexpr uint32_t R_PPC_DTPREL16        = 74;
inline constexpr uint32_t R_PPC_DTPREL16_LO     = 75;
inline constexpr uint32_t R_PPC_DTPREL16_HI     = 76;
inline constexpr uint32_t R_PPC_DTPREL16_HA     = 77;
inline constexpr uint32_t R_PPC_DTPREL32        = 78;
inline constexpr uint32_t R_PPC_GOT_TLSGD16     = 79;
inline constexpr uint32_t R_PPC_GOT_TLSGD16_LO  = 80;
inline constexpr uint32_t R_PPC_GOT_TLSGD16_HI  = 81;
inline constexpr uint32_t R_PPC_GOT_TLSGD16_HA  = 82;
inline constexpr uint32_t R_PPC_GOT_TLSLD16     = 83;
inline constexpr uint32_t R_PPC_GOT_TLSLD16_LO  = 84;
inline constexpr uint32_t R_PPC_GOT_TLSLD16_HI  = 85;
inline constexpr uint32_t R_PPC_GOT_TLSLD16_HA  = 86;
inline constexpr uint32_t R_PPC_GOT_TPREL16     = 87;
inline constexpr uint32_t R_PPC_GOT_TPREL16_LO  = 88;
inline constexpr uint32_t R_PPC_GOT_TPREL16_HI  = 89;
inline constexpr uint32_t R_PPC_GOT_TPREL16_HA  = 90;
inline constexpr uint32_t R_PPC_GOT_DTPREL16    = 91;
inline constexpr uint32_t R_PPC_GOT_DTPREL16_LO = 92;
inline constexpr uint32_t R_PPC_GOT_DTPREL16_HI = 93;
inline constexpr uint32_t R_PPC_GOT_DTPREL16_HA = 94;
inline constexpr uint32_t R_PPC_TLSGD           = 95;
inline constexpr uint32_t R_PPC_TLSLD           = 96;
// Embedded application relocation types
inline constexpr uint32_t R_PPC_EMB_NADDR32     = 101;
inline constexpr uint32_t R_PPC_EMB_NADDR16     = 102;
inline constexpr uint32_t R_PPC_EMB_NADDR16_LO  = 103;
inline constexpr uint32_t R_PPC_EMB_NADDR16_HI  = 104;
inline constexpr uint32_t R_PPC_EMB_NADDR16_HA  = 105;
inline constexpr uint32_t R_PPC_EMB_SDAI16      = 106;
inline constexpr uint32_t R_PPC_EMB_SDA2I16     = 107;
inline constexpr uint32_t R_PPC_EMB_SDA2REL     = 108;
inline constexpr uint32_t R_PPC_EMB_SDA21       = 109; // Small data area 21-bit

// Wii U Cafe OS-specific relocation type
// Used in import sections to encode RPL symbol references.
inline constexpr uint32_t R_PPC_CAFE_OS         = 201;

// ============================================================================
// Big-endian byte-swap helpers (C++20 std::endian)
// All multi-byte ELF fields in Wii U files are big-endian.  These helpers
// transparently swap on little-endian hosts and are no-ops on big-endian hosts.
// ============================================================================

namespace detail {

template<typename T>
requires std::is_integral_v<T>
[[nodiscard]] constexpr T bswap(T value) noexcept {
    if constexpr (sizeof(T) == 1) {
        return value;
    } else if constexpr (sizeof(T) == 2) {
        auto v = static_cast<uint16_t>(value);
        return static_cast<T>(static_cast<uint16_t>((v >> 8) | (v << 8)));
    } else if constexpr (sizeof(T) == 4) {
        auto v = static_cast<uint32_t>(value);
        v = ((v & 0xFF000000u) >> 24) |
            ((v & 0x00FF0000u) >>  8) |
            ((v & 0x0000FF00u) <<  8) |
            ((v & 0x000000FFu) << 24);
        return static_cast<T>(v);
    } else if constexpr (sizeof(T) == 8) {
        auto v = static_cast<uint64_t>(value);
        v = ((v & 0xFF00000000000000ull) >> 56) |
            ((v & 0x00FF000000000000ull) >> 40) |
            ((v & 0x0000FF0000000000ull) >> 24) |
            ((v & 0x000000FF00000000ull) >>  8) |
            ((v & 0x00000000FF000000ull) <<  8) |
            ((v & 0x0000000000FF0000ull) << 24) |
            ((v & 0x000000000000FF00ull) << 40) |
            ((v & 0x00000000000000FFull) << 56);
        return static_cast<T>(v);
    }
}

} // namespace detail

/// Convert a big-endian value read from a Wii U binary to the host byte order.
template<typename T>
requires std::is_integral_v<T>
[[nodiscard]] constexpr T from_be(T value) noexcept {
    if constexpr (std::endian::native == std::endian::big) {
        return value;
    } else {
        return detail::bswap(value);
    }
}

/// Convert a host-order value to big-endian for writing back to a Wii U binary.
template<typename T>
requires std::is_integral_v<T>
[[nodiscard]] constexpr T to_be(T value) noexcept {
    return from_be(value); // symmetric
}

// ============================================================================
// Packed ELF32 structs
// All fields are stored in big-endian byte order in the binary.
// Use from_be() accessors rather than reading fields directly unless you
// are certain you are running on a big-endian host.
// ============================================================================

#pragma pack(push, 1)

/// ELF32 file header (52 bytes)
struct Elf32_Ehdr {
    uint8_t    e_ident[EI_NIDENT]; // Magic, class, data, version, OS/ABI, padding
    Elf32_Half e_type;             // Object file type (ET_*)
    Elf32_Half e_machine;          // Target ISA (EM_PPC = 20)
    Elf32_Word e_version;          // ELF version (EV_CURRENT)
    Elf32_Addr e_entry;            // Entry point virtual address
    Elf32_Off  e_phoff;            // Program header table file offset
    Elf32_Off  e_shoff;            // Section header table file offset
    Elf32_Word e_flags;            // Processor-specific flags
    Elf32_Half e_ehsize;           // ELF header size in bytes (52)
    Elf32_Half e_phentsize;        // Program header entry size (32)
    Elf32_Half e_phnum;            // Number of program header entries
    Elf32_Half e_shentsize;        // Section header entry size (40)
    Elf32_Half e_shnum;            // Number of section header entries
    Elf32_Half e_shstrndx;         // Section name string table index

    // Convenience accessors that handle big-endian conversion
    [[nodiscard]] Elf32_Half type()     const noexcept { return from_be(e_type); }
    [[nodiscard]] Elf32_Half machine()  const noexcept { return from_be(e_machine); }
    [[nodiscard]] Elf32_Word version()  const noexcept { return from_be(e_version); }
    [[nodiscard]] Elf32_Addr entry()    const noexcept { return from_be(e_entry); }
    [[nodiscard]] Elf32_Off  phoff()    const noexcept { return from_be(e_phoff); }
    [[nodiscard]] Elf32_Off  shoff()    const noexcept { return from_be(e_shoff); }
    [[nodiscard]] Elf32_Word flags()    const noexcept { return from_be(e_flags); }
    [[nodiscard]] Elf32_Half ehsize()   const noexcept { return from_be(e_ehsize); }
    [[nodiscard]] Elf32_Half phentsize()const noexcept { return from_be(e_phentsize); }
    [[nodiscard]] Elf32_Half phnum()    const noexcept { return from_be(e_phnum); }
    [[nodiscard]] Elf32_Half shentsize()const noexcept { return from_be(e_shentsize); }
    [[nodiscard]] Elf32_Half shnum()    const noexcept { return from_be(e_shnum); }
    [[nodiscard]] Elf32_Half shstrndx() const noexcept { return from_be(e_shstrndx); }

    [[nodiscard]] bool has_valid_magic() const noexcept {
        return e_ident[EI_MAG0] == ELFMAG0 &&
               e_ident[EI_MAG1] == ELFMAG1 &&
               e_ident[EI_MAG2] == ELFMAG2 &&
               e_ident[EI_MAG3] == ELFMAG3;
    }

    [[nodiscard]] bool is_32bit()     const noexcept { return e_ident[EI_CLASS] == ELFCLASS32; }
    [[nodiscard]] bool is_big_endian()const noexcept { return e_ident[EI_DATA]  == ELFDATA2MSB; }
};
static_assert(sizeof(Elf32_Ehdr) == 52, "Elf32_Ehdr must be 52 bytes");

/// ELF32 section header (40 bytes)
struct Elf32_Shdr {
    Elf32_Word sh_name;      // Offset into .shstrtab
    Elf32_Word sh_type;      // Section type (SHT_*)
    Elf32_Word sh_flags;     // Section attributes (SHF_*)
    Elf32_Addr sh_addr;      // Virtual address in memory image
    Elf32_Off  sh_offset;    // Offset in file
    Elf32_Word sh_size;      // Size of section data in bytes
    Elf32_Word sh_link;      // Associated section index
    Elf32_Word sh_info;      // Extra information (meaning is type-dependent)
    Elf32_Word sh_addralign; // Required alignment (power of 2)
    Elf32_Word sh_entsize;   // Entry size (for table sections)

    [[nodiscard]] Elf32_Word name()      const noexcept { return from_be(sh_name); }
    [[nodiscard]] Elf32_Word type()      const noexcept { return from_be(sh_type); }
    [[nodiscard]] Elf32_Word flags()     const noexcept { return from_be(sh_flags); }
    [[nodiscard]] Elf32_Addr addr()      const noexcept { return from_be(sh_addr); }
    [[nodiscard]] Elf32_Off  offset()    const noexcept { return from_be(sh_offset); }
    [[nodiscard]] Elf32_Word size()      const noexcept { return from_be(sh_size); }
    [[nodiscard]] Elf32_Word link()      const noexcept { return from_be(sh_link); }
    [[nodiscard]] Elf32_Word info()      const noexcept { return from_be(sh_info); }
    [[nodiscard]] Elf32_Word addralign() const noexcept { return from_be(sh_addralign); }
    [[nodiscard]] Elf32_Word entsize()   const noexcept { return from_be(sh_entsize); }

    [[nodiscard]] bool is_rpl_zlib_compressed() const noexcept {
        return (flags() & SHF_RPL_ZLIB) != 0;
    }
};
static_assert(sizeof(Elf32_Shdr) == 40, "Elf32_Shdr must be 40 bytes");

/// ELF32 program / segment header (32 bytes)
struct Elf32_Phdr {
    Elf32_Word p_type;   // Segment type (PT_*)
    Elf32_Off  p_offset; // Offset in file
    Elf32_Addr p_vaddr;  // Virtual address
    Elf32_Addr p_paddr;  // Physical address (usually same as vaddr)
    Elf32_Word p_filesz; // Size of segment in file
    Elf32_Word p_memsz;  // Size of segment in memory
    Elf32_Word p_flags;  // Segment flags (PF_*)
    Elf32_Word p_align;  // Alignment requirement

    [[nodiscard]] Elf32_Word type()   const noexcept { return from_be(p_type); }
    [[nodiscard]] Elf32_Off  offset() const noexcept { return from_be(p_offset); }
    [[nodiscard]] Elf32_Addr vaddr()  const noexcept { return from_be(p_vaddr); }
    [[nodiscard]] Elf32_Addr paddr()  const noexcept { return from_be(p_paddr); }
    [[nodiscard]] Elf32_Word filesz() const noexcept { return from_be(p_filesz); }
    [[nodiscard]] Elf32_Word memsz()  const noexcept { return from_be(p_memsz); }
    [[nodiscard]] Elf32_Word flags()  const noexcept { return from_be(p_flags); }
    [[nodiscard]] Elf32_Word align()  const noexcept { return from_be(p_align); }
};
static_assert(sizeof(Elf32_Phdr) == 32, "Elf32_Phdr must be 32 bytes");

/// ELF32 symbol table entry (16 bytes)
struct Elf32_Sym {
    Elf32_Word  st_name;  // Offset into symbol string table
    Elf32_Addr  st_value; // Symbol value (address or offset)
    Elf32_Word  st_size;  // Size of object
    Elf32_Byte  st_info;  // Type and binding (use elf32_st_bind / elf32_st_type)
    Elf32_Byte  st_other; // Visibility (use elf32_st_visibility)
    Elf32_Half  st_shndx; // Section index

    [[nodiscard]] Elf32_Word name()  const noexcept { return from_be(st_name); }
    [[nodiscard]] Elf32_Addr value() const noexcept { return from_be(st_value); }
    [[nodiscard]] Elf32_Word size()  const noexcept { return from_be(st_size); }
    [[nodiscard]] Elf32_Half shndx() const noexcept { return from_be(st_shndx); }

    [[nodiscard]] uint8_t bind()       const noexcept { return elf32_st_bind(st_info); }
    [[nodiscard]] uint8_t type()       const noexcept { return elf32_st_type(st_info); }
    [[nodiscard]] uint8_t visibility() const noexcept { return elf32_st_visibility(st_other); }
};
static_assert(sizeof(Elf32_Sym) == 16, "Elf32_Sym must be 16 bytes");

/// ELF32 relocation entry without explicit addend (8 bytes)
struct Elf32_Rel {
    Elf32_Addr r_offset; // Virtual address of the field to relocate
    Elf32_Word r_info;   // Symbol index + relocation type packed value

    [[nodiscard]] Elf32_Addr offset()     const noexcept { return from_be(r_offset); }
    [[nodiscard]] Elf32_Word info_raw()   const noexcept { return from_be(r_info); }
    [[nodiscard]] Elf32_Word sym_index()  const noexcept { return elf32_r_sym(info_raw()); }
    [[nodiscard]] Elf32_Word reloc_type() const noexcept { return elf32_r_type(info_raw()); }
};
static_assert(sizeof(Elf32_Rel) == 8, "Elf32_Rel must be 8 bytes");

/// ELF32 relocation entry with explicit addend (12 bytes)
struct Elf32_Rela {
    Elf32_Addr  r_offset; // Virtual address of the field to relocate
    Elf32_Word  r_info;   // Symbol index + relocation type packed value
    Elf32_Sword r_addend; // Explicit addend

    [[nodiscard]] Elf32_Addr  offset()     const noexcept { return from_be(r_offset); }
    [[nodiscard]] Elf32_Word  info_raw()   const noexcept { return from_be(r_info); }
    [[nodiscard]] Elf32_Sword addend()     const noexcept { return from_be(r_addend); }
    [[nodiscard]] Elf32_Word  sym_index()  const noexcept { return elf32_r_sym(info_raw()); }
    [[nodiscard]] Elf32_Word  reloc_type() const noexcept { return elf32_r_type(info_raw()); }
};
static_assert(sizeof(Elf32_Rela) == 12, "Elf32_Rela must be 12 bytes");

#pragma pack(pop)

// ============================================================================
// RplFileInfo — Wii U SHT_RPL_FILEINFO binary layout
// This struct is stored verbatim in the SHT_RPL_FILEINFO section of every
// RPX/RPL binary. All fields are big-endian.
// ============================================================================

#pragma pack(push, 1)

/// Binary layout of the SHT_RPL_FILEINFO section data.
/// See rpx_types.hpp for a decoded representation with host-order fields.
struct RplFileInfoRaw {
    Elf32_Word magic;              // Must be 0xCAFE0402
    Elf32_Word unk0x04;
    Elf32_Word textSize;           // Total size of executable sections
    Elf32_Word textAlign;          // Alignment of text segments
    Elf32_Word dataSize;           // Total size of data sections
    Elf32_Word dataAlign;          // Alignment of data segments
    Elf32_Word loadSize;           // Size of sections loaded at runtime
    Elf32_Word loadAlign;          // Alignment of loaded sections
    Elf32_Word tempSize;           // Temporary memory needed during load
    Elf32_Word trampAdjust;        // Trampoline table adjustment
    Elf32_Word sdaBase;            // Small data area base (r13)
    Elf32_Word sda2Base;           // Small data area 2 base (r2)
    Elf32_Word stackSize;          // Default stack size
    Elf32_Word stringsOffset;      // Offset of strings within FILEINFO section
    Elf32_Word flags;              // Module flags
    Elf32_Word heapSize;           // Default heap size
    Elf32_Word tagOffset;          // Offset of tag string
    Elf32_Word minVersion;         // Minimum OS version required
    Elf32_Word compressionLevel;   // zlib compression level used
    Elf32_Word trampAddition;      // Extra trampoline slots
    Elf32_Word fileInfoPad;        // Padding
    Elf32_Word cafeSdkVersion;     // Cafe SDK version
    Elf32_Word cafeSdkRevision;    // Cafe SDK revision
    Elf32_Half tlsModuleIndex;     // TLS module index
    Elf32_Half tlsAlignShift;      // TLS alignment shift

    // Convenience accessors with big-endian conversion
    [[nodiscard]] uint32_t get_magic()               const noexcept { return from_be(magic); }
    [[nodiscard]] uint32_t get_text_size()           const noexcept { return from_be(textSize); }
    [[nodiscard]] uint32_t get_data_size()           const noexcept { return from_be(dataSize); }
    [[nodiscard]] uint32_t get_sda_base()            const noexcept { return from_be(sdaBase); }
    [[nodiscard]] uint32_t get_sda2_base()           const noexcept { return from_be(sda2Base); }
    [[nodiscard]] uint32_t get_stack_size()          const noexcept { return from_be(stackSize); }
    [[nodiscard]] uint32_t get_flags()               const noexcept { return from_be(flags); }
    [[nodiscard]] uint32_t get_heap_size()           const noexcept { return from_be(heapSize); }
    [[nodiscard]] uint32_t get_min_version()         const noexcept { return from_be(minVersion); }
    [[nodiscard]] uint32_t get_cafe_sdk_version()    const noexcept { return from_be(cafeSdkVersion); }
    [[nodiscard]] uint32_t get_cafe_sdk_revision()   const noexcept { return from_be(cafeSdkRevision); }
    [[nodiscard]] uint16_t get_tls_module_index()    const noexcept { return from_be(tlsModuleIndex); }
    [[nodiscard]] uint16_t get_tls_align_shift()     const noexcept { return from_be(tlsAlignShift); }

    [[nodiscard]] bool is_valid_magic() const noexcept {
        return get_magic() == 0xCAFE0402u;
    }
};
static_assert(sizeof(RplFileInfoRaw) == 0x60, "RplFileInfoRaw must be 0x60 (96) bytes");

#pragma pack(pop)

// ============================================================================
// RPX/RPL module flags (RplFileInfo::flags field)
// ============================================================================

inline constexpr uint32_t RPL_FLAG_IS_RPX         = 0x00000002; // File is an RPX (executable)
inline constexpr uint32_t RPL_FLAG_HAS_TLS        = 0x00000010; // Module uses TLS
inline constexpr uint32_t RPL_FLAG_USED_SDA        = 0x00000020; // Module uses small data area

// ============================================================================
// Wii U Cafe OS version encoding helpers
// Version is packed as: (major << 16) | (minor << 8) | patch
// ============================================================================

[[nodiscard]] constexpr uint32_t cafe_version(uint8_t major, uint8_t minor, uint8_t patch) noexcept {
    return (static_cast<uint32_t>(major) << 16) |
           (static_cast<uint32_t>(minor) <<  8) |
            static_cast<uint32_t>(patch);
}

[[nodiscard]] constexpr uint8_t cafe_version_major(uint32_t v) noexcept {
    return static_cast<uint8_t>(v >> 16);
}
[[nodiscard]] constexpr uint8_t cafe_version_minor(uint32_t v) noexcept {
    return static_cast<uint8_t>((v >> 8) & 0xFF);
}
[[nodiscard]] constexpr uint8_t cafe_version_patch(uint32_t v) noexcept {
    return static_cast<uint8_t>(v & 0xFF);
}

}