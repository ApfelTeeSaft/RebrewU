# RebrewU

A production-grade Wii U static recompilation framework.

## Overview

RebrewU accepts Wii U primary executable binaries (RPX) and optional secondary
modules (RPL) and performs static recompilation into structured C++ output that
can be compiled and run on host platforms. Originally built with Splatoon in mind.

## Architecture

```
RPX/RPL → [Loader] → [ELF Parser] → [Relocation Processor] → [Linker]
                                                                  ↓
                     [PPC Decoder] → [IR Builder] → [Analysis] → [Codegen]
                                                                  ↓
                                                           C++ Output + Runtime
```

## Build

### Prerequisites

- CMake 3.22+
- C++20 compiler (GCC 11+, Clang 13+, MSVC 2022)
- zlib

### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Windows (Visual Studio 2022)

Open `RebrewU.sln` or use CMake preset:

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Usage

```bash
# Recompile an RPX
rebrewu recompile --rpx game.rpx --output ./out

# Inspect binary
rebrewu inspect --rpx game.rpx

# List sections
rebrewu sections --rpx game.rpx

# Dump relocations
rebrewu relocations --rpx game.rpx

# List exports
rebrewu exports --rpx game.rpx

# List imports
rebrewu imports --rpx game.rpx

# List symbols
rebrewu symbols --rpx game.rpx

# Disassemble
rebrewu disasm --rpx game.rpx --addr 0x02000000 --count 32

# Analyze functions
rebrewu analyze --rpx game.rpx
```

## Configuration

RebrewU supports JSON configuration files for symbol overrides, function
boundaries, jump table hints, and more:

```json
{
  "symbol_overrides": {
    "0x02000000": "my_function"
  },
  "function_boundaries": [
    { "start": "0x02000000", "end": "0x02000100" }
  ],
  "ignored_regions": [],
  "jump_table_hints": [],
  "relocation_overrides": []
}
```

## Module Structure

| Module | Description |
|--------|-------------|
| `rebrewu_core` | ELF/RPX/RPL parsing, relocations, linker |
| `rebrewu_ppc` | PowerPC instruction decoder |
| `rebrewu_ir` | Internal IR (basic blocks, CFG) |
| `rebrewu_analysis` | Function discovery, CFG, jump tables |
| `rebrewu_codegen` | C++ code emitter |
| `rebrewu_config` | Configuration loading |
| `rebrewu_diagnostics` | Structured diagnostics |
| `rebrewu_runtime` | Runtime support headers |