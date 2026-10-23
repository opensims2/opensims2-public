// What a Sim can see: Set to Next 24, "Visible object in my view within
// distance in temp0" (STR# 0xA4), which the idle look-at ("CT - Start Idle
// Look At", "CT - Start Routing Look At"), "Reaction - Banter" (Sims within
// 4 tiles) and the goal check of the Sims' wants use. By the Mac binary
// (cTSPerson::GetNextObjectInView, BV 0xacfa00; TrySetToNext calls it on a
// Sim, else error 0x1C): the distance, at most 16 tiles, makes a cone of
// tiles ahead of the Sim: rows 0 to r - 1 ahead (row 0 is the Sim's own
// tile), each 2 x row + 1 tiles to either side (at most r), inside a circle
// of radius r + 1 (facing a diagonal the grid turns 45 degrees: r * sqrt(2)
// + 1 rows of half-diagonal steps, 0.7071 x 0.7071). On those tiles, the
// objects in the world (not in a slot) other than me and the objects of
// type 5, in my room, not being deleted and with nothing in the way (the
// world's line test from my tile to theirs); by ID. The binary keeps the
// list while it iterates; a current object that is not on it ends the
// search (0, false). Inferred: where the half-tile steps round to, and that
// the line test is the walls' (vm::World::lineOfSight).

#include "vm.hpp"
#include "errors.hpp"

#include "formats/binreader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace os2::vm {

std::optional<ObjectId> nextVisibleObject(VM& vm, const Object& me, ObjectId current, int16_t distance) {
    if (!me.position.inWorld())
        return std::nullopt;
    const int dir = ((me.data[objdata::Direction] % 8) + 8) % 8;
    const auto [fx, fy] = directionStep(dir);
    const int rx = -fy, ry = fx; // across (the cone is symmetric)
    const bool diagonal = dir & 1;
    const int r = std::min<int>(distance, 16);
    const int rows = diagonal ? int(float(r) * 1.4142135f) + 1 : r;
    const int bound = diagonal ? (2 * r + 2) * (r + 1) : (r + 1) * (r + 1);
    const float step = diagonal ? 0.5f : 1.0f;
    const TilePos from = VM::tileOf(me);
    const int16_t myRoom = vm.world ? vm.world->room(from) : 0;
    // The cone's tiles (one can come twice), marked on a map of the box
    // round them.
    std::vector<std::array<int, 2>> cone;
    int x0 = from.x, y0 = from.y, x1 = from.x, y1 = from.y;
    for (int d = 0; d < rows; ++d)
        for (int l = std::max(-rows, -(2 * d + 1)); l <= std::min(rows, 2 * d + 1); ++l) {
            if (l * l + d * d > bound)
                continue;
            const int tx = int(std::floor(float(from.x) + 0.5f + float(l * rx + d * fx) * step));
            const int ty = int(std::floor(float(from.y) + 0.5f + float(l * ry + d * fy) * step));
            cone.push_back({tx, ty});
            x0 = std::min(x0, tx), y0 = std::min(y0, ty), x1 = std::max(x1, tx), y1 = std::max(y1, ty);
        }
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    std::vector<uint8_t> inCone(size_t(w) * size_t(h), 0);
    for (const auto& [tx, ty] : cone)
        inCone[size_t(tx - x0) * size_t(h) + size_t(ty - y0)] = 1;
    // The objects on those tiles (as VM::objectsOnTile has them: in the
    // world, not in a slot), in one walk over the objects by ID instead of
    // one per tile: the same objects, whatever the order the tiles come in.
    std::vector<ObjectId> seen;
    for (const Object* x : vm.objectTable()) {
        if (!x || !x->position.inWorld() || x->data[objdata::ContainerId] != 0)
            continue;
        const TilePos t = VM::tileOf(*x);
        if (t.level != from.level || t.x < x0 || t.x > x1 || t.y < y0 || t.y > y1 ||
            !inCone[size_t(t.x - x0) * size_t(h) + size_t(t.y - y0)])
            continue;
        const ObjectId id = x->id;
        if (id == me.id || vm.dyingObjects.contains(id))
            continue;
        if (x->definition && x->definition->objectType() == 5)
            continue;
        if (vm.world && (vm.world->room(t) != myRoom || !vm.world->lineOfSight(from, t)))
            continue;
        seen.push_back(id);
    }
    if (current != 0 && !std::binary_search(seen.begin(), seen.end(), current))
        return std::nullopt;
    auto next = std::upper_bound(seen.begin(), seen.end(), current);
    return next == seen.end() ? std::nullopt : std::optional<ObjectId>(*next);
}

