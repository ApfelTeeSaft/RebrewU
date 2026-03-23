#pragma once
#include <string>
#include <vector>
#include <cstdint>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// diagnostics.hpp — Lightweight diagnostic engine
//
// DiagEngine is an instance-based (not singleton) diagnostic sink that is
// passed by reference through the pipeline so tests and callers can inspect
// or override reported messages.
// ============================================================================

namespace rebrewu::diagnostics {

enum class Severity { Note, Warning, Error };

struct DiagEntry {
    Severity    severity{};
    std::string message{};
};

class DiagEngine {
public:
    explicit DiagEngine(bool stderr_echo = false) noexcept
        : m_stderr_echo(stderr_echo) {}

    void error(std::string msg);
    void warning(std::string msg);
    void note(std::string msg);

    bool     has_errors()     const noexcept { return m_error_count > 0; }
    uint32_t error_count()   const noexcept { return m_error_count; }
    uint32_t warning_count() const noexcept { return m_warning_count; }

    const std::vector<DiagEntry>& entries() const noexcept { return m_entries; }

    void clear() {
        m_entries.clear();
        m_error_count = 0;
        m_warning_count = 0;
    }

private:
    std::vector<DiagEntry> m_entries{};
    uint32_t m_error_count{0};
    uint32_t m_warning_count{0};
    bool     m_stderr_echo{false};
};

}