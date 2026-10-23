#include "rooms.hpp"

#include "errors.hpp"
#include "placement.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

namespace os2::vm {

// cTSObject::InterpValue, owner 0x17 (BV): the room is never checked (there
// is no error 0x28 here); the values are asked of the world:
//   0 ambient light: the room's light level scaled to 0-100 (not known here),
//   1 and 7 outside: the room manager's lookup (nTSWorld, misnamed
//     cRoomManager::cRoomManager) returns -1 for room -1 or a room the world
//     does not have, and -1 reads as "not outside" (0),
//   2 level, 3 area (quarter tiles / 4), 4 pool (one tile of the room,
//     FloorTileInRoom, tested with nTSWorld::IsPool: vm/lot.cpp setLotRooms),
//   5 number of windows: objects whose room it is with category 0x16,
//   6 greenhouse (more than half its fences are greenhouse fences; none in
//     the base game's lots, 0),
//   past 7: error 0x29 "Bad room value requested.".
// Room 0 (a tile no room covers: fence lamps stand on such tiles) stays
// outside, as inferred before (370 failures in lot-run).
int16_t roomValue(const VM& vm, int16_t room, int16_t value) {
    if (value < 0 || value > 7)
        throw gameError(0x29, "room value " + std::to_string(value));
    if (value == 5) {
        int16_t n = 0;
        for (const auto& [id, o] : vm.eachObject())
            if (o.position.inWorld() && o.data[objdata::Room] == room && o.data[objdata::Category] == 0x16)
                ++n;
        return n;
    }
    if (value == 6)
        return 0;
    if (value == 7)
        value = 1;
    auto it = vm.rooms.find(room);
    if (it == vm.rooms.end()) {
        if (room == 0)
            return value == 1 ? 1 : 0;
        return 0; // the world has no such room
    }
    if (!(it->second.known & (1u << value)))
        throw VMError("room " + std::to_string(room) + ": value " + std::to_string(value) + " not known");
    return it->second.values[size_t(value)];
}

namespace {

// A door part that is a portal: in the binary IsPortal (BV 0xe1fe94) is an
// object with portal slot pairs; of a door's parts that is the one holding
// its routing slots, the lead (the 0,0 part; inferred from the saved
// attributes, see portalRooms).
bool isPortal(const Object& o) {
    if (!o.definition || o.definition->objectType() != kObjectTypeDoor)
        return false;
    const ObjectId lead = o.data[objdata::LeadTileObjectId];
    return !lead || lead == o.id;
}

// PortalDiagonalWallPlacement (BV 0xe1edd8): wall placement flags & 0x50.
bool diagonalDoor(const Object& o) {
    return o.definition && o.definition->objectType() == kObjectTypeDoor && (o.data[objdata::WallPlacementFlags] & 0x50);
}

} // namespace

bool portalRooms(const VM& vm, const Object& o, int16_t& a, int16_t& b) {
    if (!vm.world || !isPortal(o) || !o.position.inWorld())
        return false;
    const TilePos t = VM::tileOf(o);
    const int dir = o.data[objdata::Direction];
    // A diagonal door stands in the diagonal wall across its tile: its sides
    // are the tiles half a turn round from its facing (inferred).
    const auto front = diagonalDoor(o) ? directionStep(dir - 1) : std::array<int, 2>{0, 0};
    const auto back = directionStep(diagonalDoor(o) ? dir + 3 : dir + 4);
    const TilePos ta{t.x + front[0], t.y + front[1], t.level}, tb{t.x + back[0], t.y + back[1], t.level};
    if (!vm.world->contains(ta) || !vm.world->contains(tb))
        return false;
    a = vm.world->room(ta);
    b = vm.world->room(tb);
    return true;
}

void onRoomChanged(VM& vm, ObjectId id) {
    Object* o = vm.object(id);
    if (!o)
        return;
    const int16_t room = o->data[objdata::Room];
    if (room == o->notifiedRoom && !diagonalDoor(*o))
        return;
    o->notifiedRoom = room;
    if (!vm.functionTree(*o, kRoomChangedFunction))
        return;
    std::array<int16_t, 4> args{room, -1, 0, 0};
    if (isPortal(*o)) {
        // PortalOnRoomChanged / PortalGetRooms: only when the pair changed.
        int16_t a = 0, b = 0;
        if (!portalRooms(vm, *o, a, b) || (a == o->portalRooms[0] && b == o->portalRooms[1]))
            return;
        o->portalRooms = {a, b};
        args = {a, b, 0, 0};
    }
    vm.runFunction(id, kRoomChangedFunction, 1000, args, id);
}

void notifyRoomChanges(VM& vm) {
    // Only the objects whose room data was written since the last tick: the
    // port's room writers and Context::set mark them (VM::noteRoomChanged),
    // and the objects made or loaded by a lot mark themselves. The full scan
    // this replaces was 0.36 s of a 43,200-tick run; OS2_CHECK_ROOM_DIRTY=1
    // still runs it and reports an object the set missed.
    if (std::getenv("OS2_CHECK_ROOM_DIRTY")) {
        for (const auto& [id, o] : vm.eachObject())
            if (o.data[objdata::Room] != o.notifiedRoom && !vm.roomDirty().count(id))
                std::fprintf(stderr, "room dirty check: object %d not marked (room %d, notified %d)\n", int(id),
                             int(o.data[objdata::Room]), int(o.notifiedRoom));
    }
    std::vector<ObjectId> moved(vm.roomDirty().begin(), vm.roomDirty().end());
    vm.clearRoomDirty();
    for (ObjectId id : moved)
        onRoomChanged(vm, id);
}

void finishMove(VM& vm, ObjectId id) {
    std::vector<ObjectId> todo{id};
    for (size_t i = 0; i < todo.size() && i < 64; ++i) {
        onRoomChanged(vm, todo[i]);
        // What is in it (the containment index holds every contained object, by ID).
        for (const ObjectId cid : vm.containedObjects())
            if (const Object* c = vm.object(cid); c && c->data[objdata::ContainerId] == todo[i] && cid != todo[i])
                todo.push_back(cid);
    }
}

} // namespace os2::vm
