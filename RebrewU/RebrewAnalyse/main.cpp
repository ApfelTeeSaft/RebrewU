#include <cassert>
#include <iterator>
#include <fstream>
#include <iostream>
#include "file.h"
#include "disasm.h"
#include "rpx_image.h"
#include "wiiu_ppc.h"
#include "function.h"

#define SWITCH_ABSOLUTE 0
#define SWITCH_COMPUTED 1
#define SWITCH_BYTEOFFSET 2
#define SWITCH_SHORTOFFSET 3

struct SwitchTable {
    std::vector<size_t> labels{};
    size_t base{};
    size_t defaultLabel{};
    uint32_t r{};
    uint32_t type{};
};

void ReadTable(RPXImage& image, SwitchTable& table) {
    uint32_t pOffset;
    ppc_insn insn;
    auto* code = (uint32_t*)image.Find(table.base);
    ppc::Disassemble(code, table.base, insn);
    pOffset = insn.operands[1] << 16;

    ppc::Disassemble(code + 1, table.base + 4, insn);
    pOffset += insn.operands[2];

    if (table.type == SWITCH_ABSOLUTE) {
        const auto* offsets = (be<uint32_t>*)image.Find(pOffset);
        for (size_t i = 0; i < table.labels.size(); i++) {
            table.labels[i] = offsets[i];
        }
    }
    else if (table.type == SWITCH_COMPUTED) {
        uint32_t base;
        uint32_t shift;
        const auto* offsets = (uint8_t*)image.Find(pOffset);

        ppc::Disassemble(code + 4, table.base + 0x10, insn);
        base = insn.operands[1] << 16;

        ppc::Disassemble(code + 5, table.base + 0x14, insn);
        base += insn.operands[2];

        ppc::Disassemble(code + 3, table.base + 0x0C, insn);
        shift = insn.operands[2];

        for (size_t i = 0; i < table.labels.size(); i++) {
            table.labels[i] = base + (offsets[i] << shift);
        }
    }
    else if (table.type == SWITCH_BYTEOFFSET || table.type == SWITCH_SHORTOFFSET) {
        if (table.type == SWITCH_BYTEOFFSET) {
            const auto* offsets = (uint8_t*)image.Find(pOffset);
            uint32_t base;

            ppc::Disassemble(code + 3, table.base + 0x0C, insn);
            base = insn.operands[1] << 16;

            ppc::Disassemble(code + 4, table.base + 0x10, insn);
            base += insn.operands[2];

            for (size_t i = 0; i < table.labels.size(); i++) {
                table.labels[i] = base + offsets[i];
            }
        }
        else if (table.type == SWITCH_SHORTOFFSET) {
            const auto* offsets = (be<uint16_t>*)image.Find(pOffset);
            uint32_t base;

            ppc::Disassemble(code + 4, table.base + 0x10, insn);
            base = insn.operands[1] << 16;

            ppc::Disassemble(code + 5, table.base + 0x14, insn);
            base += insn.operands[2];

            for (size_t i = 0; i < table.labels.size(); i++) {
                table.labels[i] = base + offsets[i];
            }
        }
    }
    else {
        assert(false);
    }
}

void ScanTable(const uint32_t* code, size_t base, SwitchTable& table) {
    ppc_insn insn;
    uint32_t cr{ (uint32_t)-1 };
    for (int i = 0; i < 32; i++) {
        ppc::Disassemble(&code[-i], base - (4 * i), insn);
        if (insn.opcode == nullptr) {
            continue;
        }

        if (cr == -1 && (insn.opcode->id == PPC_INST_BGT || insn.opcode->id == PPC_INST_BLE)) {
            cr = insn.operands[0];
            if (insn.opcode->operands[1] != 0) {
                table.defaultLabel = insn.operands[1];
            }
        }
        else if (cr != -1) {
            if (insn.opcode->id == PPC_INST_CMPLWI && insn.operands[0] == cr) {
                table.r = insn.operands[1];
                table.labels.resize(insn.operands[2] + 1);
                table.base = base;
                break;
            }
        }
    }
}

void* SearchMask(const void* source, const uint32_t* compare, size_t compareCount, size_t size) {
    assert(size % 4 == 0);
    uint32_t* src = (uint32_t*)source;
    size_t count = size / 4;
    ppc_insn insn;

    for (size_t i = 0; i < count; i++) {
        size_t c = 0;
        for (c = 0; c < compareCount; c++) {
            ppc::Disassemble(&src[i + c], 0, insn);
            if (insn.opcode == nullptr || insn.opcode->id != compare[c]) {
                break;
            }
        }

        if (c == compareCount) {
            return &src[i];
        }
    }

    return nullptr;
}

static std::string out;

