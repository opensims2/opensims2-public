#include "lua50.hpp"

#include "binreader.hpp"

#include <cstdio>
#include <cstring>

namespace os2 {

namespace {

// The chunk's own sizes (the header records them): the game's files were built
// with int 4, size_t 4, Instruction 4 and Number 8, so the string lengths are
// 4 bytes even on this 64-bit build.
struct Reader {
    const std::vector<uint8_t>& bytes;
    size_t at = 0;
    bool swap = false; // the chunk is big-endian (ours is not)

    void need(size_t n) {
        if (n > bytes.size() || at + n > bytes.size())
            throw FormatError("Lua 5.0 chunk: unexpected end at byte " + std::to_string(at));
    }
    uint8_t byte() {
        need(1);
        return bytes[at++];
    }
    uint32_t uint(int size) {
        need(size_t(size));
        uint32_t v = 0;
        for (int i = 0; i < size; ++i) {
            // The chunk is little-endian (the header's byte is 1): a big-endian
            // chunk (swap) has its first byte as the most significant.
            const int shift = swap ? 8 * (size - 1 - i) : 8 * i;
            v |= uint32_t(bytes[at++]) << shift;
        }
        return v;
    }
    int32_t sint(int size) { return int32_t(uint(size)); }
    double number(int size) {
        need(size_t(size));
        uint8_t raw[8] = {};
        for (int i = 0; i < size; ++i)
            raw[swap ? 7 - i : i] = bytes[at++];
        double d;
        std::memcpy(&d, raw, sizeof d);
        return d;
    }
    // A TString: the size (the chunk's size_t) then that many bytes, the last a
    // '\0' (Lua keeps the length; the port wants the text).
    std::string string(int sizeSize) {
        const uint32_t n = uint(sizeSize);
        if (n == 0)
            return {};
        need(n);
        std::string s(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + n));
        at += n;
        if (!s.empty() && s.back() == '\0')
            s.pop_back();
        return s;
    }
};

LuaProto loadFunction(Reader& r, int intSize, int sizeSize, int numberSize, const std::string& parentSource) {
    LuaProto f;
    f.source = r.string(sizeSize);
    if (f.source.empty())
        f.source = parentSource;
    f.lineDefined = r.sint(intSize);
    f.nups = r.byte();
    f.numparams = r.byte();
    f.isVararg = r.byte();
    f.maxStack = r.byte();
    const int32_t nlines = r.sint(intSize);
    if (nlines < 0)
        throw FormatError("Lua 5.0 chunk: negative line count");
    for (int32_t i = 0; i < nlines; ++i)
        f.lines.push_back(r.sint(intSize));
    const int32_t nlocals = r.sint(intSize);
    if (nlocals < 0)
        throw FormatError("Lua 5.0 chunk: negative local count");
    for (int32_t i = 0; i < nlocals; ++i) {
        LuaProto::Local l;
        l.name = r.string(sizeSize);
        l.start = r.sint(intSize);
        l.end = r.sint(intSize);
        f.locals.push_back(std::move(l));
    }
    const int32_t nup = r.sint(intSize);
    for (int32_t i = 0; i < nup; ++i)
        f.upvalueNames.push_back(r.string(sizeSize));
    const int32_t nk = r.sint(intSize);
    if (nk < 0)
        throw FormatError("Lua 5.0 chunk: negative constant count");
    for (int32_t i = 0; i < nk; ++i) {
        LuaConstant c;
        switch (const uint8_t tag = r.byte()) {
        case LuaConstant::Nil: c.kind = LuaConstant::Nil; break;
        case LuaConstant::Boolean: c.kind = LuaConstant::Boolean; c.boolean = true; break;
        case LuaConstant::Number: c.kind = LuaConstant::Number; c.number = r.number(numberSize); break;
        case LuaConstant::String: c.kind = LuaConstant::String; c.string = r.string(sizeSize); break;
        default: throw FormatError("Lua 5.0 chunk: constant type " + std::to_string(tag) + " at " + std::to_string(i));
        }
        f.constants.push_back(std::move(c));
    }
    const int32_t np = r.sint(intSize);
    if (np < 0)
        throw FormatError("Lua 5.0 chunk: negative function count");
    for (int32_t i = 0; i < np; ++i)
        f.protos.push_back(loadFunction(r, intSize, sizeSize, numberSize, f.source));
    const int32_t nc = r.sint(intSize); // the code is the last of a function in 5.0
    if (nc < 0)
        throw FormatError("Lua 5.0 chunk: negative code count");
    for (int32_t i = 0; i < nc; ++i)
        f.code.push_back(r.uint(4));
    return f;
}

} // namespace

