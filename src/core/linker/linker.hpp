#pragma once

#include "module_graph.hpp"
#include "../rpx/rpx_types.hpp"
#include "../rpl/rpl_types.hpp"
#include "../../diagnostics/diagnostics.hpp"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <functional>
#include <filesystem>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// linker.hpp — Static-recompilation linker
//
// The Linker resolves cross-module symbol references between an RPX executable
// and its RPL libraries.  Unlike a traditional dynamic linker it does not
// patch the binary at runtime; instead it constructs a complete symbol table
// mapping (module, symbol_name) → virtual_address that the code generator
// uses to emit direct function calls across module boundaries.
// ============================================================================

namespace rebrewu::linker {

/// A fully resolved inter-module symbol reference.
struct ResolvedSymbol {
    std::string from_module{};  // importing module name
    std::string to_module{};    // exporting module name
    std::string name{};         // symbol name
    uint32_t    address{0};     // virtual address in the exporting module
    bool        is_data{false}; // data symbol vs function symbol
};

/// Configuration for the linker.
struct LinkerConfig {
    /// If true, treat unresolved imports as warnings rather than errors.
    bool allow_unresolved{false};
    /// Extra RPL search paths (in addition to paths added via add_rpl_path).
    std::vector<std::filesystem::path> search_paths{};
};

/// The static-recompilation linker.
///
/// Usage:
///   1. Construct with a DiagEngine.
///   2. Call add_rpl() for each RPL library the RPX imports.
///   3. Call link(rpx) to resolve all cross-module references.
///   4. Use resolve() to look up any symbol at code-generation time.
class Linker {
public:
    explicit Linker(diagnostics::DiagEngine& diag,
                    LinkerConfig cfg = {}) noexcept;

    // -----------------------------------------------------------------
    // Setup
    // -----------------------------------------------------------------

    /// Register an already-loaded RPL module.
    void add_rpl(std::shared_ptr<rpl::RplModule> rpl);

    /// Add a file-system search path for RPL files.
    void add_search_path(const std::filesystem::path& path);

    // -----------------------------------------------------------------
    // Linking
    // -----------------------------------------------------------------

    /// Resolve all imports in `rpx` against the registered RPL modules.
    /// Returns false if any required symbol could not be resolved and
    /// allow_unresolved is false.
    bool link(rpx::RpxModule& rpx);

    // -----------------------------------------------------------------
    // Queries (valid after link())
    // -----------------------------------------------------------------

    /// Look up a symbol by (module, name).  Returns its virtual address.
    std::optional<uint32_t> resolve(const std::string& module_name,
                                    const std::string& sym_name) const noexcept;

    /// Look up a symbol by name across all modules.
    std::optional<ResolvedSymbol> resolve_any(const std::string& sym_name) const noexcept;

    /// Returns the name of the module that exports a given address.
    std::optional<std::string> module_for_addr(uint32_t addr) const noexcept;

    const ModuleGraph& graph()   const noexcept { return m_graph; }

    /// All resolved symbols produced by the last link() call.
    const std::vector<ResolvedSymbol>& resolved_symbols() const noexcept {
        return m_resolved;
    }

    // -----------------------------------------------------------------
    // Symbol resolver callback (for RelocationProcessor)
    // -----------------------------------------------------------------

    /// Returns a SymbolResolver functor bound to this linker.
    std::function<std::optional<uint32_t>(const std::string&, const std::string&)>
    make_resolver() const;

private:
    bool resolve_module_imports(rpx::RpxModule& rpx);

    diagnostics::DiagEngine&    m_diag;
    LinkerConfig                m_cfg;
    ModuleGraph                 m_graph;
    std::vector<ResolvedSymbol> m_resolved{};
};

}