template<class... Args>
static void println(const char* fmt, Args&&... args) {
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), fmt, args...);
    out += buffer;
    out += '\n';
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("Usage: RebrewAnalyse [input RPX file path] [output jump table TOML file path]\n");
        return EXIT_SUCCESS;
    }

    const auto file = LoadFile(argv[1]);
    if (file.empty()) {
        printf("ERROR: Could not load file %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    auto image = RPXImage::ParseImage(file.data(), file.size());
    if (image.data.empty()) {
        printf("ERROR: Could not parse RPX file\n");
        return EXIT_FAILURE;
    }

    auto printTable = [&](const SwitchTable& table) {
        println("[[switch]]");
        println("base = 0x%X", static_cast<uint32_t>(table.base));
        println("r = %u", table.r);
        println("default = 0x%X", static_cast<uint32_t>(table.defaultLabel));
        println("labels = [");
        for (const auto& label : table.labels) {
            println("    0x%X,", static_cast<uint32_t>(label));
        }
        println("]");
        println("");
    };

    std::vector<SwitchTable> switches{};
    println("# Generated by RebrewAnalyse for WiiU RPX");

    auto scanPattern = [&](uint32_t* pattern, size_t count, size_t type) {
        for (const auto& section : image.sections) {
            if (!(section.flags & SectionFlags_Code)) {
                continue;
            }

            size_t base = section.base;
            uint8_t* data = section.data;
            uint8_t* dataStart = section.data;
            uint8_t* dataEnd = section.data + section.size;
            
            while (data < dataEnd && data != nullptr) {
                data = (uint8_t*)SearchMask(data, pattern, count, dataEnd - data);

                if (data != nullptr) {
                    SwitchTable table{};
                    table.type = type;
                    ScanTable((uint32_t*)data, base + (data - dataStart), table);

                    if (table.base != 0) {
                        ReadTable(image, table);
                        printTable(table);
                        switches.emplace_back(std::move(table));
                    }

                    data += 4;
                }
            }
        }
    };

    // WiiU specific jump table patterns
    uint32_t absoluteSwitch[] = {
        PPC_INST_LIS,     // lis r11, table@ha
        PPC_INST_ADDI,    // addi r11, r11, table@l
        PPC_INST_RLWINM,  // slwi r0, r0, 2
        PPC_INST_LWZX,    // lwzx r0, r11, r0
        PPC_INST_MTCTR,   // mtctr r0
        PPC_INST_BCTR,    // bctr
    };

    uint32_t computedSwitch[] = {
        PPC_INST_LIS,     // lis r11, table@ha
        PPC_INST_ADDI,    // addi r11, r11, table@l
        PPC_INST_LBZX,    // lbzx r0, r11, r0
        PPC_INST_RLWINM,  // slwi r0, r0, 2
        PPC_INST_LIS,     // lis r11, base@ha
        PPC_INST_ADDI,    // addi r11, r11, base@l
        PPC_INST_ADD,     // add r0, r11, r0
        PPC_INST_MTCTR,   // mtctr r0
    };

    uint32_t offsetSwitch[] = {
        PPC_INST_LIS,     // lis r11, table@ha
        PPC_INST_ADDI,    // addi r11, r11, table@l
        PPC_INST_LBZX,    // lbzx r0, r11, r0
        PPC_INST_LIS,     // lis r11, base@ha
        PPC_INST_ADDI,    // addi r11, r11, base@l
        PPC_INST_ADD,     // add r0, r11, r0
        PPC_INST_MTCTR,   // mtctr r0
    };

    uint32_t wordOffsetSwitch[] = {
        PPC_INST_LIS,     // lis r11, table@ha
        PPC_INST_ADDI,    // addi r11, r11, table@l
        PPC_INST_RLWINM,  // slwi r0, r0, 1
        PPC_INST_LHZX,    // lhzx r0, r11, r0
        PPC_INST_LIS,     // lis r11, base@ha
        PPC_INST_ADDI,    // addi r11, r11, base@l
        PPC_INST_ADD,     // add r0, r11, r0
        PPC_INST_MTCTR,   // mtctr r0
    };

    println("# ---- ABSOLUTE JUMPTABLE ----");
    scanPattern(absoluteSwitch, std::size(absoluteSwitch), SWITCH_ABSOLUTE);

    println("# ---- COMPUTED JUMPTABLE ----");
    scanPattern(computedSwitch, std::size(computedSwitch), SWITCH_COMPUTED);

    println("# ---- OFFSETED JUMPTABLE ----");
    scanPattern(offsetSwitch, std::size(offsetSwitch), SWITCH_BYTEOFFSET);
    scanPattern(wordOffsetSwitch, std::size(wordOffsetSwitch), SWITCH_SHORTOFFSET);

    std::ofstream f(argv[2]);
    if (!f.is_open()) {
        printf("ERROR: Could not open output file %s\n", argv[2]);
        return EXIT_FAILURE;
    }
    
    f.write(out.data(), out.size());
    f.close();

    printf("Successfully analyzed RPX file and found %zu switch tables\n", switches.size());
    return EXIT_SUCCESS;
}