LuaProto parseLua50(const std::vector<uint8_t>& bytes) {
    Reader r{bytes};
    const uint8_t signature[4] = {0x1B, 'L', 'u', 'a'};
    for (uint8_t c : signature)
        if (r.byte() != c)
            throw FormatError("not a Lua chunk (the signature is not \"\\033Lua\")");
    const uint8_t version = r.byte();
    if (version != 0x50)
        throw FormatError("Lua chunk version 0x" + hexString(version) + " (the game's are 0x50 = Lua 5.0)");
    r.swap = r.byte() != 1; // 1 = little-endian
    const int intSize = r.byte(), sizeSize = r.byte(), instructionSize = r.byte();
    const int opSize = r.byte(), aSize = r.byte(), bSize = r.byte(), cSize = r.byte();
    const int numberSize = r.byte();
    if (intSize != 4 || instructionSize != 4 || numberSize != 8 || opSize != 6 || aSize != 8 || bSize != 9 || cSize != 9)
        throw FormatError("Lua chunk with other sizes than the game's (int " + std::to_string(intSize) + ", Instruction " +
                          std::to_string(instructionSize) + ", Number " + std::to_string(numberSize) + ")");
    r.need(size_t(numberSize));
    r.at += size_t(numberSize); // the test number (TEST_NUMBER)
    return loadFunction(r, intSize, sizeSize, numberSize, {});
}

const char* lua50OpcodeName(uint32_t op) {
    static const char* const names[] = {
        "MOVE",    "LOADK",    "LOADBOOL", "LOADNIL",  "GETUPVAL", "GETGLOBAL", "GETTABLE",  "SETGLOBAL",
        "SETUPVAL", "SETTABLE", "NEWTABLE", "SELF",     "ADD",      "SUB",       "MUL",       "DIV",
        "POW",     "UNM",      "NOT",      "CONCAT",   "JMP",      "EQ",        "LT",        "LE",
        "TEST",    "CALL",     "TAILCALL", "RETURN",   "FORLOOP",  "TFORLOOP",  "TFORPREP",  "SETLIST",
        "SETLISTO", "CLOSE",   "CLOSURE"};
    return op < sizeof(names) / sizeof(*names) ? names[op] : "?";
}

