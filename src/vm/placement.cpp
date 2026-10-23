// Buy mode's object placement (placement.hpp).

#include "placement.hpp"

#include "hood_bridge.hpp"
#include "rooms.hpp"
#include "formats/build_catalog.hpp"
#include "formats/lot.hpp"
#include "formats/log_once.hpp"
#include "formats/resindex.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>
#include <tuple>

namespace os2::vm {

int placementerror::stringNumber(int code) {
    struct Entry {
        uint8_t code, number;
    };
    // The function's table, code -> 1-based STR# 0x89 entry.
    static constexpr Entry kTable[] = {{0x01, 74}, {0x03, 2}, {0x04, 3}, {0x06, 5}, {0x07, 6}, {0x08, 7}, {0x0a, 9}, {0x0b, 10}, {0x0c, 11}, {0x0e, 13}, {0x0f, 14}, {0x10, 15}, {0x11, 16}, {0x12, 17}, {0x13, 32}, {0x16, 21}, {0x17, 22}, {0x18, 23}, {0x19, 91}, {0x1a, 25}, {0x1b, 26}, {0x1c, 63}, {0x21, 32}, {0x22, 33}, {0x23, 34}, {0x24, 35}, {0x25, 91}, {0x26, 37}, {0x27, 38}, {0x28, 39}, {0x29, 40}, {0x2a, 41}, {0x2b, 42}, {0x2c, 43}, {0x2d, 44}, {0x2e, 45}, {0x2f, 8}, {0x30, 97}, {0x31, 4}, {0x32, 91}, {0x33, 91}, {0x34, 91}, {0x35, 91}, {0x37, 91}, {0x38, 93}, {0x39, 38}, {0x3a, 38}, {0x3b, 98}, {0x41, 7}, {0x43, 7}, {0x44, 6}, {0x48, 32}, {0x49, 17}, {0x4a, 76}, {0x4b, 75}, {0x4c, 47}, {0x4d, 48}, {0x4e, 49}, {0x4f, 50}, {0x50, 94}, {0x52, 32}, {0x53, 17}, {0x58, 85}, {0x5a, 51}, {0x5b, 52}, {0x5c, 53}, {0x61, 54}, {0x63, 55}, {0x64, 56}, {0x65, 57}, {0x66, 58}, {0x68, 59}, {0x69, 60}, {0x6a, 61}, {0x6b, 55}, {0x6c, 62}, {0x6d, 63}, {0x6e, 64}, {0x6f, 65}, {0x70, 66}, {0x71, 67}, {0x72, 68}, {0x73, 69}, {0x74, 69}, {0x75, 70}, {0x76, 7}, {0x77, 71}, {0x78, 72}, {0x79, 79}, {0x7a, 11}, {0x7b, 73}, {0x7c, 73}, {0x7f, 11}, {0x80, 73}, {0x81, 12}, {0x82, 76}, {0x83, 11}, {0x84, 7}, {0x85, 77}, {0x86, 78}, {0x87, 95}, {0x88, 79}, {0x89, 86}, {0x8a, 90}, {0x8b, 89}, {0x8c, 91}, {0x8d, 91}, {0x8e, 91}, {0x8f, 91}, {0x90, 91}, {0x91, 91}, {0x92, 96}};
    static constexpr uint8_t kEmpty[] = {0x14, 0x15, 0x1d, 0x1e, 0x1f, 0x3c, 0x3d, 0x3e, 0x3f, 0x46, 0x47, 0x54, 0x55, 0x56, 0x57,
                                         0x5d, 0x5e, 0x5f, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0xa5};
    for (const Entry& e : kTable)
        if (e.code == code)
            return e.number;
    for (uint8_t e : kEmpty)
        if (e == code)
            return 0;
    return 1;
}

namespace {

int normDir(int d);

using Rects = std::vector<std::array<float, 4>>;

// Object data 8 "flags" (STR# 0x8E): CanIntersectObjects (cTSObject vtable
// 0x238, BV 0xaf56b8) is 0x02 | 0x04, CanIntersectPeople (0x1d8, 0xaf56f0)
// 0x04 | 0x10, HidesFloorOnPlace (0x220, 0xaff238) 0x4000 "Hide Floor".
bool canIntersectObjects(const Object& o) { return o.data[objdata::Flags] & 0x06; }
bool canIntersectPeople(const Object& o) { return o.data[objdata::Flags] & 0x14; }
bool hidesFloorOnPlace(const Object& o) { return o.data[objdata::Flags] & 0x4000; }
// FootprintBlocksCeiling (0x23c, BV 0xaf5768): od 42 bit 0x1000 "attached to
// ceiling"; FootprintBlocksFloor (0x240) the bit clear.
bool footprintBlocksCeiling(const Object& o) { return o.data[objdata::PlacementFlags] & 0x1000; }
// IsDiagonalDoor (base 1.0 FUN_00fffc88): a door (type 8) whose wall
// placement flags (od 13, vtable 0x204 / BV 0x25c) have 0x50.
bool isDiagonalDoor(const Object& o) {
    return o.definition && o.definition->objectType() == kObjectTypeDoor &&
           (o.data[objdata::WallPlacementFlags] & 0x50);
}

// FUN_0191f43c / FUN_0191f914 (BV): two footprints cross when a non-empty
// rectangle of one overlaps a non-empty one of the other, strictly (the
// tolerance is 0 in the placement calls: TSObjectCanSetPosition is called
// with 0.0 by the buy tool and Create New Object Instance).
bool footprintsCross(const Rects& a, const Rects& b) {
    for (const auto& r : a)
        for (const auto& q : b)
            if (r[0] < r[2] && r[1] < r[3] && q[0] < q[2] && q[1] < q[3] && q[0] < r[2] && r[0] < q[2] && q[1] < r[3] &&
                r[1] < q[3])
                return true;
    return false;
}

// FUN_015b2090 / FUN_015b2448 (BV): a circle of radius r about (px, py)
// reaches a non-empty rectangle of the footprint: within r of it on both
// axes, and in a corner's region nearer than r to the corner.
bool circleReaches(float r, const Rects& fp, float px, float py) {
    if (!(r > 0))
        return false;
    for (const auto& q : fp) {
        if (!(q[0] < q[2] && q[1] < q[3]))
            continue;
        if (!(px - r <= q[2] && q[0] <= px + r && py - r <= q[3] && q[1] <= py + r))
            continue;
        const bool outX = px < q[0] || q[2] < px, outY = py < q[1] || q[3] < py;
        if (!(outX && outY))
            return true;
        const float dx = px < q[0] ? px - q[0] : px - q[2], dy = py < q[1] ? py - q[1] : py - q[3];
        if (dx * dx + dy * dy < r * r)
            return true;
    }
    return false;
}

// FUN_0142e734 (base 1.0): two places are the same when they are on one
// level and x and y are within 32 units in the last place of each other.
bool samePlace(const Position& a, const Position& b) {
    if (a.level != b.level)
        return false;
    auto close = [](float u, float v) {
        auto key = [](float f) {
            int32_t i;
            std::memcpy(&i, &f, sizeof i);
            return i < 0 ? int64_t(int32_t(0x80000000u)) - int64_t(i) : int64_t(i);
        };
        return std::llabs(key(u) - key(v)) < 32;
    };
    return close(a.x, b.x) && close(a.y, b.y);
}

// cTSObject::RelativeWallOrFenceRequired (BV 0xaf61b8, the n-th of the bits
// NumWallOrFenceRequiredFlags counts, 0xaf50dc: 1, 2, 4, 8, 0x10, 0x40 of
// od 13) as walls of the object's tile in the world: 0-3 the edge toward
// directionStep(2 k), 4 / 5 the diagonal across the tile along grid slope -1
// / +1 (the 0x40 diagonal along direction + 1, the 0x10 one along
// direction + 7, as the wall test below reads them). FUN_00ffb62c compares
// each with the other's and with its OppositeWallLocator: on one tile, the
// same wall either way.
std::vector<int> requiredWalls(const Object& o, int direction) {
    std::vector<int> out;
    const int16_t wp = o.data[objdata::WallPlacementFlags];
    for (int side = 0; side < 4; ++side)
        if (wp & (1 << side)) {
            const auto s = sideStep(direction, side);
            for (int k = 0; k < 4; ++k)
                if (directionStep(2 * k) == s)
                    out.push_back(k);
        }
    for (int k = 0; k < 2; ++k)
        if (wp & (k ? 0x40 : 0x10)) {
            const auto c = directionStep(direction + (k ? 1 : 7));
            out.push_back(c[0] == c[1] ? 5 : 4);
        }
    return out;
}

// The catalog definition's GUID (cTSObject::GetSelector, vtable 0xf0 / base
// 0xd8, and its 0x70 / 0x6c): a multi-tile part's master's (inferred: the
// selector is the catalog product), 0 without a definition.
uint32_t productGuid(VM& vm, const Object& o) {
    if (!o.definition)
        return 0;
    if (const uint16_t master = o.definition->masterId())
        if (auto instance = vm.code().masterDefinition(o.group, master))
            if (const ObjectDefinition* md = vm.code().definition(o.group, *instance))
                return md->guid();
    return o.definition->guid();
}

// The exclusive placement test of two objects on one tile (base 1.0
// FUN_00ffb7b0, BV HasEnoughClearanceFromRoof 0x8a74fc, misnamed; od 63
// "exclusive placement flags", STR# 0xFB: 1 floor, 2 wall, 4 no other object
// of same type, 8 ceiling): both on the floor or both on the ceiling ->
// 0x0C; with 4 on either, the other at the same place and elevation
// (Elevation, vtable 0xb0, against the tested one: 0 but for the fireplace
// tool's stacked caps) and the same product -> 0x0C; then, when the two need a wall
// in common (FUN_00ffb62c), both doors or windows
// (ExclusiveWallPlacementCategory, 0xaf57a4: 1 for types 8 and 9) -> 0x17
// "Must be against unused wall", else both with 2 -> 0x0C; 0 when they may
// share the tile. The BV's od 82 tests are the expansion's.
int exclusiveError(VM& vm, const Object& o, const Position& at, int direction, const Object& x) {
    const int16_t a = o.data[objdata::ExclusivePlacementFlags], b = x.data[objdata::ExclusivePlacementFlags];
    if (a & b & 9)
        return placementerror::Objects;
    if (((a | b) & 4) && samePlace(x.position, at) && x.position.elevation == at.elevation) {
        const uint32_t pa = productGuid(vm, o), pb = productGuid(vm, x);
        if (pa && pb && pa == pb)
            return placementerror::Objects;
    }
    const auto mine = requiredWalls(o, direction), theirs = requiredWalls(x, x.data[objdata::Direction]);
    bool shared = false;
    for (int w : mine)
        shared = shared || std::find(theirs.begin(), theirs.end(), w) != theirs.end();
    if (!shared)
        return 0;
    auto category = [](const Object& y) {
        const uint16_t t = y.definition ? y.definition->objectType() : 0;
        return t == kObjectTypeDoor || t == kObjectTypeWindow ? 1 : 0;
    };
    if (category(o) && category(o) == category(x))
        return 0x17;
    return (a & b & 2) ? placementerror::Objects : 0;
}

// cTSObject::RunTree(5, the other's ID, no parameters, true, false) (BV
// 0xafe524): the "allow intersection" tree (OBJf entry 5) with the other as
// its stack object, run to its end; true lets the two overlap.
bool allowsIntersection(VM& vm, const Object& o, const Object& other) {
    if (!vm.functionTree(o, 5))
        return false;
    try {
        const auto t = vm.runFunction(o.id, 5, 1000, {}, other.id);
        return t && t->status == Thread::Status::Finished && t->result;
    } catch (const VMError&) {
        return false;
    }
}

int normDir(int d) { return ((d % 8) + 8) % 8; }

// HasFloorTest(tile, 0xF, all): every quarter of the tile (VM level) has a
// floor pattern.
bool floorWhole(const Lot& lot, TilePos t) {
    if (t.x < 0 || t.y < 0 || !lot.hasLevel(t.level - 1))
        return false;
    for (uint16_t q : lot.floor(t.level - 1, uint32_t(t.x), uint32_t(t.y)))
        if (!q)
            return false;
    return true;
}

// FUN_00164c40 on the tile a level under `t` (Mac BV): a quarter with a
// pool's marker (3ARY 0x14, IsPool: products of type 3); without the lot's
// geometry, or a lot read without markers, the world's pool tiles of the
// ground.
bool poolUnder(const VM& vm, const World& w, TilePos t) {
    if (const Lot* lot = vm.placementLot; lot && !lot->deckFoundationStyles.empty()) {
        for (int q = 0; q < 4; ++q)
            if (lot->deckFoundationStyle(t.level - 2, t.x, t.y, q) == 3)
                return true;
        return false;
    }
    return t.level == 1 && w.pool(t);
}

// nTSWorld::IsFlat (BV 0x165af8): the tile's four grid points (on its lot
// level) at exactly the same height. A level the lot does not keep counts as
// flat.
bool flatTile(const Lot& lot, TilePos t) {
    const int li = t.level - 1;
    if (!lot.hasLevel(li) || lot.elevations.empty() || t.x < 0 || t.y < 0 || t.x >= int(lot.width) ||
        t.y >= int(lot.height))
        return true;
    const float z = lot.elevation(li, t.x, t.y);
    return lot.elevation(li, t.x + 1, t.y) == z && lot.elevation(li, t.x, t.y + 1) == z &&
           lot.elevation(li, t.x + 1, t.y + 1) == z;
}

// nTSWorld::IsPartWater / IsAllWater (BV 0x1660f8): some / every grid
// point of the tile a water vertex (cWorldDB::IsWaterVertex, 0xd25d8: on the
// ground only, the water's vtable 0x34; here the lot's water level above the
// terrain, as the walls' 0x47 reads it: inferred).
int waterVertices(const Lot& lot, TilePos t) {
    if (t.level != 1 || lot.elevations.empty() || lot.water.empty() || t.x < 0 || t.y < 0 || t.x >= int(lot.width) ||
        t.y >= int(lot.height))
        return 0;
    int n = 0;
    for (int dx = 0; dx <= 1; ++dx)
        for (int dy = 0; dy <= 1; ++dy)
            n += lot.waterLevel(t.x + dx, t.y + dy) > lot.elevation(0, t.x + dx, t.y + dy) + 1e-4f;
    return n;
}

// HasFloorTest(tile, 0xF, any) (cWorldDB vtable 0xb4, BV 0xd29b0 ->
// FUN_000d2798 with "all" 0: a floored quarter is enough, the visibility not
// read).
bool floorSome(const Lot& lot, TilePos t) {
    if (t.x < 0 || t.y < 0 || !lot.hasLevel(t.level - 1) || t.x >= int(lot.width) || t.y >= int(lot.height))
        return false;
    for (uint16_t q : lot.floor(t.level - 1, uint32_t(t.x), uint32_t(t.y)))
        if (q)
            return true;
    return false;
}

std::vector<const Slots::Slot*> containerSlots(VM& vm, const Object& o);

} // namespace

int slotPlacementError(VM& vm, const Object& x, const Object& container, int slot) {
    int first = 0;
    auto error = [&](int code) {
        if (!first)
            first = code;
    };
    if (const ObjectId lead = x.data[objdata::LeadTileObjectId]; lead && lead != x.id)
        error(0x11);
    const auto slots = containerSlots(vm, container);
    if (slot < 0 || size_t(slot) >= slots.size()) {
        error(placementerror::NoPlace);
        return first;
    }
    if (const ObjectId held = vm.containedIn(container.id, int16_t(slot)); held && held != x.id)
        error(placementerror::Objects);
    if (container.data[objdata::SupportStrength] < x.data[objdata::Weight])
        error(placementerror::Weight);
    for (const Object* c = &container; c; c = c->data[objdata::ContainerId] ? vm.object(c->data[objdata::ContainerId]) : nullptr) {
        if (c->id == x.id) {
            error(placementerror::Objects);
            break;
        }
        if (c->data[objdata::ContainerId] == c->id)
            break;
    }
    const int h = slots[size_t(slot)]->height;
    const int mask = h == 13 ? -1 : h >= 1 && h <= 16 ? 1 << (h - 1) : 0;
    if (!(x.data[objdata::AllowedHeightFlags] & mask))
        error(0x25);
    return first;
}

void placeInSlot(VM& vm, Object& x, const Object& container, int slot) {
    vm.setContainer(x, container.id, int16_t(slot));
    vm.setPosition(x, container.position);
    x.data[objdata::Room] = container.data[objdata::Room];
    vm.noteRoomChanged(x.id);
    x.data[objdata::Direction] = container.data[objdata::Direction];
}

namespace {

// Container slots of an object's SLOT, in slot-number order.
std::vector<const Slots::Slot*> containerSlots(VM& vm, const Object& o) {
    std::vector<const Slots::Slot*> out;
    if (!o.definition)
        return out;
    if (const Slots* s = vm.code().slots(o.group, o.definition->slotId()))
        for (const auto& slot : s->slots)
            if (slot.type == 0)
                out.push_back(&slot);
    return out;
}

void runEntry(VM& vm, ObjectId id, size_t function) {
    try {
        vm.runFunction(id, function);
    } catch (const VMError&) {
        // A tree that fails does not stop the placement (as with Inits).
    }
}

} // namespace

// The step to the tile on a side of an object facing `direction`: 0 front,
// 1 right, 2 behind, 3 left (the order of the wall flags, STR# 0xE5).
std::array<int, 2> sideStep(int direction, int side) {
    const auto f = directionStep(direction);
    switch (side) {
    case 0: return f;
    case 1: return {f[1], -f[0]}; // clockwise of the front
    case 2: return {-f[0], -f[1]};
    default: return {-f[1], f[0]};
    }
}


void runUserPlacement(VM& vm, ObjectId id) {
    for (ObjectId part : groupOf(vm, id))
        if (const Object* o = vm.object(part); o && (o->data[objdata::FlagField2] & 0x200))
            runEntry(vm, part, entry::UserPlacement);
}

std::array<int, 2> partOffset(uint16_t subIndex, int direction) {
    int x = int8_t(subIndex & 0xFF), y = int8_t(subIndex >> 8);
    for (int i = 0; i < normDir(direction) / 2; ++i) {
        const int nx = y, ny = -x; // a quarter turn clockwise
        x = nx, y = ny;
    }
    return {x, y};
}

std::vector<ObjectId> groupOf(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    if (!o)
        return {};
    const ObjectId lead = multiTileGroup(*o);
    std::vector<ObjectId> out;
    for (const auto& [other, x] : vm.eachObject())
        if (other == lead || x.data[objdata::LeadTileObjectId] == lead)
            out.push_back(other);
    if (std::find(out.begin(), out.end(), id) == out.end())
        out.push_back(id);
    return out;
}

namespace {

Position placeOf(const Placement::Part& part) {
    return part.at ? *part.at : Position{float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f, part.tile.level};
}

// What is tested of a part where the placement puts it: its place and
// facing, its footprint there (TSObjectWorldSpaceFootprint, vtable 0x100 /
// base 0xe8: GetFootprint at that place) and, for a diagonal door, its
// footprints 0 and 2 there (TSObjectIndexedWorldSpaceFootprint).
struct Held {
    const Object* o = nullptr;
    Position at;
    int direction = 0;
    Rects fp;
    std::optional<Rects> door0, door2;
};

// The parts' footprints where the placement puts them: the group is stood
// there for the reading only (the binary computes the footprint at the
// tested place, GetFootprint(location, direction)), then put back. A part
// already on its tested tile keeps its place there (an object tested where
// it stands, s2pkg placement-sweep: the modular stairs' steps are a quarter
// tile apart).
std::vector<Held> heldParts(VM& vm, const Placement& p) {
    struct Saved {
        ObjectId id;
        Position position;
        int16_t direction;
    };
    std::vector<Saved> saved;
    std::vector<Position> places;
    for (const auto& part : p.parts)
        if (Object* o = vm.object(part.id)) {
            saved.push_back({part.id, o->position, o->data[objdata::Direction]});
            const bool there = !part.at && o->position.inWorld() && !o->data[objdata::ContainerId] && VM::tileOf(*o) == part.tile;
            if (!there)
                vm.setPosition(*o, placeOf(part));
            o->data[objdata::Direction] = int16_t(p.direction);
            places.push_back(o->position);
        }
    std::vector<Held> out;
    for (size_t i = 0; i < p.parts.size() && i < places.size(); ++i) {
        const Object* o = vm.object(p.parts[i].id);
        if (!o)
            continue;
        Held h{o, places[i], p.direction, vm.routingFootprint(*o), std::nullopt, std::nullopt};
        if (isDiagonalDoor(*o))
            h.door0 = vm.indexedFootprint(*o, 0), h.door2 = vm.indexedFootprint(*o, 2);
        out.push_back(std::move(h));
    }
    for (const Saved& s : saved)
        if (Object* o = vm.object(s.id)) {
            vm.setPosition(*o, s.position);
            o->data[objdata::Direction] = s.direction;
        }
    return out;
}

// DoesFootprintIntersectDoor (BV 0x8a8fdc): a footprint against a door's
// footprints 0 and 2 (none: no test).
bool crossesDoor(const Rects& fp, const std::optional<Rects>& door0, const std::optional<Rects>& door2) {
    return (door0 && footprintsCross(fp, *door0)) || (door2 && footprintsCross(fp, *door2));
}

std::string nameOf(const Object& x) {
    return "\"" + (x.definition ? x.definition->fileName : std::string("?")) + "\"";
}

// TestPlacementWithRespectToObjects as base 1.0 has it (FUN_00ffb990, one
// function; BV TestPlacementWithRespectToObjects 0x8ace04 calling
// TestPlacementWithRespectToObject 0x8ac158 per object), with the flags the
// buy tool (FUN_009eb860) and Create New Object Instance (TryCreateObject
// 0xe34b2c) pass: 0, no door circle (flag 2) and no person circle (flag 1).
// A person with person data 68 bit 0x02 passes. Every object on the 3x3
// tiles round the part (its level, on the lot), not of its own group:
// on its own tile the exclusive test first (exclusiveError); then a person
// passes another person or an object that CanIntersectPeople, or when it
// can itself; an object passes a person when it CanIntersectPeople, and
// another object when either CanIntersectObjects; then either one's "allow
// intersection" tree (OBJf 5, asked both ways) lets them; then only two
// footprints on one layer (both FootprintBlocksCeiling or both
// FootprintBlocksFloor) are tested: two people closer than 0.283
// (0x274b968); a held diagonal door, the other's footprint against its
// door footprints 0 and 2; a person in the way of an object, the circle
// of 0.3 about the person against the object's footprint, unless the
// object is a "Low Small Object" (od 40 bit 0x4000: then the person's own
// box); a diagonal door in the way of an object, the object's footprint
// against the door's; else the two footprints. Then the diagonal doors two
// tiles away (the 5x5 ring, for an object): either one a diagonal door,
// neither CanIntersectObjects, no tree letting them, one layer, the
// footprint against the door's. A failure is 0x0C.
int objectsError(VM& vm, const Held& h, const std::set<ObjectId>& mine, std::string& why) {
    const Object& o = *h.o;
    const World& w = *vm.world;
    const bool person = o.person.has_value();
    if (person && (o.person->data[68] & 0x02))
        return 0;
    const TilePos t{int(std::floor(h.at.x)), int(std::floor(h.at.y)), h.at.level};
    const bool ceiling = footprintBlocksCeiling(o);
    auto treesLet = [&](const Object& x) { return allowsIntersection(vm, o, x) || allowsIntersection(vm, x, o); };
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            const TilePos u{t.x + dx, t.y + dy, t.level};
            if (!w.contains(u))
                continue;
            for (ObjectId other : vm.objectsOnTile(u)) {
                if (mine.count(other))
                    continue;
                const Object& x = *vm.object(other);
                if (dx == 0 && dy == 0)
                    if (const int e = exclusiveError(vm, o, h.at, h.direction, x)) {
                        why = nameOf(x) + (e == 0x17 ? " uses that wall" : " is in the way (exclusive placement)");
                        return e;
                    }
                if (person) {
                    if (canIntersectPeople(x) || canIntersectPeople(o))
                        continue;
                } else if (x.person) {
                    if (canIntersectPeople(o))
                        continue;
                } else if (canIntersectObjects(o) || canIntersectObjects(x)) {
                    continue;
                }
                if (treesLet(x) || ceiling != footprintBlocksCeiling(x))
                    continue;
                bool hit;
                if (person && x.person)
                    hit = std::hypot(x.position.x - h.at.x, x.position.y - h.at.y) < 0.283f;
                else if (isDiagonalDoor(o))
                    hit = crossesDoor(vm.routingFootprint(x), h.door0, h.door2);
                else if (x.person && !(o.data[objdata::FlagField2] & 0x4000))
                    hit = circleReaches(0.3f, h.fp, x.position.x, x.position.y);
                else if (!person && isDiagonalDoor(x))
                    hit = crossesDoor(h.fp, vm.indexedFootprint(x, 0), vm.indexedFootprint(x, 2));
                else
                    hit = footprintsCross(h.fp, vm.routingFootprint(x));
                if (hit && std::getenv("OS2_DEBUG_PLACEMENT")) {
                    std::fprintf(stderr, "  %s at (%.3f,%.3f) dir %d vs %s at (%.3f,%.3f) dir %d\n", nameOf(o).c_str(), h.at.x, h.at.y, h.direction, nameOf(x).c_str(), x.position.x, x.position.y, x.data[objdata::Direction]);
                    for (const auto& r : h.fp) std::fprintf(stderr, "    me [%.3f %.3f %.3f %.3f]\n", r[0], r[1], r[2], r[3]);
                    for (const auto& r : vm.routingFootprint(x)) std::fprintf(stderr, "    it [%.3f %.3f %.3f %.3f]\n", r[0], r[1], r[2], r[3]);
                }
                if (hit) {
                    why = x.person ? "a Sim is in the way" : nameOf(x) + " is in the way";
                    return placementerror::Objects;
                }
            }
        }
    if (person)
        return 0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) {
            const TilePos u{t.x + dx, t.y + dy, t.level};
            if ((std::abs(dx) <= 1 && std::abs(dy) <= 1) || !w.contains(u))
                continue;
            for (ObjectId other : vm.objectsOnTile(u)) {
                if (mine.count(other))
                    continue;
                const Object& x = *vm.object(other);
                if (!isDiagonalDoor(o) && !isDiagonalDoor(x))
                    continue;
                if (canIntersectObjects(o) || canIntersectObjects(x) || treesLet(x) ||
                    ceiling != footprintBlocksCeiling(x))
                    continue;
                const bool hit = !isDiagonalDoor(x) ? crossesDoor(vm.routingFootprint(x), h.door0, h.door2)
                                                    : crossesDoor(h.fp, vm.indexedFootprint(x, 0), vm.indexedFootprint(x, 2));
                if (hit) {
                    why = nameOf(x) + " is in the way";
                    return placementerror::Objects;
                }
            }
        }
    return 0;
}

