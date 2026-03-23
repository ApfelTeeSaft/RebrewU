#include "config.hpp"
#include <nlohmann/json.hpp>
#include <fstream>

namespace rebrewu::config {

std::optional<RebrewConfig>
ConfigLoader::load_json(const std::filesystem::path& path, std::string& error) {
    std::ifstream f(path);
    if (!f) { error = "cannot open " + path.string(); return {}; }
    try {
        nlohmann::json j;
        f >> j;
        RebrewConfig cfg;
        if (j.contains("output_dir"))
            cfg.output_dir = j["output_dir"].get<std::string>();
        if (j.contains("verbose"))
            cfg.verbose = j["verbose"].get<bool>();
        if (j.contains("emit_comments"))
            cfg.emit_comments = j["emit_comments"].get<bool>();
        if (j.contains("function_prefix"))
            cfg.function_prefix = j["function_prefix"].get<std::string>();
        if (j.contains("data_prefix"))
            cfg.data_prefix = j["data_prefix"].get<std::string>();
        if (j.contains("rpl_paths"))
            for (const auto& p : j["rpl_paths"])
                cfg.rpl_paths.push_back(p.get<std::string>());
        if (j.contains("symbols")) {
            for (const auto& s : j["symbols"]) {
                SymbolOverride ov;
                ov.name    = s.value("name", "");
                ov.address = s.value("address", 0u);
                if (s.contains("size")) ov.size = s["size"].get<uint32_t>();
                ov.force_function = s.value("force_function", false);
                ov.ignore = s.value("ignore", false);
                cfg.symbols.push_back(std::move(ov));
            }
        }
        return cfg;
    } catch (const std::exception& e) {
        error = std::string("JSON parse error: ") + e.what();
        return {};
    }
}

std::optional<RebrewConfig>
ConfigLoader::load_toml(const std::filesystem::path& path, std::string& error) {
    // Minimal TOML support not yet implemented — delegate to JSON error
    // Prolly won't implement it due to me just directly editing the recomp anyway
    error = "TOML config loading not yet implemented for " + path.string();
    return {};
}

std::optional<RebrewConfig>
ConfigLoader::load(const std::filesystem::path& path, std::string& error) {
    if (path.extension() == ".toml")
        return load_toml(path, error);
    return load_json(path, error);
}

bool ConfigLoader::write_default(const std::filesystem::path& path,
                                  std::string& error) {
    std::ofstream f(path);
    if (!f) { error = "cannot write " + path.string(); return false; }
    nlohmann::json j;
    j["output_dir"]     = "output";
    j["verbose"]        = false;
    j["emit_comments"]  = true;
    j["function_prefix"]= "func_";
    j["data_prefix"]    = "data_";
    j["rpl_paths"]      = nlohmann::json::array();
    j["symbols"]        = nlohmann::json::array();
    f << j.dump(2) << "\n";
    return true;
}

}