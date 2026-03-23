#include <catch2/catch_test_macros.hpp>
#include "core/rpx/rpx_types.hpp"
#include "core/linker/linker.hpp"
#include "analysis/function_discovery.hpp"
#include "analysis/cfg_builder.hpp"
#include "diagnostics/diagnostics.hpp"

using namespace rebrewu;

// ============================================================================
// Helpers: build a minimal synthetic RpxModule with a small code section
// ============================================================================

static rpx::RpxModule make_test_module() {
    rpx::RpxModule mod;
    mod.name = "test";
    mod.entry_point = 0x0200'0000;

    rpx::RpxSection text;
    text.name    = ".text";
    text.address = 0x0200'0000;
    text.flags   = rebrewu::elf::SHF_ALLOC | rebrewu::elf::SHF_EXECINSTR;
    text.type    = rebrewu::elf::SHT_PROGBITS;

    // A tiny function:
    //   mflr r0          0x7C0802A6
    //   stw  r0, 4(r1)   0x90010004
    //   blr              0x4E800020
    std::array<uint8_t, 12> code = {
        0x7C, 0x08, 0x02, 0xA6,  // mflr r0
        0x90, 0x01, 0x00, 0x04,  // stw  r0, 4(r1)
        0x4E, 0x80, 0x00, 0x20   // blr
    };
    text.data.assign(code.begin(), code.end());
    text.size = static_cast<uint32_t>(text.data.size());

    mod.sections.push_back(std::move(text));

    // Add an export pointing at entry
    rpx::RpxExport exp;
    exp.address = 0x0200'0000;
    exp.name    = "entry";
    mod.exports.push_back(std::move(exp));

    return mod;
}

// ============================================================================
// FunctionDiscovery tests
// ============================================================================

TEST_CASE("FunctionDiscovery finds entry via export", "[function_discovery]") {
    diagnostics::DiagEngine diag;
    linker::Linker lnk(diag);
    auto mod = make_test_module();

    analysis::FunctionDiscovery::Config cfg;
    analysis::FunctionDiscovery disc(mod, lnk, cfg);
    auto bounds = disc.discover();

    REQUIRE_FALSE(bounds.empty());
    bool found_entry = false;
    for (const auto& b : bounds)
        if (b.start == 0x0200'0000) found_entry = true;
    REQUIRE(found_entry);
}

TEST_CASE("FunctionDiscovery::add_hint adds a forced candidate", "[function_discovery]") {
    diagnostics::DiagEngine diag;
    linker::Linker lnk(diag);
    auto mod = make_test_module();

    analysis::FunctionDiscovery::Config cfg;
    analysis::FunctionDiscovery disc(mod, lnk, cfg);
    disc.add_hint({0x0200'0004, "inner", true});
    auto bounds = disc.discover();

    bool found = false;
    for (const auto& b : bounds)
        if (b.start == 0x0200'0004) found = true;
    REQUIRE(found);
}

// ============================================================================
// CFGBuilder tests
// ============================================================================

TEST_CASE("CFGBuilder builds a function with one return block", "[cfg_builder]") {
    auto mod = make_test_module();
    analysis::CFGBuilder builder(mod);
    auto func = builder.build(0x0200'0000, "entry");
    REQUIRE(func.has_value());
    REQUIRE_FALSE(func->empty());
    REQUIRE(func->name == "entry");
}

TEST_CASE("CFGBuilder fails gracefully on non-code address", "[cfg_builder]") {
    rpx::RpxModule empty_mod;
    analysis::CFGBuilder builder(empty_mod);
    auto func = builder.build(0xDEAD'BEEF, "bad");
    REQUIRE_FALSE(func.has_value());
}
