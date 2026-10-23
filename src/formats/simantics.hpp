#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace os2 {

// SimAntics object resources. All start with a 64-byte, NUL-padded file name.

// OBJD: object definition. The body is an array of 16-bit fields (layout
// inherited from The Sims 1) followed by the object's title. The game's
// editor labels them (objects.package, STR# 0x7FE59FD0/0xCC) counting from
// the version: label i is the field at file offset 0x40 + 2i.
struct ObjectDefinition {
    std::string fileName;
    uint32_t version = 0;
    std::vector<uint16_t> fields; // starting at file offset 0x44
    std::string title;

    static ObjectDefinition parse(std::span<const uint8_t> data);

    // 16-bit field at a file offset (>= 0x44), 0 if absent
    uint16_t field(size_t fileOffset) const {
        if (fileOffset < 0x44 || (fileOffset - 0x44) / 2 >= fields.size())
            return 0;
        return fields[(fileOffset - 0x44) / 2];
    }
    uint32_t field32(size_t fileOffset) const { return field(fileOffset) | uint32_t(field(fileOffset + 2)) << 16; }

    uint16_t initialStackSize() const { return field(0x44); }
    uint16_t baseGraphic() const { return field(0x46); }
    uint16_t numGraphics() const { return field(0x48); }
    uint16_t mainBhav() const { return field(0x4A); }
    uint16_t interactionGroup() const { return field(0x50); }
    uint16_t objectType() const { return field(0x52); }
    uint16_t masterId() const { return field(0x54); }
    uint16_t subIndex() const { return field(0x56); }
    uint32_t guid() const { return field32(0x5C); }
    uint16_t price() const { return field(0x64); }
    uint16_t slotId() const { return field(0x68); }
    // The grid-aligned / diagonal pair of a door, window or curtain (labels
    // 21-24, OpenTS2's DiagonalSelectorGUID / GridAlignedSelectorGUID; the
    // binary reads them as properties 0xCE / 0xCF): the diagonal one has no
    // diagonal selector (docs/buy-build-mac.md §7.6.1).
    uint32_t diagonalSelector() const { return field32(0x6A); }
    uint32_t gridAlignedSelector() const { return field32(0x6E); }
    uint32_t proxyGuid() const { return field32(0x7A); }
    uint16_t numAttributes() const { return field(0xB4); } // 0 in most objects that have attributes
    uint32_t originalGuid() const { return field32(0xCC); }
    // 0-based index into the group's "3D Object Names" STR# (instance 0x85).
    // Inferred from the data: for 97% of the objects whose name matches one of
    // the model names, this field selects that entry.
    uint16_t modelIndex() const { return field(0xD0); }
};

// OBJf: object functions, i.e. the (guard, action) BHAV pair for each engine
// event (init, main, load, cleanup, ...), indexed by function number.
struct ObjectFunctions {
    struct Entry {
        uint16_t guard = 0;
        uint16_t action = 0;
    };
    std::string fileName;
    uint32_t unknown1 = 0, unknown2 = 0;
    std::vector<Entry> functions;

    static ObjectFunctions parse(std::span<const uint8_t> data);
    static const char* functionName(size_t index); // "init", "main", ... or ""
};

// BCON: constants used by BHAV operands.
struct Constants {
    std::string fileName;
    uint8_t flags = 0; // bit 15 of the stored count
    std::vector<int16_t> values;

    static Constants parse(std::span<const uint8_t> data);
};

// FCNS: named float constants (cTSFloatConstants): 64-byte name, "SNCF",
// u32 version, u32 0, u32 count, then per entry a Pascal name, a float and
// (version >= 0x4B, inferred from the data) a Pascal description.
struct FloatConstants {
    struct Entry {
        std::string name;
        float value = 0;
        std::string description;
    };
    std::string fileName;
    uint32_t version = 0;
    std::vector<Entry> entries;

    static FloatConstants parse(std::span<const uint8_t> data);
    // The value of that name, or `fallback`.
    float value(const std::string& name, float fallback) const;
};

