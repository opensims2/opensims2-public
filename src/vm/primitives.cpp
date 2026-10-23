#include "characters.hpp"
#include "engine.hpp"
#include "vm.hpp"
#include "wants.hpp"

#include "hood_bridge.hpp"
#include "autonomy.hpp"
#include "interactions.hpp"
#include "kill.hpp"
#include "ownership.hpp"
#include "placement.hpp"
#include "errors.hpp"
#include "sound.hpp"
#include "lua_scripts.hpp"

#include "formats/binreader.hpp"
#include "formats/log_once.hpp"

#include <algorithm>
#include <cstdlib>

namespace os2::vm {

namespace {

// Expression (0x02): operands lhs data u16, rhs data u16, flags u8,
// operator u8, lhs owner u8, rhs owner u8. Operators 0-17 are The Sims 1's;
// 18-20 are inferred from how the game's trees use them (|= to accumulate
// flags, ^= 1 to toggle a hand, := abs() on differences).
Exit expression(Context& c) {
    const auto& o = c.instruction.operands;
    const auto lhsData = int16_t(le16(&o[0]));
    const auto rhsData = int16_t(le16(&o[2]));
    const uint8_t op = o[5], lhsOwner = o[6], rhsOwner = o[7];

    const int32_t rhs = c.get(rhsOwner, rhsData);
    auto lhs = [&] { return int32_t(c.get(lhsOwner, lhsData)); };
    auto assign = [&](int32_t v) {
        c.set(lhsOwner, lhsData, int16_t(v));
        return Exit::True;
    };
    auto test = [](bool b) { return b ? Exit::True : Exit::False; };
    // Flags are numbered from 1. cTSObject::TryExpression only refuses a flag
    // above 16 as a u16 (error 0x0D); flag 0 shifts by 63 on the PPC: setting
    // or clearing it changes nothing, testing it tests the sign (sraw).
    auto bit = [&] {
        if (uint16_t(rhs) > 16)
            throw VMError("Expression: flag " + std::to_string(rhs) + " out of range");
        return rhs == 0 ? int32_t(0) : int32_t(1) << (rhs - 1);
    };

    switch (op) {
    case 0: return test(lhs() > rhs);
    case 1: return test(lhs() < rhs);
    case 2: return test(lhs() == rhs);
    case 3: return assign(lhs() + rhs);
    case 4: return assign(lhs() - rhs);
    case 5: return assign(rhs);
    case 6: return assign(lhs() * rhs);
    case 7:
        if (rhs == 0)
            throw VMError("Expression: division by zero");
        return assign(lhs() / rhs);
    case 8: return test(rhs == 0 && bit() == 0 ? int16_t(lhs()) < 0 : (lhs() & bit()) != 0);
    case 9: return assign(lhs() | bit());
    case 10: return assign(lhs() & ~bit());
    case 11: {
        const int16_t v = int16_t(lhs() + 1);
        assign(v);
        return test(v < rhs);
    }
    case 12:
        if (rhs == 0)
            throw VMError("Expression: modulo by zero");
        return assign(lhs() % rhs);
    case 13: return assign(lhs() & rhs);
    case 14: return test(lhs() >= rhs);
    case 15: return test(lhs() <= rhs);
    case 16: return test(lhs() != rhs);
    case 17: {
        const int16_t v = int16_t(lhs() - 1);
        assign(v);
        return test(v > rhs);
    }
    case 18: return assign(lhs() | rhs);
    case 19: return assign(lhs() ^ rhs);
    case 20: return assign(std::abs(rhs));
    case 21: // "Assign 32bit Value": two consecutive variables (low word first),
             // as "CT - Offer Job" copies a job GUID from the stack object's
             // definition into person data 89/90 (TryExpression BV 0xe4bb64
             // copies the element and the next one of both references)
        c.set(lhsOwner, lhsData, int16_t(rhs));
        c.set(lhsOwner, int16_t(lhsData + 1), c.get(rhsOwner, int16_t(rhsData + 1)));
        return Exit::True;
    default: throw gameError(err::BadOperator, "Expression: operator " + std::to_string(op)); // > 21 (TryExpression)
    }
}

// Operand layouts below follow The Sims 1's primitives where the game's own
// trees agree with them; enumerations are named after the editor's labels
// (STR# 0x7FE59FD0/0xNN, noted where used).

constexpr uint8_t kOwnerParam = 0x09;
constexpr uint8_t kOwnerStackObjectId = 0x0A;

// Sleep (0x00): u16 index of the parameter holding the ticks left, counted
// down once per tick (one call to VM::run). Idle for Input (0x11), which
// also looks at the interaction queue, is in queue.cpp.
// The Mac binary (cTSObject::TryIdle): a parameter out of range is error
// 0x08; 0 (or less) is true at once, else it counts down and waits.
Exit countDownParam(Context& c) {
    const auto param = int16_t(le16(&c.instruction.operands[0]));
    if (param < 0 || size_t(param) >= c.frame.params.size())
        throw gameError(err::StackNumberOutOfRange, "Sleep parameter " + std::to_string(param));
    // A long sleep is in the scheduler's list; an object Notify (0x31) took
    // out of it wakes with the counter at 0 (TryIdle asks the scheduler
    // whether it still sleeps: vtable 0x124, 0x11c). Objects only: a Sim's
    // notification is for its Idle for Input (queue.cpp). The tutorial's
    // controller sleeps until a button's event tree notifies it.
    Object& me = c.me();
    if (me.idleInterrupt && !me.person) {
        me.idleInterrupt = false;
        c.set(kOwnerParam, param, 0);
        return Exit::True;
    }
    const int16_t ticks = c.get(kOwnerParam, param);
    if (ticks <= 0)
        return Exit::True;
    c.set(kOwnerParam, param, int16_t(ticks - 1));
    return Exit::Yield;
}

// Random Number (0x08): destination (data u16 at 0, owner u16 at 2), limit
// (data u16 at 4, owner u16 at 6), as in The Sims 1 (the owners are u16:
// SwizzleParams in the Mac binary). Stores a value in [0, limit); the Mac
// binary (base and Bon Voyage) stores 0 for a limit of 0 and returns true
// (the "Random number range evaluated to zero." error is not thrown here).
// A negative limit stores 0 too (inferred). Bytes 8+ are not read.
Exit randomNumber(Context& c) {
    const auto& o = c.instruction.operands;
    const int16_t range = c.get(uint8_t(le16(&o[6])), int16_t(le16(&o[4])));
    int16_t value = 0;
    if (range > 0)
        value = int16_t(std::uniform_int_distribution<int>(0, range - 1)(c.vm.random));
    c.set(uint8_t(le16(&o[2])), int16_t(le16(&o[0])), value);
    return Exit::True;
}

// Break Point (0x0F): stops in the editor's debugger when (data u16, owner
// u16) is non-zero (all 0xFF in about half the uses); the game goes on through
// the true branch. Some trees point that branch back at the Break Point (472
// of 2,637) to halt the thread for good, which the game only ends by
// resetting the object; here that fails the thread.
Exit breakPoint(Context& c) {
    const auto& t = c.instruction.onTrue;
    if (t.kind == Behavior::TargetKind::Instruction && t.index == c.frame.pc)
        throw VMError("Break Point: halts here (its true branch loops to itself)");
    return Exit::True;
}

// Remove Object Instance (0x12), as the Mac binary's cTSObject::
// TryKillObject (docs/primitives-mac.md §4b): target u16 at 0 (STR# 0x9C: 0
// me, 1 stack object, 2 "target - UNUSED", which the binary takes as object
// ID 0; anything else is the game's error 0x09), flags u8 at 2. The 7 fixed
// GUIDs (car and pedestrian portals, the outside trash can, the mailbox) are
// never removed: false. A missing object: false. Otherwise the object goes
// on the kill queue (kill.hpp: it dies at the end of the tick with its parts
// and contents; without 0x02 the binary asks for its "cleanup", object flag
// 0x1000 = !cleanup, which only the kill itself reads) and the result is
// true, or "wait" when I removed myself without 0x01 "return immediately":
// my thread dies with me.
Exit removeObjectInstance(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t target = le16(&o[0]);
    if (target > 2)
        throw gameError(err::IllegalOwner, "Remove Object Instance target " + std::to_string(target));
    const ObjectId id = target == 0 ? c.thread.me : target == 1 ? c.frame.stackObject : 0;
    const Object* x = c.vm.object(id);
    if (!x)
        return Exit::False;
    static constexpr uint32_t kProtected[] = {0xCC7B08F8, 0x6C7B0905, 0xCC203477, 0x865A6812,
                                              0x81E6BEF9, 0xD20C0AEE, 0x39CCF441};
    if (std::find(std::begin(kProtected), std::end(kProtected), x->guid()) != std::end(kProtected))
        return Exit::False;
    queueKill(c.vm, id);
    return id == c.thread.me && !(o[2] & 0x01) ? Exit::Yield : Exit::True;
}

// Person data of a neighbor: its Sim's while it is on the lot, else the
// neighborhood's copy (as data owner 0x20 sees it).
std::array<int16_t, kPersonDataCount>& neighborData(VM& vm, int16_t id, Neighbor& n) {
    if (Object* sim = vm.neighborSim(id))
        return sim->person->data;
    return n.personData;
}

// Set to Next (0x1F), checked against the Mac binary (TrySetToNext, base
// 1.0 and Bon Voyage; docs/primitives-mac.md §4): GUID u32 at 0, search type
// = byte 4 & 0x7F (STR# 0xA4); byte 4 bit 7 names an explicit target (owner
// u8 at 5, data u8 at 7), else the target is the stack object ID; local u8
// at 6 (type 9). Byte 8 (flags: including disabled objects, a "where" object
// data filter in u16 9 / u16 11) is Bon Voyage only: base trees are node
// version 0, whose flags the game zeroes, so it is not read. The iterating
// searches start after the target's current value (0 = from the start) and
// write what they find into the target; when nothing is found they write 0
// and return false (the binary does so for the list searches; for the
// neighbor and family ones it is inferred). The verify searches (26, 29, 30)
// only test the stack object. Search types not in the binary's list are the
// game's error 0x1E. Implemented:
//   0 object, 1 person, 2 non-person, 4 object of the GUID, 18 object of the
//   GUID in temps 0 (low) / 1 (high), 6/12 category (object data 59) =
//   parameter 0 / temp 0, 16 vehicle (OBJD type; the GUID is not used), 25
//   object in the room in temp 0, 27 "Utility Using Object" (object data 76
//   or 77 non-zero): objects by ID;
//   3 the next part of the target's multi-tile object, round its group;
//   8 object on the target's tile and level, by ID after it, round to the
//   first again (target 0, out of the world or contained: false, unchanged);
//   9 object on the tiles +X, -X, +Y, -Y of the object in the local, in that
//   order, in the anchor's room (the binary keeps "the last valid one" of
//   those; here they are iterated, inferred);
//   14/15 the instance of the "job object" whose GUID is person data 89
//   (low) / 90 (high) of the Sim in the stack object / of the neighbor
//   whose ID is in the stack object (missing: error 0x37);
//   5 neighbor ID, 7 neighbor of the GUID's type, 17 neighbor of the family
//   in temp 0, 21/22/23/31 parent/sibling/child/spouse of the neighbor in
//   temp 0 (VM::familyTree; siblings are ties of their own), 28/35/36
//   neighbors whose relationship from the neighbor in temp 0 has variable 3
//   non-zero / variable 0 > temp 1 / variable 1 & temp 1 non-zero, 11 lot
//   ID; 24 visible object within temp 0 tiles (view.cpp); 10 career: false.
//   32 owner of the object in temp 0 (ownership.hpp);
//   19/20 the next/previous object of the multi-tile group of the object in
//   temp 0 (the module's list per group, walked in its order);
//   13 the nearest person to the person in temp 0, then the next nearest after
//   the target, with its distance and direction in temps 4 and 5.
// Not done: none of the base game's types up to 37 (the AL's 38/39 and the
// species ones 40-51 are above).
Exit setToNext(Context& c) {
    const auto& o = c.instruction.operands;
    const uint32_t guid = le32(&o[0]);
    const uint8_t type = o[4] & 0x7F;
    const bool explicitTarget = o[4] & 0x80;
    const uint8_t owner = explicitTarget ? o[5] : kOwnerStackObjectId;
    const auto data = explicitTarget ? int16_t(o[7]) : int16_t(0);
    const ObjectId current = c.get(owner, data);
    auto found = [&](int16_t id) {
        c.set(owner, data, id);
        return Exit::True;
    };
    auto none = [&] {
        c.set(owner, data, 0);
        return Exit::False;
    };
    auto result = [&](std::optional<int16_t> id) { return id ? found(*id) : none(); };

    // Neighbor searches go through neighbor IDs instead of objects.
    auto nextNeighbor = [&](auto matches) {
        auto& neighbors = c.vm.neighbors;
        for (auto it = neighbors.upper_bound(current); it != neighbors.end(); ++it)
            if (matches(it->first, it->second))
                return found(it->first);
        return none();
    };
    // Relationship from the neighbor in temp 0 to each candidate.
    auto relationFromTemp0 = [&](int16_t to) -> const std::vector<int16_t>* {
        auto rel = c.vm.relationships.find({VM::RelationshipKey{true, c.thread.temps[0]}, VM::RelationshipKey{true, to}});
        return rel == c.vm.relationships.end() ? nullptr : &rel->second;
    };
    // The common filter (SetToNextIsObjectEligible, BV 0x1be54dc): a valid
    // object, which an object Remove Object Instance queued to die at the
    // end of the tick is not (ValidateObject: object flag 0x10000 or 0x20000;
    // the reaper found the Sim it had just turned into an urn and reaped
    // it again), without flag 0x01 an object data 56 bit 0 or 2 set (the
    // game's "disabled" bits; the doc's BV filter names bit 0), without flag
    // 0x04 an object data 107 other than 0 (the flag is not printed by the
    // editor), and with flag 0x02 object data u16 9 equal to u16 11.
    // Node version 0 had neither the flags nor the filter (SetToNextParam
    // zeroes them on load): the base game's trees keep other bytes there
    // (the debug tombstone's "New Baby" filtered every object out, so no
    // one cared for the baby). That version is also where object data 56 is
    // the "dry clothes value" and 107 does not exist, so the two data tests
    // are the BV layout's, like the flags.
    const bool bvFilter = c.instruction.nodeVersion != 0 && o.size() > 12;
    const bool where = bvFilter && (o[8] & 0x02);
    const int16_t whereField = where ? int16_t(le16(&o[9])) : 0, whereValue = where ? int16_t(le16(&o[11])) : 0;
    auto eligible = [&](const Object& x) {
        if (dying(c.vm, x.id))
            return false;
        if (bvFilter && !(o[8] & 0x01) && (x.data[56] & 5) != 0)
            return false;
        if (bvFilter && !(o[8] & 0x04) && x.data[107] != 0)
            return false;
        return !where || (whereField >= 0 && size_t(whereField) < x.data.size() && x.data[size_t(whereField)] == whereValue);
    };
    // The person data 186 "species" (0 = the human; only Bon Voyage has the
    // field, so in the base game it reads 0): the Set to Next types 40-45
    // (0x28-0x2D) are the people and 46-51 (0x2E-0x33) the neighbors, each
    // taking one species; the type 1 (the people) and the type 5 (the neighbor
    // ID) take only the humans (docs/primitives-mac.md §0x1F).
    int16_t speciesLo = 0, speciesHi = 0;
    switch (type) {
    case 1: case 5: case 40: case 46: speciesLo = 0, speciesHi = 0; break;
    case 41: case 47: speciesLo = INT16_MIN, speciesHi = -1; break;
    case 42: case 48: speciesLo = 1, speciesHi = 2; break;
    case 43: case 49: speciesLo = 1, speciesHi = 1; break;
    case 44: case 50: speciesLo = 2, speciesHi = 2; break;
    case 45: case 51: speciesLo = 3, speciesHi = 3; break;
    default: break;
    }
    auto speciesOk = [&](const std::array<int16_t, kPersonDataCount>& pd) {
        const int16_t s = pd[persondata::Species];
        return s >= speciesLo && s <= speciesHi;
    };
    switch (type) {
    case 5: return nextNeighbor([&](int16_t id, Neighbor& n) { return speciesOk(neighborData(c.vm, id, n)); }); // neighbor id
    case 7: return nextNeighbor([guid](int16_t, const Neighbor& n) { return n.guid == guid; });      // neighbor of type
    case 46: case 47: case 48: case 49: case 50: case 51: // the neighbors of one species (0x2E-0x33)
        return nextNeighbor([&](int16_t id, Neighbor& n) { return speciesOk(neighborData(c.vm, id, n)); });
    case 17: {                                                                                        // neighbor of family in temp 0
        const int16_t family = c.thread.temps[0];
        return nextNeighbor([&](int16_t id, Neighbor& n) { return neighborData(c.vm, id, n)[persondata::FamilyNumber] == family; });
    }
    case 28:   // relationship variable 3 non-zero
    case 35: { // relationship variable 0 (daily) > temp 1
        if (!c.vm.neighbor(c.thread.temps[0]))
            return none();
        const int16_t limit = c.thread.temps[1];
        return nextNeighbor([&](int16_t id, const Neighbor&) {
            const auto* r = relationFromTemp0(id);
            if (!r)
                return false;
            return type == 28 ? r->size() > 3 && (*r)[3] != 0 : !r->empty() && (*r)[0] > limit;
        });
    }
    case 32: { // owner of the object in temp 0 with ownership >= temp 1 (ownership.hpp); none: -1
        if (!c.vm.object(c.thread.temps[0]))
            return Exit::False;
        const int16_t next = nextObjectOwner(c.vm, c.thread.temps[0], current, c.thread.temps[1]);
        c.set(owner, data, next);
        return next == -1 ? Exit::False : Exit::True;
    }
    case 26: return c.vm.object(c.frame.stackObject) ? Exit::True : Exit::False;                     // verify stack object
    case 29: return c.vm.neighbor(c.frame.stackObject) ? Exit::True : Exit::False;                   // verify neighbor id in stack object
    case 30: {                                                                                        // verify sim in stack object
        const Object* x = c.vm.object(c.frame.stackObject);
        return x && x->person ? Exit::True : Exit::False;
    }
    case 10: return Exit::False;                                                                      // career
    case 24: return result(nextVisibleObject(c.vm, c.me(), current, c.thread.temps[0]));             // visible object within temp 0 (view.cpp)
    case 11: // lot ID
    case 36: // neighbor with relationship variable 1 & temp 1
        if (type == 36 && !c.vm.neighbor(c.thread.temps[0]))
            return none();
        return result(nextInNeighborhood(c, type, current));
    case 21:   // parent of temp 0
    case 22:   // sibling of temp 0
    case 23:   // child of temp 0
    case 31: { // spouse of temp 0
        const int16_t of = c.thread.temps[0];
        std::set<int16_t> ids;
        if (auto it = c.vm.familyTree.find(of); it != c.vm.familyTree.end()) {
            const auto& ties = it->second;
            ids = type == 21 ? ties.parents : type == 22 ? ties.siblings : type == 23 ? ties.children : ties.spouses;
        }
        auto next = ids.upper_bound(current);
        return next == ids.end() ? none() : found(*next);
    }
    case 8: { // object on the same tile and level, after the target, as TrySetToNext walks it (Mac BV
              // 0xe47f..: FUN_00e1e8a8/FUN_00e1e484): the tile's objects (FirstValidObjectOnTile /
              // NextValidObjectOnTile; here by ID), each followed by what is in its slots, depth first,
              // so a Sim sitting on a chair and a plate on a counter are found too (the chair's "Shoo
              // Sims off Tile" skips the sitter, the bunny's "Don't Stand on Sims" finds it). After the
              // last one the walk starts again, so with nothing else there the target itself is found
              // ("Toilet - Standard Entry" and "MT - Get in Temp 0 & 1 - Max X & Y" stop when they are
              // back at the start). Each object found must pass the common filter and stand on the
              // target's tile and level. Target 0, missing or out of the world: false, and the target is
              // left as it is (the binary returns before writing).
        if (current == 0)
            return Exit::False;
        const Object* x = c.vm.object(current);
        if (!x || !x->position.inWorld() || dying(c.vm, x->id))
            return Exit::False;
        const TilePos at = VM::tileOf(*x);
        const auto ids = c.vm.objectsOnTileWithContents(at);
        const auto self = std::find(ids.begin(), ids.end(), current);
        if (self == ids.end())
            return Exit::False;
        const size_t from = size_t(self - ids.begin());
        for (size_t k = 1; k <= ids.size(); ++k) {
            const ObjectId id = ids[(from + k) % ids.size()];
            if (id == current)
                return found(current);
            const Object* y = c.vm.object(id);
            if (y && eligible(*y) && VM::tileOf(*y) == at)
                return found(id);
        }
        return found(current);
    }
    case 9: { // adjacent to the object in the local (TrySetToNext, Mac BV 0xe48300..): one object a side,
              // the last one on the tile (nTSWorld::LastValidObjectOnTile), in the anchor's room, the
              // sides in the order +X, +Y, -X, -Y; the search goes on from the side of the target's
              // tile (relative to the anchor), so a target on no side ends it. Walking every object of
              // the four tiles (by ID) instead, "Set to Next - Good Dining Chair", which goes round a
              // tile's objects with type 8 between, came back to a chair it had left and spun out the
              // instruction budget (N002_Lot22).
        const int16_t anchorId = c.get(0x19, o[6]);
        const Object* anchor = c.vm.object(anchorId);
        if (!anchor)
            throw gameError(err::NoTreeParameter, "Set to Next 9: object " + std::to_string(anchorId) + " does not exist");
        if (!anchor->position.inWorld())
            return none();
        const TilePos at = VM::tileOf(*anchor);
        const int16_t room = c.vm.world ? c.vm.world->room(at) : 0;
        int dx = 1, dy = 0;
        auto advance = [&] { // the next side; false after -Y
            if (dx >= 1)
                dx = 0, dy = 1;
            else if (dx < 0)
                dx = 0, dy = -1;
            else if (dy < 1)
                return false;
            else
                dx = -1, dy = 0;
            return true;
        };
        if (const Object* x = current ? c.vm.object(current) : nullptr) {
            const TilePos t = VM::tileOf(*x);
            dx = std::clamp(t.x - at.x, -1, 1), dy = std::clamp(t.y - at.y, -1, 1);
            if (!advance())
                return none();
        }
        const auto around = c.vm.objectsAround(at, 1); // the four sides' objects in one walk
        for (;;) {
            const TilePos t{at.x + dx, at.y + dy, at.level};
            if (!c.vm.world || c.vm.world->contains(t)) {
                const auto& ids = around[size_t(dx + 1) * 3 + size_t(dy + 1)];
                if (!ids.empty() && (!c.vm.world || c.vm.world->room(t) == room))
                    return found(ids.back());
            }
            if (!advance())
                return none();
        }
    }
    case 34: { // within one tile of the object in the local (the anchor's tile and the eight around it), by ID
        const Object* anchor = c.vm.object(c.get(0x19, o[6]));
        if (!anchor || !anchor->position.inWorld())
            return none();
        const TilePos at = VM::tileOf(*anchor);
        std::vector<ObjectId> ids;
        for (const auto& [id, x] : c.vm.eachObject()) {
            const TilePos t = VM::tileOf(x);
            if (id != anchor->id && x.position.inWorld() && x.data[objdata::ContainerId] == 0 && t.level == at.level &&
                std::abs(t.x - at.x) <= 1 && std::abs(t.y - at.y) <= 1)
                ids.push_back(id);
        }
        auto it = std::upper_bound(ids.begin(), ids.end(), current);
        return it == ids.end() ? none() : found(*it);
    }
    case 3: { // part of a multi-tile object
        // TrySetToNext (BV): the target must be a multi-tile object's part;
        // the next part of its group, round to the first, so a group left
        // with one part (the others burnt) gives the target itself: "Burn -
        // Spread - MT" loops on Set to Next until it is back at its start
        // and spun out the instruction budget when this said false.
        const Object* x = c.vm.object(current);
        if (!x)
            return none();
        const ObjectId group = multiTileGroup(*x);
        const auto& objects = c.vm.objects();
        auto inGroup = [&](const auto& entry) { return entry.first != current && multiTileGroup(entry.second) == group; };
        auto it = std::find_if(objects.upper_bound(current), objects.end(), inGroup);
        if (it == objects.end())
            it = std::find_if(objects.begin(), objects.end(), inGroup);
        if (it != objects.end())
            return found(it->first);
        // Alone: still a part of a multi-tile object when its OBJD says so
        // (editor field 10 "Multi-Tile master id", which the trees test too).
        const bool multiTile = x->definition && int16_t(x->definition->field(0x40 + 2 * 10)) > 0;
        return multiTile ? found(current) : none();
    }
    case 14:   // job object of the Sim in the stack object
    case 15: { // job object of the neighbor in the stack object
        const std::array<int16_t, kPersonDataCount>* pd = nullptr;
        if (type == 14) {
            const Object& sim = c.stackObject();
            if (!sim.person)
                throw gameError(err::NotAPerson, "object " + std::to_string(sim.id));
            pd = &sim.person->data;
        } else {
            Neighbor* n = c.vm.neighbor(c.frame.stackObject);
            if (!n)
                throw gameError(err::MissingNeighbor, "neighbor " + std::to_string(c.frame.stackObject));
            pd = &neighborData(c.vm, c.frame.stackObject, *n);
        }
        constexpr int16_t kJobGuidLow = 89, kJobGuidHigh = 90;
        const uint32_t g = uint16_t((*pd)[kJobGuidLow]) | uint32_t(uint16_t((*pd)[kJobGuidHigh])) << 16;
        if (!g || !c.vm.code().definitionByGuid(g))
            return none();
        for (const auto& [id, x] : c.vm.eachObject())
            if (x.guid() == g)
                return found(id);
        return none();
    }
    }

    std::function<bool(const Object&)> matches;
    uint32_t ofGuid = 0; // types 4 and 18
    switch (type) {
    case 0: matches = [](const Object&) { return true; }; break;                   // object
    case 1:                                                                        // person (the humans only)
    case 40: case 41: case 42: case 43: case 44: case 45:                          // the people of one species
        matches = [&](const Object& x) { return x.person.has_value() && speciesOk(x.person->data); };
        break;
    case 2: matches = [](const Object& x) { return !x.person; }; break;             // non-person
    case 4:                                                                         // object of type
    case 18: {                                                                      // object of type GUID in temp 0/1
        const uint32_t g = type == 4 ? guid
                                     : uint16_t(c.thread.temps[0]) | uint32_t(uint16_t(c.thread.temps[1])) << 16;
        if (!c.vm.code().definitionByGuid(g))
            return none();
        matches = [g](const Object& x) { return x.guid() == g; };
        ofGuid = g;
        break;
    }
    case 6:                                                                         // category == param 0
    case 12: {                                                                      // category == temp 0
        const int16_t category = type == 6 ? c.get(kOwnerParam, 0) : c.thread.temps[0];
        matches = [category](const Object& x) { return x.data[objdata::Category] == category; };
        break;
    }
    case 16:                                                                        // vehicle
        matches = [](const Object& x) { return x.definition && x.definition->objectType() == kObjectTypeVehicle; };
        break;
    case 25: {                                                                      // object in the room in temp 0
        const int16_t room = c.thread.temps[0];
        matches = [room](const Object& x) { return x.data[objdata::Room] == room; };
        break;
    }
    case 14:   // job object of the Sim in the target
    case 15: { // job object of the neighbor in the target
        // The first instance of the definition whose GUID is the Sim's
        // person data 89 (low word) and 90 (docs/primitives-mac.md §4);
        // none -> false. A missing neighbor is error 0x37.
        const std::array<int16_t, kPersonDataCount>* pd = nullptr;
        if (type == 14) {
            const Object* sim = c.vm.object(current);
            pd = sim && sim->person ? &sim->person->data : nullptr;
        } else {
            const Neighbor* n = c.vm.neighbor(current);
            if (!n)
                throw VMError("Set to Next: neighbor " + std::to_string(current) + " does not exist");
            const Object* sim = c.vm.neighborSim(current);
            pd = sim && sim->person ? &sim->person->data : &n->personData;
        }
        if (!pd)
            return none();
        const uint32_t g = uint16_t((*pd)[89]) | uint32_t(uint16_t((*pd)[90])) << 16;
        for (const auto& [id, x] : c.vm.eachObject())
            if (g && x.guid() == g)
                return found(id);
        return none();
    }
    case 27:                                                                        // utility using object
        matches = [](const Object& x) { return x.data[76] != 0 || x.data[77] != 0; };
        break;
    case 33:                                                                        // hauntable (the definition's field 0x1C)
        matches = [](const Object& x) { return x.definition && x.definition->field(0x1C) != 0; };
        break;
    case 37: { // want category in temp 0: OBJD field 0x2F (offset 0x9E), as the want trees' categories (BV TrySetToNext 0x25)
        const int16_t category = c.thread.temps[0];
        matches = [category](const Object& x) { return x.definition && int16_t(x.definition->field(0x9E)) == category; };
        break;
    }
    case 38: { // object definition of the selector category in temp 0 and the
               // subcategory in temp 1: OBJD fields 75/76, which the Apartment
               // Life content uses ("Stereo - RadioStation - Pop" 117/4,
               // "Token - Badge - Gardening" 121/8, accessories 153/0-4). The
               // binary walks its definition list by index (the first
               // appearance of each GUID, as the OBJD scan here), keeps the
               // next index (1-based) in the target and writes the GUID to
               // temps 2 (low) and 3 (high). A negative temp matches anything
               // (temp 0 negative: the subcategory is not read).
        const int16_t category = c.thread.temps[0], subcategory = c.thread.temps[1];
        const auto& defs = c.vm.code().definitions();
        for (int32_t at = current < 1 ? 0 : current; at < int32_t(defs.size()); ++at) {
            const auto& d = defs[size_t(at)];
            if (category >= 0 && (d.category != category || (subcategory >= 0 && d.subcategory != subcategory)))
                continue;
            if (at >= 0x7FFF)
                throw gameError(err::ValueOutOfRange, "Set to Next 38: definition " + std::to_string(at + 1));
            c.set(owner, data, int16_t(at + 1));
            c.thread.temps[2] = int16_t(uint16_t(d.guid));
            c.thread.temps[3] = int16_t(uint16_t(d.guid >> 16));
            return Exit::True;
        }
        return none();
    }
    case 39: { // next neighbor the neighbor in temp 0 has a relationship with:
               // its relationship table's variable 1 (lifetime) >= 0, skipping
               // the neighbor itself and the ones the common filter rejects
               // (dying, object data 56 bits 0/2, data 107, the "where" field).
               // The walk stops at a neighbor with no object in the lot, as
               // the binary does (its object id is 0).
        const int16_t from = c.thread.temps[0];
        if (!c.vm.neighbor(from))
            throw gameError(err::NoSimInTemp0, "Set to Next 39: temp 0 is not a neighbor id: " + std::to_string(from));
        auto& neighbors = c.vm.neighbors;
        for (auto it = neighbors.upper_bound(current); it != neighbors.end(); ++it) {
            const int16_t id = it->first;
            if (id == from)
                continue;
            const auto* rel = relationFromTemp0(id);
            if (rel && rel->size() > 1 && (*rel)[1] < 0)
                continue;
            const Object* sim = c.vm.neighborSim(id);
            if (!sim)
                break;
            if (!eligible(*sim))
                continue;
            return found(id);
        }
        return none();
    }
    case 13: { // nearest person to the person in temp 0 ("Reaction - Banter", its only user)
        // PC case 0xD @ 0x9393b2: the anchor is GetObjectByID(temp 0) (missing:
        // error 0x17) and must be a person (QueryInterface cITSPerson
        // 0x5A69D3D5; else error 0x1C). The candidates are the people, nearest
        // to the anchor first (the module's spatial query; here the module's
        // list order breaks the ties), each passing the common filter (dying,
        // object data 56 bits 0/2, 107, the "where" pair; 0x9394f0..0x939591).
        // The found one goes to the target, its distance and direction from the
        // anchor to temps 4 and 5 (the binary rounds a float distance,
        // 0x00e5c948, and an angle, 0x00c3e7b0, at 0x9395ec..0x93960d; here the
        // same whole-tile distance and 0-7 face as Get Distance To / Get
        // Direction To). The target is the cursor: the next call goes on from
        // the person after it, 0 starts at the nearest (the binary's target-0
        // path only writes zeroed temps; the only user enumerates from 0 and
        // the label is "the nearest person"). The anchor itself is not a
        // candidate (inferred: a Sim does not banter with itself).
        const int16_t anchorId = c.thread.temps[0];
        const Object* anchor = c.vm.object(anchorId);
        if (!anchor)
            throw gameError(err::NoObject, "Set to Next 13: object " + std::to_string(anchorId) + " does not exist");
        if (!anchor->person)
            throw gameError(err::NotAPerson, "Set to Next 13");
        auto distanceTo = [&](const Object& x) {
            const double dx = x.position.x - anchor->position.x, dy = x.position.y - anchor->position.y;
            const double dz = 10.0 * (x.position.level - anchor->position.level);
            return long(std::trunc(std::sqrt(dx * dx + dy * dy + dz * dz)));
        };
        std::vector<std::pair<long, const Object*>> people;
        for (const auto& entry : c.vm.objectList())
            if (const Object* x = c.vm.object(entry.second); x && x->person && x->id != anchorId && eligible(*x))
                people.emplace_back(distanceTo(*x), x);
        std::stable_sort(people.begin(), people.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        size_t at = 0;
        if (current != 0) {
            const auto found = std::find_if(people.begin(), people.end(),
                                            [&](const auto& p) { return p.second->id == current; });
            at = found == people.end() ? people.size() : size_t(found - people.begin()) + 1;
        }
        if (at >= people.size())
            return none();
        const Object& next = *people[at].second;
        c.set(owner, data, next.id);
        c.thread.temps[4] = int16_t(std::min(32767L, people[at].first));
        c.thread.temps[5] = int16_t(directionOf(next.position.x - anchor->position.x,
                                               next.position.y - anchor->position.y));
        return Exit::True;
    }
    case 19:   // next object of the multi-tile group of the object in temp 0
    case 20: { // previous one: "Set to Next" / "Set to Prev" (0x7FFD4F2D:2006/2007, the fireplace's group)
        // PC 0x939b00.. (type 0x13) and 0x939d81.. (0x14): GetObjectByID(temp
        // 0) through the object module (+0x74); missing, or for 19 one queued
        // to die (object flags 0x10000/0x20000), writes 0 to the target and
        // fails. Then the module's list of the anchor's group, keyed by the
        // object's +0xe4 (+0x14c: one list per group, none for an object in
        // no group), walked in the module's list order, forward (19) or
        // backward (20), skipping objects that are gone or dying. Both
        // wrappers live in the fireplace's group and a fireplace's parts are
        // a multi-tile group; the modular stairs' delete handler (0x9234f0)
        // walks the same list to take a staircase's other parts, which a
        // per-level or per-room list would not give (it would take the other
        // stairs with it). Nothing else uses the two types: the AL and the
        // base game have one use of each, those wrappers.
        const int16_t anchorId = c.thread.temps[0];
        const Object* anchor = c.vm.object(anchorId);
        if (!anchor || (type == 19 && dying(c.vm, anchorId))) {
            c.set(owner, data, 0);
            return Exit::False;
        }
        const ObjectId lead = multiTileGroup(*anchor);
        std::vector<const Object*> group;
        for (const auto& entry : c.vm.objectList()) {
            const Object* x = c.vm.object(entry.second);
            if (x && x->data[objdata::LeadTileObjectId] == lead)
                group.push_back(x);
        }
        // No group (the only object under that data): the binary's key is
        // negative and it returns false with the target left as it is.
        if (group.size() < 2)
            return Exit::False;
        const auto alive = [&](size_t i) { return !dying(c.vm, group[i]->id); };
        if (current == 0) {
            if (type == 19) {
                for (size_t i = 0; i < group.size(); ++i)
                    if (alive(i))
                        return found(group[i]->id);
            } else {
                for (size_t i = group.size(); i-- > 0;)
                    if (alive(i))
                        return found(group[i]->id);
            }
            return Exit::False;
        }
        // The current target must be in the list: the binary ends there with
        // (target != 0), i.e. true and the target left as it is.
        const auto at = std::find_if(group.begin(), group.end(), [&](const Object* x) { return x->id == current; });
        if (at == group.end())
            return Exit::True;
        c.set(owner, data, 0);
        const size_t i = size_t(at - group.begin());
        if (type == 19) {
            for (size_t k = i + 1; k < group.size(); ++k)
                if (alive(k))
                    return found(group[k]->id);
            return Exit::False;
        }
        // The backward walk starts two places before the current one (0x939f04
        // DEC EBX, then 0x939f12 LEA EBP,[EBX-1]): the object just before it
        // is skipped, a real off-by-one in the game.
        if (i >= 2)
            for (size_t k = i - 1; k-- > 0;)
                if (alive(k))
                    return found(group[k]->id);
        return Exit::False;
    }
    default:
        if (type <= 37)
            throw VMError("Set to Next: search type " + std::to_string(type) + " not implemented");
        throw gameError(err::BadSearchType, "Set to Next type " + std::to_string(type));
    }
    // People (1, and the species ones 40-45) and a definition's instances
    // (4, 18) are their own vectors in TrySetToNext (GetNextPerson BV
    // 0xe2521c; the selector's instances): the one after the current one,
    // which must be in it (a current object that is not, as "Dartboard -
    // Shoo Sims" leaves the dartboard there between people, ends the search:
    // false); 0 starts at the first. Their order: Object::appendOrder.
    const bool people = type == 1 || (type >= 40 && type <= 45);
    if (people || type == 4 || type == 18) {
        // The candidates by ID, as a walk over every object would meet them:
        // the people, or the objects of the GUID (both kept by the VM).
        std::vector<const Object*> v;
        for (const ObjectId id : people ? c.vm.people() : c.vm.objectsOfGuid(ofGuid))
            if (const Object* x = c.vm.object(id); x && matches(*x))
                v.push_back(x);
        std::sort(v.begin(), v.end(), [](const Object* a, const Object* b) { return a->appendOrder < b->appendOrder; });
        size_t k = 0;
        // Type 18 from a Sim (base game 1.0, cTSObject_TrySetToNext
        // 0xb577b0, walks the lot's list from the current object): the
        // school trees start from the student ("Sub - Is Stack Obj my
        // Student?", "Sub - Set to Sim's School", "Sub - Send to School"),
        // who is not in the JobData's instances, and the game finds the
        // JobData; so a Sim before every object of the GUID is read as the
        // start (inferred: the Sims come first in that list; with the BV's
        // "not in it: false" Alexander, N001_Lot11, never went to his
        // private school; live QA 15, round 18).
        const Object* from = type == 18 && current > 0 ? c.vm.object(current) : nullptr;
        if (from && from->person && !matches(*from)) {
        } else if (current != 0) {
            const auto at = std::find_if(v.begin(), v.end(), [&](const Object* x) { return x->id == current; });
            if (at == v.end())
                return none();
            k = size_t(at - v.begin()) + 1;
        }
        for (; k < v.size(); ++k)
            if (eligible(*v[k]))
                return found(v[k]->id);
        return none();
    }
    // The object module's list (TrySetToNext, BV 0xe47288: the objects
    // after the current one in that list; a current one not in it, or 0,
    // starts at the front), not ID order: a lot's list (ObjM) can have a
    // multi-tile part before its lead, and the ghost's "Function - Goal
    // Check" steps from the stereo's part to its lead (object data 19) and
    // on with Set to Next 6: by ID it found the part again and spun out the
    // instruction budget (N001_Lot68 with visitors).
    const auto& list = c.vm.objectList();
    auto it = list.begin();
    if (const Object* cur = current > 0 ? c.vm.object(current) : nullptr)
        it = list.upper_bound(cur->listOrder);
    for (; it != list.end(); ++it)
        if (const Object* x = c.vm.object(it->second); x && matches(*x) && eligible(*x))
            return found(x->id);
    return none();
}

// Test Object Type (0x20), as the Mac binary has it (TryTestObjectType,
// base 1.0 and Bon Voyage): GUID u32 at 0, object (data u16 at 4, owner u8
// at 6), flags u8 at 7. GUID 0x4C7CAB2B stands for the GUID of my temporary
// inventory token ("the GUID of the temporary inventory token"). Flag 0x04
// tests no object: if the GUID has a definition, its high word goes to temp 1
// and the low word to temp 0 and the result is true, else false. Otherwise
// true when the GUID is the object's definition's (0x01: the definition's
// original GUID, OBJD 0xCC) or, failing that, its multi-tile master's; 0x02:
// the ID is a neighbor ID (the neighbor's definition). An object (or
// neighbor) that does not exist is false, not an error.
constexpr uint32_t kTempTokenGuid = 0x4C7CAB2B;

Exit testObjectType(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[7];
    uint32_t guid = le32(&o[0]);
    if (guid == kTempTokenGuid)
        guid = c.me().tempToken.loaded ? c.me().tempToken.token.guid : 0;
    auto& code = c.vm.code();
    if (flags & 0x04) {
        if (!guid || !code.definitionByGuid(guid))
            return Exit::False;
        c.thread.temps[1] = int16_t(guid >> 16);
        c.thread.temps[0] = int16_t(guid & 0xFFFF);
        return Exit::True;
    }
    const int16_t id = c.get(o[6], int16_t(le16(&o[4])));
    const ObjectDefinition* def = nullptr;
    uint32_t group = 0;
    if (flags & 0x02) {
        const Neighbor* n = c.vm.neighbor(id);
        if (!n)
            return Exit::False;
        if (const auto d = code.definitionByGuid(n->guid)) {
            def = code.definition(d->first, d->second);
            group = d->first;
        }
        if (!def)
            return n->guid == guid && !(flags & 0x01) ? Exit::True : Exit::False;
    } else {
        const Object* x = c.vm.object(id);
        if (!x || !x->definition)
            return Exit::False;
        def = x->definition;
        group = x->group;
    }
    auto matches = [&](const ObjectDefinition& d) { return (flags & 0x01 ? d.originalGuid() : d.guid()) == guid; };
    if (matches(*def))
        return Exit::True;
    if (def->masterId() != 0 && int16_t(def->subIndex()) != -1)
        if (const auto m = code.masterDefinition(group, def->masterId()))
            if (const ObjectDefinition* md = code.definition(group, *m); md && matches(*md))
                return Exit::True;
    return Exit::False;
}

// Create New Object Instance (0x2A), as the Mac binary has it
// (TryCreateObject, base 1.0 0xb56320 and Bon Voyage 0xe34b2c): GUID u32 at
// 0, where s8 at 4 (STR# 0xA7), flags u8 at 5, local u8 at 6 (where 8 and
// 9), slot u8 at 9 (where 4 and 10), flags u8 at 10; bytes 7-8 are not read
// here. Where: 0 in front of me (my place + a tile the way I face, eight
// ways), 1 "on top of me" and 2 "in my hand" (both: my slot 0), 3 in front
// of the stack object (in its room), 4 in slot (byte 9) of the stack object,
// 5 underneath me (my place), 6 out of world, 7 below the object whose ID is
// parameter 0 (no parameters: error 0x08), 8 below the object in the local,
// 9 next to me in the direction in the local plus mine (N/E/S/W only; false
// with a wall between, FUN_00b49108), 10 in slot (byte 9) of the object in
// temp 0. A place is the anchor's own (its FTilePt, not its tile's centre).
// Flags (byte 5): 0x01 do not duplicate (false when an object of that GUID
// is already on the tile), 0x02 the new object's Main gets my ID in
// parameter 0 and my stack object, 0x04 a Sim of the neighbor in the stack
// object (false without one), 0x08 false when the tile is not empty, 0x10
// temp 0 goes to the Main's parameter 0 (0x02 with 0x10: error 0x36), 0x20
// the new object turns the way the stack object faces (Orient), 0x40 the
// GUID is that of my temporary inventory token, 0x80 the GUID is in temps 0
// (low) / 1 (high). Byte 10: 0x01 "moving in a new Sim" (nothing here),
// 0x02 copies the design materials of the object in temp 5 (missing: error
// 0x0A). False, with no error: no definition for the GUID, a dead definition
// (OBJD type 0x11), nowhere to go, off the lot, not in the anchor's room, a
// full slot or one the container does not have; a template definition (type
// 0x10) is error 0x90. The object is made out of the world (its Init runs
// there, with VM::initCreatedObjects: every part's Init and its Main
// thread, as FreeSO's VMEntity.Init; an Init that fails does not fail the
// creator, it is kept in createdInits), then tested where it goes
// (TSObjectCanSetPosition with its own facing, or the slot's
// TSObjectCanPlaceInSlot) and deleted with a false result when it does not
// fit, else put there (SetPosition: every part, its wall adjacency, the
// "placement" tree; or PlaceInSlot). Otherwise the stack object becomes the
// new object and the result is true.
Exit createObjectInstance(Context& c) {
    const auto& o = c.instruction.operands;
    const int8_t where = int8_t(o[4]);
    const uint8_t flags = o[5], flags2 = o[10];
    if (where < 0 || where > 10)
        throw VMError("Create New Object Instance: placement " + std::to_string(where) + " not implemented");
    if ((flags & 0x12) == 0x12)
        throw gameError(err::MoreThanOnePassingValue);
    uint32_t guid = le32(&o[0]);
    if (flags & 0x80)
        guid = uint16_t(c.thread.temps[0]) | uint32_t(uint16_t(c.thread.temps[1])) << 16;
    if (flags & 0x40)
        guid = c.me().tempToken.loaded ? c.me().tempToken.token.guid : 0;
    const Neighbor* neighbor = nullptr;
    if (flags & 0x04) {
        neighbor = c.vm.neighbor(c.frame.stackObject);
        if (!neighbor)
            return Exit::False;
        guid = neighbor->guid;
    }
    const auto defAt = c.vm.code().definitionByGuid(guid);
    const ObjectDefinition* def = defAt ? c.vm.code().definition(defAt->first, defAt->second) : nullptr;
    if (!def)
        return Exit::False;
    if (def->objectType() == 0x10)
        throw gameError(err::TemplateObject, hexString(guid));
    if (def->objectType() == 0x11)
        return Exit::False;
    if (std::getenv("OS2_LOG_CREATE")) // who makes what, for looking into
        std::fprintf(stderr, "t%llu %d in \"%s\" #%u creates \"%s\"\n", (unsigned long long)c.vm.ticks, c.me().id,
                     c.frame.behavior->fileName.c_str(), c.frame.pc, def->fileName.c_str());
    const Object* materialSource = nullptr;
    if (flags2 & 0x02) {
        materialSource = c.vm.object(c.thread.temps[5]);
        if (!materialSource)
            throw gameError(err::NoTreeParameter, "object " + std::to_string(c.thread.temps[5]) + " in temp 5");
    }

    // Where it goes: a place, or a containment slot (base 1.0 TryCreateObject
    // 0xb56320, BV 0xe34b2c). The place is the anchor's own (its FTilePt,
    // cTSObject +0x1c0: the exact x, y, not its tile's centre) moved a tile
    // the way it faces for 0 and 3 (eight ways), and the anchor's level; the
    // new object is not turned to anything (flag 0x20 below).
    Object& me = c.me();
    std::optional<Position> at;
    const Object* container = nullptr;
    int slot = o[9];
    int16_t room = -1;        // cases 3, 4, 10: the place must be in the anchor's room
    int backToMe = -1;        // case 9: no wall between the place and me
    auto anchor = [&](const Object* x) {
        if (!x)
            throw gameError(err::InvalidObjectId);
        if (x->position.inWorld())
            at = Position{x->position.x, x->position.y, x->position.level};
        return x;
    };
    auto step = [&](int d) {
        if (!at)
            return;
        const auto [dx, dy] = directionStep(((d % 8) + 8) % 8);
        at->x += float(dx), at->y += float(dy);
    };
    switch (where) {
    case 0: anchor(&me); step(me.data[objdata::Direction]); break;
    case 1:
    case 2: container = &me, slot = 0; break;
    case 5: anchor(&me); break;
    case 3: {
        const Object* x = anchor(&c.stackObject());
        step(x->data[objdata::Direction]);
        room = x->data[objdata::Room];
        break;
    }
    case 4: container = &c.stackObject(), room = container->data[objdata::Room]; break;
    case 6: break;
    case 7:
        if (c.frame.params.empty())
            throw gameError(err::StackNumberOutOfRange, "no parameter 0");
        anchor(c.vm.object(c.frame.params[0]));
        break;
    case 8:
        if (o[6] >= c.frame.locals.size())
            throw gameError(err::StackNumberOutOfRange, "local " + std::to_string(o[6]));
        anchor(c.vm.object(c.get(0x19, o[6])));
        break;
    case 9: {
        anchor(&me);
        const int local = c.get(0x19, o[6]);
        const int d = ((local + me.data[objdata::Direction]) % 8 + 8) % 8;
        if (d % 2 == 0) // N/E/S/W only (the others: my place)
            step(d);
        backToMe = ((local + me.data[objdata::Direction] + 4) % 8 + 8) % 8;
        break;
    }
    case 10:
        container = c.vm.object(c.thread.temps[0]);
        if (!container)
            throw gameError(err::InvalidObjectId);
        room = container->data[objdata::Room];
        break;
    }
    if (container) {
        size_t slots = 0;
        if (const Slots* t = container->definition ? c.vm.code().slots(container->group, container->definition->slotId())
                                                   : nullptr)
            for (const auto& sl : t->slots)
                slots += sl.type == 0;
        if (size_t(slot) >= slots && !(container->person && slot < 2))
            return Exit::False;
        if (c.vm.containedIn(container->id, int16_t(slot)))
            return Exit::False;
    }
    const std::optional<TilePos> tile =
        at ? std::optional<TilePos>(TilePos{int(std::floor(at->x)), int(std::floor(at->y)), at->level}) : std::nullopt;
    if (tile && (flags & 0x09)) {
        for (ObjectId id : c.vm.objectsOnTile(*tile)) {
            if (id == me.id)
                continue; // "non-empty" means another object (inferred)
            const Object* x = c.vm.object(id);
            if ((flags & 0x08) || (x && x->guid() == guid))
                return Exit::False;
        }
    }
    // Off the lot: false (WorldDB 0xc8 on the place's tile).
    if (tile && c.vm.world && !c.vm.world->contains(*tile))
        return Exit::False;
    // The anchor's room (cases 3, 4 and 10, the room manager's room at the
    // place, definition 2).
    if (tile && room != -1 && c.vm.world && c.vm.world->room(*tile) != room)
        return Exit::False;
    // Case 9: a wall between the place and me (FUN_00b49108, the place's
    // walls turned to the way back to me: the binary's loop starts at the
    // second side, so a wall on the place's -Y edge is never seen).
    if (tile && backToMe >= 0 && c.vm.world) {
        const auto s = directionStep(backToMe);
        if (!(s[0] == 0 && s[1] == -1) && c.vm.world->wallBetween(*tile, s[0], s[1], true))
            return Exit::False;
    }

    std::vector<ObjectId> before;
    for (const auto& [id, _] : c.vm.eachObject())
        before.push_back(id);
    Object* x = &c.vm.instantiate(defAt->first, defAt->second);
    const ObjectId created = x->id;
    if (where != 6 && !container && !at) {
        // Nowhere in the world to put it.
        const ObjectId group = multiTileGroup(*x);
        std::vector<ObjectId> parts;
        for (const auto& [id, y] : c.vm.eachObject())
            if (multiTileGroup(y) == group)
                parts.push_back(id);
        for (ObjectId id : parts)
            c.vm.removeObject(id);
        return Exit::False;
    }
    x->data[objdata::Room] = -1; // out of the world while its Init runs
    c.vm.noteRoomChanged(x->id);
    if (materialSource)
        x->materials = materialSource->materials;
    if (neighbor && x->person) {
        x->person->data = neighbor->personData;
        x->person->data[persondata::NeighborId] = c.frame.stackObject;
    } else if (x->person && !x->person->data[persondata::NeighborId]) {
        // A Sim made from its definition (a unique NPC: the Grim Reaper, the
        // therapist, ...) is that GUID's neighbor: the neighborhood keeps one
        // without a character file for each (N002's #35 is the reaper,
        // GUID 0x84EC24A8), and a new one gets the next ID. Without it the
        // reaper's "Wants - Refresh All - Verify" asked Force Recalc of Wants
        // of neighbor 0 and its Main died, so no one dead was ever reaped
        // (inferred: the binary's person creation links the neighbor behind
        // virtual calls not followed).
        x->person->data[persondata::NeighborId] = c.vm.neighborForGuid(x->guid());
    }
    // A person made in play is a visitor: the engine's handler of the
    // object-made event (BV 0x4700c8, event 0x8F: a person, OBJD type 2,
    // whose person data 32 "Person Type" is 0 gets 1). The trees that make
    // a member of the family set it back to 0 ("Make Baby - Common", "Sim -
    // Create New Sim - Generic", "Make New Character"), NPC Inits to 2, and
    // cTSNeighborhood::UpdateInstanceVisitorTypes (BV 0x4d7e94) clears it
    // for the family's members. Done here, before the new object's Init
    // (inferred: the event comes with the creation).
    if (x->person && x->person->data[kPersondataPersonType] == 0)
        x->person->data[kPersondataPersonType] = 1;
    const ObjectId stackObject = c.frame.stackObject, meId = me.id;
    const int16_t temp0 = c.thread.temps[0];
    // The object manager makes it out of the world, its Init run (vtable 0x40
    // of the manager); then flag 0x20 turns it the way the stack object faces
    // (Orient, vtable 0x2a0 / base 0x234, when the two differ). Was: "the
    // stack object's level", and every new object turned to its anchor.
    if (c.vm.initCreatedObjects)
        c.vm.initialize(created, before);
    if (flags & 0x20)
        if (const Object* so = c.vm.object(stackObject); so && c.vm.object(created))
            for (ObjectId part : groupOf(c.vm, created))
                if (Object* y = c.vm.object(part))
                    y->data[objdata::Direction] = so->data[objdata::Direction];
    const Object* made = c.vm.object(created);
    if (!made)
        return Exit::False; // its Init took it away
    // In the world: TSObjectCanSetPosition (BV 0x8ae264: TestPlaceInWorld,
    // the buy tool's position tests without its slot search) at the place
    // with the object's own facing, else the slot's TSObjectCanPlaceInSlot
    // (BV 0x8aa14c); what does not fit is deleted (the manager's vtable 0x44)
    // and the result is false. Then SetPosition (vtable 0x2a4 / base 0x238:
    // the wall adjacency, the "placement" tree, OBJf 9, the room) or
    // PlaceInSlot (0x2a8). The Fires a Fire spreads to a tile it cannot take
    // are not made.
    auto drop = [&](const char* why) {
        if (std::getenv("OS2_LOG_CREATE"))
            std::fprintf(stderr, "  \"%s\" does not fit%s%s\n", def->fileName.c_str(), why[0] ? ": " : "", why);
        for (ObjectId id : groupOf(c.vm, created))
            c.vm.removeObject(id);
        return Exit::False;
    };
    if (container) {
        // (A person's hands, slots 0 and 1, are not in a SLOT here: not tested.)
        if (c.vm.checkCreatedPlacement && !container->person)
            if (const int e = slotPlacementError(c.vm, *made, *container, slot))
                return drop(("error 0x" + hexString(uint32_t(e)) + " in the slot").c_str());
        if (Object* y = c.vm.object(created))
            placeInSlot(c.vm, *y, *container, slot);
    } else if (at && c.vm.world) {
        const Placement fit = checkPlacement(c.vm, created, *tile, made->data[objdata::Direction], false, &*at);
        if (!fit.ok && c.vm.checkCreatedPlacement)
            return drop(("at (" + std::to_string(tile->x) + ", " + std::to_string(tile->y) + ", " +
                         std::to_string(tile->level) + "): " + fit.reason)
                            .c_str());
        applyPlacement(c.vm, created, fit);
        if (std::getenv("OS2_LOG_CREATE"))
            if (const Object* y = c.vm.object(created))
                std::fprintf(stderr, "  placed at (%.3f, %.3f, %d) facing %d\n", y->position.x, y->position.y,
                             y->position.level, y->data[objdata::Direction]);
    } else if (at) {
        if (Object* y = c.vm.object(created))
            c.vm.setPosition(*y, *at);
    }
    if (c.vm.initCreatedObjects && (flags & 0x12))
        if (auto it = c.vm.threads.find(created); it != c.vm.threads.end() && !it->second.stack.empty()) {
            Frame& f = it->second.stack.front();
            if (flags & 0x02)
                f.stackObject = stackObject;
            if (!f.params.empty())
                f.params[0] = flags & 0x10 ? temp0 : meId;
        }
    c.frame.stackObject = created;
    return Exit::True;
}

// Run Tree by Name (0x1C), as the Mac binary's cTSObject::TryCallNamedTree:
// name index u8 at 4 (1-based) in STR# 0x12F of the global group (flags u16
// at 2 & 0x01), of the running code's semi-global (0x02) or of its own
// group (neither: only the flags choose, as TryCallNamedTree does; u16 0 is
// 0x012F in 168 instructions, among them the firefighter's "Fire -
// Extinguishable?", which names its own table's "Interaction - Extinguish -
// TEST" and got the global table's "CT - Update Dirty State"); when that
// table is missing or shorter, the others are tried (inferred from the
// data: the memory objects ask for entry 85 of the global table, "CT - Add
// Memory", and have none). An empty name is false. The tree is looked up by
// name in the stack object's private, semi-global and global trees (flags
// 0x04/0x08 are passed to that search; not told apart here); none: false.
// Modes (u8 at 5, STR# 0xDE): 0 "run in my stack" runs it at once to its
// end (RunOneTickTree: my thread and temps, the stack object's code, the
// stack object as such) and branches on its result; 1 "run in stack
// object's stack" does the same in the stack object's own thread (it is
// "me", with its temps); 2 "push onto my stack" pushes it as a subroutine
// (GosubObjectTree) and branches when it returns. No stack object: error
// 0x15. Arguments: flags 0x20 the current tree's parameters (up to 4), 0x10
// three (owner u8, data u16) at 6, 9 and 12, neither: my temps (the tree
// sim's default parameters are the object's temps, cTSTreeSim::OnInit).
// Mode 3 (the tree of the object whose GUID is in temps 0/1) is not done.
Exit runTreeByName(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t mode = o[5];
    const uint16_t flags = le16(&o[2]);
    if (mode > 3)
        throw gameError(err::BadNamedTreeParameter, "Run Tree by Name: mode " + std::to_string(mode));

    auto& code = c.vm.code();
    const Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        throw VMError("code owner does not exist");
    const size_t index = size_t(o[4]) - 1;
    const uint32_t semi = c.frame.group == owner->group ? owner->semiGroup : (c.frame.group == kGlobalGroup ? 0 : c.frame.group);
    std::vector<uint32_t> tables;
    if (flags & 0x01)
        tables = {kGlobalGroup, c.frame.group, semi};
    else if (flags & 0x02)
        tables = {semi, c.frame.group, kGlobalGroup};
    else
        tables = {c.frame.group, semi, kGlobalGroup};
    std::string name;
    for (uint32_t group : tables)
        if (const StringTable* t = group ? code.strings(group, kNamedTreeStrings) : nullptr)
            if (index < t->count(lang::EnglishUS)) {
                name = t->get(index, lang::EnglishUS);
                break;
            }
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
        name.pop_back();
    if (name.empty())
        return Exit::False;

    if (!c.vm.object(c.frame.stackObject))
        throw gameError(err::NoStackObject, "Run Tree by Name");
    const Object& target = c.stackObject();
    std::optional<uint16_t> tree;
    uint32_t treeGroup = 0;
    for (uint32_t group : {target.group, target.semiGroup, kGlobalGroup}) {
        if (!group)
            continue;
        // Only trees a call opcode reaches (not the private trees of a library).
        tree = code.behaviorByName(group, name);
        if (tree && c.vm.resolveTree(target, *tree).first == group) {
            treeGroup = group;
            break;
        }
        tree.reset();
    }
    if (!tree)
        return Exit::False;
    // OS2_TRACE_NAMED=1: the named trees the lot's trees call (a mode 2 push
    // that does not resolve looks exactly like a tree that runs and does
    // nothing).
    static const bool traceNamed = std::getenv("OS2_TRACE_NAMED") != nullptr;
    if (traceNamed)
        std::fprintf(stderr, "named tree: mode %u index %zu name \"%s\" stack object %d -> group %08X instance %u\n",
                     unsigned(mode), index, name.c_str(), int(c.frame.stackObject), treeGroup, tree ? unsigned(*tree) : 0u);

    std::array<int16_t, 8> args{};
    if (flags & 0x20)
        std::copy_n(c.frame.params.begin(), std::min<size_t>(c.frame.params.size(), 4), args.begin());
    else if (flags & 0x10)
        for (size_t k = 0; k < 3; ++k)
            args[k] = c.get(o[6 + 3 * k], int16_t(le16(&o[7 + 3 * k])));
    else
        std::copy_n(c.thread.temps.begin(), args.size(), args.begin());
    const ObjectId targetId = target.id;
    if (mode == 3) {
        // Mode 3: the tree of the object whose GUID is in the temps 0/1 (the
        // low word in temp 0), run at once like the mode 0. The Mac's
        // TryCallNamedTree and the PC's base class (FUN_008D3510, case 3) agree
        // on this: the GUID is looked up in the world, the object gives its
        // tree (its Main) and the tree runs in my stack, with the same
        // arguments the other modes get. A GUID no object has is the game's
        // error 0x7A ("You sent me a crappy GUID, please fix it.").
        const uint32_t guid = uint16_t(c.thread.temps[0]) | uint32_t(uint16_t(c.thread.temps[1])) << 16;
        const auto ids = c.vm.objectsOfGuid(guid);
        const Object* obj = ids.empty() ? nullptr : c.vm.object(ids.front());
        const uint16_t objTree = obj ? c.vm.functionTree(*obj, 1) : 0;
        if (!objTree)
            throw gameError(err::CrappyGuid, "Run Tree by Name: mode 3");
        return runOneTickTree(c.vm, c.thread.temps, c.thread.me, obj->id, obj->id, objTree, args,
                              c.thread.actionStrings)
                   ? Exit::True
                   : Exit::False;
    }
    if (mode == 0)
        return runOneTickTree(c.vm, c.thread.temps, c.thread.me, targetId, targetId, *tree, args, c.thread.actionStrings)
                   ? Exit::True
                   : Exit::False;
    if (mode == 1) {
        // On the stack object's tree sim: without passed values (flags
        // 0x10/0x20) the parameters are its temps, as Gosub takes a null
        // parameter list (TryCallNamedTree BV 0xb06244, Gosub 0xe1a428):
        // 'Call "Look At" For All' sets the Sim's temps 0-2 (target,
        // duration, speed) and runs "CT - Look At" on it.
        auto it = c.vm.threads.find(targetId);
        std::array<int16_t, kTempCount> scratch{};
        auto& temps = it != c.vm.threads.end() ? it->second.temps : scratch;
        if (!(flags & 0x30))
            std::copy_n(temps.begin(), std::min(args.size(), temps.size()), args.begin());
        return runOneTickTree(c.vm, temps, targetId, targetId, targetId, *tree, args, c.thread.actionStrings) ? Exit::True
                                                                                                               : Exit::False;
    }
    std::array<int16_t, 4> first{};
    std::copy_n(args.begin(), 4, first.begin());
    c.vm.call(c.thread, targetId, *tree, first);
    Frame& callee = c.thread.stack.back();
    for (size_t i = 4; i < args.size() && i < callee.params.size(); ++i)
        callee.params[i] = args[i];
    return Exit::Call;
}

// Relationship (0x1A), as the Mac binary has it (TryNewRelationship, base
// 1.0 and Bon Voyage; node version 1): variable u8 at 0 (STR# 0x102), flags
// u8 at 1, from (owner u8 at 2, data u16), to (owner u8 at 5, data u16),
// value (owner u8 at 8, data u16). Flags: 0x01 "Fail if too small", 0x02
// "Use Neighbor IDs", 0x04 "Get value from" the value (sets the variable;
// without it the variable is "put into" the value), 0x08 (no editor text)
// an object's own relationship table: "from" is an object (error 0x17 if it
// does not exist) and "to" is used as is, a neighbor ID (the ownership
// trees) in the same table and key space as the object IDs below
// (docs/relationships-mac.md §3; the lots keep these in their SRELs,
// vm::loadLot). With 0x02 both are
// neighbor IDs (a missing one: error 0x1C). With neither, both are objects
// (error 0x17 if one is missing): two Sims are keyed by their neighbor IDs
// (the same relationship 0x02 reaches), otherwise the table is the "from"
// object's with the "to" object's ID as the key. A relationship with no more
// entries than the variable number fails (false) with 0x01 and otherwise
// grows to variable + 1 entries.
//
// Node version 0 (7 instructions in the game; NewRelationshipVersion0Param,
// converted on load by the game): variable u8 at 0, mode u8 at 1 (0 me to
// the stack object, 1 the stack object to me, 2 the stack object to the
// object in the local, 3 that object to the stack object), flags u8 at 2,
// local u8 at 3, value (owner u8 at 4, data u16 at 6). With flag 0x02 the
// two are named by their person data 31 (neighbor ID) instead of object data
// 11 (object ID); the local's value is taken as is.
Exit relationship(Context& c) {
    const auto& o = c.instruction.operands;
    uint8_t var = o[0], flags = o[1];
    int16_t from = 0, to = 0;
    uint8_t valueOwner = o[8];
    int16_t valueData = int16_t(le16(&o[9]));
    if (c.instruction.nodeVersion == 0) {
        flags = o[2];
        valueOwner = o[4], valueData = int16_t(le16(&o[6]));
        auto idOf = [&](ObjectId id) -> int16_t {
            if (!(flags & 0x02))
                return id;
            const Object* x = c.vm.object(id);
            if (!x || !x->person)
                throw gameError(err::NotAPerson, "object " + std::to_string(id));
            return x->person->data[persondata::NeighborId];
        };
        const int16_t me = idOf(c.thread.me), stack = idOf(c.frame.stackObject);
        const int16_t local = c.get(0x19, o[3]);
        switch (o[1]) {
        case 0: from = me, to = stack; break;
        case 1: from = stack, to = me; break;
        case 2: from = stack, to = local; break;
        case 3: from = local, to = stack; break;
        default: throw VMError("Relationship: version 0 mode " + std::to_string(o[1]) + " not implemented");
        }
        flags &= 0x07;
    }
    if (flags & ~0x0F)
        throw VMError("Relationship: flags " + hexString(flags) + " not implemented");
    if (var >= kMaxRelationshipVars)
        throw VMError("Relationship: variable " + std::to_string(var) + " out of range");
    if (c.instruction.nodeVersion != 0) {
        from = c.get(o[2], int16_t(le16(&o[3])));
        to = c.get(o[5], int16_t(le16(&o[6])));
    }
    std::pair<VM::RelationshipKey, VM::RelationshipKey> pair;
    if (flags & 0x02) {
        for (int16_t id : {from, to})
            if (!c.vm.neighbor(id))
                throw gameError(err::NotAPerson, "neighbor " + std::to_string(id));
        pair = {{true, from}, {true, to}};
    } else if (flags & 0x08) {
        if (!c.vm.object(from))
            throw gameError(err::NoObject, "object " + std::to_string(from));
        pair = VM::objectRelationship(from, to); // "to" raw: the same table and keys as below
    } else {
        const Object* a = c.vm.object(from);
        const Object* b = c.vm.object(to);
        if (!a || !b)
            throw gameError(err::NoObject, "object " + std::to_string(a ? to : from));
        if (a->person && b->person)
            pair = {c.vm.relationshipKey(from), c.vm.relationshipKey(to)};
        else
            pair = VM::objectRelationship(from, to);
    }
    auto it = c.vm.relationships.find(pair);
    if (it == c.vm.relationships.end()) {
        if (flags & 0x01)
            return Exit::False;
        it = c.vm.relationships.emplace(pair, std::vector<int16_t>{}).first;
    }
    auto& vars = it->second;
    if (var >= vars.size()) {
        if (flags & 0x01)
            return Exit::False;
        vars.resize(var + 1);
    }
    if (flags & 0x04)
        vars[var] = c.get(valueOwner, valueData);
    else
        c.set(valueOwner, valueData, vars[var]);
    return Exit::True;
}

// Array Operation (0x76), by the Mac binary (TryArrayOperation, BV
// 0xe2a158): operation u8 at 1, byte 2 nonzero = the stack object's arrays
// (else mine; no stack object: error 0x08), array u16 at 3 (past the
// object's arrays, OBJD field 0x3B: error 0x6C), a value (owner u8 at 5,
// data u16 at 6) and an index (owner u8 at 8, data u16 at 9):
//   0 clear                         1 size into the value (error 0x13 if it
//                                     cannot be written)
//   2 resize to the value (new elements 0)
//   3 set every element to the value
//   4 insert the value at the front, 5 at the end, 6 before the index
//     (an index outside the elements, error 0x6D; nothing is written back)
//   7 remove the first element, 8 the last (empty: error 0x6D)
//   9 remove the element at the index (outside: error 0x6D)
//  10 find the value from the index on and put where it is in the index;
//     false if the index is outside or the value absent
//  11 swap the elements at the value and at the index (outside: 0x6D)
//  12 sort, largest first; 13 sort, smallest first
// Another operation is error 0x01. Only 10 can be false.
Exit arrayOperation(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t op = o[1];
    Object* holder = &c.me();
    if (o[2] != 0) {
        holder = c.vm.object(c.frame.stackObject);
        if (!holder)
            throw gameError(err::StackNumberOutOfRange, "Array Operation: stack object " + std::to_string(c.frame.stackObject));
    }
    auto& values = holder->array(int16_t(le16(&o[3]))).values;
    auto get1 = [&] { return c.get(o[5], int16_t(le16(&o[6]))); };
    auto set1 = [&](int16_t v) { c.set(o[5], int16_t(le16(&o[6])), v); };
    auto get2 = [&] { return c.get(o[8], int16_t(le16(&o[9]))); };
    auto set2 = [&](int16_t v) { c.set(o[8], int16_t(le16(&o[9])), v); };
    auto index = [&](int16_t i, const char* what) {
        if (i < 0 || size_t(i) >= values.size())
            throw gameError(err::NoArrayElement, std::string("Array Operation: ") + what + " at " + std::to_string(i) +
                                                     " of " + std::to_string(values.size()));
        return size_t(i);
    };
    auto insert = [&](size_t at, int16_t v) {
        if (values.size() >= kMaxArrayLength)
            throw VMError("Array Operation: array full");
        values.insert(values.begin() + ptrdiff_t(at), v);
        return Exit::True;
    };
    switch (op) {
    case 0: values.clear(); return Exit::True;
    case 1: set1(int16_t(values.size())); return Exit::True;
    case 2: {
        const int16_t n = get1();
        if (n < 0 || size_t(n) > kMaxArrayLength)
            throw VMError("Array Operation: size " + std::to_string(n) + " out of range");
        values.resize(size_t(n));
        return Exit::True;
    }
    case 3: std::fill(values.begin(), values.end(), get1()); return Exit::True;
    case 4: return insert(0, get1());
    case 5: return insert(values.size(), get1());
    case 6: {
        const int16_t v = get1();
        return insert(index(get2(), "insert"), v);
    }
    case 7:
    case 8:
        if (values.empty())
            throw gameError(err::NoArrayElement, "Array Operation: remove from an empty array");
        values.erase(op == 7 ? values.begin() : values.end() - 1);
        return Exit::True;
    case 9: values.erase(values.begin() + ptrdiff_t(index(get2(), "remove"))); return Exit::True;
    case 10: {
        const int16_t v = get1();
        const int16_t from = get2();
        if (from < 0 || size_t(from) >= values.size())
            return Exit::False;
        auto it = std::find(values.begin() + from, values.end(), v);
        if (it == values.end())
            return Exit::False;
        set2(int16_t(it - values.begin()));
        return Exit::True;
    }
    case 11: {
        const size_t a = index(get1(), "swap"), b = index(get2(), "swap");
        std::swap(values[a], values[b]);
        return Exit::True;
    }
    case 12: std::sort(values.begin(), values.end(), std::greater<>()); return Exit::True;
    case 13: std::sort(values.begin(), values.end()); return Exit::True;
    default: throw gameError(err::BadOperator, "Array Operation: operation " + std::to_string(op));
    }
}

// Change Material (0x6D), by the Mac binary (TryChangeMaterial, BV
// 0xe2f128) and SwizzleParams (docs/primitives-mac.md §5): material u16 at 0
// (an index into STR# 0x88 "Material Names"), flags u8 at 2, mesh group u16
// at 3 (into STR# 0x87 "Mesh Groups"; 0xFFFF / negative = the whole model),
// the object it affects (owner u8 at 5, data u16), an object (owner u8 at 8,
// data u16 at 9) whose tables are used with flags 0x08 (material names) and
// 0x01 (mesh group names) instead of the running code's, flags u8 at 13.
// Flags: 0x10 the material index is in temp 0, 0x20 the mesh group index in
// temp 1; the material names come from the global group's STR# 0x88 with
// 0x02, the semi-global's with 0x04 (none: no name), else the object's own;
// the mesh group names likewise with 0x40 and 0x80. Byte 13 0x02/0x04/0x20
// (the snapshot and "Moving Texture" materials) skip the material name (not
// done here: nothing changes). A material without a name is error 0x46
// ("Missing Material / Mesh Group Name"); a mesh group without one is not:
// the binary looks the name up and never reads it (the shrubs' "Function -
// Main" names mesh group 0 of a table their group does not have, and the
// game goes on). The change is recorded on the target for the renderer: a
// named mesh group gets the material, an unnamed one the whole model
// (inferred).
Exit changeMaterial(Context& c) {
    const auto& o = c.instruction.operands;
    const uint8_t flags = o[2];
    const Object* owner = c.vm.object(c.frame.codeOwner);
    if (!owner)
        throw VMError("code owner does not exist");
    const Object* other = nullptr;
    if (flags & 0x09) {
        const int16_t id = c.get(o[8], int16_t(le16(&o[9])));
        other = c.vm.object(id);
        if (!other)
            throw gameError(err::NoObject, "Change Material: object " + std::to_string(id));
    }
    const ObjectId target = c.get(o[5], int16_t(le16(&o[6])));
    Object* x = c.vm.object(target);
    if (!x)
        throw gameError(err::NoObject, "Change Material: object " + std::to_string(target));
    // The names come from the running tree's group (TryChangeMaterial looks
    // the table up in the group of the code the stack element runs, BV): the
    // semi-global "Events - Use Remote" of the TVs names the screens in its
    // own STR# 0x88; the code owner's group when that has none.
    // `where`: 0 the object's own, 1 the global group, 2 its semi-global.
    auto name = [&](const Object& from, uint16_t table, int index, int where) -> std::string {
        const StringTable* t = nullptr;
        auto has = [&](const StringTable* f) { return f && index >= 0 && size_t(index) < f->count(lang::EnglishUS); };
        if (where == 1)
            t = c.vm.code().strings(kGlobalGroup, table);
        else if (where == 2) {
            if (const uint32_t semi = c.vm.code().semiGlobalFor(from.group))
                t = c.vm.code().strings(semi, table);
        } else {
            if (&from == owner && c.frame.group)
                if (const StringTable* f = c.vm.code().strings(c.frame.group, table); has(f))
                    t = f;
            if (!t)
                t = c.vm.code().strings(from.group, table);
        }
        return has(t) ? t->get(size_t(index), lang::EnglishUS) : std::string();
    };
    if (o[13] & 0x26)
        return Exit::True; // the snapshot / moving texture materials (not done)
    const int materialIndex = flags & 0x10 ? c.thread.temps[0] : int(le16(&o[0]));
    const int group = flags & 0x20 ? c.thread.temps[1] : int(int16_t(le16(&o[3])));
    const std::string material = name(flags & 0x08 ? *other : *owner, kMaterialStrings, materialIndex,
                                      flags & 0x02 ? 1 : flags & 0x04 ? 2 : 0);
    if (material.empty())
        throw gameError(err::MissingMaterialName, "Change Material: no material " + std::to_string(materialIndex) + " in STR# " +
                                  hexString(kMaterialStrings) + " of " +
                                  hexString((flags & 0x08 ? *other : *owner).group));
    const std::string meshGroup = group < 0 ? "" : name(flags & 0x01 ? *other : *owner, kMeshGroupStrings, group,
                                                       flags & 0x40 ? 1 : flags & 0x80 ? 2 : 0);
    if (meshGroup.empty())
        x->materials.clear();
    x->materials[meshGroup] = material;
    return Exit::True;
}

// Manage Inventory (0x33), as cTSObject::TryInventoryAction (BV 0xe26a40)
// and cTSInventory do it, on the base game's (version 0) operands, which
// ManageInventoryParam::UpgradeVersion (BV 0xac3f00) converts:
//   byte 0: bits 0-1 the inventory kind ("G = 0, H, F, N", STR# 0xFC), 0x04
//     counted tokens, bits 3-4 the match value of the singular operations
//     (token flags: 0x01 visible, 0x02 memory) and bits 3-5 the flags a new
//     counted token gets, 0x80 the find searches backwards (no base tree);
//   inventory ID (owner u8 at 1, data u16 at 2; not read for the global
//     inventory), operation u8 at 4, GUID u32 at 5, byte 9 the category of
//     counted tokens (the singular operations get match mask 0x03: the BV
//     sets byte 9 to 0x0C, whose bits 2-6 are the mask and 0-1 more value
//     bits), first operand (owner u8 at 10, data u16 at 11), second (13, 14).
// A singular token matches a GUID and the flags when its flags & mask equal
// the value & mask and it is not marked for deletion (flag 0x04;
// MatchesGuidAndFlags, FindNextSingularTokenOfGUID BV 0x494038). GUID 0 is
// the stack object's (error 0x15 without one) in the singular 0, 2 and 3 and
// the counted 0, 2, 4, 6 and 7. Token indices in the operands start at 1.
// Singular:
//   0 add a token of the GUID with flags = the match value (false at 0x7FFF)
//   1 remove the token at the index (first operand); false if none
//   2 remove every matching token of the GUID; false if none
//   3 find the next matching token of the GUID after the index in the first
//     operand (0: from the first), its index into the first; false if none
//   4 push the second operand onto the properties of the token at the index; false if none
//   5 pop the last property of the token at the index into the second; false if none or empty
//   6 read the token at the index into my temporary token; false if none
//   7 property (first, from 1) of my temporary token into the second; false if none
//   8 set property (first) of my temporary token to the second; false if none
//   9 save my temporary token back where it was read; false if it is gone (a
//     temporary token read from counted tokens runs the counted 9 instead)
//  10 token count into the second
//  11 add a copy of my temporary token, with the flags in its field 6
// Counted (properties 0 and 1 are the count and the category):
//   0 add the second operand to the count of the GUID and category (byte 9);
//     a new token gets flags byte 0 bits 3-5; false if the number is not > 0
//   1 the same for the token at the index (first operand); false if none
//   2 subtract the second from the GUID and category (the token goes at 0);
//     false if none or the number is not > 0
//   3 the same for the token at the index; false if none
//   4 remove the token of the GUID and category; false if none
//   5 remove the token at the index (< 1: error 0x74); false if none
//   6 find the GUID and category: count into the second, index into the first; false if none
//   7 read the token of the GUID and category into my temporary token; false if none
//   8 read the token at the index into my temporary token; false if none
//   9 find the next token of the category after the index in the first: its
//     index into the first, its GUID into the second and the one after it
//  10 token count into the second
//  11 copy the token at the index to the inventory of kind byte 6 & 7 (ID in
//     the second operand), adding its count there, if byte 6 & 0x08; false if
//     not or no token there
// Operations past these are false.
Exit manageInventory(Context& c) {
    const auto& o = c.instruction.operands;
    TempToken& temp = c.me().tempToken;
    uint8_t op = o[4];
    InventoryKind kind = InventoryKind(o[0] & 0x03);
    bool counted = o[0] & 0x04;
    int16_t id = 0;
    if (!counted && op == 9) { // where the temporary token was read from
        kind = temp.kind, id = temp.inventoryId, counted = temp.counted;
    } else if (kind != InventoryKind::Global) {
        id = c.get(o[1], int16_t(le16(&o[2])));
    }
    const int16_t matchValue = int16_t((o[0] >> 3) & 0x03), matchMask = 0x03;
    const int16_t newCountedFlags = int16_t((o[0] >> 3) & 0x07);
    const bool backwards = o[0] & 0x80;
    const int16_t category = o[9];
    auto get1 = [&] { return c.get(o[10], int16_t(le16(&o[11]))); };
    auto set1 = [&](int16_t v) { c.set(o[10], int16_t(le16(&o[11])), v); };
    auto get2 = [&] { return c.get(o[13], int16_t(le16(&o[14]))); };
    auto set2 = [&](int16_t v, int16_t at = 0) { c.set(o[13], int16_t(le16(&o[14]) + at), v); };
    auto guid = [&] {
        const uint32_t g = le32(&o[5]);
        if (g)
            return g;
        if (!c.vm.object(c.frame.stackObject))
            throw gameError(err::NoStackObject, "Manage Inventory");
        return c.stackObject().guid();
    };
    Inventory& inventory = c.vm.inventory(kind, id);
    auto& tokens = counted ? inventory.counted : inventory.singular;
    // OS2_TRACE_INV=1: every Manage Inventory call (which inventory, which
    // operation and GUID, how many tokens before it) — the NPC pool's tokens
    // and the service cars' drivers are behind these.
    static const bool traceInv = std::getenv("OS2_TRACE_INV") != nullptr;
    if (traceInv)
        std::fprintf(stderr, "manage inventory: kind %u id %d %s op %u guid %08X tokens %zu\n", unsigned(kind), int(id),
                     counted ? "counted" : "singular", unsigned(op), le32(&o[5]), tokens.size());
    auto at = [&](int16_t index) -> InventoryToken* { // index from 1
        return index >= 1 && size_t(index) <= tokens.size() ? &tokens[size_t(index) - 1] : nullptr;
    };
    auto load = [&](size_t i) {
        temp = {.token = tokens[i], .loaded = true, .kind = kind, .inventoryId = id, .counted = counted,
                .index = int16_t(i)};
        temp.copyFields();
    };

    if (counted) {
        auto find = [&](uint32_t g, int16_t cat) {
            return std::find_if(tokens.begin(), tokens.end(),
                                [&](const auto& t) { return t.guid == g && categoryOf(t) == cat; });
        };
        auto add = [&](std::vector<InventoryToken>& list, uint32_t g, int16_t cat, int16_t flags, int16_t n) {
            if (n <= 0)
                return false;
            auto it = std::find_if(list.begin(), list.end(),
                                   [&](const auto& t) { return t.guid == g && categoryOf(t) == cat; });
            if (it == list.end())
                list.push_back({.guid = g, .flags = flags, .properties = {n, cat}});
            else
                it->properties[0] = int16_t(std::min<int>(countOf(*it) + n, 0x7FFF));
            return true;
        };
        auto subtract = [&](uint32_t g, int16_t cat, int16_t n) {
            if (n <= 0)
                return false;
            auto it = find(g, cat);
            if (it == tokens.end())
                return false;
            const int16_t left = int16_t(std::max(countOf(*it) - n, 0));
            if (left == 0)
                tokens.erase(it);
            else
                it->properties[0] = left;
            return true;
        };
        switch (op) {
        case 0: {
            const uint32_t g = guid();
            return add(tokens, g, category, newCountedFlags, get2()) ? Exit::True : Exit::False;
        }
        case 1: {
            const InventoryToken* t = at(get1());
            if (!t)
                return Exit::False;
            const uint32_t g = t->guid;
            const int16_t cat = categoryOf(*t);
            add(tokens, g, cat, newCountedFlags, get2());
            return Exit::True;
        }
        case 2: {
            const uint32_t g = guid();
            return subtract(g, category, get2()) ? Exit::True : Exit::False;
        }
        case 3: {
            const InventoryToken* t = at(get1());
            if (!t)
                return Exit::False;
            subtract(t->guid, categoryOf(*t), get2());
            return Exit::True;
        }
        case 4: {
            auto it = find(guid(), category);
            if (it == tokens.end())
                return Exit::False;
            tokens.erase(it);
            return Exit::True;
        }
        case 5: {
            const int16_t index = get1();
            if (index < 1)
                throw gameError(err::TokenIndexOutOfRange, "Manage Inventory");
            if (!at(index))
                return Exit::False;
            tokens.erase(tokens.begin() + (index - 1));
            return Exit::True;
        }
        case 6: {
            auto it = find(guid(), category);
            if (it == tokens.end())
                return Exit::False;
            set2(countOf(*it));
            set1(int16_t(it - tokens.begin() + 1));
            return Exit::True;
        }
        case 7: {
            auto it = find(guid(), category);
            if (it == tokens.end())
                return Exit::False;
            load(size_t(it - tokens.begin()));
            return Exit::True;
        }
        case 8: {
            const int16_t index = get1();
            if (!at(index))
                return Exit::False;
            load(size_t(index - 1));
            return Exit::True;
        }
        case 9: {
            // FindNextCountedTokenOfCategory (BV 0x493e98) from the token
            // after the index (0: the first).
            const int16_t from = std::max<int16_t>(get1(), 0);
            for (size_t i = size_t(from); i < tokens.size(); ++i) {
                if (categoryOf(tokens[i]) != category)
                    continue;
                const uint32_t g = tokens[i].guid;
                set1(int16_t(i + 1));
                set2(int16_t(g & 0xFFFF));
                set2(int16_t(g >> 16), 1);
                return Exit::True;
            }
            return Exit::False;
        }
        case 10: set2(int16_t(tokens.size())); return Exit::True;
        case 11: {
            // The target's vtable 0xC4 (BV 0x497eb4): the count is added to
            // the token of the same GUID and category there.
            if (!(o[6] & 0x08))
                return Exit::False;
            const InventoryKind to = InventoryKind(o[6] & 0x07);
            if (o[6] & 0x04)
                throw gameError(err::NoInventory, "Manage Inventory: inventory kind " + std::to_string(o[6] & 0x07));
            const int16_t toId = to == InventoryKind::Global ? int16_t(0) : get2();
            const InventoryToken* t = at(get1());
            if (!t)
                return Exit::False;
            const InventoryToken copy = *t;
            if (countOf(copy) > 0)
                add(c.vm.inventory(to, toId).counted, copy.guid, categoryOf(copy), copy.flags, countOf(copy));
            return Exit::True;
        }
        default: return Exit::False;
        }
    }

    auto matches = [&](const InventoryToken& t, uint32_t g) {
        return t.guid == g && !(t.flags & tokenflag::MarkedForDeletion) &&
               (t.flags & matchMask) == (matchValue & matchMask);
    };
    switch (op) {
    case 0: {
        const uint32_t g = guid();
        if (tokens.size() >= kMaxInventoryTokens)
            return Exit::False;
        tokens.push_back({.guid = g, .flags = matchValue});
        return Exit::True;
    }
    case 1: {
        const int16_t index = get1();
        if (!at(index))
            return Exit::False;
        tokens.erase(tokens.begin() + (index - 1));
        return Exit::True;
    }
    case 2: {
        const uint32_t g = guid();
        return std::erase_if(tokens, [&](const auto& t) { return matches(t, g); }) ? Exit::True : Exit::False;
    }
    case 3: {
        // From the token after the index in the first operand (0: the
        // first); backwards from the one before it (0: the last).
        const uint32_t g = guid();
        const int16_t start = get1();
        const int n = int(tokens.size());
        if (!backwards) {
            for (int i = std::max<int>(start, 0); i < n; ++i)
                if (matches(tokens[size_t(i)], g))
                    return set1(int16_t(i + 1)), Exit::True;
        } else {
            for (int i = (start - 1 < 0 || start - 1 >= n ? n : start - 1) - 1; i >= 0; --i)
                if (matches(tokens[size_t(i)], g))
                    return set1(int16_t(i + 1)), Exit::True;
        }
        return Exit::False;
    }
    case 4: {
        InventoryToken* t = at(get1());
        if (!t)
            return Exit::False;
        if (t->properties.size() >= kMaxTokenProperties)
            throw VMError("Manage Inventory: too many token properties");
        t->properties.push_back(get2());
        return Exit::True;
    }
    case 5: {
        InventoryToken* t = at(get1());
        if (!t || t->properties.empty())
            return Exit::False;
        const int16_t v = t->properties.back();
        t->properties.pop_back();
        set2(v);
        return Exit::True;
    }
    case 6: {
        // The token at the index; the GUID is not looked at: trees leave
        // stale GUIDs there ("Token - Add Food Cooked" finds a token with
        // one GUID and reads it with another).
        const int16_t index = get1();
        if (!at(index))
            return Exit::False;
        load(size_t(index - 1));
        return Exit::True;
    }
    case 7:
    case 8: {
        // Property numbers start at 1, as token indices ("Food - Update -
        // Food Unit Info Token" pushes three and sets 2 and 3).
        const int16_t p = get1();
        if (p < 1 || size_t(p) > temp.token.properties.size())
            return Exit::False;
        if (op == 7)
            set2(temp.token.properties[size_t(p) - 1]);
        else
            temp.token.properties[size_t(p) - 1] = get2();
        return Exit::True;
    }
    case 9: {
        if (!temp.loaded || temp.index < 0 || size_t(temp.index) >= tokens.size())
            return Exit::False;
        tokens[size_t(temp.index)] = temp.token;
        return Exit::True;
    }
    case 10: set2(int16_t(tokens.size())); return Exit::True;
    case 11: {
        // A copy of the token with the flags of field 6 (BV 0xe27798).
        if (!temp.loaded || tokens.size() >= kMaxInventoryTokens)
            return Exit::False;
        InventoryToken copy = temp.token;
        copy.flags = temp.copied[2];
        tokens.push_back(std::move(copy));
        return Exit::True;
    }
    default: return Exit::False;
    }
}

// cTSFamilyManager::RemoveFromFamily (BV 0x4fe800): family number 0 on the
// neighbor and its Sim on the lot.
static void removeFromFamily(VM& vm, int16_t id, Neighbor& n) {
    n.personData[persondata::FamilyNumber] = 0;
    neighborData(vm, id, n)[persondata::FamilyNumber] = 0;
}

static size_t familyMembers(VM& vm, int16_t family) {
    size_t members = 0;
    for (auto& [id, other] : vm.neighbors)
        members += neighborData(vm, id, other)[persondata::FamilyNumber] == family;
    return members;
}

// Generic Sims Call 4 "add to family", by TryGenericSimCall (BV 0xe36834)
// and cTSFamilyManager::AddToFamily (BV 0x4fddcc): the neighbor in the stack
// object (none: error 0x37) joins the family in temp 0: 0 = the lot's
// family, one the neighborhood does not have is made (MakeNewFamily, BV
// 0x4ffd28: below 0x7FD9 it takes the lowest free number from 1, above it
// that number); the primitive keeps it in its state. When the Sim is the
// last member of a family with a house, the game sends message 0xEC1AAC03
// (the family is evicted from its lot: VM::evictFamily) and waits (2)
// until the family has no house, then temp 0 := the old family's funds
// (with the lot's price back); else temp 0 := 0 at once. The Sim
// leaves its old family (false when the neighborhood does not have it),
// gets person data 61 := the family and 67 := 0, brings its clothes to the
// family's bin (AddMemberClothing) and, between two real families, the
// pieces of the old bin that fit it (with no one left in the old family,
// all of them); an old family left empty (and real) is removed. There is no
// limit on the members (none in the binary). Temp 1 nonzero marks a family
// made here (family vtable 0xE8, not understood) and the binary names it
// after the Sim's last name: not kept.
Exit addToFamily(Context& c) {
    VM& vm = c.vm;
    auto& temps = c.thread.temps;
    const int16_t id = c.frame.stackObject;
    Neighbor* n = vm.neighbor(id);
    if (!n)
        throw gameError(err::MissingNeighbor, "Generic Sims Call 4: neighbor " + std::to_string(id));
    auto& data = neighborData(vm, id, *n);
    const int16_t old = data[persondata::FamilyNumber];
    auto oldIt = old ? vm.families.find(old) : vm.families.end();
    int16_t family = int16_t(c.frame.primitiveState & 0xFFFF);
    if (!c.frame.primitiveState) { // the first pass
        family = temps[0] ? temps[0] : vm.globals[global::CurrentFamily];
        if (!vm.families.count(family)) {
            if (family < kReservedFamilies) {
                family = 1;
                while (vm.families.count(family))
                    ++family;
            }
            vm.families[family];
        }
        c.frame.primitiveState = 0x10000 | uint16_t(family);
        temps[0] = 0;
        if (oldIt != vm.families.end() && familyMembers(vm, old) <= 1 && oldIt->second.house != 0) {
            if (vm.lots.count(oldIt->second.house)) {
                if (vm.evictFamily)
                    vm.evictFamily(old);
                else
                    oldIt->second.house = 0;
                return Exit::Yield; // the game waits for the move out
            }
            temps[0] = int16_t(familyFunds(vm, old)); // no such lot: at once
        }
    } else { // waiting for the old family to leave its lot
        if (oldIt != vm.families.end() && oldIt->second.house != 0)
            return Exit::Yield;
        temps[0] = int16_t(familyFunds(vm, old));
    }
    if (old != 0) {
        if (oldIt == vm.families.end())
            return Exit::False; // RemoveFromFamily fails
        removeFromFamily(vm, id, *n);
    }
    n->personData[persondata::FamilyNumber] = family;
    data[persondata::FamilyNumber] = family;
    n->personData[67] = 0;
    data[67] = 0;
    auto& bin = vm.families[family].wardrobe;
    auto addClothing = [&](const Tgi& k) { // cTSFamily::AddClothing keeps one of each
        if (std::find(bin.begin(), bin.end(), k) == bin.end())
            bin.push_back(k);
    };
    auto real = [](int16_t f) { return f > 0 && f < 0x7FDA; };
    // cTSFamily::AddMemberClothing (Mac BV 0x47ec68) returns at once unless
    // the family's number is in 1..0x7FD9: the NPC family 0x7FFF and the
    // other reserved ones never get a clothing bin.
    if (vm.memberClothing && real(family))
        for (const Tgi& k : vm.memberClothing(id))
            addClothing(k);
    oldIt = old ? vm.families.find(old) : vm.families.end();
    if (oldIt != vm.families.end() && real(family) && real(old) && old != family) {
        const bool anyone = familyMembers(vm, old) > 0;
        for (const Tgi& k : std::vector<Tgi>(oldIt->second.wardrobe))
            if (!anyone || (vm.clothingFits && vm.clothingFits(id, k)))
                addClothing(k);
    }
    if (oldIt != vm.families.end() && real(old) && old != family && familyMembers(vm, old) == 0) {
        vm.families.erase(oldIt); // RemoveFamily (its house first set to 0)
        vm.removedFamilies.insert(old);
    }
    return Exit::True;
}

// Generic Sims Call (0x01): call index u8 at 0 (the other bytes are 0); the
// arguments and results are in the temps, as the editor's labels describe each
// call (STR# 0xDC, 54 calls). Calls that only ask the engine for something
// outside the simulation (camera, UI, preloading, lot loading and transitions,
// tutorial state, the Sim's 3D look, the wants recalculation) are recorded in
// VM::engineRequests and return true. Implemented in the VM: 4 add to family
// (addToFamily), 6 remove from family, 10/11 build and buy off/on, 13 abort the
// interactions whose object is the stack object from every queue, 14 house radio station, 18
// simulator speed (0 pause, 1 unpause, 2-4 into the Speed global as is), 25
// selected Sim, 21-23 the family tree (child of, spouse of, remove the ties
// between two Sims; VM::familyTree), 40 game state transitions off/on, 48 remapped neighbor ID
// (false: there are no imported lots), 49 can change footprint (true: the
// footprints here are whole tiles, inferred). 7 is deprecated ("tree error" in the
// labels). Those that need the neighborhood are in hood_bridge.cpp; the rest
// need the camera or the world.
Exit genericSimsCall(Context& c) {
    // The call is the u16 at 0 (GenericSimCallParam); one the game does not
    // know is its error 0x2A (Mac binary: "Function - Main" of two objects
    // passes 0xFFFF). The base game's editor labels have 54 calls (0-53);
    // Apartment Life's content goes up to 0x54 (84): its Sim semi-globals'
    // "Function - Main - Set Skin" (0x7F01EC29:0x22A4, instruction 2) calls
    // 60, and the AL's BHAVs use 0x36-0x54 (prim-usage on the AL: ~230
    // instructions). The PC data has no labels for those, so they are handed
    // to the engine like the base's engine-only calls: recording the request
    // and returning true is what lets the AL's Sims' Main run at all (with
    // the error, the Sims of N001_Lot11 reset thousands of times in a day —
    // 5,914 for one of them in 29,600 ticks —, the Main livelocked and the
    // look_at limit tripped by the restarts).
    const uint16_t call16 = le16(&c.instruction.operands[0]);
    if (call16 > 84)
        throw gameError(err::InvalidGlobalSimCall, std::to_string(call16));
    const auto call = uint8_t(call16);
    auto& temps = c.thread.temps;
    auto& vm = c.vm;
    auto request = [&] {
        vm.request({call, c.thread.me, c.frame.stackObject, temps});
        return Exit::True;
    };
    switch (call) {
    case 0:  // exit lot
    case 1:  // center view on stack object
    case 2:  // set action icon to stack object
    case 3:  // uncenter view
    case 8:  // family tutorial complete
    case 9:  // architecture tutorial complete
    case 15: // my routing footprint := temp 0
    case 16: // change normal outfit
    case 17: // swap lot (temp 0)
    case 19: // swap lot to the lot of the family of neighbor in temp 0
    case 20: // swap lot via family in temp 0
    case 24: // preload new Sim age
    case 26: // start lot transition
    case 29: // preload visitor
    case 30: // preload clothing
    case 31: // preload object
    case 33: // update footprint
    case 37: // lot transition done
    case 38: // unlink character
    case 41: // facial overlay zits toggle
    case 42: // Sim fitness update
    case 44: // hide/unhide puck
    case 45: // copy last name
    case 46: // set Sim hair override
    case 47: // clear Sim hair override
    case 51: // show info on lot loading screen
    case 53: // set facial overlay state
        return request();
    case 54: case 55: case 56: case 57: case 58: case 59: case 60: case 61: case 62: case 63:
    case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71: case 72: case 73:
    case 74: case 75: case 76: case 77: case 78: case 79: case 80: case 81: case 82: case 83:
    case 84: // the AL calls beyond the base's table (no labels): the engine's
        return request();
    case 39: // force recalc of wants (wants.cpp)
        return wantsGenericCall(c);
    case 4: // add to family (TryGenericSimCall BV 0xe36834)
        return addToFamily(c);
    case 6: { // remove from family (BV 0xe36834): false unless in a family the neighborhood has
        Neighbor* n = vm.neighbor(c.frame.stackObject);
        if (!n)
            return Exit::False;
        const int16_t old = neighborData(vm, c.frame.stackObject, *n)[persondata::FamilyNumber];
        if (old == 0 || !vm.families.count(old))
            return Exit::False;
        removeFromFamily(vm, c.frame.stackObject, *n);
        return Exit::True;
    }
    case 7: throw VMError("Generic Sims Call: 7 (make new neighbor) is deprecated");
    case 10:
    case 11: vm.buildBuyEnabled = call == 11; return Exit::True;
    case 14: vm.globals[global::HouseRadioStation] = temps[0]; return Exit::True;
    case 18: // TryGenericSimCall (BV): above 1 the speed as is, 1 unpause, else pause
        if (std::getenv("OS2_LOG_SPEED")) // test aid: who changes the game speed (docs: the carpool's Wait for Riders)
            std::fprintf(stderr, "game speed call: %d (stack object %d, was speed %d paused %d)\n", int(temps[0]),
                         int(c.frame.stackObject), int(vm.globals[global::Speed]), int(vm.globals[global::Paused]));
        if (temps[0] <= 1)
            vm.globals[global::Paused] = temps[0] != 1;
        else
            vm.globals[global::Speed] = temps[0], vm.globals[global::Paused] = 0;
        return Exit::True;
    // The family tree (cTSFamilyRelationshipArray, BV 0x5442e8/0x544028),
    // both neighbors needed (else error 0x37); always true.
    case 21: // add child (temp 0) of parent (temp 1)
    case 22: // add spouse (temp 0) of the initial spouse (temp 1)
    case 23: // remove the ties between temp 0 and its relative in temp 1
        if (!vm.neighbor(temps[0]) || !vm.neighbor(temps[1]))
            throw gameError(err::MissingNeighbor, "Generic Sims Call " + std::to_string(call));
        if (call == 21)
            vm.addChildTie(temps[0], temps[1]);
        else if (call == 22)
            vm.addSpouseTie(temps[0], temps[1]);
        else
            vm.removeTies(temps[0], temps[1]);
        return Exit::True;
    case 25: {
        // Select a Sim (TryGenericSimCall BV 0xe36834): temp 0 = 0 selects
        // nobody; else it must be a person (error 0x1C), handed to
        // SetSelectedPerson (BV 0x52a55c), whose message the UI follows
        // (the app: followVmSelection).
        if (temps[0]) {
            const Object* p = vm.object(temps[0]);
            if (!p)
                throw gameError(err::NoObject, "Generic Sims Call 25");
            if (!p->person)
                throw gameError(err::NotAPerson, "Generic Sims Call 25");
        }
        vm.globals[global::SelectedPersonId] = temps[0];
        return Exit::True;
    }
    case 40: vm.gameStateTransitionsEnabled = temps[0] != 0; return Exit::True;
    case 13: // abort the interactions of the stack object: waiting ones are removed, running ones cancelled
        cancelInteractionsWith(vm, c.frame.stackObject);
        return Exit::True;
    case 12: // distance to the camera in temp 0: the binary asks the stack object (no stack object:
             // error 0x0A); there is no camera here, so the distance is 0 (inferred)
        if (!vm.object(c.frame.stackObject))
            throw gameError(err::NoTreeParameter, "Generic Sims Call 12");
        temps[0] = 0;
        return Exit::True;
    case 48: return Exit::False;
    case 49: // can change footprint (to the footprint index in temp 0) without intersecting another object's?
        return Exit::True; // footprints are whole tiles here, which a footprint change does not alter (inferred)
    default:
        if (auto r = neighborhoodSimsCall(c, call))
            return *r;
        if (auto r = idleSimsCall(c, call)) // 43, 52 (autonomy.cpp)
            return *r;
        throw VMError("Generic Sims Call: " + std::to_string(call) + " not implemented");
    }
}

} // namespace

// Opcodes the game has no handler for (cTSObject::OnTryElement in the Mac
// binary, base 1.0 and Bon Voyage; docs/primitives-mac.md §2): the game's
// error 0x05. Tutorial (0x0A) returns true and does nothing.
static Exit noHandler(Context& c) {
    throw gameError(err::NotSupported, "opcode " + hexString(c.instruction.opcode));
}
static Exit tutorial(Context&) { return Exit::True; }

// Follow Sim (0x7D) and Run Script (0x7E): the two Bon Voyage primitives of
// the PC executable's base class (docs/re-pc-ghidra.md sec (g)).
//
// Run Script (0x7E): the operands' bytes 0-1 are the STR# id (0x130, "Lua
// Scripts") and bytes 2-3 the script id counted from 1; entry id - 1 is the
// script's name, the text before " -- " being its file name
// (docs/lua-scripts.md). The engine asks the same entry for the description
// tree (the "defined in description" request) and for the `.objLua` chunk
// (0x4D45F8E6, "defined in objLua file"); the 0x7E runs the *Lua* chunk of
// that name, never a tree (the AL's "NPC - Get Outfit GUID for Type in
// T[0,1]" is a tree that wraps a one-instruction 0x7E of the same name, so
// running the tree would be a recursion), and the result is the value
// SetScriptReturnValue left it (vm/lua_scripts.cpp).
//
// Follow Sim (0x7D) has 1 use in the AL and what it does with the object array
// is still unmeasured: false.
static Exit runScript(Context& c) {
    const auto& o = c.instruction.operands;
    const uint16_t table = uint16_t(le16(&o[0]));
    const uint16_t id = uint16_t(le16(&o[2]));
    if (id == 0)
        return Exit::False;
    const StringTable* names = c.vm.code().strings(kGlobalGroup, table);
    std::string name;
    if (names && size_t(id - 1) < names->count(lang::EnglishUS))
        name = names->get(size_t(id - 1), lang::EnglishUS);
    // "Name  -- the comment" (the entry's text; the name is before " -- ").
    if (const size_t cut = name.find(" -- "); cut != std::string::npos)
        name.resize(cut);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r' || name.back() == '\n'))
        name.pop_back();
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
        name.erase(name.begin());
    if (name.empty()) {
        logOnce("0x7E", "name " + std::to_string(table) + "/" + std::to_string(id),
                "Run Script: no script name at STR# " + hexString(table) + " entry " + std::to_string(id - 1));
        return Exit::False;
    }
    return runLuaScript(c, name) ? Exit::True : Exit::False;
}

static Exit runFollowSim(Context&) { return Exit::False; }

void registerPrimitives(VM& vm) {
    for (uint16_t op : {0x04, 0x05, 0x06, 0x09, 0x15, 0x18, 0x26, 0x27, 0x28, 0x29, 0x2B, 0x2C, 0x2F})
        vm.setPrimitive(op, noHandler);
    vm.setPrimitive(0x7D, runFollowSim);
    vm.setPrimitive(0x7E, runScript);
    for (uint16_t op = 0x34; op <= 0x68; ++op)
        vm.setPrimitive(op, noHandler);
    vm.setPrimitive(0x0A, tutorial);
    vm.setPrimitive(0x00, countDownParam);
    vm.setPrimitive(0x01, genericSimsCall);
    vm.setPrimitive(0x02, expression);
    vm.setPrimitive(0x08, randomNumber);
    vm.setPrimitive(0x0F, breakPoint);
    vm.setPrimitive(0x12, removeObjectInstance);
    vm.setPrimitive(0x1A, relationship);
    vm.setPrimitive(0x1C, runTreeByName);
    vm.setPrimitive(0x1F, setToNext);
    vm.setPrimitive(0x20, testObjectType);
    vm.setPrimitive(0x2A, createObjectInstance);
    vm.setPrimitive(0x33, manageInventory);
    vm.setPrimitive(0x6D, changeMaterial);
    vm.setPrimitive(0x76, arrayOperation);
    registerRoutingPrimitives(vm);
    registerNeighborhoodPrimitives(vm);
    registerAnimationPrimitives(vm);
    registerMeasurePrimitives(vm);
    registerTimerPrimitives(vm);
    registerViewPrimitives(vm);
    registerReachPrimitives(vm); // Run Functional Tree, Reach/Put (reach.cpp)
    registerInteractionPrimitives(vm); // Push Interaction, Idle for Input (queue.cpp)
    registerSoundPrimitives(vm);       // Play / Stop Sound Event, Stop ALL Sounds (sound.cpp)
    registerDialogPrimitives(vm);      // Dialog (dialog.cpp)
    registerAppearancePrimitives(vm);  // Change Light, Change Outfit (appearance.cpp)
    registerMotivePrimitives(vm);      // Set Motive Change, Find 5 Worst Motives (motives.cpp)
    registerAutonomyPrimitives(vm);    // Find Best Interaction, Gosub Found Action (autonomy.cpp)
    registerMessagePrimitives(vm);     // Message (message.cpp)
    registerEnginePrimitives(vm);      // Refresh, Debug, UI/camera, Test Sim Interacting With (engine_prims.cpp)
    registerTutorialPrimitives(vm);    // UI Effect with its strings looked up (tutorial.cpp)
    registerPersonPrimitives(vm);      // Age, RayTrace (person_prims.cpp)
    registerCharacterPrimitives(vm);   // Make New Character (characters.cpp)
}

} // namespace os2::vm
