#include "linker.hpp"

namespace rebrewu::linker {

Linker::Linker(diagnostics::DiagEngine& diag, LinkerConfig cfg) noexcept
    : m_diag(diag), m_cfg(std::move(cfg)) {}

void Linker::add_rpl(std::shared_ptr<rpl::RplModule> rpl) {
    if (!rpl) return;
    m_graph.add_module(rpl);
}

void Linker::add_search_path(const std::filesystem::path& path) {
    m_cfg.search_paths.push_back(path);
}

bool Linker::link(rpx::RpxModule& rpx) {
    m_resolved.clear();

    // Build dependency edges
    for (const auto& imp : rpx.imports) {
        m_graph.add_dependency(rpx.name, imp.from_module);
    }

    return resolve_module_imports(rpx);
}

bool Linker::resolve_module_imports(rpx::RpxModule& rpx) {
    bool all_ok = true;

    for (auto& imp : rpx.imports) {
        const auto* node = m_graph.find(imp.from_module);
        if (!node || !node->module) {
            if (!m_cfg.allow_unresolved) {
                m_diag.error("RPL not loaded: " + imp.from_module +
                             " (needed for symbol '" + imp.name + "')");
                all_ok = false;
            }
            continue;
        }

        const auto& rpl = *node->module;
        const auto* exp = rpl.export_named(imp.name);
        if (!exp) {
            if (!m_cfg.allow_unresolved) {
                m_diag.error("symbol '" + imp.name + "' not found in " + imp.from_module);
                all_ok = false;
            }
            continue;
        }

        ResolvedSymbol rs;
        rs.from_module = rpx.name;
        rs.to_module   = imp.from_module;
        rs.name        = imp.name;
        rs.address     = exp->address;
        rs.is_data     = exp->is_data;
        m_resolved.push_back(std::move(rs));
    }

    return all_ok;
}

std::optional<uint32_t> Linker::resolve(const std::string& module_name,
                                         const std::string& sym_name) const noexcept {
    for (const auto& rs : m_resolved) {
        if (rs.to_module == module_name && rs.name == sym_name)
            return rs.address;
    }
    // Also check the module's own exports
    const auto* node = m_graph.find(module_name);
    if (node && node->module) {
        const auto* exp = node->module->export_named(sym_name);
        if (exp) return exp->address;
    }
    return {};
}

std::optional<ResolvedSymbol> Linker::resolve_any(const std::string& sym_name) const noexcept {
    for (const auto& rs : m_resolved) {
        if (rs.name == sym_name) return rs;
    }
    return {};
}

std::optional<std::string> Linker::module_for_addr(uint32_t addr) const noexcept {
    for (const auto& node : m_graph.nodes()) {
        if (!node.module) continue;
        for (const auto& exp : node.module->exports) {
            if (exp.address == addr) return node.name;
        }
    }
    return {};
}

std::function<std::optional<uint32_t>(const std::string&, const std::string&)>
Linker::make_resolver() const {
    return [this](const std::string& mod, const std::string& sym) {
        return resolve(mod, sym);
    };
}

}