// The floors' visibility layer (cWorldDB +0x9c, a byte a quarter;
// SetFloorVisibility BV 0xd8ed4, FloorVisible 0xd29bc): what the placement
// test reads of it. The scene graph's object hides the floor of each part's
// tile that HidesFloorOnPlace (od 8 bit 0x4000 "Hide Floor": the sunken hot
// tub, the two-storey windows) while the object is not hidden (od 34 &
// 0x21, nTSSG::cObject::UpdateVisibilityAndRendering BV 0x661e5c ->
// SetFloorVisibilityForHiddenObjects 0x661c28); the pool tool hides the
// ground's floor over a pool (ModifyFloorVisibility 0x9adec8), which the
// test does not read on the ground. Here from the objects in the world.
bool floorHidden(VM& vm, TilePos t, const std::set<ObjectId>& mine) {
    for (ObjectId other : vm.objectsOnTile(t))
        if (const Object* x = vm.object(other);
            x && !mine.count(other) && hidesFloorOnPlace(*x) && !(x->data[objdata::HiddenFlags] & 0x21))
            return true;
    return false;
}

// A wall on the edge between `t` and its neighbour for the placement tests:
// the world's (the walk-over fences too) or the wall a door stands in (the
// VM's world opens it for routing; the game's keeps the wall, the door being
// a portal in it).
bool placementWall(const World& w, TilePos t, int dx, int dy) {
    if (w.wallBetween(t, dx, dy, true))
        return true;
    for (const World::Door& d : w.doors()) {
        if (d.diagonal || d.tile.level != t.level)
            continue;
        const auto s = directionStep(d.dir + 4);
        if ((d.tile == t && s[0] == dx && s[1] == dy) ||
            (d.tile == TilePos{t.x + dx, t.y + dy, t.level} && s[0] == -dx && s[1] == -dy))
            return true;
    }
    return false;
}

} // namespace

