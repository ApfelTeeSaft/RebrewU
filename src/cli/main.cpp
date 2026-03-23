#include "cli_options.hpp"
#include "../core/rpx/rpx_loader.hpp"
#include "../core/rpl/rpl_loader.hpp"
#include "../core/linker/linker.hpp"
#include "../core/relocation/reloc_processor.hpp"
#include "../analysis/function_discovery.hpp"
#include "../analysis/cfg_builder.hpp"
#include "../codegen/cpp_emitter.hpp"
#include "../codegen/naming.hpp"
#include "../config/config.hpp"
#include "../diagnostics/diagnostics.hpp"
#include <iostream>
#include <cstdlib>

using namespace rebrewu;

int main(int argc, char** argv) {
    auto opts = cli::parse(argc, argv);
    if (!opts) return EXIT_FAILURE;

    diagnostics::DiagEngine diag(opts->verbose);

    rpx::RpxLoader rpx_loader(diag);
    auto rpx_opt = rpx_loader.load(opts->rpx_path);
    if (!rpx_opt) {
        std::cerr << "Failed to load RPX: " << opts->rpx_path << "\n";
        return EXIT_FAILURE;
    }
    auto& rpx = *rpx_opt;

    linker::Linker lnk(diag);
    rpl::RplLoader rpl_loader(diag);
    for (const auto& rpl_path : opts->rpl_paths) {
        auto rpl_opt = rpl_loader.load(rpl_path);
        if (!rpl_opt) {
            std::cerr << "warning: failed to load RPL: " << rpl_path << "\n";
            continue;
        }
        lnk.add_rpl(std::make_shared<rpl::RplModule>(std::move(*rpl_opt)));
    }
    lnk.link(rpx);

    if (opts->command == cli::Command::ListSymbols) {
        for (const auto& sym : rpx.symbols)
            std::cout << std::hex << sym.address << "  " << sym.name << "\n";
        return EXIT_SUCCESS;
    }

    if (opts->command == cli::Command::ListSections) {
        for (const auto& sec : rpx.sections)
            std::cout << sec.name << "  addr=0x" << std::hex << sec.address
                      << "  size=0x" << sec.size << "\n";
        return EXIT_SUCCESS;
    }

    if (opts->command == cli::Command::ListExports) {
        for (const auto& exp : rpx.exports)
            std::cout << std::hex << exp.address << "  " << exp.name << "\n";
        return EXIT_SUCCESS;
    }

    if (opts->command == cli::Command::ListImports) {
        for (const auto& imp : rpx.imports)
            std::cout << imp.from_module << "!" << imp.name
                      << "  stub=0x" << std::hex << imp.stub_address << "\n";
        return EXIT_SUCCESS;
    }

    if (opts->no_codegen) return EXIT_SUCCESS;

    // Function discovery
    analysis::FunctionDiscovery::Config disc_cfg;
    analysis::FunctionDiscovery disc(rpx, lnk, disc_cfg);
    // Manual hints: vtable entries that point to mflr-prolog functions
    // (the data-pointer scan finds the stwu at +4 but vtable uses the mflr at +0)
    disc.add_hint({0x02AAE470u, "fn_02AAE470_ctor", false});
    // Functions reached only via indirect BCTR dispatch (vtable calls, function
    // pointer tables) that were missed by static data-pointer scanning.
    // Collected from unhandled-dispatch log of a first run.
    for (uint32_t addr : {
            0x02043938u, 0x023C620Cu, 0x023C6238u, 0x023C6264u,
            0x023F7290u, 0x024AE7B0u, 0x025CE364u, 0x02619CE8u,
            0x027DF720u, 0x02828210u, 0x028C1CB0u, 0x02A29934u,
            0x02AF2350u, 0x02C0ABA8u, 0x02C32F08u, 0x02C3758Cu,
            0x02C851D4u,
        }) {
        disc.add_hint({addr, {}, false});
    }
    //  addresses dispatched from runtime-written vtables; force=true to
    // bypass is_valid_code_addr in case the hint falls in a coverage gap.
    for (uint32_t addr : {
            0x028C1C60u, 0x028FB6C4u,
        }) {
        disc.add_hint({addr, {}, true});
    }
    auto boundaries = disc.discover();

    if (opts->verbose)
        std::cerr << "Discovered " << boundaries.size() << " function candidates.\n";

    analysis::CFGBuilder cfg_builder(rpx);
    ir::IRModule ir_module;
    ir_module.name   = rpx.name;
    ir_module.is_rpx = true;

    uint32_t built_ok = 0, built_fail = 0;
    for (const auto& fb : boundaries) {
        auto func = cfg_builder.build(fb.start, fb.name);
        if (func) {
            ir_module.functions.push_back(std::move(*func));
            ++built_ok;
        } else {
            ++built_fail;
            if (opts->verbose)
                std::cerr << "  build failed @ 0x" << std::hex << fb.start
                          << ": " << cfg_builder.last_error() << "\n";
        }
    }
    if (opts->verbose)
        std::cerr << "Built " << std::dec << built_ok << " functions ("
                  << built_fail << " failed).\n"
                  << "Entry point: 0x" << std::hex << rpx.entry_point << "\n";

    for (const auto& sym : rpx.symbols) {
        if (sym.address != 0 && !sym.name.empty())
            ir_module.add_symbol(sym.address, sym.name);
    }
    for (const auto& exp : rpx.exports) {
        if (exp.address != 0 && !exp.name.empty()) {
            ir_module.add_symbol(exp.address, exp.name);
            ir_module.exports[exp.name] = exp.address;
        }
    }

    for (const auto& sec : rpx.sections) {
        if (!sec.is_allocated() || sec.is_executable() || sec.data.empty()) continue;
        ir::IRDataSection ds;
        ds.name       = sec.name;
        ds.guest_addr = sec.address;
        ds.data       = sec.data;
        ds.read_only  = !sec.is_writable();
        ir_module.data_sections.push_back(std::move(ds));
    }

    codegen::NamingContext names(rpx.name);
    codegen::CppEmitter emitter(ir_module, rpx, lnk, std::move(names));
    if (!emitter.emit(opts->output_dir)) {
        std::cerr << "Code generation failed: " << emitter.last_error() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Done. Output written to " << opts->output_dir << "\n";
    return EXIT_SUCCESS;
}
