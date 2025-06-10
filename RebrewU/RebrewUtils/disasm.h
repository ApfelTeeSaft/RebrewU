#pragma once

#include "wiiu_ppc.h"
#include <cstdint>
#include <string>

namespace ppc {

// Disassemble a PowerPC instruction
bool Disassemble(const uint32_t* data, uint32_t address, ppc_insn& insn);
bool Disassemble(const void* data, size_t offset, uint32_t address, ppc_insn& insn);

// Get instruction info
const ppc_opcode* GetOpcode(uint32_t instruction);
bool DecodeInstruction(uint32_t instruction, ppc_insn& insn);

// Instruction format helpers
void FormatInstruction(const ppc_insn& insn, char* buffer, size_t buffer_size);
std::string FormatOperand(uint32_t operand, int format);

// Register name helpers
const char* GetRegisterName(int reg, char reg_type = 'r');
const char* GetConditionName(int cr_field, int condition);
const char* GetSpecialRegisterName(int spr);

// Branch target calculation
uint32_t CalculateBranchTarget(uint32_t address, const ppc_insn& insn);
bool IsBranchInstruction(const ppc_insn& insn);
bool IsConditionalBranch(const ppc_insn& insn);
bool IsUnconditionalBranch(const ppc_insn& insn);

// Instruction classification
bool IsLoadInstruction(const ppc_insn& insn);
bool IsStoreInstruction(const ppc_insn& insn);
bool IsFloatingPointInstruction(const ppc_insn& insn);
bool IsPairedSingleInstruction(const ppc_insn& insn);
bool IsPrivilegedInstruction(const ppc_insn& insn);

// WiiU specific instruction helpers
bool IsGameCubeCompatInstruction(const ppc_insn& insn);
bool RequiresSpecialHandling(const ppc_insn& insn);

}