Placement checkPlacement(VM& vm, ObjectId id, TilePos tile, int direction, bool slots, const Position* at) {
    Placement p;
    p.level = tile.level;
    p.direction = normDir(direction);
    const Object* lead = vm.object(id);
    auto fail = [&](int code, std::string why) {
        p.ok = false;
        p.error = code;
        p.reason = std::move(why);
        return p;
    };
    if (!lead || !lead->definition)
        return fail(placementerror::NoPlace, "no such object");
    if (!vm.world)
        return fail(placementerror::OutOfBounds, "no lot");
    const World& w = *vm.world;
    const std::vector<ObjectId> group = groupOf(vm, id);
    const std::set<ObjectId> mine(group.begin(), group.end());
    const uint16_t leadSub = lead->definition->subIndex() == 0xFFFF ? 0 : lead->definition->subIndex();
    const auto leadOff = partOffset(leadSub, p.direction);
    // OBJD field 0xB0 "level offset": parts on the levels above (the
    // upper half of a staircase).
    const int leadLevel = int16_t(lead->definition->field(0xB0));
    for (ObjectId part : group) {
        const Object* o = vm.object(part);
        const uint16_t sub = o->definition && o->definition->masterId() ? o->definition->subIndex() : 0;
        const auto off = partOffset(sub, p.direction);
        const int dl = o->definition ? int16_t(o->definition->field(0xB0)) - leadLevel : 0;
        p.parts.push_back({part, {tile.x + off[0] - leadOff[0], tile.y + off[1] - leadOff[1], tile.level + dl}});
        if (at) {
            Position& q = *(p.parts.back().at = Position{at->x + float(off[0] - leadOff[0]),
                                                         at->y + float(off[1] - leadOff[1]), at->level + dl});
            q.elevation = at->elevation;
        }
    }
    std::set<std::tuple<int, int, int>> tiles;
    for (const auto& part : p.parts)
        tiles.insert({part.tile.x, part.tile.y, part.tile.level});

    // Step 4 of TryPlace: a free slot of an object on the tile that takes
    // this one (single-tile objects): the allowed heights hold the slot's
    // height bit (FreeSO's rule for The Sims 1) and the surface is strong
    // enough. The first such slot wins (FindFirstAvailableSlot).
    if (slots && group.size() == 1) {
        const int16_t heights = lead->data[objdata::AllowedHeightFlags];
        bool tooHeavy = false;
        for (ObjectId other : vm.objectsOnTile(tile)) {
            const Object& x = *vm.object(other);
            if (mine.count(other) || x.person)
                continue;
            const auto slots = containerSlots(vm, x);
            for (size_t i = 0; i < slots.size() && !p.container; ++i) {
                const int h = slots[i]->height;
                if (h < 1 || h > 16 || !(heights & (1 << (h - 1))))
                    continue;
                if (const ObjectId held = vm.containedIn(other, int16_t(i)); held && !mine.count(held))
                    continue;
                if (lead->data[objdata::Weight] >= x.data[objdata::SupportStrength]) {
                    tooHeavy = true;
                    continue;
                }
                p.container = other, p.slot = int(i);
            }
            if (p.container)
                break;
        }
        if (p.container) {
            p.ok = true;
            return p;
        }
        if (tooHeavy && !(heights & 1))
            return fail(placementerror::Weight, "the surface cannot hold it");
    }

    const std::vector<Held> held = heldParts(vm, p);
    for (const auto& part : p.parts) {
        const Object& o = *vm.object(part.id);
        const TilePos t = part.tile;
        const int16_t pf = o.data[objdata::PlacementFlags], pf2 = o.data[objdata::PlacementFlags2];
        // TestPlacementCommon (docs/buy-build-mac.md §3.2): 1 in the lot,
        // 2 a level of the lot, 3 a floor under it (not in the air), 7 the
        // floor or terrain the placement flags allow (on the ground the
        // world cannot tell floor from terrain: either flag will do).
        if (t.x < 0 || t.y < 0 || !w.contains({t.x, t.y, w.firstLevel()}))
            return fail(placementerror::OutOfBounds, "out of the lot");
        // TSObjectIsInLockedArea / IsOnLockedTile (BV 0x8a9d00, 0x8a66b0): a
        // tile on the lot that is locked (the road strip, cIWorldDB slot 0xc4)
        // -> 0x02, unless the object's placement flags (od 42) have 0x40.
        // (Objects whose AllowOnlyOnLockedTile bit is set, cTSObject +0xea
        // bit 10, must stand on locked tiles instead: not mapped here.)
        if (!(pf & 0x40) && w.locked(t))
            return fail(placementerror::OutOfBounds, "on a locked tile (the road strip)");
        if (!w.contains(t))
            return fail(placementerror::LevelOutOfBounds, "level out of the lot");
        // 3: a floor under it, unless in the air (p & 0x200) or p2 & 4:
        // FUN_0016d518 -> WorldDB 0xb8(tile, 0xF, !HidesFloorOnPlace) (base
        // 1.0 TestPlacementCommon FUN_00ffc9b0: WorldDB 0x90 with the same
        // arguments), the ground always (VM level 1, even when a pool gives
        // the lot a level under it), above it all four quarters of the tile
        // floored (HasFloorTest, FUN_000d2798) and, unless the object hides
        // the floor itself, all four visible (the visibility layer,
        // floorHidden: a sunken hot tub's tiles take no other object). The
        // BV's TileHasFloorFromObject (od 98 bit 0x01, the elevators) is not
        // in base 1.0. Without the lot's geometry (no VM::placementLot), a
        // floor in some quarter (the world's).
        if (t.level != 1 && t.level > w.firstLevel() && !(pf & 0x200) && !(pf2 & 0x04)) {
            if (!(vm.placementLot ? floorWhole(*vm.placementLot, t) : w.hasFloor(t)))
                return fail(placementerror::NoFloor, "no floor");
            if (!hidesFloorOnPlace(o) && floorHidden(vm, t, mine))
                return fail(placementerror::NoFloor, "the floor is hidden (under a \"Hide Floor\" object)");
        }
        // ... and, the call's last argument set for objects, the tile a level
        // down without a pool's marker in any quarter (FUN_00164c40: IsPool
        // of each quarter's DeckFoundationStyle; docs/buy-build-mac.md
        // §7.25): over a pool there is no floor, even on the ground. The
        // game passes it for every object, but N001_Lot82's pool lamp (od 42
        // 0x0800 only, od 82 0) stands on the ground over its pool: pool
        // objects (p & 0x800) are let through here (inferred; how the game
        // lets them is not found).
        if (!(pf & 0x200) && !(pf2 & 0x04) && !(pf & 0x800) && poolUnder(vm, w, t))
            return fail(placementerror::NoFloor, "over a pool");
        // On a fence post (p & 0x2000 "fence post" with a fence's post at the
        // place: the partition manager's 0xec and PartitionStyleIsFence; here
        // the lot's posts, FPL, at the grid point): the tests 4, 5 and 7 do
        // not apply, and without the post it is 0x79 (below).
        const Position here = held[size_t(&part - p.parts.data())].at;
        const bool onPost = (pf & 0x2000) && vm.placementLot &&
                            std::any_of(vm.placementLot->fencePosts.begin(), vm.placementLot->fencePosts.end(),
                                        [&](const Lot::FencePost& fp) {
                                            return fp.level == t.level - 1 && std::fabs(fp.x - here.x) < 0.01f &&
                                                   std::fabs(fp.y - here.y) < 0.01f;
                                        });
        // 4: partly in water (IsPartWater) without p & 4 "under water" or
        // 0x4000 "water surface" -> 0x90.
        if (vm.placementLot && !onPost && !(pf & 0x04) && !(pf & 0x4000) && waterVertices(*vm.placementLot, t) > 0)
            return fail(0x90, "in the water");
        // 5 a pool tile needs p & 0x800 (0x8F); pool objects need no floor there.
        const bool inPool = w.pool(t);
        // (Parts in the air, p & 0x200, or on the water, 0x4000, may hang over it: inferred.)
        if (inPool && !onPost && !(pf & (0x800 | 0x200 | 0x4000)))
            return fail(0x8F, "not in a pool");
        // 6: the tile flat (nTSWorld::IsFlat, BV 0x165af8: its four grid
        // points at exactly one height), unless p & 0x100 -> 0x12 "Can't
        // place on slope".
        if (vm.placementLot && !(pf & 0x100) && !flatTile(*vm.placementLot, t))
            return fail(0x12, "on a slope");
        // 7, with the lot's geometry (TestPlacementCommon BV 0x8aad90, base
        // 1.0 FUN_00ffc9b0): unless the tile is all water, a pool tile or
        // over a pool, a floored quarter (vtable 0xb4) without p & 1 -> 0x8C
        // "not allowed on the floor"; terrain (the ground, vtable 0xb8,
        // without all four quarters floored and visible, 0xb0) without p & 2
        // -> 0x8D. Without it, the floor or terrain the world cannot tell
        // apart: either flag will do.
        const Lot* lot = vm.placementLot;
        const int water = lot ? waterVertices(*lot, t) : 0;
        if (lot && !onPost) {
            const bool exempt = water == 4 || inPool || poolUnder(vm, w, t);
            if (!exempt && !(pf & 0x01) && floorSome(*lot, t))
                return fail(0x8C, "not allowed on the floor");
            const bool allFloored = t.level != 1 ? floorWhole(*lot, t) : floorWhole(*lot, t) && !floorHidden(vm, t, mine);
            if (!exempt && !(pf & 0x02) && t.level == 1 && !allFloored)
                return fail(0x8D, "not allowed on terrain");
        } else if (!lot && pf && !(pf & 0x03) && !(pf & 0x200) && !(inPool && (pf & 0x800))) {
            return fail(placementerror::NoFloor, inPool || !(pf & 0x800) ? "not allowed on the floor" : "only in a pool");
        }
        // 8: od 30 "room placement" (base 1.0 FUN_00ffc9b0: the partition
        // manager's room of definition 0 at the place, 0xe4(place, 0, 0),
        // outside when it is 0, FUN_0154dda4): 1 outside only -> 0x0A
        // inside, 2 inside only -> 0x0B outside. Definition 0 is the rooms of
        // the light-blocking walls (3ARY 0x0B: a fenced yard is outdoors), at
        // the place's quarter (FUN_0015d2f0; a tile's centre is the north
        // one); with the lot's geometry only (the world's rooms are 3ARY
        // 0x09's). (The BV asks definition 1 for "inside only".)
        if (const int16_t rp = o.data[objdata::RoomPlacement]; lot && (rp == 1 || rp == 2)) {
            const auto layer = lot->roomLayers.find(0x0B);
            const int li = t.level - 1;
            if (layer != lot->roomLayers.end() && lot->hasLevel(li) && t.x >= 0 && t.y >= 0 && t.x < int(lot->width) &&
                t.y < int(lot->height)) {
                const size_t i = (size_t(li - lot->minLevel) * lot->width + size_t(t.x)) * lot->height + size_t(t.y);
                if (i < layer->second.size()) {
                    const float fx = here.x - std::floor(here.x), fy = here.y - std::floor(here.y);
                    const int q = fx <= fy ? (1.0f - fx <= fy ? 1 : 0) : (1.0f - fx <= fy ? 2 : 3); // west, north, east, south
                    const bool outside = layer->second[i][size_t(q)] == 0;
                    if (rp == 1 && !outside)
                        return fail(0x0A, "must be outside");
                    if (rp == 2 && outside)
                        return fail(0x0B, "must be inside");
                }
            }
        }
        // 9: p & 0x2000 without a fence's post there -> 0x79.
        if (lot && (pf & 0x2000) && !onPost)
            return fail(0x79, "not on a fence post");
        // TestPlacementWithRespectToWalls (§3.3): the wall flags (od 13) on
        // the four sides, and no wall through the object (unless p & 0x400).
        // Doors and windows are wall portals (IsWallPortal, §3.3): the wall
        // runs between their parts.
        const bool portal = o.definition && (o.definition->objectType() == kObjectTypeDoor ||
                                             o.definition->objectType() == kObjectTypeWindow);
        const int16_t wp = o.data[objdata::WallPlacementFlags];
        // A diagonal portal (cTSObject::PortalDiagonalWallPlacement, BV
        // 0xe1edd8: wall flags & 0x50) stands in the diagonal wall across its
        // tile; the sides below say which diagonal (inferred: the binary's
        // IntersectsWalls tests the footprint against the wall segments).
        // IntersectsWalls (BV 0x8a7f1c) is skipped for p & 0x400 "wall" and
        // p & 0x2000 "fence post".
        if (w.diagonalWall(t) && !(pf & 0x2400) && !(portal && (wp & 0x50)))
            return fail(placementerror::WallIntersect, "diagonal wall");
        if (!(pf & 0x2400) && !portal)
            for (int k = 0; k < 4; ++k) {
                const auto s = directionStep(k * 2);
                if (tiles.count({t.x + s[0], t.y + s[1], t.level}) && placementWall(w, t, s[0], s[1]))
                    return fail(placementerror::WallIntersect, "a wall cuts the object");
            }
        static const char* const sides[4] = {"in front", "on the right", "behind", "on the left"};
        for (int side = 0; side < 4; ++side) {
            const auto s = sideStep(p.direction, side);
            // For pool objects (p & 0x800) the pool's edge is a wall (the
            // pool walls stand a level down; "Must be against a pool wall").
            const bool poolEdge = (pf & 0x800) && w.pool(t) != w.pool({t.x + s[0], t.y + s[1], t.level});
            const bool wall = placementWall(w, t, s[0], s[1]) || poolEdge;
            if ((wp & (1 << side)) && !wall)
                return fail(placementerror::NeedsWall, std::string("needs a wall ") + sides[side]);
            if ((wp & (0x100 << side)) && wall)
                return fail(placementerror::WallIntersect, std::string("no wall allowed ") + sides[side]);
        }
        // The two diagonals (FUN_008a7a60's sides 5 and 6, the wall locator
        // between two corners of the tile, FUN_0016530c; only for objects
        // with wall flags): 0x10 / 0x40 need that diagonal wall across the
        // tile, and one there is error 8 unless 0x20 / 0x80 allows it. In
        // our directions the 0x40 diagonal runs along direction + 1 and the
        // 0x10 one along direction + 7 (from the diagonal windows saved in
        // N002_Lot3, wall flags 0xC0: each passes where it stands).
        for (int k = 0; k < 2 && wp; ++k) {
            const int need = k ? 0x40 : 0x10, allow = k ? 0x80 : 0x20;
            const auto c = directionStep(p.direction + (k ? 1 : 7));
            const bool wall = w.diagonalSides(t) & (c[0] == c[1] ? 0x20 : 0x10);
            if ((wp & need) && !wall)
                return fail(placementerror::NeedsWall, "needs a diagonal wall");
            if (!(wp & allow) && wall)
                return fail(placementerror::WallIntersect, "no diagonal wall allowed");
        }
        // TestPlacementWithRespectToObjects (objectsError), then the height
        // test of TestPlaceInWorld (BV 0x8ad754, base 1.0 FUN_00ffd864): od 4
        // & vtable 0x2c(1) of a 0xFBA4DA53 object made for it, which is 1 <<
        // (1 - 1) (base 0xbc362c -> 0xbc3650; the slots' heights read the
        // same) -> bit 0 "on the floor", else 0x19 "Must place in one of
        // these places". Each part is tested whole (TSMTObjectCanSetPosition
        // BV 0x8add68: TSObjectCanSetPositionImpl part by part).
        std::string why;
        if (const int e = objectsError(vm, held[size_t(&part - p.parts.data())], mine, why))
            return fail(e, why);
        if (!(o.data[objdata::AllowedHeightFlags] & 1))
            return fail(placementerror::PlaceList, "needs a surface");
    }
    p.ok = true;
    return p;
}