std::string disassembleLua50(uint32_t i, const std::vector<LuaConstant>& constants) {
    const uint32_t op = i & 0x3F;
    const int a = int((i >> 24) & 0xFF), b = int((i >> 15) & 0x1FF), c = int((i >> 6) & 0x1FF);
    const int bx = int((i >> 6) & 0x3FFFF), sbx = bx - 131071;
    // Lua 5.0's RK: below MAXSTACK (250) a register, from it a constant.
    auto rk = [&](int x) -> std::string {
        if (x < 250)
            return "R" + std::to_string(x);
        const size_t k = size_t(x - 250);
        if (k >= constants.size())
            return "Kst" + std::to_string(k);
        const auto& v = constants[k];
        switch (v.kind) {
        case LuaConstant::String: return "Kst \"" + v.string + "\"";
        case LuaConstant::Number: {
            char buf[40];
            std::snprintf(buf, sizeof buf, "Kst %g", v.number);
            return buf;
        }
        case LuaConstant::Boolean: return v.boolean ? "Kst true" : "Kst false";
        case LuaConstant::Nil: return "Kst nil";
        }
        return "Kst" + std::to_string(k);
    };
    std::string out = lua50OpcodeName(op);
    // LOADK/GETGLOBAL/SETGLOBAL take a constant index: show what it holds.
    auto kst = [&](int index) -> std::string {
        if (index < 0 || size_t(index) >= constants.size())
            return {};
        const auto& v = constants[size_t(index)];
        switch (v.kind) {
        case LuaConstant::String: return " \"" + v.string + "\"";
        case LuaConstant::Number: {
            char buf[40];
            std::snprintf(buf, sizeof buf, " %g", v.number);
            return buf;
        }
        case LuaConstant::Boolean: return v.boolean ? " true" : " false";
        case LuaConstant::Nil: return " nil";
        }
        return {};
    };
    switch (op) {
    case 1:  // LOADK
    case 5:  // GETGLOBAL
    case 7:  // SETGLOBAL
        return out + " R" + std::to_string(a) + " " + std::to_string(bx) + kst(bx);
    case 31: // SETLIST
    case 32: // SETLISTO
    case 34: // CLOSURE
        return out + " R" + std::to_string(a) + " " + std::to_string(bx);
    case 20: // JMP
        return out + " " + std::to_string(sbx);
    case 28: // FORLOOP
    case 30: // TFORPREP
        return out + " R" + std::to_string(a) + " " + std::to_string(sbx);
    case 33: // CLOSE
        return out + " R" + std::to_string(a);
    case 27: // RETURN: A B
        return out + " R" + std::to_string(a) + " " + std::to_string(b);
    case 29: // TFORLOOP: A C
        return out + " R" + std::to_string(a) + " " + std::to_string(c);
    case 6:  // GETTABLE: A B C, C is RK
        return out + " R" + std::to_string(a) + " R" + std::to_string(b) + " " + rk(c);
    case 9:  // SETTABLE: A B C, B and C are RK
    case 21: // EQ
    case 22: // LT
    case 23: // LE
        return out + " " + std::to_string(a) + " " + rk(b) + " " + rk(c);
    case 11: // SELF: A B C, C is RK
        return out + " R" + std::to_string(a) + " R" + std::to_string(b) + " " + rk(c);
    case 12: // ADD
    case 13: // SUB
    case 14: // MUL
    case 15: // DIV
    case 16: // POW
        return out + " R" + std::to_string(a) + " " + rk(b) + " " + rk(c);
    case 24: // TEST: A B C
        return out + " " + std::to_string(a) + " R" + std::to_string(b) + " " + std::to_string(c);
    default:
        return out + " " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c);
    }
}

} // namespace os2

namespace {

std::string constantText(const os2::LuaConstant& c) {
    char buf[64];
    switch (c.kind) {
    case os2::LuaConstant::Nil: return "nil";
    case os2::LuaConstant::Boolean: return c.boolean ? "true" : "false";
    case os2::LuaConstant::Number:
        std::snprintf(buf, sizeof buf, "%.14g", c.number);
        return buf;
    case os2::LuaConstant::String: return "\"" + c.string + "\"";
    }
    return "?";
}

void describeProto(const os2::LuaProto& f, int depth, std::string& out) {
    const std::string pad(size_t(depth) * 2, ' ');
    char head[512];
    std::snprintf(head, sizeof head, "%sfunction %s (defined at line %d): %d params, %d upvalues, %d stack, %zu instructions\n",
                  pad.c_str(), f.source.empty() ? "?" : f.source.c_str(), f.lineDefined, f.numparams, f.nups, f.maxStack,
                  f.code.size());
    out += head;
    if (!f.constants.empty()) {
        out += pad + "  constants:";
        for (const auto& c : f.constants)
            out += " " + constantText(c);
        out += "\n";
    }
    for (size_t i = 0; i < f.code.size(); ++i) {
        const int line = i < f.lines.size() ? f.lines[i] : 0;
        std::snprintf(head, sizeof head, "%s  %3zu  %-34s ; line %d\n", pad.c_str(), i,
                      os2::disassembleLua50(f.code[i], f.constants).c_str(), line);
        out += head;
    }
    for (const auto& p : f.protos)
        describeProto(p, depth + 1, out);
}

} // namespace

namespace os2 {

std::string describeLua50(const LuaProto& chunk, const std::string& what) {
    std::string out;
    if (!what.empty())
        out += what + "\n";
    describeProto(chunk, 0, out);
    return out;
}

} // namespace os2