namespace {

// Look At (0x6E): turns a Sim's head (and maybe spine) towards something.
// The editor's strings (Sims2.exe, in reverse order) list "Targeting ID in",
// "Targeting Camera", "default 3/4 height", slots ("targeting slot", "routing
// slot", "containment slot", " number %d"), ", including Spine", ", using
// duration in temp 0", ", no early exit", the turn towards/away speeds in
// degrees per second (or in temps 1/2), "STOP", ", ignoring Room" and
// ", ignoring Frustrum"; the game's errors include "Target object out of
// world." and "Requesting a look_at operation that is already running".
// As nTSSG::cAnimatable::try_look_at (BV 0x6a19f8) does it, on the base
// game's operands converted as LookAtParam::UpgradeVersion (0xac4990) does
// for node version 0 (both speeds = the s16 at 4 / 2; byte 0 bit 0x08 also
// sets byte 15 bit 0x02): not a Sim is error 0x42; more than 25 requests in
// a simulator turn on one Sim error 0x95; byte 0: 0x80 the camera is the
// target (nothing recorded here), 0x01 STOP, else the target (owner u8 at 1,
// data u16 at 2; none is error 0x17). The turn speeds (byte 4 x 2 degrees a
// second, temp 1 with byte 0 bit 0x08; byte 5 x 2, temp 1 / temp 2 with byte
// 15 bits 0x02 / 0x01) must be positive (error 0x51), STOP included. STOP
// ends the looks and is true. A target in another room is false unless byte
// 0 bit 0x20 ("ignoring Room"; the room is object data "room", inferred the
// same as the game's); without bit 0x40 the game also refuses a target
// outside what the Sim's head can turn to (a test on its skeleton, not
// done here). The duration: s16 at 6, or temp 0 with bit 0x10, the
// request's HoldTicks in the animation clock's ms, counted down once the
// head is on the target (0: a glance; -1 the game's DAT_02c0e324, no end:
// until the next request); with 0x04 the spine twists too, with 0x02 a
// target past the head's limits is held at them instead of given up
// (app/look_at.cpp, docs/anim-mac.md §12.3). The look runs on its own: the
// primitive is true at once. The look point: byte 14 (byte 8 its index) is
// 0 the target's box at three quarters of its height; 1 targeting, 2
// routing and 3 containment slot are the target's SLOT entries of type 4,
// 3 and 0 — the mapping the content measures: the "CT - Teach - Look
// Object" trees use 1 and 3, each index inside that type's count (a
// dummy's incision bone, a garden's plants, a Sim's spine) — and past the
// count they are error 0x1B. The request keeps the slot for the app to aim
// at (Object::LookAt). Not done: the event tree (u16 9: the look's states
// as events 9901-9906).
Exit lookAt(Context& c) {
    const auto& o = c.instruction.operands;
    Object& me = c.me();
    if (!me.person)
        throw gameError(0x42, "Look At");
    if (me.lookAtTick != c.vm.ticks)
        me.lookAtTick = c.vm.ticks, me.lookAtRequests = 0;
    if (++me.lookAtRequests > 25)
        throw gameError(0x95, "Look At");
    const uint8_t flags = o[0];
    std::optional<ObjectId> target;
    if (!(flags & 0x81)) {
        target = c.get(o[1], int16_t(le16(&o[2])));
        if (!c.vm.object(*target))
            throw gameError(err::NoObject, "Look At: object " + std::to_string(*target));
    }
    int towards = 0, away = 0;
    uint8_t flags15 = o[15];
    if (c.instruction.nodeVersion == 0) {
        towards = away = (int16_t(le16(&o[4])) / 2) & 0xFF;
        if (flags & 0x08)
            flags15 |= 0x02;
    } else {
        towards = o[4], away = o[5];
    }
    towards = flags & 0x08 ? c.thread.temps[1] : towards * 2;
    away = flags15 & 0x02 ? c.thread.temps[1] : flags15 & 0x01 ? c.thread.temps[2] : away * 2;
    if (towards <= 0 || away <= 0)
        throw gameError(0x51, "Look At: " + std::to_string(towards) + ", " + std::to_string(away) + " degrees a second");
    me.lookAwayRate = int16_t(away);
    if (flags & 0x01) {
        me.lookAt.reset();
        return Exit::True;
    }
    if (flags & 0x80)
        return Exit::True;
    const Object* x = c.vm.object(*target);
    // The look point's slot (byte 14, index byte 8): the target's SLOT
    // entries of the kind's type — 1 targeting, 2 routing, 3 containment
    // (4, 3 and 0; the content's Look Ats use 1 and 3). Past the type's
    // count, error 0x1B. Read before the room and head tests (the head
    // test needs the point; the room test's place in the binary was not
    // read).
    uint8_t slotKind = 0, slotIndex = 0;
    if (o[14] >= 1 && o[14] <= 3) {
        const uint16_t type = o[14] == 1 ? 4 : o[14] == 2 ? 3 : 0;
        size_t count = 0;
        if (x->definition)
            if (const Slots* t = c.vm.code().slots(x->group, x->definition->slotId()))
                for (const auto& s : t->slots)
                    count += s.type == type;
        if (o[8] >= count)
            throw gameError(err::SlotOutOfRange,
                            "slot " + std::to_string(o[8]) + " of object " + std::to_string(x->id));
        slotKind = o[14], slotIndex = o[8];
    }
    if (!x->position.inWorld() && !x->data[objdata::ContainerId]) {
        me.lookAt.reset();
        return Exit::False; // ours: nowhere to look
    }
    if (!(flags & 0x20) && x->data[objdata::Room] != me.data[objdata::Room])
        return Exit::False;
    const int ms = flags & 0x10 ? int(c.thread.temps[0]) : int(int16_t(le16(&o[6])));
    Object::LookAt look{*target, c.vm.ticks, ms < 0 ? 0 : ms * 30 / 1000, int16_t(towards), int16_t(away)};
    look.durationMs = ms < 0 ? -1 : ms;
    look.spine = flags & 0x04;
    look.noEarlyExit = flags & 0x02;
    look.serial = ++me.lookAtSerial;
    look.slotKind = slotKind;
    look.slotIndex = slotIndex;
    me.lookAt = look;
    return Exit::True;
}

} // namespace

void registerViewPrimitives(VM& vm) { vm.setPrimitive(0x6E, lookAt); }

} // namespace os2::vm
