#include <catch2/catch_test_macros.hpp>
#include "codegen/naming.hpp"
#include "codegen/cpp_emitter.hpp"
#include "ir/ir_module.hpp"
#include "core/rpx/rpx_types.hpp"
#include "core/linker/linker.hpp"
#include "diagnostics/diagnostics.hpp"
#include <sstream>

using namespace rebrewu;

// ============================================================================
// NamingContext tests
// ============================================================================

TEST_CASE("NamingContext::sanitize strips non-identifier characters", "[naming]") {
    REQUIRE(codegen::NamingContext::sanitize("hello_world") == "hello_world");
    REQUIRE(codegen::NamingContext::sanitize("foo::bar")    == "foo__bar");
    REQUIRE(codegen::NamingContext::sanitize("1bad")        == "_1bad");
    REQUIRE(codegen::NamingContext::sanitize("")            == "");
}

TEST_CASE("NamingContext::function_name generates stable synthetic name", "[naming]") {
    codegen::NamingContext ctx("mymod");
    std::string n1 = ctx.function_name(0x0200'0000);
    std::string n2 = ctx.function_name(0x0200'0000);
    REQUIRE(n1 == n2);
    REQUIRE_FALSE(n1.empty());
}

TEST_CASE("NamingContext::function_name uses hint when provided", "[naming]") {
    codegen::NamingContext ctx("mymod");
    std::string n = ctx.function_name(0x100, "OSCreateThread");
    REQUIRE(n.find("OSCreateThread") != std::string::npos);
}

TEST_CASE("NamingContext::set_name overrides generated name", "[naming]") {
    codegen::NamingContext ctx("mymod");
    ctx.set_name(0x1000, "my_custom_name");
    REQUIRE(ctx.function_name(0x1000) == "my_custom_name");
}

TEST_CASE("NamingContext::block_label is deterministic", "[naming]") {
    codegen::NamingContext ctx("mod");
    auto l1 = ctx.block_label(0x100, 0x200);
    auto l2 = ctx.block_label(0x100, 0x200);
    REQUIRE(l1 == l2);
    REQUIRE_FALSE(l1.empty());
}

TEST_CASE("NamingContext::data_name generates distinct names for different addrs", "[naming]") {
    codegen::NamingContext ctx("mod");
    auto a = ctx.data_name(0x100);
    auto b = ctx.data_name(0x200);
    REQUIRE(a != b);
}

// ============================================================================
// CppEmitter smoke tests
// ============================================================================

TEST_CASE("CppEmitter::emit_header writes a pragma once", "[cpp_emitter]") {
    diagnostics::DiagEngine diag;
    linker::Linker lnk(diag);
    ir::IRModule ir_mod;
    rpx::RpxModule rpx;
    codegen::NamingContext names("test");
    codegen::CppEmitter emitter(ir_mod, rpx, lnk, std::move(names));

    std::ostringstream oss;
    emitter.emit_header(oss);
    REQUIRE(oss.str().find("#pragma once") != std::string::npos);
}

TEST_CASE("CppEmitter::emit returns true for empty module", "[cpp_emitter]") {
    diagnostics::DiagEngine diag;
    linker::Linker lnk(diag);
    ir::IRModule ir_mod;
    rpx::RpxModule rpx;
    codegen::NamingContext names("test");
    codegen::CppEmitter emitter(ir_mod, rpx, lnk, std::move(names));

    // Emit to a temporary directory
    auto tmp = std::filesystem::temp_directory_path() / "rebrewu_test_codegen";
    REQUIRE(emitter.emit(tmp));
}
