#pragma once
// Lua 5.0 pre-compiled chunks: the game's `ObjectScripts/*.objLua` (and the
// `ObjectScripts.package` resources) are Lua 5.0 bytecode, what the 0x7E (Run
// Script) runs (docs/lua-scripts.md). The format is Lua 5.0.3's
// lundump.c/lopcodes.h (MIT): the signature "\033Lua", the version 0x50, the
// sizes of the C types it was built with (the game's 32-bit build: int 4,
// size_t 4, Instruction 4, Number 8) and a function tree with the lines,
// locals, upvalues, constants, nested functions and, last, the code (5.0 puts
// the code after the protos, unlike 5.1).
#include <cstdint>
#include <string>
#include <vector>

namespace os2 {

struct LuaConstant {
    enum Kind : uint8_t { Nil = 0, Boolean = 1, Number = 3, String = 4 };
    Kind kind = Nil;
    double number = 0;   // Number
    std::string string;  // String
    bool boolean = false;
};

struct LuaProto {
    std::string source;
    int32_t lineDefined = 0;
    int nups = 0, numparams = 0, isVararg = 0, maxStack = 0;
    std::vector<int32_t> lines; // one per instruction
    struct Local {
        std::string name;
        int32_t start = 0, end = 0;
    };
    std::vector<Local> locals;
    std::vector<std::string> upvalueNames;
    std::vector<LuaConstant> constants;
    std::vector<LuaProto> protos; // nested functions (KPROTO)
    std::vector<uint32_t> code;   // 32-bit instructions
};

// Parses one chunk (a whole file). Throws FormatError on a bad header or body.
LuaProto parseLua50(const std::vector<uint8_t>& bytes);

// The opcode name ("MOVE", ...), or "?" past the 35 of Lua 5.0.
const char* lua50OpcodeName(uint32_t op);
// One instruction with its 5.0 fields: op 6 bits, C 9, B 9, A 8; Bx/sBx 18.
// With `constants`, an RK operand (>= 250, Lua 5.0's MAXSTACK) is shown as
// `Kst "NPC Type"` instead of the bare number.
std::string disassembleLua50(uint32_t instruction, const std::vector<LuaConstant>& constants = {});

// The whole chunk as text: each function with its constants and every
// instruction disassembled, indented by nesting (a nested function is a CLOSURE
// with its own prototype). `what` is a prefix line, e.g. the file name.
std::string describeLua50(const LuaProto& chunk, const std::string& what = {});

} // namespace os2
