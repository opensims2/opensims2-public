#include "simantics.hpp"

#include <bit>

#include "binreader.hpp"

namespace os2 {

namespace {

constexpr size_t kNameSize = 64;
constexpr uint32_t kMaxCount = 0x10000;

uint32_t count(BinReader& r, const char* what) {
    const uint32_t n = r.u32();
    if (n > kMaxCount)
        throw FormatError(std::string("bad ") + what + " count");
    return n;
}

void expectTag(BinReader& r, const char tag[5], const char* what) {
    const auto at = r.pos();
    const auto b = r.bytes(4);
    if (std::string(reinterpret_cast<const char*>(b.data()), 4) != tag)
        throw FormatError(std::string(what) + ": missing '" + tag + "' at offset " + std::to_string(at));
}

} // namespace

// ---------------------------------------------------------------- OBJD

ObjectDefinition ObjectDefinition::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    ObjectDefinition o;
    o.fileName = r.fixed(kNameSize);
    o.version = r.u32();
    // Fields run up to the title at the end of the resource: u32 length +
    // characters, or u8 length + characters in some objects. Find the split
    // whose length matches exactly and whose title is printable text.
    const size_t start = r.pos();
    auto printable = [&](size_t from) {
        for (size_t i = from; i < data.size(); ++i)
            if (data[i] < 0x20)
                return false;
        return true;
    };
    for (size_t prefix : {4u, 1u})
        for (size_t end = start; end + prefix <= data.size(); end += 2) {
            const size_t len = prefix == 4 ? le32(&data[end]) : data[end];
            if (end + prefix + len != data.size() || !printable(end + prefix))
                continue;
            o.fields.resize((end - start) / 2);
            for (auto& f : o.fields)
                f = r.u16();
            r.skip(prefix);
            o.title = std::string(reinterpret_cast<const char*>(&data[end + prefix]), len);
            return o;
        }
    throw FormatError("OBJD: cannot locate the title");
}

// ---------------------------------------------------------------- OBJf

ObjectFunctions ObjectFunctions::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    ObjectFunctions f;
    f.fileName = r.fixed(kNameSize);
    // Normally two u32 then the tag; a few resources have extra zero padding before it.
    const std::string_view bytes(reinterpret_cast<const char*>(data.data()), data.size());
    const size_t tag = bytes.find("fJBO", kNameSize);
    if (tag == std::string_view::npos || tag < kNameSize + 8)
        throw FormatError("OBJf: missing 'fJBO'");
    r.seek(tag - 8);
    f.unknown1 = r.u32();
    f.unknown2 = r.u32();
    r.skip(4);
    f.functions.resize(count(r, "OBJf function"));
    for (auto& e : f.functions) {
        e.guard = r.u16();
        e.action = r.u16();
    }
    r.expectEnd("OBJf");
    return f;
}

const char* ObjectFunctions::functionName(size_t i) {
    static const char* const names[] = {
        "init",          "main",          "load",          "cleanup",         "queueSkipped",
        "allowIntersect", "wallAdjacency", "roomChanged",  "dynMultiTile",    "placement",
        "pickup",        "userPlacement", "userPickup",    "levelInfo",       "servingSurface",
        "portal",        "gardening",     "washHands",     "prepareFood",     "cookFood",
        "placeOnSurface", "dispose",      "eat",           "pickupFromSlot",  "washDish",
        "eatingSurface", "sit",           "stand",
    };
    return i < std::size(names) ? names[i] : "";
}

// ---------------------------------------------------------------- BCON

Constants Constants::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    Constants c;
    c.fileName = r.fixed(kNameSize);
    // u16 count; bit 15 is a flag.
    const uint16_t header = r.u16();
    c.values.resize(header & 0x7FFF);
    c.flags = uint8_t(header >> 15);
    for (auto& v : c.values)
        v = r.i16();
    r.expectEndOrFill("BCON");
    return c;
}

// ---------------------------------------------------------------- FCNS

FloatConstants FloatConstants::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    FloatConstants c;
    c.fileName = r.fixed(kNameSize);
    r.u32(); // "SNCF"
    c.version = r.u32();
    r.u32();
    const uint32_t count = r.u32();
    if (count > kMaxCount)
        throw std::runtime_error("FCNS: bad count");
    for (uint32_t i = 0; i < count; ++i) {
        Entry e;
        e.name = r.str8();
        e.value = std::bit_cast<float>(r.u32());
        if (c.version >= 0x4B)
            e.description = r.str8();
        c.entries.push_back(std::move(e));
    }
    return c;
}

float FloatConstants::value(const std::string& name, float fallback) const {
    for (const auto& e : entries)
        if (e.name == name)
            return e.value;
    return fallback;
}

// ---------------------------------------------------------------- GLOB

SemiGlobal SemiGlobal::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    SemiGlobal g;
    g.fileName = r.fixed(kNameSize);
    // Pascal string, or a C string in some resources.
    if (!r.atEnd() && data[r.pos()] < 0x20)
        g.semiGlobalName = r.str8();
    else
        g.semiGlobalName = r.cstr();
    r.expectEndOrFill("GLOB");
    return g;
}

