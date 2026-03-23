#pragma once

#include "../rpl/rpl_types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <memory>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// module_graph.hpp — Dependency graph of RPL modules
//
// The Wii U OS loader resolves RPL imports by searching a list of loaded
// modules.  ModuleGraph tracks the loaded RPL set and the dependency edges
// (which module imports from which) so that the recompiler can emit the
// correct inter-module call stubs.
// ============================================================================

namespace rebrewu::linker {

/// A node in the dependency graph.
struct ModuleNode {
    std::string                           name{};       // module name (no .rpl)
    std::shared_ptr<rpl::RplModule>       module{};     // nullptr if not yet loaded
    std::vector<std::string>              depends_on{}; // names of imported modules
    std::vector<std::string>              depended_by{};// names of modules that import this

    bool is_loaded() const noexcept { return module != nullptr; }
};

/// Directed acyclic graph of module dependencies.
class ModuleGraph {
public:
    ModuleGraph() = default;

    // -----------------------------------------------------------------
    // Graph construction
    // -----------------------------------------------------------------

    /// Register a module (without its RPL data; marks the node as known).
    void add_module(const std::string& name);

    /// Register a module with its loaded RPL data.
    void add_module(std::shared_ptr<rpl::RplModule> mod);

    /// Declare that `importer` depends on `exporter`.
    void add_dependency(const std::string& importer,
                        const std::string& exporter);

    // -----------------------------------------------------------------
    // Queries
    // -----------------------------------------------------------------

    ModuleNode* find(const std::string& name) noexcept;
    const ModuleNode* find(const std::string& name) const noexcept;

    bool contains(const std::string& name) const noexcept;

    /// Topological sort of all nodes (dependency order, leaves first).
    /// Returns empty vector if there is a cycle.
    std::vector<std::string> topo_sort() const;

    /// All modules that transitively import from `name`.
    std::vector<std::string> dependents_of(const std::string& name) const;

    /// All modules that `name` transitively imports.
    std::vector<std::string> dependencies_of(const std::string& name) const;

    const std::vector<ModuleNode>& nodes() const noexcept { return m_nodes; }

private:
    std::vector<ModuleNode> m_nodes{};
    bool topo_visit(const std::string& name,
                    std::unordered_map<std::string,int>& state,
                    std::vector<std::string>& order) const;
};

}