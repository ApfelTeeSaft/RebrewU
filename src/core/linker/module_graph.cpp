#include "module_graph.hpp"
#include <algorithm>

namespace rebrewu::linker {

void ModuleGraph::add_module(const std::string& name) {
    if (!contains(name)) {
        ModuleNode node;
        node.name = name;
        m_nodes.push_back(std::move(node));
    }
}

void ModuleGraph::add_module(std::shared_ptr<rpl::RplModule> mod) {
    if (!mod) return;
    const std::string& name = mod->name;
    auto* existing = find(name);
    if (existing) {
        existing->module = std::move(mod);
    } else {
        ModuleNode node;
        node.name   = name;
        node.module = std::move(mod);
        m_nodes.push_back(std::move(node));
    }
}

void ModuleGraph::add_dependency(const std::string& importer,
                                  const std::string& exporter) {
    add_module(importer);
    add_module(exporter);

    auto* imp = find(importer);
    auto* exp = find(exporter);

    if (imp && std::find(imp->depends_on.begin(), imp->depends_on.end(),
                          exporter) == imp->depends_on.end()) {
        imp->depends_on.push_back(exporter);
    }
    if (exp && std::find(exp->depended_by.begin(), exp->depended_by.end(),
                          importer) == exp->depended_by.end()) {
        exp->depended_by.push_back(importer);
    }
}

ModuleNode* ModuleGraph::find(const std::string& name) noexcept {
    for (auto& n : m_nodes)
        if (n.name == name) return &n;
    return nullptr;
}

const ModuleNode* ModuleGraph::find(const std::string& name) const noexcept {
    for (const auto& n : m_nodes)
        if (n.name == name) return &n;
    return nullptr;
}

bool ModuleGraph::contains(const std::string& name) const noexcept {
    return find(name) != nullptr;
}

bool ModuleGraph::topo_visit(const std::string& name,
                              std::unordered_map<std::string,int>& state,
                              std::vector<std::string>& order) const {
    auto it = state.find(name);
    if (it != state.end()) {
        if (it->second == 1) return false; // cycle
        return true;
    }
    state[name] = 1; // visiting
    const auto* node = find(name);
    if (node) {
        for (const auto& dep : node->depends_on) {
            if (!topo_visit(dep, state, order))
                return false;
        }
    }
    state[name] = 2; // done
    order.push_back(name);
    return true;
}

std::vector<std::string> ModuleGraph::topo_sort() const {
    std::unordered_map<std::string,int> state;
    std::vector<std::string> order;
    for (const auto& n : m_nodes) {
        if (state.find(n.name) == state.end()) {
            if (!topo_visit(n.name, state, order))
                return {}; // cycle detected
        }
    }
    return order;
}

std::vector<std::string> ModuleGraph::dependents_of(const std::string& name) const {
    std::vector<std::string> result;
    const auto* node = find(name);
    if (!node) return result;
    // BFS over depended_by
    std::vector<std::string> queue = node->depended_by;
    std::unordered_map<std::string,bool> visited;
    visited[name] = true;
    while (!queue.empty()) {
        auto cur = queue.back(); queue.pop_back();
        if (visited[cur]) continue;
        visited[cur] = true;
        result.push_back(cur);
        const auto* n = find(cur);
        if (n) for (const auto& d : n->depended_by) queue.push_back(d);
    }
    return result;
}

std::vector<std::string> ModuleGraph::dependencies_of(const std::string& name) const {
    std::vector<std::string> result;
    const auto* node = find(name);
    if (!node) return result;
    std::vector<std::string> queue = node->depends_on;
    std::unordered_map<std::string,bool> visited;
    visited[name] = true;
    while (!queue.empty()) {
        auto cur = queue.back(); queue.pop_back();
        if (visited[cur]) continue;
        visited[cur] = true;
        result.push_back(cur);
        const auto* n = find(cur);
        if (n) for (const auto& d : n->depends_on) queue.push_back(d);
    }
    return result;
}

}