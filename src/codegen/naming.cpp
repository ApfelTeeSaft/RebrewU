#include "naming.hpp"
#include <cctype>
#include <cstdio>

namespace rebrewu::codegen {

NamingContext::NamingContext(std::string_view module_name)
    : m_module_prefix(module_name) {}

std::string NamingContext::sanitize(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            out += c;
        else
            out += '_';
    }
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0])))
        out = "_" + out;
    return out;
}

std::string NamingContext::function_name(uint32_t addr,
                                          std::string_view hint) const {
    auto it = m_overrides.find(addr);
    if (it != m_overrides.end()) return it->second;

    auto cit = m_cache.find(addr);
    if (cit != m_cache.end()) return cit->second;

    std::string name;
    if (!hint.empty()) {
        name = m_module_prefix + "_" + sanitize(hint);
    } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s_fn_%08X",
                      m_module_prefix.c_str(), addr);
        name = buf;
    }
    m_used_names.insert(name);
    m_cache[addr] = name;
    return name;
}

std::string NamingContext::block_label(uint32_t /*func_addr*/,
                                        uint32_t block_addr) const {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "L_%08X", block_addr);
    return buf;
}

std::string NamingContext::data_name(uint32_t addr,
                                      std::string_view hint) const {
    auto it = m_overrides.find(addr);
    if (it != m_overrides.end()) return it->second;

    if (!hint.empty()) return m_module_prefix + "_" + sanitize(hint);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s_data_%08X",
                  m_module_prefix.c_str(), addr);
    return buf;
}

void NamingContext::set_name(uint32_t addr, std::string name) {
    m_overrides[addr] = std::move(name);
}

bool NamingContext::is_name_used(std::string_view name) const {
    return m_used_names.find(std::string(name)) != m_used_names.end();
}

}