bool shouldAutoRotate(VM& vm, ObjectId id) {
    for (ObjectId part : groupOf(vm, id))
        if (const Object* o = vm.object(part); o && (o->data[objdata::WallPlacementFlags] & 0x5F))
            return true;
    return false;
}

int autoRotate(VM& vm, ObjectId id, TilePos tile, int direction) {
    int d = normDir(direction);
    for (int k = 0; k < 4; ++k, d = normDir(d + 2))
        if (checkPlacement(vm, id, tile, d, false).ok)
            return d;
    return normDir(direction);
}

std::optional<std::pair<uint32_t, uint16_t>> diagonalTwin(VM& vm, ObjectId id) {
    const Object* lead = vm.object(id);
    if (!lead || !lead->definition)
        return std::nullopt;
    // The product: the multi-tile master's OBJD (the parts of a 2-tile door
    // carry no selectors), else the object's own.
    const ObjectDefinition* product = lead->definition;
    if (product->masterId())
        if (const auto master = vm.code().masterDefinition(lead->group, product->masterId()))
            if (const ObjectDefinition* d = vm.code().definition(lead->group, *master))
                product = d;
    const uint32_t diagonal = product->diagonalSelector();
    const uint32_t twin = diagonal ? diagonal : product->gridAlignedSelector();
    if (!twin)
        return std::nullopt;
    return vm.code().definitionByGuid(twin);
}