// GLOB: name of the semi-global group an object's BHAVs may call into.
struct SemiGlobal {
    std::string fileName;
    std::string semiGlobalName;

    static SemiGlobal parse(std::span<const uint8_t> data);
};

// TPRP: parameter and local variable labels of a BHAV (same instance).
struct BhavLabels {
    std::string fileName;
    uint32_t version = 0;
    std::vector<std::string> params;
    std::vector<std::string> locals;
    std::vector<uint8_t> paramFlags;
    std::array<uint32_t, 3> unknown{};

    static BhavLabels parse(std::span<const uint8_t> data);
};

// SLOT: routing/containment slots. Sims 1 layout (versions 4-10) or Sims 2
// layout (versions 0x43-0x47, 8 extra bytes per slot).
struct Slots {
    struct Slot {
        // 0 containment, 3 routing, 4 target (the Look At's byte 14 kinds;
        // view.cpp). 1 is the Sims 1 layout's "location"; measured, of the
        // 4.121 distinct SLOTs of the Ultimate Collection only four objects
        // carry a 1 (the carried Baby Bottle, Smart Milk, Paper Airplane and
        // Newspaper, Sims 2-format files too) and none carries a 2.
        uint16_t type = 0;
        std::array<float, 3> offset{};
        int32_t standing = 0, sitting = 0, ground = 0;
        uint32_t flags = 0;
        int32_t snapTargetSlot = 0;
        int32_t minProximity = 0, maxProximity = 0, optimalProximity = 0, maxSize = 0, unknown10 = 0; // v >= 6
        float gradient = 0;                                                                         // v >= 7
        int32_t height = 0;                                                                          // v >= 8
        int32_t facing = 0, resolution = 0;                                                          // v >= 9/10
        std::array<uint32_t, 2> ts2Extra{0, 0xFFFFFFFF}; // v >= 0x40: the name index; v >= 0x41: +0x1c
        // Routing distances in tiles, as cTSSlotDescriptor::Read has them by
        // the file's version (base 1.0 0xbc3f1c; docs/routing-mac.md §9.5):
        // the proximities whole tiles before 0xA, sixteenths to 0x42, floats
        // from 0x43; the resolution sixteenths before 0x47, a float from it
        // (the raw ints above keep the bits). The offset's x and y are in
        // sixteenths before 0x47 too (scaled here).
        float minDistance = 0, maxDistance = 0, optimalDistance = 0, resolutionTiles = 0;
    };
    std::string fileName;
    uint32_t version = 0;
    bool sims2Layout = false;
    std::vector<Slot> slots;

    static Slots parse(std::span<const uint8_t> data);
};

// TTAB: interactions (pie menu entries) with their action/guard BHAVs and
// motive advertisements. Versions 0x3F-0x4E as stored by the base game.
struct InteractionTable {
    struct Advertisement {
        int16_t minimum = 0;
        int16_t delta = 0; // added when the personality type matches
        uint16_t personality = 0;
    };
    struct Interaction {
        uint16_t action = 0;
        uint16_t guard = 0;
        uint16_t flags = 0, flags2 = 0;
        uint32_t stringIndex = 0; // into the TTAs with the same instance
        uint32_t attenuationCode = 0;
        float attenuationValue = 0;
        uint32_t autonomy = 0;
        int32_t joinIndex = -1;
        uint16_t uiDisplayType = 0;      // version >= 0x45
        uint32_t facialAnimation = 0;    // version >= 0x46
        float memoryMultiplier = 0;      // version >= 0x4A
        uint32_t objectType = 0;         // version >= 0x4C
        uint32_t modelTableId = 0;       // version >= 0x4C
        std::vector<std::vector<Advertisement>> motives; // 1 table before 0x44, 7 after (one per age group)
    };
    std::string fileName;
    int32_t typeCode = -1;
    uint32_t version = 0;
    uint32_t unused = 0;
    std::vector<Interaction> interactions;
    std::string name; // optional trailing name, e.g. "Private Function Table"

    static InteractionTable parse(std::span<const uint8_t> data);
};

} // namespace os2
