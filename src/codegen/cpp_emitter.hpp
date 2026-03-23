#pragma once
#include "naming.hpp"
#include "../ir/ir_module.hpp"
#include "../core/rpx/rpx_types.hpp"
#include "../core/linker/linker.hpp"
#include <ostream>
#include <filesystem>
#include <set>

namespace rebrewu::codegen {
  struct EmitConfig {
    bool emit_comments{true};           // include source-address comments
    bool emit_guest_addr_labels{true};  // label every guest instruction
    bool verbose{false};
    bool emit_data_sections{true};
    bool use_goto{true};               // use goto for block jumps (vs setjmp)
    uint32_t functions_per_file{500};  // 0 = all in one file
    std::string runtime_header{"rebrewu_runtime.h"};
  };

  class CppEmitter {
  public:
    CppEmitter(
      const ir::IRModule& module,
      const rpx::RpxModule& rpx_module,
      const linker::Linker& linker,
      NamingContext names,
      EmitConfig cfg = {}
    );

    // Emit entire module to a directory
    // Produces: <outdir>/<module_name>.cpp, <outdir>/<module_name>.h
    bool emit(const std::filesystem::path& outdir);

    // Emit a single function to a stream
    void emit_function(const ir::IRFunction& func, std::ostream& out);

    // Emit module header (declarations)
    void emit_header(std::ostream& out);

    // Emit module data (static arrays)
    void emit_data(std::ostream& out);

    std::string_view last_error() const { return m_last_error; }

  private:
    void emit_file_prologue(std::ostream& out);
    void emit_block(const ir::BasicBlock& block, const ir::IRFunction& func, std::ostream& out, int indent);
    void emit_instr(const ir::IRInstr& instr, std::ostream& out, int indent);
    std::string format_operand(const ir::IROperand& op) const;
    std::string format_vreg(const ir::VReg& vr) const;

    const ir::IRModule& m_ir;
    const rpx::RpxModule& m_rpx;
    const linker::Linker& m_linker;
    NamingContext m_names;
    EmitConfig m_cfg;
    std::string m_last_error;
    // Transient state valid only during emit_function()
    const ir::IRFunction* m_current_func{nullptr};
    std::set<uint32_t> m_fp_temps{};
  };
}
