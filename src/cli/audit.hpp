#pragma once
#include "analysis/function_discovery.hpp"
#include "analysis/cfg_builder.hpp"
#include <filesystem>
struct AuditInventory {
    std::vector<rebrewu::analysis::FunctionDiscovery::FrameAudit> frames;
    std::vector<rebrewu::analysis::FunctionDiscovery::ThunkAudit> thunks;
    void frame(const rebrewu::analysis::FunctionDiscovery::FrameAudit& f) { frames.push_back(f); }
    void thunk(const rebrewu::analysis::FunctionDiscovery::ThunkAudit& f) { thunks.push_back(f); }
    bool write(const rebrewu::rpx::RpxModule&, const rebrewu::ir::IRModule&,
               const rebrewu::analysis::CFGBuilder&, const std::filesystem::path&) const;
};
