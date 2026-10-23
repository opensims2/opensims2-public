// prim-usage: how the game's trees use one primitive (operand bytes), to work
// out operand layouts.
#include "prim_cmds.hpp"

#include "formats/bhav.hpp"
#include "formats/dbpf.hpp"
#include "formats/resindex.hpp"
#include "formats/strings.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace os2 {

namespace {

constexpr uint32_t kTypeBhav = 0x42484156;

// Every use of `opcode` in the BHAVs under `res`: a histogram of each
// operand byte, then up to `examples` uses (group, instance, tree name,
// node, operands). With `filter` ("byte=value", the byte 0-15 and the value
// 0-255), only the uses whose byte has that value are listed.
int cmdPrimUsage(const std::string& res, uint16_t opcode, size_t examples, int filterByte, int filterValue) {
    ResourceIndex index(res);
    std::array<std::map<uint8_t, size_t>, 16> bytes;
    size_t uses = 0, shown = 0;
    std::vector<std::string> lines;
    for (const auto& loc : index.ofType(kTypeBhav)) {
        Behavior b;
        try {
            b = Behavior::parse(index.read(loc));
        } catch (const std::exception&) {
            continue;
        }
        for (size_t i = 0; i < b.instructions.size(); ++i) {
            const auto& in = b.instructions[i];
            if (in.opcode != opcode)
                continue;
            ++uses;
            for (size_t k = 0; k < 16; ++k)
                ++bytes[k][in.operands[k]];
            if (filterByte >= 0 && in.operands[size_t(filterByte)] != uint8_t(filterValue))
                continue;
            if (shown++ < examples) {
                char buf[512];
                int n = std::snprintf(buf, sizeof buf, "%08X %04X #%-3zu v%u ", loc.entry->tgi.group,
                                      unsigned(loc.entry->tgi.instance), i, in.nodeVersion);
                for (uint8_t x : in.operands)
                    n += std::snprintf(buf + n, sizeof buf - size_t(n), "%02X ", x);
                std::snprintf(buf + n, sizeof buf - size_t(n), " %s", b.fileName.c_str());
                lines.emplace_back(buf);
            }
        }
    }
    std::printf("%s (0x%02X): %zu uses\n", primitiveName(opcode).c_str(), opcode, uses);
    for (size_t k = 0; k < 16; ++k) {
        std::vector<std::pair<size_t, uint8_t>> v;
        for (auto [x, n] : bytes[k])
            v.emplace_back(n, x);
        std::sort(v.rbegin(), v.rend());
        std::printf("  byte %2zu (%3zu values):", k, v.size());
        for (size_t j = 0; j < v.size(); ++j)
            std::printf(" %02X x%zu", v[j].second, v[j].first);
        std::printf("\n");
    }
    for (const auto& l : lines)
        std::printf("  %s\n", l.c_str());
    return 0;
}

// The event trees of Animate Sim/Overlay/Object (u16 at 4): which values
// they compare parameter 0 (the event) with, and which other parameters
// they read (Expression operands, owner 9), by primitive. Private trees
// (0x1000-0x1FFF) are looked up in the caller's group, globals (< 0x1000)
// in the global group; semi-globals are counted, not resolved.
int cmdEventTreeSweep(const std::string& res) {
    constexpr uint32_t kGlobalGroup = 0x7FD46CD0;
    ResourceIndex index(res);
    std::map<std::pair<uint32_t, uint16_t>, Behavior> trees;
    for (const auto& loc : index.ofType(kTypeBhav))
        try {
            trees.emplace(std::make_pair(loc.entry->tgi.group, uint16_t(loc.entry->tgi.instance)),
                          Behavior::parse(index.read(loc)));
        } catch (const std::exception&) {
        }
    struct Stats {
        std::set<std::pair<uint32_t, uint16_t>> trees;
        size_t semi = 0;
        std::map<std::string, size_t> compares, params;
        std::set<std::string> engine, readers;
    };
    std::map<uint16_t, Stats> by;
    for (const auto& [key, b] : trees)
        for (const auto& in : b.instructions) {
            if (in.opcode != 0x69 && in.opcode != 0x6A && in.opcode != 0x6B)
                continue;
            const uint16_t tree = uint16_t(in.operands[4] | in.operands[5] << 8);
            if (!tree)
                continue;
            Stats& st = by[in.opcode];
            if (tree >= 0x2000) {
                ++st.semi;
                continue;
            }
            const auto at = std::make_pair(tree < 0x1000 ? kGlobalGroup : key.first, tree);
            if (!st.trees.insert(at).second)
                continue;
            const auto it = trees.find(at);
            if (it == trees.end())
                continue;
            for (const auto& x : it->second.instructions) {
                if (x.opcode != 0x02)
                    continue;
                const auto& o = x.operands;
                const int16_t lhs = int16_t(o[0] | o[1] << 8), rhs = int16_t(o[2] | o[3] << 8);
                const uint8_t op = o[5], lhsOwner = o[6], rhsOwner = o[7];
                if (lhsOwner == 0x09)
                    ++st.params["param " + std::to_string(lhs)];
                if (rhsOwner == 0x09)
                    ++st.params["param " + std::to_string(rhs)];
                if (lhsOwner == 0x09 && lhs == 0 && op <= 5) {
                    char k[64];
                    std::snprintf(k, sizeof k, "op %u owner 0x%02X data %d", op, rhsOwner, rhs);
                    ++st.compares[k];
                    // The engine's events (9997-9999, or BCON 0x124 "G - Anim Events - Std")
                    if ((rhsOwner == 0x07 && rhs >= 9997 && rhs <= 9999) || (rhsOwner == 0x1A && (rhs >> 7) == 0xA4))
                        st.engine.insert(it->second.fileName);
                }
                if ((lhsOwner == 0x09 && lhs >= 1 && lhs <= 4) || (rhsOwner == 0x09 && rhs >= 1 && rhs <= 4))
                    st.readers.insert(it->second.fileName + " (param " +
                                      std::to_string(lhsOwner == 0x09 ? lhs : rhs) + ")");
            }
        }
    for (const auto& [opcode, st] : by) {
        std::printf("%s (0x%02X): %zu private/global event trees, %zu uses of semi-global ones\n",
                    primitiveName(opcode).c_str(), opcode, st.trees.size(), st.semi);
        std::vector<std::pair<size_t, std::string>> v;
        for (const auto& [k, n] : st.compares)
            v.emplace_back(n, k);
        std::sort(v.rbegin(), v.rend());
        for (size_t i = 0; i < v.size() && i < 16; ++i)
            std::printf("  param 0 compared: %-36s x%zu\n", v[i].second.c_str(), v[i].first);
        for (const auto& [k, n] : st.params)
            std::printf("  reads %s x%zu\n", k.c_str(), n);
        for (const auto& n : st.engine)
            std::printf("  engine events in: %s\n", n.c_str());
        for (const auto& n : st.readers)
            std::printf("  reads %s\n", n.c_str());
    }
    return 0;
}

} // namespace

int primCommand(const std::string& cmd, int argc, char** argv) {
    if (cmd == "event-tree-sweep" && argc == 3)
        return cmdEventTreeSweep(argv[2]);
    if (cmd == "prim-usage" && (argc == 4 || argc == 5 || argc == 6)) {
        int filterByte = -1, filterValue = -1;
        if (argc == 6) {
            // "byte=value"
            const std::string f = argv[5];
            const size_t eq = f.find('=');
            if (eq == std::string::npos)
                throw std::runtime_error("prim-usage: o filtro e byte=value");
            filterByte = std::stoi(f.substr(0, eq));
            filterValue = std::stoi(f.substr(eq + 1));
        }
        return cmdPrimUsage(argv[2], uint16_t(std::stoul(argv[3], nullptr, 16)),
                            argc >= 5 ? std::stoul(argv[4]) : 40, filterByte, filterValue);
    }
    return -1;
}

const char* primUsage() {
    return "       s2pkg prim-usage <Res-dir> <opcode-hex> [examples]\n"
           "       s2pkg event-tree-sweep <Res-dir>\n";
}

} // namespace os2
