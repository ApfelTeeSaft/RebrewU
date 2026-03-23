#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <unordered_map>

// ============================================================================
// RebrewU — Wii U static recompilation framework
// ir_types.hpp — Core IR value/instruction/operand types
//
// The IR is a simple 3-address SSA-style representation.  Each IRInstr
// produces at most one result (a VReg) and consumes zero or more IROperands.
// Guest register names (GPR0..GPR31, FPR0..FPR31, CR0..CR7, LR, CTR, XER)
// are preserved as-is so that the C++ emitter can map them to host variables.
// ============================================================================

namespace rebrewu::ir {

// ============================================================================
// VReg — virtual register
// ============================================================================

enum class RegKind : uint8_t {
    GPR,   // general purpose r0-r31
    FPR,   // floating point f0-f31
    CR,    // condition register field cr0-cr7
    LR,    // link register
    CTR,   // count register
    XER,   // fixed-point exception register
    Temp,  // synthetic temporaries introduced by the recompiler
};

struct VReg {
    RegKind kind{RegKind::Temp};
    uint32_t index{0};  // register number (or temp id)

    bool operator==(const VReg&) const noexcept = default;

    static constexpr VReg gpr(uint32_t n) noexcept { return {RegKind::GPR, n}; }
    static constexpr VReg fpr(uint32_t n) noexcept { return {RegKind::FPR, n}; }
    static constexpr VReg cr (uint32_t n) noexcept { return {RegKind::CR,  n}; }
    static constexpr VReg lr ()           noexcept { return {RegKind::LR,  0}; }
    static constexpr VReg ctr()           noexcept { return {RegKind::CTR, 0}; }
    static constexpr VReg xer()           noexcept { return {RegKind::XER, 0}; }
    static constexpr VReg temp(uint32_t n)noexcept { return {RegKind::Temp,n}; }
};

// ============================================================================
// IROperand — an instruction operand (register, immediate, or block label)
// ============================================================================

struct ImmOp  { uint64_t value; };
struct RegOp  { VReg     reg;   };
struct LabelOp{ uint32_t block_id; };  // branch target block

using IROperand = std::variant<ImmOp, RegOp, LabelOp>;

// Helpers
inline IROperand imm(uint64_t v)      { return ImmOp{v};       }
inline IROperand reg(VReg r)          { return RegOp{r};        }
inline IROperand label(uint32_t bid)  { return LabelOp{bid};    }

// ============================================================================
// Opcode
// ============================================================================

enum class Opcode : uint16_t {
    // Control flow
    Jump,        // unconditional branch to block
    Branch,      // conditional branch (cond, true_block, false_block)
    IndirectJump,// bctr — indirect branch through CTR
    Call,        // bl — direct call (addr, [ret_reg])
    IndirectCall,// bctrl — indirect call through CTR
    Return,      // blr
    ConditionalReturn, // bclr — return to LR if condition, else fall through (cond, fallthrough_addr)

    // Integer arithmetic
    Add,
    Sub,
    Mul,
    MulHigh,
    MulHighU,
    Div,
    DivU,
    Neg,
    Abs,

    // Bitwise
    And,
    Or,
    Xor,
    Not,
    Shl,
    Shr,
    Sar,
    RotLeft,
    RotRight,
    CountLeadingZeros,
    CountLeadingOnes,
    PopCount,
    ExtractBits,  // rlwinm / rlwimi style

    // Comparison
    CmpSigned,
    CmpUnsigned,
    CmpFloat,

    // Memory
    Load8,
    Load8S,
    Load16,
    Load16S,
    Load32,
    Load64,
    LoadFloat32,
    LoadFloat64,
    Store8,
    Store16,
    Store32,
    Store64,
    StoreFloat32,
    StoreFloat64,

    // Register transfer
    Move,
    ZeroExtend,
    SignExtend,

    // Floating point
    FAdd,
    FSub,
    FMul,
    FDiv,
    FNeg,
    FAbs,
    FSqrt,
    FRound,
    FMadd,
    FMsub,
    FNmadd,
    FNmsub,
    FCvtToInt,
    FCvtFromInt,
    FCvtPrecision,

    // Misc
    Nop,
    Phi,           // SSA φ-node
    Undefined,     // unreachable / undefined behaviour
};

// ============================================================================
// IRInstr — a single IR instruction
// ============================================================================

struct IRInstr {
    Opcode             opcode{Opcode::Nop};
    std::optional<VReg>result{};         // destination VReg (empty if void)
    std::vector<IROperand> operands{};   // source operands
    uint32_t           guest_addr{0};    // originating guest PC (0 = synthetic)

    // Convenience ctor
    IRInstr(Opcode op, std::optional<VReg> dst, std::vector<IROperand> ops,
            uint32_t addr = 0)
        : opcode(op), result(dst), operands(std::move(ops)), guest_addr(addr) {}

    IRInstr() = default;
};

// ============================================================================
// BasicBlock — a maximal straight-line sequence of IRInstr
// ============================================================================

struct BasicBlock {
    uint32_t id{0};          // unique block id within a function
    uint32_t guest_start{0}; // guest address of first instruction
    uint32_t guest_end{0};   // guest address past last instruction (exclusive)

    std::vector<IRInstr> instrs{};

    // Successor block IDs (0-2 successors for branch, 0 for return)
    std::vector<uint32_t> successors{};
    // Predecessor block IDs
    std::vector<uint32_t> predecessors{};

    bool is_entry{false};
    bool is_exit {false};

    void append(IRInstr instr) { instrs.push_back(std::move(instr)); }
};

}