ObjectId createDiagonalTwin(VM& vm, ObjectId id) {
    const auto twin = diagonalTwin(vm, id);
    if (!twin)
        return 0;
    try {
        return createGhost(vm, twin->first, twin->second);
    } catch (const std::exception& err) {
        os2::logOnce("the diagonal twin of a held object", std::to_string(id), err.what());
        return 0;
    }
}

AutoRotation autoRotate(VM& vm, ObjectId id, ObjectId twin, TilePos tile, int direction) {
    const int d = normDir(direction);
    for (const ObjectId held : {id, twin}) {
        if (!held || !vm.object(held))
            continue;
        for (int k = 0, at = d; k < 4; ++k, at = normDir(at + 2))
            if (checkPlacement(vm, held, tile, at, false).ok)
                return {held, at};
    }
    return {id, d};
}

Placement currentPlacement(VM& vm, ObjectId id) {
    Placement p;
    const Object* lead = vm.object(id);
    if (!lead)
        return p;
    p.ok = lead->position.inWorld();
    p.level = lead->position.level;
    p.direction = lead->data[objdata::Direction];
    p.container = lead->data[objdata::ContainerId];
    p.slot = p.container ? lead->data[objdata::SlotNumber] : -1;
    for (ObjectId part : groupOf(vm, id))
        if (const Object* o = vm.object(part))
            p.parts.push_back({part, VM::tileOf(*o)});
    return p;
}

