#include <catch2/catch_test_macros.hpp>
#include "ppc/decoder/ppc_decode.hpp"
#include "ppc/instructions/ppc_fields.hpp"
#include <array>

using namespace rebrewu::ppc;

// ============================================================================
// ppc_fields helpers
// ============================================================================

TEST_CASE("ppc_field extracts correct bits", "[ppc_fields]") {
    // NOP instruction: ori 0,0,0 = 0x60000000
    //   primary opcode = bits 0-5 = 24
    uint32_t nop_word = 0x60000000u;
    REQUIRE(ppc_field(nop_word, 0, 5) == 24u);
    REQUIRE(ppc_field(nop_word, 6, 10) == 0u);
}

TEST_CASE("sign_extend works for 16-bit values", "[ppc_fields]") {
    REQUIRE(sign_extend_16(0x8000u) == -32768);
    REQUIRE(sign_extend_16(0x7FFFu) ==  32767);
    REQUIRE(sign_extend_16(0xFFFFu) == -1);
    REQUIRE(sign_extend_16(0x0000u) == 0);
    REQUIRE(sign_extend_16(0x0001u) == 1);
}

TEST_CASE("sign_extend works for 26-bit values (branch displacement)", "[ppc_fields]") {
    // +4 encoded as 26-bit: 0x000004
    REQUIRE(sign_extend(4, 26) == 4);
    // -4 encoded as 26-bit two's-complement: 0x3FFFFFC
    uint32_t neg4 = (0x4000000u - 4u) & 0x3FFFFFFu;
    REQUIRE(sign_extend(neg4, 26) == -4);
}

// ============================================================================
// decode() basic sanity
// ============================================================================

TEST_CASE("decode NOP (ori 0,0,0)", "[ppc_decode]") {
    auto insn = decode(0x60000000u, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::NOP);
    REQUIRE(insn->iclass   == InstrClass::Integer);
    REQUIRE(insn->addr     == 0x0200'0000u);
}

TEST_CASE("decode B (unconditional branch forward)", "[ppc_decode]") {
    // b +8 = 0x48000008
    //   primary op = 18, LI = 2 (=8>>2), AA=0, LK=0
    auto insn = decode(0x48000008u, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::B);
    REQUIRE(insn->iclass   == InstrClass::Branch);
    REQUIRE_FALSE(insn->lk);
    REQUIRE_FALSE(insn->aa);
    REQUIRE(insn->target == 0x0200'0008u);
}

TEST_CASE("decode BL (branch-and-link)", "[ppc_decode]") {
    // bl +8 = 0x48000009  (LK=1)
    auto insn = decode(0x48000009u, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::BL);
    REQUIRE(insn->lk);
    REQUIRE(insn->is_call());
}

TEST_CASE("decode BLR (branch to link register)", "[ppc_decode]") {
    // blr = 0x4E800020  (op=19, xop=16, BO=20, BI=0, LK=0)
    auto insn = decode(0x4E800020u, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::BLR);
    REQUIRE(insn->is_return());
    REQUIRE_FALSE(insn->lk);
}

TEST_CASE("decode BCTR (branch to count register)", "[ppc_decode]") {
    // bctr = 0x4E800420  (op=19, xop=528)
    auto insn = decode(0x4E800420u, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::BCTR);
    REQUIRE(insn->is_indirect_branch());
}

TEST_CASE("decode ADDI r3, r3, -1", "[ppc_decode]") {
    // addi r3, r3, -1 = 0x3863FFFF
    //   op=14, rD=3, rA=3, simm=-1
    auto insn = decode(0x3863FFFFu, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::ADDI);
    REQUIRE(insn->rD == 3);
    REQUIRE(insn->rA == 3);
    REQUIRE(insn->imm == -1);
}

TEST_CASE("decode LI r0, 0 (addi r0, 0, 0)", "[ppc_decode]") {
    // li r0, 0 = addi r0, 0, 0 = 0x38000000
    auto insn = decode(0x38000000u, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::LI);
    REQUIRE(insn->rD == 0);
    REQUIRE(insn->rA == 0);
    REQUIRE(insn->imm == 0);
}

TEST_CASE("decode LWZ r3, 0(r1)", "[ppc_decode]") {
    // lwz r3, 0(r1) = 0x80610000
    //   op=32, rD=3, rA=1, d=0
    auto insn = decode(0x80610000u, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::LWZ);
    REQUIRE(insn->iclass   == InstrClass::Load);
    REQUIRE(insn->rD == 3);
    REQUIRE(insn->rA == 1);
    REQUIRE(insn->imm == 0);
}

TEST_CASE("decode STW r3, 4(r1)", "[ppc_decode]") {
    // stw r3, 4(r1) = 0x90610004
    //   op=36, rS=3, rA=1, d=4
    auto insn = decode(0x90610004u, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::STW);
    REQUIRE(insn->iclass   == InstrClass::Store);
    REQUIRE(insn->imm == 4);
}

TEST_CASE("decode MTLR r0 (mtspr 8, r0)", "[ppc_decode]") {
    // mtlr r0 = 0x7C0803A6  (op=31, xop=467, spr=8, rS=0)
    auto insn = decode(0x7C0803A6u, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::MTLR);
}

TEST_CASE("decode MFLR r0 (mfspr r0, 8)", "[ppc_decode]") {
    // mflr r0 = 0x7C0802A6  (op=31, xop=339, spr=8, rD=0)
    auto insn = decode(0x7C0802A6u, 0x0200'0000u);
    REQUIRE(insn.has_value());
    REQUIRE(insn->mnemonic == Mnemonic::MFLR);
}

TEST_CASE("decode_block handles an empty span", "[ppc_decode]") {
    auto result = decode_block({}, 0x0200'0000u);
    REQUIRE(result.empty());
}

TEST_CASE("decode_block decodes sequence of NOP + BLR", "[ppc_decode]") {
    std::array<uint8_t, 8> bytes = {
        0x60, 0x00, 0x00, 0x00,  // NOP
        0x4E, 0x80, 0x00, 0x20   // BLR
    };
    auto result = decode_block(bytes, 0x0200'0000u);
    REQUIRE(result.size() == 2);
    REQUIRE(result[0].mnemonic == Mnemonic::NOP);
    REQUIRE(result[1].mnemonic == Mnemonic::BLR);
}