// ---------------------------------------------------------------- TPRP

BhavLabels BhavLabels::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    BhavLabels t;
    t.fileName = r.fixed(kNameSize);
    expectTag(r, "PRPT", "TPRP");
    t.version = r.u32();
    t.unknown[0] = r.u32();
    t.params.resize(count(r, "TPRP param"));
    t.locals.resize(count(r, "TPRP local"));
    for (auto& p : t.params)
        p = r.str8();
    for (auto& l : t.locals)
        l = r.str8();
    t.unknown[1] = r.u32();
    t.paramFlags.resize(t.params.size());
    for (auto& f : t.paramFlags)
        f = r.u8();
    t.unknown[2] = r.u32();
    r.u32();
    r.expectEnd("TPRP");
    return t;
}

// ---------------------------------------------------------------- SLOT

Slots Slots::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    Slots s;
    s.fileName = r.fixed(kNameSize);
    // cTSSlotLoader::Read (base 1.0 0xbc50c0; docs/routing-mac.md §9.5): three
    // u32, the second the version (1 to 0x47), then the count. The Sims 1
    // layout has the tag third ("unused, version, 'TOLS'"), the Sims 2's
    // first; the game reads both alike.
    s.sims2Layout = data.size() >= kNameSize + 4 && std::string(reinterpret_cast<const char*>(&data[kNameSize]), 4) == "TOLS";
    r.u32();
    s.version = r.u32();
    if (s.sims2Layout)
        r.u32();
    else
        expectTag(r, "TOLS", "SLOT");
    const uint32_t n = count(r, "slot");
    const uint32_t v = s.version;
    if (v < 1 || v > 0x47) // the loader refuses these
        throw FormatError("SLOT: version " + std::to_string(v) + " not supported");
    // (Versions 1 and 2 have a layout of their own, not followed: no SLOT of
    // the base game has them; read here as 3.)
    s.slots.resize(n);
    // cTSSlotDescriptor::Read (base 1.0 0xbc3f1c, BV 0x4b08a4), field by
    // field as the version has them.
    for (auto& sl : s.slots) {
        sl.type = r.u16();
        sl.offset = {r.f32(), r.f32(), 0};
        if (v < 0x47) // sixteenths of a tile before 0x47
            sl.offset[0] *= 0.0625f, sl.offset[1] *= 0.0625f;
        sl.offset[2] = r.f32();
        sl.standing = r.i32();
        sl.sitting = r.i32();
        sl.ground = r.i32();
        sl.flags = r.u32();
        if (v >= 4)
            sl.snapTargetSlot = r.i32();
        if (v >= 5) {
            if (v >= 0x43) {
                sl.minDistance = r.f32(), sl.maxDistance = r.f32(), sl.optimalDistance = r.f32();
                sl.minProximity = std::bit_cast<int32_t>(sl.minDistance);
                sl.maxProximity = std::bit_cast<int32_t>(sl.maxDistance);
                sl.optimalProximity = std::bit_cast<int32_t>(sl.optimalDistance);
            } else {
                sl.minProximity = r.i32(), sl.maxProximity = r.i32(), sl.optimalProximity = r.i32();
                const float scale = v >= 0xA ? 0.0625f : 1.0f; // whole tiles before 0xA
                sl.minDistance = float(sl.minProximity) * scale;
                sl.maxDistance = float(sl.maxProximity) * scale;
                sl.optimalDistance = float(sl.optimalProximity) * scale;
            }
            sl.maxSize = r.i32();
            sl.unknown10 = r.i32();
        }
        if (v >= 7)
            sl.gradient = r.f32();
        if (v >= 8)
            sl.height = r.i32();
        if (v >= 9) {
            sl.facing = r.i32();
        } else if (sl.flags & 0x200) {
            sl.facing = -3, sl.flags &= ~0x200u;
        } else if (sl.flags & 0x400) {
            sl.facing = -1, sl.flags &= ~0x400u;
        }
        if (v >= 0x47) {
            sl.resolutionTiles = r.f32();
            sl.resolution = std::bit_cast<int32_t>(sl.resolutionTiles);
        } else if (v >= 0xA) {
            sl.resolution = r.i32();
            sl.resolutionTiles = float(sl.resolution) * 0.0625f;
        } else {
            sl.gradient *= 0.0625f;
        }
        sl.ts2Extra = {v >= 0x40 ? r.u32() : 0u, v >= 0x41 ? r.u32() : 0xFFFFFFFFu};
    }
    r.expectEnd("SLOT");
    return s;
}

// ---------------------------------------------------------------- TTAB