namespace {

// nTSWorld::WallSegmentRotate (BV 0x1563540): a quarter turn takes the
// edges 1 -> 2 -> 4 -> 8 -> 1 and swaps the diagonals.
int wallSegmentRotate(int s, int quarters) {
    for (int k = 0; k < (quarters & 3); ++k)
        s = ((s << 1) & 0x0E) | ((s >> 3) & 1) | ((s & 0x10) << 1) | ((s & 0x20) >> 1);
    return s;
}

// FUN_001645b0: the six wall segments of a tile, world-aligned.
int tileWallSegments(const World& w, TilePos t) {
    if (!w.contains(t))
        return 0;
    int s = 0;
    for (int k = 0; k < 4; ++k) { // segment 1 << k: the edge toward direction 2k - 2
        const auto d = directionStep(2 * k + 6);
        if (w.wallBetween(t, d[0], d[1], true))
            s |= 1 << k;
    }
    const int diagonals = w.diagonalSides(t);
    if (diagonals & 0x10)
        s |= 0x10;
    if (diagonals & 0x20)
        s |= 0x20;
    return s;
}

} // namespace

int16_t wallAdjacencyFlags(const VM& vm, const Object& o) {
    int flags = uint16_t(o.data[objdata::WallAdjacencyFlags]) & 0xFF00;
    if (!vm.world || !o.position.inWorld())
        return int16_t(flags);
    const TilePos t = VM::tileOf(o);
    const int turn = (-int(o.data[objdata::Direction]) / 2) & 3;
    const int s = wallSegmentRotate(tileWallSegments(*vm.world, t), turn);
    if (s & 0x01) flags |= 0x01;
    if (s & 0x02) flags |= 0x04;
    if (s & 0x04) flags |= 0x02;
    if (s & 0x08) flags |= 0x08;
    if (s & 0x10) flags |= 0x40;
    if (s & 0x20) flags |= 0x80;
    if (t.level == 0) {
        const int above = wallSegmentRotate(tileWallSegments(*vm.world, {t.x, t.y, t.level + 1}), turn);
        if (above & 0x01) flags |= 0x10;
        if (above & 0x04) flags |= 0x20;
    }
    return int16_t(flags);
}

