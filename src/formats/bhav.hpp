#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace os2 {

// BHAV: a SimAntics behaviour tree. Each instruction runs a primitive
// (opcode < 0x100) or calls another BHAV (global 0x100-0xFFF, private
// 0x1000-0x1FFF, semi-global 0x2000+) and branches on true/false.
//
// Header after the 64-byte name: u16 signature (0x8000-0x8009), u16 count,
// u8 tree type, u8 argument count, u8 local count, u8 flags, u32 tree version
// (0x8009 adds one byte after it, skipped).
// Instruction layout by signature:
//   < 0x8003: opcode u16, true u8, false u8, 8 operand bytes
//   0x8003-4: opcode u16, true u8, false u8, 16 operand bytes
//   0x8005-6: opcode u16, true u8, false u8, node version u8, 16 operand bytes
//   >= 0x8007: opcode u16, true u16, false u16, node version u8, 16 operand bytes
struct Behavior {
    enum class TargetKind : uint8_t { Instruction, True, False, Error };
    struct Target {
        TargetKind kind = TargetKind::Error;
        uint16_t index = 0; // for TargetKind::Instruction
    };
    struct Instruction {
        uint16_t opcode = 0;
        Target onTrue, onFalse;
        uint8_t nodeVersion = 0;
        std::array<uint8_t, 16> operands{};
    };

    std::string fileName;
    uint16_t signature = 0;
    uint8_t treeType = 0;
    uint8_t argCount = 0;
    uint8_t localCount = 0;
    uint8_t flags = 0;
    uint32_t treeVersion = 0;
    std::vector<Instruction> instructions;

    static Behavior parse(std::span<const uint8_t> data);

    // One line per instruction: index, opcode (primitive name or call), operands, branches.
    std::string disassemble() const;
};

// Name of a primitive opcode (< 0x100), e.g. "Expression"; "prim_0xNN" when unnamed.
std::string primitiveName(uint16_t opcode);

} // namespace os2