InteractionTable InteractionTable::parse(std::span<const uint8_t> data) {
    BinReader r(data);
    InteractionTable t;
    t.fileName = r.fixed(kNameSize);
    if (r.atEnd())
        return t; // empty placeholder tables
    if (r.remaining() == 4) {
        // Empty table in the Sims 1 layout: u16 count (0), u16 version.
        if (r.u16() != 0)
            throw FormatError("TTAB: Sims 1 layout tables are not supported");
        t.version = r.u16();
        return t;
    }
    t.typeCode = r.i32();
    t.version = r.u32();
    // 0x4F..0x51 keep the 0x4E layout byte for byte: the 276 resources the
    // game refuses in the AL objects.package (149 + 106 + 21) all parse to the
    // trailing name with the same entry layout. 0x53 keeps the same layout too,
    // with 11 motive tables instead of 7: the reader selects 7 for 0x44..0x52
    // and 11 for 0x53 (FUN_0087c720 @ 0x87c880 in the PC binary) and the 7 AL
    // resources (all "untitled tree table") close to the trailing name with the
    // 11 counts, the fields and the advertisements in the 0x4E order
    // (docs/data-findings.md section 19). 0x52 never shows up in the UC content
    // and nothing above 0x55 does, so both stay refused. 0x54 and 0x55 changed
    // the entry again: 46 fixed bytes and then two arrays of advertisement
    // groups (docs/data-findings.md sections 17 and 18).
    const uint32_t v = t.version;
    if (v < 0x3F || v == 0x52 || v > 0x55)
        throw FormatError("TTAB: unsupported version " + hexString(t.version));
    t.unused = r.u32();
    t.interactions.resize(r.u16());
    if (v <= 0x53) {
        for (auto& it : t.interactions) {
            it.action = r.u16();
            it.guard = r.u16();
            // 0x53 is the 0x4E layout with 11 motive tables (the PC reader's
            // count: 7 up to 0x52, 11 at 0x53, none from 0x54 on).
            std::vector<uint32_t> counts(v < 0x44 ? 1 : v == 0x53 ? 11 : 7);
            for (auto& c : counts)
                c = count(r, "TTAB motive");
            it.flags = r.u16();
            it.flags2 = r.u16();
            it.stringIndex = r.u32();
            it.attenuationCode = r.u32();
            it.attenuationValue = r.f32();
            it.autonomy = r.u32();
            it.joinIndex = r.i32();
            if (v >= 0x45)
                it.uiDisplayType = r.u16();
            if (v >= 0x46)
                it.facialAnimation = r.u32();
            if (v >= 0x4A)
                it.memoryMultiplier = r.f32();
            if (v >= 0x4C) {
                it.objectType = r.u32();
                it.modelTableId = r.u32();
            }
            for (auto c : counts) {
                std::vector<Advertisement> table(c);
                for (auto& a : table) {
                    a.minimum = r.i16();
                    a.delta = r.i16();
                    a.personality = r.u16();
                }
                it.motives.push_back(std::move(table));
            }
        }
    } else {
        // 0x54+: the entry has no motive counts in its fixed part; the
        // advertisements come in two arrays of groups instead. The first group
        // is a count and the advertisements; the second one carries a count of
        // 4-byte items (whose content is still unknown) before its own count
        // and advertisements. Both arrays are read as one motive table per
        // group, in file order.
        for (auto& it : t.interactions) {
            it.action = r.u16();
            it.guard = r.u16();
            it.flags = r.u16();
            it.flags2 = r.u16();
            it.stringIndex = r.u32();
            it.attenuationCode = r.u32();
            it.attenuationValue = r.f32();
            it.autonomy = r.u32();
            it.joinIndex = r.i32();
            it.uiDisplayType = r.u16();
            it.facialAnimation = r.u32();
            it.memoryMultiplier = r.f32();
            it.objectType = r.u32();
            it.modelTableId = r.u32();
            for (int array = 0; array < 2; ++array) {
                const uint32_t groups = count(r, "TTAB group");
                for (uint32_t g = 0; g < groups; ++g) {
                    if (array == 1) {
                        const uint32_t items = count(r, "TTAB item");
                        for (uint32_t i = 1; i < items; ++i)
                            r.bytes(4); // unknown 4-byte item, skipped
                    }
                    std::vector<Advertisement> table(count(r, "TTAB advertisement"));
                    for (auto& a : table) {
                        a.minimum = r.i16();
                        a.delta = r.i16();
                        a.personality = r.u16();
                    }
                    it.motives.push_back(std::move(table));
                }
            }
        }
    }
    if (!r.atEnd()) {
        // Trailing table name: u8 or u32 length. One table ("Reaction
        // Interaction Table", group 0x7F9FC607, version 0x48) has 96 zero bytes
        // before it: 16 empty advertisements (6 bytes each) beyond what its
        // counts (16, 0, ...) announce, skipped here (docs/data-findings.md).
        for (size_t skip = 0; skip < r.remaining(); skip += 6) {
            const size_t at = r.pos() + skip, rest = r.remaining() - skip;
            if (skip && data[at - 6] | data[at - 5] | data[at - 4] | data[at - 3] | data[at - 2] | data[at - 1])
                break;
            const bool name8 = size_t(data[at]) + 1 == rest;
            const bool name32 = rest >= 4 && size_t(le32(&data[at])) + 4 == rest;
            if (!name8 && !name32)
                continue;
            r.bytes(skip);
            if (name8)
                t.name = r.str8();
            else
                t.name = std::string(reinterpret_cast<const char*>(r.bytes(rest).data() + 4), rest - 4);
            break;
        }
    }
    r.expectEnd("TTAB");
    return t;
}

} // namespace os2