void updateWallAdjacency(VM& vm, ObjectId id, bool force) {
    Object* o = vm.object(id);
    if (!o || (!force && !vm.functionTree(*o, 6)))
        return;
    const int16_t flags = wallAdjacencyFlags(vm, *o);
    if (flags == o->data[objdata::WallAdjacencyFlags])
        return;
    o->data[objdata::WallAdjacencyFlags] = flags;
    constexpr std::array<int16_t, 4> kNoParams{};
    vm.runFunction(id, 6, 1000, kNoParams);
}

void updateObjectsAdjacentToWalls(VM& vm, const World& before) {
    if (!vm.world)
        return;
    const World& w = *vm.world;
    auto changed = [&](TilePos t) { return tileWallSegments(before, t) != tileWallSegments(w, t); };
    std::vector<ObjectId> touched;
    for (const auto& [id, o] : vm.eachObject())
        if (o.position.inWorld() && !o.data[objdata::ContainerId] && changed(VM::tileOf(o)))
            touched.push_back(id);
    for (ObjectId id : touched)
        updateWallAdjacency(vm, id);
}

void applyPlacement(VM& vm, ObjectId id, const Placement& p) {
    for (const auto& part : p.parts) {
        Object* o = vm.object(part.id);
        if (!o)
            continue;
        o->data[objdata::Direction] = int16_t(p.direction);
        vm.setPosition(*o, part.at ? *part.at
                                   : Position{float(part.tile.x) + 0.5f, float(part.tile.y) + 0.5f, part.tile.level});
        if (p.container) {
            vm.setContainer(*o, p.container, int16_t(p.slot));
            if (const Object* c = vm.object(p.container)) {
                vm.setPosition(*o, c->position);
                o->data[objdata::Room] = c->data[objdata::Room];
                vm.noteRoomChanged(o->id);
            }
        } else {
            vm.setContainer(*o, 0, -1);
            // SetLocation: the room (definition 2) where it now stands.
            if (vm.world) {
                o->data[objdata::Room] = vm.world->room(VM::tileOf(*o));
                vm.noteRoomChanged(o->id);
            }
        }
    }
    // The pool ladder's and diving board's portals follow it (build mode
    // does the same, build_edit.cpp).
    vm.refreshObjectPortals();
    // The path planner hears of an object that blocks (AddThisToPathPlanner
    // puts in nothing for the others).
    if (std::any_of(p.parts.begin(), p.parts.end(), [&](const Placement::Part& part) {
            const Object* o = vm.object(part.id);
            return o && VM::blocksRouting(*o);
        }))
        vm.planner.reset();
    // SetPosition (BV 0xafd048): UpdateWallAdjacency (entry 6 when od 5
    // changes), OnPlacement (entry 9), then FinishEditing tells the room
    // (OnRoomChanged, entry 7).
    // (UpdateWallAdjacency(false) comes first.)
    for (const auto& part : p.parts)
        updateWallAdjacency(vm, part.id);
    for (const auto& part : p.parts)
        runEntry(vm, part.id, entry::Placement);
    for (const auto& part : p.parts)
        if (vm.object(part.id))
            finishMove(vm, part.id);
    (void)id;
}

void pickUp(VM& vm, ObjectId id) {
    for (ObjectId part : groupOf(vm, id)) {
        Object* o = vm.object(part);
        if (!o)
            continue;
        vm.setPosition(*o, Position{});
        vm.setContainer(*o, 0, -1);
        runEntry(vm, part, entry::Pickup);
    }
    vm.refreshObjectPortals();
    vm.planner.reset();
}

ObjectId createGhost(VM& vm, uint32_t group, uint16_t definitionInstance) {
    std::vector<ObjectId> before;
    for (const auto& [id, o] : vm.eachObject())
        before.push_back(id);
    Object& lead = vm.instantiate(group, definitionInstance);
    const ObjectId leadId = lead.id;
    for (ObjectId part : groupOf(vm, leadId)) {
        Object* o = vm.object(part);
        vm.setPosition(*o, Position{});
        vm.setContainer(*o, 0, -1);
    }
    // As cEdithObjectModule::MakeNewOutOfWorldObject (docs/primitives-mac.md
    // §4c): the Init of each part, then the current value = the OBJD's price.
    for (ObjectId part : groupOf(vm, leadId)) {
        runEntry(vm, part, 0); // Init
        if (Object* o = vm.object(part); o && o->definition)
            o->data[objdata::CurrentValue] = int16_t(std::min<int>(o->definition->price(), 32767));
    }
    return leadId;
}

ObjectId createTestObject(VM& vm, const std::string& spec) {
    const auto colon = spec.find(':');
    const uint32_t guid = uint32_t(std::stoul(spec.substr(0, colon), nullptr, 0));
    const auto at = vm.code().definitionByGuid(guid);
    if (!at)
        throw std::runtime_error("no OBJD with GUID " + spec.substr(0, colon));
    const ObjectId id = createGhost(vm, at->first, at->second);
    Object& o = *vm.object(id);
    for (size_t a = colon == std::string::npos ? spec.size() : colon + 1, b; a < spec.size(); a = b + 1) {
        b = spec.find(',', a);
        if (b == std::string::npos)
            b = spec.size();
        const std::string kv = spec.substr(a, b - a);
        const auto eq = kv.find('=');
        const size_t attr = std::stoul(kv.substr(0, eq));
        if (o.attributes.size() <= attr)
            o.attributes.resize(attr + 1);
        o.attributes[attr] = int16_t(std::stoi(kv.substr(eq + 1)));
    }
    return id;
}

void deleteGroup(VM& vm, ObjectId id) {
    for (ObjectId part : groupOf(vm, id)) {
        // What it holds goes with it.
        std::vector<ObjectId> held;
        for (const auto& [other, x] : vm.eachObject())
            if (x.data[objdata::ContainerId] == part && other != part)
                held.push_back(other);
        for (ObjectId h : held)
            deleteGroup(vm, h);
        runEntry(vm, part, entry::Delete);
        runEntry(vm, part, entry::Cleanup);
        // The thread goes with it: it is killed (the status the run gives a
        // thread whose object is gone) and dropped at the end of the tick (the
        // tick walks the map by iterator, and the map's nodes are only stable
        // while it is not erased).
        if (auto it = vm.threads.find(part); it != vm.threads.end()) {
            it->second.status = Thread::Status::Killed;
            vm.deadThreads.emplace_back(part, it->second.serial);
        }
        vm.removeObject(part);
    }
    vm.refreshObjectPortals();
    vm.planner.reset();
    computeLotData(vm);
}

int32_t funds(VM& vm) {
    const int16_t family = vm.globals[global::CurrentFamily];
    if (family && vm.families.count(family))
        return familyFunds(vm, family);
    return int32_t(vm.globals[global::BudgetDiv10000]) * 10000 + vm.globals[global::BudgetMod10000];
}

void addFunds(VM& vm, int32_t amount) {
    const int32_t now = funds(vm) + amount;
    const int16_t family = vm.globals[global::CurrentFamily];
    if (family && vm.families.count(family)) {
        setFamilyFunds(vm, family, now);
        return;
    }
    vm.globals[global::BudgetDiv10000] = int16_t(now / 10000);
    vm.globals[global::BudgetMod10000] = int16_t(now % 10000);
}

bool noCharge(VM& vm) {
    // Global 9 names the lot's family (LoadLot); a lot run without its
    // neighborhood still has one when it names one.
    if (vm.globals[global::CurrentFamily] != 0)
        return false;
    const int16_t selected = vm.globals[global::SelectedPersonId];
    return !(selected > 0 && vm.object(ObjectId(selected)));
}

bool canAfford(VM& vm, int64_t cost) {
    return cost <= 0 || noCharge(vm) || cost <= funds(vm);
}

void chargeFunds(VM& vm, int32_t amount) {
    if (amount && !noCharge(vm))
        addFunds(vm, amount);
}

bool buy(VM& vm, ObjectId ghost, const Placement& p, int price, std::string* why) {
    auto refuse = [&](const std::string& r) {
        if (why)
            *why = r;
        return false;
    };
    if (!p.ok)
        return refuse(p.reason.empty() ? "does not fit" : p.reason);
    if (!canAfford(vm, price))
        return refuse("not enough funds");
    applyPlacement(vm, ghost, p);
    runUserPlacement(vm, ghost);
    chargeFunds(vm, -price);
    for (const auto& part : p.parts)
        vm.startMain(part.id);
    computeLotData(vm);
    return true;
}

