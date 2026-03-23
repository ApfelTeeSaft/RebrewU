#include "cli_options.hpp"
#include "rebrewu/version.hpp"
#include <cstring>
#include <iostream>

namespace rebrewu::cli {

void print_usage(const char* prog) {
    std::cout <<
        "Usage: " << prog << " <command> [options]\n\n"
        "Commands:\n"
        "  recompile   Recompile an RPX to C++ (default)\n"
        "  inspect     Inspect binary structure\n"
        "  symbols     List symbols\n"
        "  sections    List sections\n"
        "  exports     List RPL exports\n"
        "  imports     List RPL imports\n"
        "  disasm      Disassemble a range\n"
        "  help        Show this help\n"
        "  version     Show version\n\n"
        "Options:\n"
        "  -o <dir>    Output directory (default: output)\n"
        "  -l <rpl>    Add RPL search path or file\n"
        "  -c <cfg>    Config file path\n"
        "  -v          Verbose output\n"
        "  --no-color  Disable coloured output\n";
}

void print_version() {
    std::cout << "RebrewU v" << rebrewu::VERSION_STRING << "\n";
}

std::optional<CliOptions> parse(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return {};
    }

    CliOptions opts;
    int i = 1;

    std::string_view cmd = argv[i];
    if (cmd == "recompile")        { opts.command = Command::Recompile; ++i; }
    else if (cmd == "inspect")     { opts.command = Command::Inspect;   ++i; }
    else if (cmd == "symbols")     { opts.command = Command::ListSymbols; ++i; }
    else if (cmd == "sections")    { opts.command = Command::ListSections; ++i; }
    else if (cmd == "exports")     { opts.command = Command::ListExports; ++i; }
    else if (cmd == "imports")     { opts.command = Command::ListImports; ++i; }
    else if (cmd == "disasm")      { opts.command = Command::Disassemble; ++i; }
    else if (cmd == "analyze")     { opts.command = Command::Analyze;   ++i; }
    else if (cmd == "help")        { print_usage(argv[0]); return {}; }
    else if (cmd == "version")     { print_version(); return {}; }
    // else treat first arg as RPX path with default Recompile command

    for (; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "--no-color") {
            opts.color = false;
        } else if (arg == "--no-codegen") {
            opts.no_codegen = true;
        } else if (arg == "-o" && i + 1 < argc) {
            opts.output_dir = argv[++i];
        } else if (arg == "-l" && i + 1 < argc) {
            opts.rpl_paths.emplace_back(argv[++i]);
        } else if (arg == "-c" && i + 1 < argc) {
            opts.config_path = argv[++i];
        } else if (arg.starts_with("-")) {
            std::cerr << "unknown option: " << arg << "\n";
            return {};
        } else {
            if (opts.rpx_path.empty())
                opts.rpx_path = arg;
        }
    }

    return opts;
}

}