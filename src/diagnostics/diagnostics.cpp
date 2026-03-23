#include "diagnostics.hpp"
#include <iostream>

namespace rebrewu::diagnostics {

void DiagEngine::error(std::string msg) {
    ++m_error_count;
    m_entries.push_back({Severity::Error, std::move(msg)});
    if (m_stderr_echo)
        std::cerr << "error: " << m_entries.back().message << "\n";
}

void DiagEngine::warning(std::string msg) {
    ++m_warning_count;
    m_entries.push_back({Severity::Warning, std::move(msg)});
    if (m_stderr_echo)
        std::cerr << "warning: " << m_entries.back().message << "\n";
}

void DiagEngine::note(std::string msg) {
    m_entries.push_back({Severity::Note, std::move(msg)});
    if (m_stderr_echo)
        std::cerr << "note: " << m_entries.back().message << "\n";
}

}