int sell(VM& vm, ObjectId id) {
    const int value = saleValue(vm, id);
    const auto group = groupOf(vm, id);
    if (group.size() == 1)
        runEntry(vm, id, entry::UserDelete);
    pickUp(vm, id);
    if (noCharge(vm))
        return computeLotData(vm), 0; // TotalCost is 0: nothing back
    addFunds(vm, value);
    computeLotData(vm);
    return value;
}

bool isFolderMarker(const Object& o) {
    switch (o.guid()) {
    case 0x4C859106: case 0xACC53DFF: case 0xEC85F0F3: // come and see
    case 0x000007C4: case 0xECFEDB37:                  // go here
    case 0x6CD4A8BC:                                   // grave marker
        return true;
    default:
        return false;
    }
}

bool objectMayBeInFoundation(VM& vm, const Object& o) {
    return isFolderMarker(o) ||
           (!o.person && o.definition && o.definition->objectType() == kObjectTypeSupport && vm.conformsToCeiling(o));
}

bool nonCeilingConforming(VM& vm, const Object& o) { return !isFolderMarker(o) && !vm.conformsToCeiling(o); }

std::optional<FireplaceLinks> fireplaceLinks(VM& vm, ObjectId id) {
    Object* o = vm.object(id);
    if (!o)
        return std::nullopt;
    std::optional<uint16_t> tree;
    for (const uint32_t g : {o->group, o->semiGroup})
        if (g && !tree)
            tree = vm.code().behaviorByName(g, "CT - Get Link Data");
    if (!tree)
        return std::nullopt;
    try {
        Thread t = vm.start(id, *tree, {}, id);
        for (int turn = 0; t.status == Thread::Status::Running && turn < 4; ++turn)
            vm.run(t, vm.instructionsPerTurn);
        if (t.status != Thread::Status::Finished)
            return std::nullopt;
        // The tree writes the object's temps (cTSObject vtable 0x4c reads
        // them back): its thread's are the object's own here.
        return FireplaceLinks{ObjectId(t.temps[0]), ObjectId(t.temps[1]), t.temps[2]};
    } catch (const VMError&) {
        return std::nullopt;
    }
}

bool isFireplaceObject(VM& vm, ObjectId id, bool anyPart) {
    const auto l = fireplaceLinks(vm, id);
    return l && (l->type == 1 || l->type == 4 || (anyPart && l->type >= 1 && l->type <= 7));
}

std::vector<ObjectId> linkedFireplace(VM& vm, ObjectId id) {
    std::vector<ObjectId> out;
    const auto start = fireplaceLinks(vm, id);
    if (!start)
        return {id};
    std::set<ObjectId> seen{id};
    for (ObjectId at = start->above; at != -1 && vm.object(at) && seen.insert(at).second;) {
        out.push_back(at);
        const auto l = fireplaceLinks(vm, at);
        at = l ? l->above : -1;
    }
    for (ObjectId at = start->below; at != -1 && vm.object(at) && seen.insert(at).second;) {
        out.push_back(at);
        const auto l = fireplaceLinks(vm, at);
        at = l ? l->below : -1;
    }
    out.push_back(id);
    return out;
}

void linkFireplaceObject(VM& vm, ObjectId id, ObjectId below, ObjectId above) {
    Object* o = vm.object(id);
    if (!o)
        return;
    const std::array<int16_t, 2> args{int16_t(below), int16_t(above)};
    try {
        if (auto t = vm.runFunction(id, 35, 1000, args, id); !t) {
            // No link tree: nothing keeps the chain (inferred: every piece of
            // the base game's fireplaces has one).
        }
    } catch (const VMError&) {
    }
}

int railingObjectModel(VM& vm, ObjectId id) {
    try {
        if (auto t = vm.runFunction(id, 48); t && t->status == Thread::Status::Finished && t->result)
            return t->temps[0];
    } catch (const VMError&) {
    }
    return 1;
}

int railingCost(VM& vm, ObjectId id) {
    if (!vm.railingPrice)
        if (ResourceIndex* index = vm.code().index())
            vm.railingPrice = railingToolPrice(*index);
    const int model = railingObjectModel(vm, id);
    if (model == 4)
        return 2 * vm.railingPrice;
    return model > 1 && model < 4 ? vm.railingPrice : 0;
}

void setRailingModel(VM& vm, ObjectId id, int model) {
    const int16_t args[1] = {int16_t(model)};
    try {
        vm.runFunction(id, 47, 1000, args);
    } catch (const VMError&) {
    }
}

bool railingSide(VM& vm, ObjectId id, float x, float y) {
    const Object* o = vm.object(id);
    if (!o || !o->definition)
        return false;
    // The binary compares one coordinate by the facing (0: x, 2: y, 4: x,
    // 6: y, its own axes); here the side of the facing's line the point is
    // on, left = counter-clockwise of the facing (inferred mapping of the
    // axes).
    const auto [fx, fy] = directionStep(o->data[objdata::Direction]);
    const bool left = -float(fy) * (x - o->position.x) + float(fx) * (y - o->position.y) > 0;
    return o->definition->objectType() == 10 ? !left : left;
}

void dayPassed(VM& vm) {
    for (const auto& [id, o] : vm.eachObject()) {
        if (!o.definition || o.person || multiTileGroup(o) != id)
            continue;
        const ObjectDefinition& d = *o.definition;
        if (d.field(0x8A) != 0) // self depreciating
            continue;
        const int dep = d.field(0x86) ? int16_t(d.field(0x86)) : int16_t(d.field(0x88));
        const int limit = int16_t(d.field(0x8C));
        if (dep == 0 && limit == 0)
            continue;
        Object* x = vm.object(id);
        int v = x->data[objdata::CurrentValue] - dep;
        if (v < limit)
            v = limit;
        x->data[objdata::CurrentValue] = int16_t(v);
    }
    computeLotData(vm); // cTSLot on the day's message 0x89
}

void computeLotData(VM& vm) {
    constexpr int16_t kMovementFlags = 43; // bit 0x10: stays after evict (STR# 0xCB #4)
    int32_t family = 0, lot = 0;
    for (const auto& [id, o] : vm.eachObject()) {
        if (!o.definition || o.person || multiTileGroup(o) != id)
            continue;
        // In the world: placed, or in a slot of something in the world.
        const Object* at = &o;
        for (int depth = 0; at && !at->position.inWorld() && at->data[objdata::ContainerId] && depth < 8; ++depth)
            at = vm.object(at->data[objdata::ContainerId]);
        if (!at || !at->position.inWorld())
            continue;
        const ObjectDefinition& d = *o.definition;
        const uint16_t type = d.objectType();
        if (type == 7 || (d.field(0x90) & 0x400))
            continue;
        const int value = o.data[objdata::CurrentValue];
        // GetBuildModeType: field 0x45 of the catalog definition, a
        // multi-tile part's master's (a door's parts have 0, the master 8).
        const ObjectDefinition* catalog = &d;
        if (const uint16_t master = d.masterId())
            if (auto instance = vm.code().masterDefinition(o.group, master))
                if (const ObjectDefinition* md = vm.code().definition(o.group, *instance))
                    catalog = md;
        const bool buyMode = catalog->field(0xCA) == 0;
        if (buyMode && !(o.data[kMovementFlags] & 0x10))
            family += value; // IsDeletedByEvict
        else if (!buyMode || type == 0xB || type == 0xC)
            lot += value + (type == 0xB || type == 0xC ? railingCost(vm, id) : 0); // stairs: + RailingCost
    }
    auto& v = vm.lotValues;
    if (!v.known)
        v.lot = int32_t(vm.globals[46]) * 1000 + vm.globals[47];
    v.lotObjects = lot;
    v.familyObjects = family;
    v.known = true;
    const int32_t total = lot + family;
    vm.globals[48] = int16_t(total / 1000);
    vm.globals[49] = int16_t(total - 1000 * (total / 1000));
    vm.globals[46] = int16_t(v.lot / 1000);
    vm.globals[47] = int16_t(v.lot - 1000 * (v.lot / 1000));
}

int saleValue(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    if (!o)
        return 0;
    const Object* lead = vm.object(multiTileGroup(*o));
    return std::max<int>(0, (lead ? lead : o)->data[objdata::CurrentValue]);
}

} // namespace os2::vm
