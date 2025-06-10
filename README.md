# RebrewU - WiiU PowerPC Static Recompiler

RebrewU is a static recompiler that converts WiiU RPX executables into C++ code, which can then be recompiled for any platform. This project is designed specifically for the WiiU's PowerPC architecture and handles the unique aspects of the WiiU system, including paired single floating-point instructions and RPX file format.

**DISCLAIMER:** This project only converts the game code to C++. It does not provide a runtime implementation. Making the game work requires implementing the necessary runtime environment, including WiiU system calls, hardware abstractions, and library functions.

## Features

- **RPX File Support**: Parses WiiU RPX executable format
- **PowerPC Architecture**: Full support for WiiU's PowerPC 750-based CPU
- **Paired Single Instructions**: Handles WiiU-specific paired single floating-point operations
- **Function Analysis**: Automatic detection and analysis of functions and control flow
- **Jump Table Detection**: Static analysis of switch statements and jump tables
- **Configurable Optimizations**: Various optimization levels for generated code
- **Cross-Platform**: Generates code that can be compiled on any platform

## Building

### Prerequisites

- CMake 3.20 or later
- Clang 18+ (recommended) or MSVC 2022
- Git (for submodules)

### Dependencies

- [fmt](https://github.com/fmtlib/fmt) - Formatting library
- [tomlplusplus](https://github.com/marzer/tomlplusplus) - TOML configuration parsing
- [xxHash](https://github.com/Cyan4973/xxHash) - Fast hashing
- zlib - Compression library (for RPX decompression)

### Build Instructions

#### Windows (Visual Studio)

1. Clone the repository:
```bash
git clone --recursive https://github.com/apfelteesaft/RebrewU.git
cd RebrewU
```

2. Open the solution in Visual Studio 2022:
```bash
RebrewU.sln
```

3. Build the solution (Ctrl+Shift+B)

#### Cross-Platform (CMake)

1. Clone the repository:
```bash
git clone --recursive https://github.com/apfelteesaft/RebrewU.git
cd RebrewU
```

2. Configure and build:
```bash
mkdir build
cd build
cmake ..
cmake --build . --config Release
```

## Usage

### RebrewAnalyse

RebrewAnalyse analyzes RPX files and detects jump tables and function boundaries:

```bash
RebrewAnalyse [input RPX file] [output jump table TOML file]
```

Example:
```bash
RebrewAnalyse game.rpx jump_tables.toml
```

### RebrewRecomp

RebrewRecomp performs the actual recompilation:

```bash
RebrewRecomp [config TOML file] [PPC context header file]
```

Example:
```bash
RebrewRecomp config.toml RebrewUtils/ppc_context.h
```

## Configuration

RebrewRecomp uses TOML configuration files. Here's an example configuration:

```toml
[main]
file_path = "game.rpx"
out_directory_path = "recompiled"
switch_table_file_path = "jump_tables.toml"

# Optimizations (use with caution)
skip_lr = false
skip_msr = false
ctr_as_local = false
xer_as_local = false
reserved_as_local = false
cr_as_local = false
non_argument_as_local = false
non_volatile_as_local = false

# System function addresses (game-specific)
# These need to be found through reverse engineering
restgprlr_14_address = 0x02000000
savegprlr_14_address = 0x02001000
# ... additional addresses

# Manual function definitions
[[functions]]
address = 0x02002000
size = 0x100

# Mid-assembly hooks for custom implementations
[[midasm_hook]]
name = "CustomHook"
address = 0x02003000
registers = ["r3", "r4"]
```

## Architecture Details

### PowerPC Features Supported

- **General Purpose Registers**: All 32 GPRs
- **Floating Point Registers**: All 32 FPRs with paired single support
- **Vector Registers**: Graphics Quantization Registers (GQR) for paired singles
- **Special Purpose Registers**: LR, CTR, XER, CR, MSR, FPSCR
- **Instruction Set**: Complete PowerPC instruction set including WiiU extensions

### WiiU-Specific Features

- **Paired Single Arithmetic**: ps_add, ps_sub, ps_mul, ps_div, ps_madd
- **Paired Single Load/Store**: psq_l, psq_st with quantization support
- **Memory Layout**: Handles WiiU's specific memory regions (MEM1, MEM2)
- **Endianness**: Proper big-endian to little-endian conversion

### Recompilation Process

1. **Analysis Phase**: 
   - Parse RPX file structure
   - Extract sections and symbols
   - Analyze function boundaries
   - Detect jump tables and control flow

2. **Translation Phase**:
   - Convert PowerPC instructions to C++ code
   - Handle register allocation and optimization
   - Generate function calls and branches
   - Apply optimizations

3. **Output Phase**:
   - Generate header files with configuration
   - Create C++ source files for each function
   - Generate function mapping tables
   - Output build system files

## Implementation Notes

### Instruction Translation

Instructions are translated directly without decompilation, meaning the output is not human-readable but maintains exact semantic equivalence. The CPU state is passed as an argument to every function, including all PowerPC registers.

### Memory Access

All memory loads and stores are marked volatile to prevent unsafe compiler optimizations. Endianness conversion is handled automatically:

```cpp
#define PPC_LOAD_U32(addr)  __builtin_bswap32(*(volatile uint32_t*)(base + (addr)))
#define PPC_STORE_U32(addr, val)  (*(volatile uint32_t*)(base + (addr)) = __builtin_bswap32(val))
```

### Paired Single Support

The recompiler handles WiiU's unique paired single instructions by using the floating-point registers' full 64-bit width to store two 32-bit floats:

```cpp
union PPCFPRegister {
    struct {
        float ps0;  // Primary single
        float ps1;  // Secondary single
    };
    double f64;
    uint64_t u64;
};
```

### Function Calls

Indirect function calls are resolved using a function mapping table generated during recompilation:

```cpp
#define PPC_CALL_INDIRECT_FUNC(addr) \
    do { \
        extern PPCFuncMapping PPCFuncMappings[]; \
        for (auto* mapping = PPCFuncMappings; mapping->func; mapping++) { \
            if (mapping->address == (addr)) { \
                mapping->func(ctx, base); \
                return; \
            } \
        } \
    } while(0)
```

## Optimizations

RebrewRecomp offers several optimization levels:

- **Register Optimization**: Convert frequently-used registers to local variables
- **Link Register Skipping**: Skip LR operations when exceptions aren't used
- **Condition Register Optimization**: Local storage for condition registers
- **Non-volatile Register Optimization**: Significant performance improvements

⚠️ **Warning**: Only enable optimizations after achieving a working recompilation.

## Limitations

- **No Runtime Provided**: Users must implement their own WiiU runtime environment
- **x86 Platform Focus**: Currently optimized for x86 platforms due to intrinsic usage
- **No Exception Support**: Exception handling is not currently implemented
- **Limited MMIO Support**: Memory-mapped I/O operations need custom implementation

## Contributing

1. Fork the repository
2. Create a feature branch: `git checkout -b feature-name`
3. Make your changes and test thoroughly
4. Submit a pull request with detailed description

## Acknowledgments

This project was heavily inspired by [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) and uses techniques and knowledge from the [Xenia](https://github.com/xenia-project/xenia) Xbox 360 emulator project.

## Support

For issues, questions, or contributions, please use the GitHub issue tracker.

**Important**: This tool is for educational and preservation purposes. Ensure you have legal rights to any RPX files you recompile.