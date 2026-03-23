// SPDX-License-Identifier: MIT
// RebrewU - Wii U Static Recompilation Framework
// PPC instruction field extraction for Espresso (PPC 750CL)
//
// PPC bit numbering is big-endian: bit 0 is the MSB of the 32-bit word,
// bit 31 is the LSB. All field extractors operate on the raw uint32_t word.

#pragma once

#include <cstdint>

namespace rebrewu::ppc {

// ---------------------------------------------------------------------------
// Low-level bit manipulation helpers
// ---------------------------------------------------------------------------

/// Extract a contiguous field from a PPC instruction word.
/// PPC bit notation: bit 0 = MSB. Field [hi:lo] has (hi-lo+1) bits.
/// Converts to conventional LSB-first extraction internally.
/// @param word   raw 32-bit instruction word
/// @param hi     PPC bit number of the most-significant bit of the field
/// @param lo     PPC bit number of the least-significant bit of the field
[[nodiscard]] constexpr uint32_t ppc_field(uint32_t word, int hi, int lo) noexcept {
    // PPC bit N corresponds to shift (31 - N) in a conventional uint32_t.
    // Field [hi:lo] occupies conventional bits [(31-hi) .. (31-lo)].
    const int shift = 31 - lo;
    const int width = lo - hi + 1;
    const uint32_t mask = (width >= 32) ? ~0u : ((1u << width) - 1u);
    return (word >> shift) & mask;
}

/// Extract a single PPC bit (returns 0 or 1).
[[nodiscard]] constexpr uint32_t ppc_bit(uint32_t word, int bit) noexcept {
    return (word >> (31 - bit)) & 1u;
}

// ---------------------------------------------------------------------------
// Sign-extension helpers
// ---------------------------------------------------------------------------

/// Sign-extend a value with the given number of significant bits to int32_t.
[[nodiscard]] inline constexpr int32_t sign_extend(uint32_t val, int bits) noexcept {
    const uint32_t sign_bit = 1u << (bits - 1);
    return static_cast<int32_t>((val ^ sign_bit) - sign_bit);
}

[[nodiscard]] inline constexpr int32_t sign_extend_16(uint32_t val) noexcept {
    return sign_extend(val & 0xFFFFu, 16);
}

[[nodiscard]] inline constexpr int32_t sign_extend_14(uint32_t val) noexcept {
    return sign_extend(val & 0x3FFFu, 14);
}

[[nodiscard]] inline constexpr int32_t sign_extend_24(uint32_t val) noexcept {
    return sign_extend(val & 0xFFFFFFu, 24);
}

// ---------------------------------------------------------------------------
// Primary opcode
// ---------------------------------------------------------------------------

/// OPCD – bits 0:5 (6-bit primary opcode)
[[nodiscard]] constexpr uint32_t OPCD(uint32_t instr) noexcept {
    return ppc_field(instr, 0, 5);
}

// ---------------------------------------------------------------------------
// Register fields (5-bit, unsigned)
// ---------------------------------------------------------------------------

/// RT / RS – bits 6:10 (destination / source GPR)
[[nodiscard]] constexpr uint32_t RT(uint32_t instr) noexcept { return ppc_field(instr, 6, 10); }
[[nodiscard]] constexpr uint32_t RS(uint32_t instr) noexcept { return ppc_field(instr, 6, 10); }

/// RA – bits 11:15
[[nodiscard]] constexpr uint32_t RA(uint32_t instr) noexcept { return ppc_field(instr, 11, 15); }

/// RB – bits 16:20
[[nodiscard]] constexpr uint32_t RB(uint32_t instr) noexcept { return ppc_field(instr, 16, 20); }

/// RC – bits 21:25 (used by some X-form with 3 reg sources, e.g. rlwnm)
[[nodiscard]] constexpr uint32_t RC(uint32_t instr) noexcept { return ppc_field(instr, 21, 25); }

// ---------------------------------------------------------------------------
// Floating-point register fields (same bit positions, different namespace)
// ---------------------------------------------------------------------------

[[nodiscard]] constexpr uint32_t FRT(uint32_t instr) noexcept { return ppc_field(instr, 6, 10); }
[[nodiscard]] constexpr uint32_t FRS(uint32_t instr) noexcept { return ppc_field(instr, 6, 10); }
[[nodiscard]] constexpr uint32_t FRA(uint32_t instr) noexcept { return ppc_field(instr, 11, 15); }
[[nodiscard]] constexpr uint32_t FRB(uint32_t instr) noexcept { return ppc_field(instr, 16, 20); }
/// FRC – bits 21:25 (A-form FP multiply source)
[[nodiscard]] constexpr uint32_t FRC(uint32_t instr) noexcept { return ppc_field(instr, 21, 25); }

// ---------------------------------------------------------------------------
// Branch fields
// ---------------------------------------------------------------------------

/// BO – bits 6:10 (branch options)
[[nodiscard]] constexpr uint32_t BO(uint32_t instr) noexcept { return ppc_field(instr, 6, 10); }

/// BI – bits 11:15 (branch condition bit in CR)
[[nodiscard]] constexpr uint32_t BI(uint32_t instr) noexcept { return ppc_field(instr, 11, 15); }

/// BD – bits 16:29 (14-bit branch displacement, sign-extended, left-shifted by 2)
/// Returns the byte offset (already << 2 and sign-extended).
[[nodiscard]] inline int32_t BD(uint32_t instr) noexcept {
    // Raw 14-bit field from bits 16:29
    const uint32_t raw = ppc_field(instr, 16, 29);
    // Sign-extend 14 bits, then shift left 2 for byte offset
    return sign_extend_14(raw) << 2;
}

/// LI – bits 6:29 (24-bit branch target, sign-extended, left-shifted by 2)
[[nodiscard]] inline int32_t LI(uint32_t instr) noexcept {
    const uint32_t raw = ppc_field(instr, 6, 29);
    return sign_extend_24(raw) << 2;
}

/// AA – bit 30 (absolute address flag)
[[nodiscard]] constexpr uint32_t AA(uint32_t instr) noexcept { return ppc_bit(instr, 30); }

/// LK – bit 31 (link/call flag)
[[nodiscard]] constexpr uint32_t LK(uint32_t instr) noexcept { return ppc_bit(instr, 31); }

// ---------------------------------------------------------------------------
// Immediate fields
// ---------------------------------------------------------------------------

/// D / SI – bits 16:31, 16-bit signed immediate
[[nodiscard]] inline int32_t D(uint32_t instr) noexcept {
    return sign_extend_16(ppc_field(instr, 16, 31));
}
[[nodiscard]] inline int32_t SI(uint32_t instr) noexcept { return D(instr); }

/// UI – bits 16:31, 16-bit unsigned immediate
[[nodiscard]] constexpr uint32_t UI(uint32_t instr) noexcept { return ppc_field(instr, 16, 31); }

// ---------------------------------------------------------------------------
// Extended opcodes
// ---------------------------------------------------------------------------

/// XO (X-form) – bits 21:30 (10-bit)
[[nodiscard]] constexpr uint32_t XO_X(uint32_t instr) noexcept { return ppc_field(instr, 21, 30); }

/// XO (XL-form) – bits 21:30 (same positions as X-form)
[[nodiscard]] constexpr uint32_t XO_XL(uint32_t instr) noexcept { return ppc_field(instr, 21, 30); }

/// XO (XFX-form) – bits 21:30 (mtspr/mfspr use bits 11:20 for spr, 21:30 for xo)
[[nodiscard]] constexpr uint32_t XO_XFX(uint32_t instr) noexcept { return ppc_field(instr, 21, 30); }

/// XO (A-form) – bits 26:30 (5-bit, used by float A-form instructions)
[[nodiscard]] constexpr uint32_t XO_A(uint32_t instr) noexcept { return ppc_field(instr, 26, 30); }

/// Generic XO alias (X-form, most common)
[[nodiscard]] constexpr uint32_t XO(uint32_t instr) noexcept { return XO_X(instr); }

// ---------------------------------------------------------------------------
// Rotate/shift mask fields
// ---------------------------------------------------------------------------

/// SH – bits 16:20 (5-bit shift amount)
[[nodiscard]] constexpr uint32_t SH(uint32_t instr) noexcept { return ppc_field(instr, 16, 20); }

/// MB – bits 21:25 (mask begin for rlwimi/rlwinm/rlwnm)
[[nodiscard]] constexpr uint32_t MB(uint32_t instr) noexcept { return ppc_field(instr, 21, 25); }

/// ME – bits 26:30 (mask end)
[[nodiscard]] constexpr uint32_t ME(uint32_t instr) noexcept { return ppc_field(instr, 26, 30); }

// ---------------------------------------------------------------------------
// Condition register fields
// ---------------------------------------------------------------------------

/// CRFD – bits 6:8 (3-bit CR field destination, used in compare instructions)
[[nodiscard]] constexpr uint32_t CRFD(uint32_t instr) noexcept { return ppc_field(instr, 6, 8); }

/// CRFS – bits 11:13 (3-bit CR field source, used in mcrf)
[[nodiscard]] constexpr uint32_t CRFS(uint32_t instr) noexcept { return ppc_field(instr, 11, 13); }

/// BT/BA/BB for CR logical instructions (5-bit CR bit fields)
[[nodiscard]] constexpr uint32_t CR_BT(uint32_t instr) noexcept { return ppc_field(instr, 6, 10); }
[[nodiscard]] constexpr uint32_t CR_BA(uint32_t instr) noexcept { return ppc_field(instr, 11, 15); }
[[nodiscard]] constexpr uint32_t CR_BB(uint32_t instr) noexcept { return ppc_field(instr, 16, 20); }

/// FLM – bits 7:14 (8-bit field mask for mtfsf)
[[nodiscard]] constexpr uint32_t FM(uint32_t instr) noexcept { return ppc_field(instr, 7, 14); }

// ---------------------------------------------------------------------------
// SPR (Special Purpose Register) field
// ---------------------------------------------------------------------------
// SPR encoding in mtspr/mfspr is split: spr[4:0] = bits 11:15, spr[9:5] = bits 16:20.
// The architectural SPR number is constructed as: (bits16:20 << 5) | bits11:15.

[[nodiscard]] constexpr uint32_t SPR(uint32_t instr) noexcept {
    const uint32_t lo = ppc_field(instr, 11, 15);  // spr[4:0]
    const uint32_t hi = ppc_field(instr, 16, 20);  // spr[9:5]
    return (hi << 5) | lo;
}

// ---------------------------------------------------------------------------
// Trap / Misc fields
// ---------------------------------------------------------------------------

/// TO – bits 6:10 (trap operand / TO field)
[[nodiscard]] constexpr uint32_t TO(uint32_t instr) noexcept { return ppc_field(instr, 6, 10); }

/// NB – bits 16:20 (byte count for lswi/stswi; 0 means 32)
[[nodiscard]] constexpr uint32_t NB(uint32_t instr) noexcept { return ppc_field(instr, 16, 20); }

/// OE – bit 21 (overflow enable)
[[nodiscard]] constexpr uint32_t OE(uint32_t instr) noexcept { return ppc_bit(instr, 21); }

/// Rc – bit 31 (record bit; updates CR0 for integer or CR1 for float)
[[nodiscard]] constexpr uint32_t Rc(uint32_t instr) noexcept { return ppc_bit(instr, 31); }

// ---------------------------------------------------------------------------
// Paired-single / quantized memory fields
// ---------------------------------------------------------------------------

/// W – bit 15 (wide flag for psq_l/psq_st: 0=pair, 1=single)
[[nodiscard]] constexpr uint32_t W(uint32_t instr) noexcept { return ppc_bit(instr, 15); }

/// I – bits 12:14 (3-bit immediate GQR index for psq_l/psq_st D-form)
[[nodiscard]] constexpr uint32_t I(uint32_t instr) noexcept { return ppc_field(instr, 12, 14); }

/// GQR_IDX – alias for I field (GQR index in quantized load/store)
[[nodiscard]] constexpr uint32_t GQR_IDX(uint32_t instr) noexcept { return I(instr); }

/// PSQ D-form displacement – bits 16:31 minus the W/I bits.
/// For psq_l/psq_st the offset is bits 20:31 (12-bit signed).
[[nodiscard]] inline int32_t PSQ_D(uint32_t instr) noexcept {
    return sign_extend(ppc_field(instr, 20, 31), 12);
}

// ---------------------------------------------------------------------------
// SPR number constants (architectural encoding, post-swap)
// ---------------------------------------------------------------------------

namespace spr {
    // Core SPRs defined by the PowerPC architecture
    inline constexpr uint32_t XER     =   1;
    inline constexpr uint32_t LR      =   8;
    inline constexpr uint32_t CTR     =   9;
    inline constexpr uint32_t DSISR   =  18;
    inline constexpr uint32_t DAR     =  19;
    inline constexpr uint32_t DEC     =  22;
    inline constexpr uint32_t SDR1    =  25;
    inline constexpr uint32_t SRR0    =  26;
    inline constexpr uint32_t SRR1    =  27;
    // Time base (mftb uses tbr encoding, same decode path)
    inline constexpr uint32_t TBL     = 268;
    inline constexpr uint32_t TBU     = 269;
    // SPRG
    inline constexpr uint32_t SPRG0   = 272;
    inline constexpr uint32_t SPRG1   = 273;
    inline constexpr uint32_t SPRG2   = 274;
    inline constexpr uint32_t SPRG3   = 275;
    // Processor version
    inline constexpr uint32_t PVR     = 287;
    // HID registers (Espresso-specific)
    inline constexpr uint32_t HID0    = 1008;
    inline constexpr uint32_t HID1    = 1009;
    inline constexpr uint32_t HID2    =  920;
    // Espresso write-gather pipe / DMA
    inline constexpr uint32_t WPAR    =  921;
    inline constexpr uint32_t DMAU    =  922;
    inline constexpr uint32_t DMAL    =  923;
    // Paired-single GQR registers (GQR0-GQR7 = 912-919)
    inline constexpr uint32_t GQR0    =  912;
    inline constexpr uint32_t GQR1    =  913;
    inline constexpr uint32_t GQR2    =  914;
    inline constexpr uint32_t GQR3    =  915;
    inline constexpr uint32_t GQR4    =  916;
    inline constexpr uint32_t GQR5    =  917;
    inline constexpr uint32_t GQR6    =  918;
    inline constexpr uint32_t GQR7    =  919;
    // Performance monitoring (750CL)
    inline constexpr uint32_t UPMC1   = 937;
    inline constexpr uint32_t UPMC2   = 938;
    inline constexpr uint32_t UPMC3   = 941;
    inline constexpr uint32_t UPMC4   = 942;
    inline constexpr uint32_t USIA    = 939;
    // L2 cache control
    inline constexpr uint32_t L2CR    = 1017;
    // Instruction cache throttle
    inline constexpr uint32_t ICTC    = 1019;
    // Thermal management
    inline constexpr uint32_t THRM1   = 1020;
    inline constexpr uint32_t THRM2   = 1021;
    inline constexpr uint32_t THRM3   = 1022;
    // Processor ID
    inline constexpr uint32_t PIR     = 1023;

    /// Returns true if the given SPR number corresponds to a GQR register.
    [[nodiscard]] constexpr bool is_gqr(uint32_t spr_num) noexcept {
        return spr_num >= GQR0 && spr_num <= GQR7;
    }

    /// Returns the GQR index (0-7) for a GQR SPR number.
    /// Caller must ensure is_gqr(spr_num) is true.
    [[nodiscard]] constexpr uint32_t gqr_index(uint32_t spr_num) noexcept {
        return spr_num - GQR0;
    }
}

}