#include "pch.h"
#include "recompiler.h"
#include "test_recompiler.h"

int main(int argc, char* argv[]) {
#ifndef REBREW_RECOMP_CONFIG_FILE_PATH
    if (argc < 3) {
        printf("Usage: RebrewRecomp [input TOML file path] [PPC context header file path]\n");
        printf("       RebrewRecomp [test directory path] [output directory path]\n");
        return EXIT_SUCCESS;
    }
#endif

    const char* path = 
#ifdef REBREW_RECOMP_CONFIG_FILE_PATH
        REBREW_RECOMP_CONFIG_FILE_PATH
#else
        argv[1]
#endif
        ;

    if (std::filesystem::is_regular_file(path)) {
        // Regular recompilation mode
        Recompiler recompiler;
        if (!recompiler.LoadConfig(path)) {
            printf("ERROR: Failed to load configuration file\n");
            return -1;
        }

        printf("Analyzing RPX file...\n");
        recompiler.Analyse();

        // Set entry point symbol name
        auto entry = recompiler.image.symbols.find(recompiler.image.entry_point);
        if (entry != recompiler.image.symbols.end()) {
            const_cast<Symbol&>(*entry).name = "_start";
        }

        const char* headerFilePath =
#ifdef REBREW_RECOMP_HEADER_FILE_PATH
            REBREW_RECOMP_HEADER_FILE_PATH
#else
            argv[2]
#endif
            ;

        printf("Starting recompilation...\n");
        recompiler.Recompile(headerFilePath);
        printf("Recompilation completed successfully!\n");
    }
    else if (argc >= 3) {
        printf("Running test recompilation...\n");
        TestRecompiler::RecompileTests(argv[1], argv[2]);
        printf("Test recompilation completed!\n");
    }
    else {
        printf("ERROR: Invalid arguments or file not found\n");
        return -1;
    }

    return EXIT_SUCCESS;
}