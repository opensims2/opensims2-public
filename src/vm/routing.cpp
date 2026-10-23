// Positions in the world: occupancy of the lot's tiles, the goals of a
// routing slot (VM::routingGoals), the footprints objects put in the path
// planner (VM::routingFootprint) and Go Here (go_here.hpp). The rest of the
// routing: routing_plan.cpp (planning a way: obstacles, stairs, portals,
// turns), routing_loco.cpp (the Sims in the way and the locomotion step),
// routing_walk.cpp (the routing primitives and the walk, tick by tick);
// routing_internal.hpp holds what they share.
#include "vm.hpp"
#include "formats/log_once.hpp"

#include "errors.hpp"
#include "go_here.hpp"
#include "interactions.hpp"
#include "locomotion.hpp"
#include "routing_internal.hpp"
#include "sim_motion.hpp"

#include "formats/binreader.hpp"
#include "formats/model.hpp"
#include "formats/rcol.hpp"
#include "formats/resindex.hpp"
#include "formats/scenegraph.hpp"
#include "formats/skeleton.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace os2::vm {

using namespace routing_internal;

bool VM::blocksRouting(const Object& o) {
    // What cTSObject::AddThisToPathPlanner (BV 0xaf68e0) leaves out of the
    // path planner: an object in a slot (GetContainer), one whose footprint
    // "blocks the ceiling" (FootprintBlocksCeiling: placement flags, od 42,
    // bit 0x1000 "attached to ceiling": the ceiling lamps), doors and windows
    // (types 8 and 9, and 12, the modular stairs' portals, unless OBJD field
    // 0x4b is 0x93: no base-game OBJD has it), out of the world, and the ones that let people through
    // (TSObjectGetPathPlannerCostAndPassibility: CanIntersectPeople, object
    // data 8 & 0x14 "allow object and person intersection" / "allow person
    // intersection": with a route penalty (od 10 >= 1) or an "allow
    // intersection" tree they go in as passable with cost od 10 x scale + 1,
    // but the scale (BV 0x02cc4c04, in __bss) has no writer: 0, so the cost
    // is 1, the floor's, and leaving them out is the same). The rest go in
    // by their footprints (routingFootprint), the stairs (types 10 and 11)
    // too since round 15 (OS2_STAIRS_OUT_OF_PLANNER=1 leaves them out, to
    // compare); Sims are blockers of their own (§5).
    if (!o.position.inWorld() || o.person || o.data[objdata::ContainerId] != 0)
        return false;
    if (o.definition) {
        static const bool stairsIn = std::getenv("OS2_STAIRS_OUT_OF_PLANNER") == nullptr;
        switch (o.definition->objectType()) {
        case kObjectTypeDoor:
        case kObjectTypeWindow:
        case kObjectTypeModularStairsPortal:
            // ... unless OBJD field 0x4b (file offset 0x40 + 2 x 0x4b) is
            // 0x93 (AddThisToPathPlanner, BV 0xaf68e0).
            if (o.definition->field(0x40 + 2 * 0x4b) != 0x93)
                return false;
            break;
        case kObjectTypeStairs:
        case kObjectTypeModularStairs:
            if (!stairsIn)
                return false;
            break;
        default: break;
        }
    }
    if (o.data[42] & 0x1000)
        return false;
    return (o.data[objdata::Flags] & (0x04 | 0x10)) == 0;
}

bool VM::tileFree(TilePos t, std::span<const ObjectId> ignore) {
    if (!world)
        throw VMError("no world (positions need a lot)");
    // A stair's landings and the tiles over its steps (on either of its
    // levels) stay open to walkers, floor or not: the portal trees' TEST
    // snaps the walker onto the portal's point at the top of the steps, and
    // its goal checks look there (every stair of N003_Lot192's decks was
    // locked). Placement (tryPlace) still wants a floor.
    for (const Stair& s : world->stairs())
        if ((t.level == s.bottom.level || t.level == s.top.level) && world->contains(t) &&
            t.x >= std::min(s.bottom.x, s.top.x) && t.x <= std::max(s.bottom.x, s.top.x) &&
            t.y >= std::min(s.bottom.y, s.top.y) && t.y <= std::max(s.bottom.y, s.top.y))
            return true;
    if (!world->walkable(t))
        return false;
    // A blocking object on the tile (whole tiles: placement, not routing).
    std::set<ObjectId> skip;
    for (ObjectId id : ignore)
        if (const Object* o = object(id))
            skip.insert(multiTileGroup(*o));
    for (const Object* o : byId_) {
        if (!o || o->position.level != t.level || !blocksRouting(*o))
            continue;
        const TilePos at = tileOf(*o);
        if (at.x == t.x && at.y == t.y && !skip.count(multiTileGroup(*o)))
            return false;
    }
    return true;
}

bool VM::pointFree(float x, float y, int level, std::span<const ObjectId> ignore) {
    if (!world)
        throw VMError("no world (positions need a lot)");
    const TilePos t{int(std::floor(x)), int(std::floor(y)), level};
    for (const Stair& s : world->stairs())
        if ((t.level == s.bottom.level || t.level == s.top.level) && world->contains(t) &&
            t.x >= std::min(s.bottom.x, s.top.x) && t.x <= std::max(s.bottom.x, s.top.x) &&
            t.y >= std::min(s.bottom.y, s.top.y) && t.y <= std::max(s.bottom.y, s.top.y))
            return true;
    if (!world->walkable(t))
        return false;
    std::set<ObjectId> skip;
    for (ObjectId id : ignore)
        if (const Object* o = object(id))
            skip.insert(multiTileGroup(*o));
    // A person's footprint at the point (TSObjectBaseFootprintWidth 0.2,
    // snapped to the 1/8 grid as GetFootprint does; docs/routing-mac.md §9.5).
    const auto box = personFootprintAt(nullptr, x, y);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            for (ObjectId id : objectsOnTile({t.x + dx, t.y + dy, level})) {
                const Object* o = object(id);
                if (!o || !blocksRouting(*o) || skip.count(multiTileGroup(*o)))
                    continue;
                for (const auto& f : routingFootprint(*o))
                    if (f[0] < box[2] && box[0] < f[2] && f[1] < box[3] && box[1] < f[3])
                        return false;
            }
    return true;
}

std::vector<ObjectId> VM::objectsOnTileWithContents(TilePos t) const {
    std::vector<ObjectId> out;
    std::set<ObjectId> seen;
    std::function<void(ObjectId)> add = [&](ObjectId id) {
        if (!seen.insert(id).second)
            return;
        out.push_back(id);
        std::vector<std::pair<int16_t, ObjectId>> inside;
        for (const ObjectId in : contained_.ids)
            if (const Object* o = uint16_t(in) < byId_.size() ? byId_[uint16_t(in)] : nullptr;
                o && o->id != id && o->data[objdata::ContainerId] == id && !dyingObjects.contains(o->id))
                inside.emplace_back(o->data[objdata::SlotNumber], o->id);
        std::sort(inside.begin(), inside.end());
        for (const auto& [slot, x] : inside)
            add(x);
    };
    for (ObjectId id : objectsOnTile(t))
        add(id);
    return out;
}

// The tile index: built from the objects in the world on the first query and
// kept by setPosition/removeObject. OS2_CHECK_TILE_INDEX=1 compares every
// objectsOnTile with the full scan it replaced.
static bool checkTileIndex() {
    static const bool on = std::getenv("OS2_CHECK_TILE_INDEX") != nullptr;
    return on;
}

void VM::ensureTileIndex() const {
    if (tilesBuilt_)
        return;
    tilesBuilt_ = true; // before the walk: indexObject reads it
    for (const Object* o : byId_)
        if (o)
            indexObject(*o);
}

void VM::indexObject(const Object& o) const {
    if (o.position.inWorld())
        tiles_[tileOf(o)].push_back(o.id);
}

void VM::unindexTile(TilePos t, ObjectId id) const {
    const auto it = tiles_.find(t);
    if (it == tiles_.end())
        return;
    auto& ids = it->second;
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    if (ids.empty())
        tiles_.erase(it);
}

void VM::unindexObject(const Object& o) const {
    if (!tilesBuilt_ || !o.position.inWorld())
        return;
    unindexTile(tileOf(o), o.id);
}

void VM::setPosition(Object& o, Position p) {
    if (!tilesBuilt_) {
        o.position = p;
        return;
    }
    const bool was = o.position.inWorld();
    const TilePos old = was ? tileOf(o) : TilePos{};
    o.position = p;
    const bool is = p.inWorld();
    // The walk changes the point, not the tile, most of the time.
    if (was && is && old == tileOf(o))
        return;
    if (was)
        unindexTile(old, o.id);
    if (is)
        indexObject(o);
}

std::vector<ObjectId> VM::objectsOnTile(TilePos t) const {
    ensureTileIndex();
    std::vector<ObjectId> out;
    if (const auto it = tiles_.find(t); it != tiles_.end())
        for (const ObjectId id : it->second)
            if (const Object* o = size_t(uint16_t(id)) < byId_.size() ? byId_[uint16_t(id)] : nullptr;
                o && o->position.inWorld() && o->data[objdata::ContainerId] == 0 && tileOf(*o) == t)
                out.push_back(id);
    // The scan this replaces walked byId_: ID order.
    std::sort(out.begin(), out.end());
    if (checkTileIndex()) {
        std::vector<ObjectId> want;
        for (const Object* o : byId_)
            if (o && o->position.inWorld() && o->data[objdata::ContainerId] == 0 && tileOf(*o) == t)
                want.push_back(o->id);
        if (want != out)
            std::fprintf(stderr, "tile index: %d,%d,%d has %zu objects, the scan %zu\n", t.x, t.y, t.level, out.size(),
                         want.size());
    }
    return out;
}

std::vector<std::vector<ObjectId>> VM::objectsAround(TilePos at, int r) const {
    // The scan, not the index: the radius of a "nearest X" search is large
    // (20+ tiles), and walking (2r + 1)^2 buckets costs far more than the
    // objects. byId_: ID order.
    const int side = 2 * r + 1;
    std::vector<std::vector<ObjectId>> out(size_t(side) * size_t(side));
    for (const Object* o : byId_) {
        if (!o || !o->position.inWorld() || o->data[objdata::ContainerId] != 0)
            continue;
        const TilePos t = tileOf(*o);
        if (t.level == at.level && std::abs(t.x - at.x) <= r && std::abs(t.y - at.y) <= r)
            out[size_t(t.x - at.x + r) * size_t(side) + size_t(t.y - at.y + r)].push_back(o->id);
    }
    return out;
}

} // namespace os2::vm

namespace os2::vm {

namespace {

// cTSObject::RunTree(5, ...): whether `o`'s "allow intersection" tree lets
// `other` overlap it (defined with the Go Here code below).
bool allowsIntersection(VM& vm, const Object& o, ObjectId other);

} // namespace

namespace routing_internal {

// Either one's tree lets the two overlap (TSObjectCanSetPosition and
// TSObjectMoveAwayFromOtherObjects ask both ways).
bool intersectionAllowed(VM& vm, const Object& a, const Object& b) {
    return allowsIntersection(vm, a, b.id) || allowsIntersection(vm, b, a.id);
}

// How far a point is from the nearest wall edge of its tile and the tiles
// around (a diagonal wall: from its tile's centre); 1e9 with none.
float nearestWallEdge(const World& w, float px, float py, int level) {
    const TilePos t{int(std::floor(px)), int(std::floor(py)), level};
    float nearest = 1e9f;
    for (int x = t.x - 1; x <= t.x + 1; ++x)
        for (int y = t.y - 1; y <= t.y + 1; ++y) {
            const TilePos u{x, y, level};
            if (!w.contains(u))
                continue;
            if (w.diagonalWall(u))
                nearest = std::min(nearest, std::hypot(float(x) + 0.5f - px, float(y) + 0.5f - py));
            auto segment = [&](float ax, float ay, float bx, float by) {
                const float vx = bx - ax, vy = by - ay;
                const float k = std::clamp(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
                nearest = std::min(nearest, std::hypot(ax + vx * k - px, ay + vy * k - py));
            };
            if (w.wallBetween(u, -1, 0)) segment(float(x), float(y), float(x), float(y + 1));
            if (w.wallBetween(u, 0, -1)) segment(float(x), float(y), float(x + 1), float(y));
        }
    return nearest;
}

} // namespace routing_internal

namespace {

// cTSRoute::EvalTileForGoal (docs/routing-mac.md §8.2), for a candidate
// goal point of a slot around `target` (whose tile is `centre`): the code
// (0 out of the world, 2 another room, 3 a wall between it and the object,
// 4 a wall within 0.3 tile, 5 nothing to stand on, 6 an object on it, 7
// valid) and, when valid, the penalties added to its cost: +10 for a wall
// within 0.4 tile, +10 for another room when the slot ignores rooms, +5 for
// a door on its tile. Occupied, from TSObjectCanSetPositionAndOrientation's
// errors in the binary: 1 for an object in the way (a blocking object's
// footprint under the Sim's box at the point, not a part of the target) and 6 for a person (object type 2,
// vtable 0x184: another Sim, not the router, whose centre is within 0.283 of
// the point, TSObjectCanSetPositionAndOrientation's test between people,
// unless the two may overlap; round n30, read in BV 0x475074: was half a
// tile, inferred). Not done: the pushable objects (+10 instead of
// 1: an object of type 8 when the route may push), the water/terrain query
// (+5), and the slot flags that allow 2/3 with +10.
int evalGoal(VM& vm, const World& w, const Object& target, TilePos centre, float px, float py, int level,
             bool ignoreRooms, ObjectId router, float& penalty) {
    penalty = 0;
    const TilePos t{int(std::floor(px)), int(std::floor(py)), level};
    if (!w.contains(t))
        return 0;
    if (w.room(t) != w.room(centre)) {
        if (!ignoreRooms)
            return 2;
        penalty += 10;
    }
    if (!(t.x == centre.x && t.y == centre.y) && !w.lineOfSight(centre, t))
        return 3;
    const float nearest = nearestWallEdge(w, px, py, level);
    if (nearest < 0.3f)
        return 4;
    if (nearest < 0.4f)
        penalty += 10;
    if (!w.walkable(t))
        return 5;
    // The Sim's footprint box at the point against the objects' footprints
    // (TSObjectCanSetPositionAndOrientation), not the whole tile: a glass
    // on the floor takes only its model's box.
    const ObjectId group = multiTileGroup(target);
    const Object* walker = vm.object(router);
    // The router's footprint at the point, as GetFootprint gives it (the
    // 0.2 box snapped to the 1/8 grid, docs/routing-mac.md §9.5).
    const auto box = VM::personFootprintAt(walker, px, py);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            for (ObjectId id : vm.objectsOnTile({t.x + dx, t.y + dy, level})) {
                const Object* o = vm.object(id);
                if (!o || multiTileGroup(*o) == group || !VM::blocksRouting(*o))
                    continue;
                bool overlaps = false;
                for (const auto& f : vm.routingFootprint(*o))
                    overlaps |= f[0] < box[2] && box[0] < f[2] && f[1] < box[3] && box[1] < f[3];
                if (overlaps && !(walker && intersectionAllowed(vm, *o, *walker)))
                    return 1;
            }
    for (const ObjectId id : vm.people()) { // by ID, as objects() is
        const Object* o = vm.object(id);
        if (o && o->person && id != router && id != target.id && o->position.inWorld() && o->position.level == level &&
            o->data[objdata::ContainerId] == 0 && std::hypot(o->position.x - px, o->position.y - py) < kPeopleApart &&
            !(walker && peopleMayOverlap(vm, *walker, *o)))
            return 6;
    }
    for (const World::Door& d : w.doors())
        if (d.tile == t)
            penalty += 5;
    return 7;
}

// The route result of a candidate EvalTileForGoal turned down (BV
// 0x478f8c..: 1/6 destination occupied, 2-4 wall in the way, 5 altitudes
// don't match; 0 records nothing).
int16_t refusedResult(int code) {
    switch (code) {
    case 1:
    case 6: return int16_t(RouteFail::DestinationOccupied);
    case 2:
    case 3:
    case 4: return int16_t(RouteFail::WallInTheWay);
    case 5: return int16_t(RouteFail::AltitudesDontMatch);
    default: return 0;
    }
}

} // namespace

// Routing slots (SLOT type 3) say where a Sim stands to use an object. Read
// as FreeSO's VMSlotParser reads The Sims 1's, with the Sims 2 values (see
// Slots::Slot): flags bits 0-7 are the allowed directions around the object
// (bit n = direction n relative to the object's own; 0x55 the four sides,
// 0xFF all eight), 0x200 makes them absolute, 0x800 ignores rooms, 0x1000
// snaps the facing, 0x10000 uses the average position of a multi-tile
// object; the distances are in tiles; "facing" is -3 anywhere, -2 towards the
// object, -1 away from it, 0-7 a direction relative to the object's
// (inferred: the global SLOT's eight offset slots, 0.7 tiles around the
// object, all face away from it that way, as a Sim stands to sit down). A slot
// with an offset or without directions is a point (offsets are in tiles in
// the object's model space, whose forward axis is +Y); in the Sims 2 most
// object slots are points with no offset: the position comes from a bone of
// the model ("No bone on model corresponding to name in routing slot"), which
// is taken here as the object's own tile. Goals are whole tiles: a range of
// distances becomes the tiles whose centres are within it (half a tile of
// slack), in an allowed direction, with no wall in between and in the
// object's room; each costs how far it is from the optimal distance.
VM::RouteGoals VM::routingGoals(const Slots::Slot& slot, const Object& target, bool evaluate, ObjectId router) {
    RouteGoals out;
    out.level = target.position.level;
    float cx = target.position.x, cy = target.position.y;
    if ((slot.flags & 0x10000) && multiTileGroup(target)) {
        float sx = 0, sy = 0;
        int n = 0;
        for (const auto& [id, o] : objects_)
            if (multiTileGroup(o) == multiTileGroup(target) && o.position.inWorld())
                sx += o.position.x, sy += o.position.y, ++n;
        if (n)
            cx = sx / float(n), cy = sy / float(n);
    }
    const int objDir = ((target.data[objdata::Direction] % 8) + 8) % 8;
    // The model of a multi-tile object is the lead tile's: its bones are
    // placed from the lead's point and direction, whichever part's slot
    // names them (the straight staircase's top pad, "Hi 0,0", has the
    // "route_staircasestraight_*_top" bones 3.7 tiles up from the lead "Lo
    // 0,4"; from the pad itself they fell 3.7 tiles past the top, the stair
    // portal's Snap failed and no one reached the upper floors of N001_Lot11;
    // inferred: the scene graph's model is the lead's).
    float bcx = cx, bcy = cy;
    int boneDir = objDir;
    if (const ObjectId lead = multiTileGroup(target); lead != target.id)
        if (const Object* l = object(lead); l && l->position.inWorld())
            bcx = l->position.x, bcy = l->position.y, boneDir = ((l->data[objdata::Direction] % 8) + 8) % 8;
    const uint32_t dirs = slot.flags & 0xFF;
    const bool absolute = slot.flags & 0x200, ignoreRooms = slot.flags & 0x800;
    auto facingAt = [&](float px, float py) {
        if (slot.facing >= 0)
            return (objDir + slot.facing) % 8;
        if (slot.facing == -2 || slot.facing == -1) {
            const int d = slot.facing == -2 ? directionOf(cx - px, cy - py) : directionOf(px - cx, py - cy);
            return d < 0 ? objDir : d;
        }
        return -1;
    };
    const TilePos centre = tileOf(target);
    // The slot's weights (BV 0x478d2c, the slot's vtable 0xD8): sitting x 10
    // and the larger of standing and ground x 10 (10 when neither is set).
    const int sitWeight = std::max(slot.sitting, 0) * 10;
    int standWeight = std::max(std::max(slot.standing, slot.ground), 0) * 10;
    if (sitWeight == 0 && standWeight == 0)
        standWeight = 10;
    // A bone slot's goals sit in the object itself (their +0x10, the chair)
    // when it weighs sitting over standing and the object is a chair
    // (GetPossibleSlotsFromModelRoutingSlot, BV 0x475b98; IsChair: the OBJf
    // "sit" entry, 26, has a tree): the high chair's slot (sitting 1,
    // standing 0); not the toilets' (1 and 1).
    const ObjectId boneChair = sitWeight > standWeight && functionTree(target, 26) ? target.id : 0;
    // A candidate goal: with `evaluate`, EvalTileForGoal's verdict and penalties.
    auto add = [&](float px, float py, float cost) {
        if (evaluate && world) {
            float penalty = 0;
            const int code = evalGoal(*this, *world, target, centre, px, py, out.level, ignoreRooms, router, penalty);
            if (code != 7) {
                if (const int16_t why = refusedResult(code))
                    out.refused = why;
                return false;
            }
            cost += penalty;
        }
        out.goals.push_back({int(std::floor(px)), int(std::floor(py)), cost});
        out.points.push_back({px, py});
        out.facings.push_back(facingAt(px, py));
        return true;
    };

    float ox = slot.offset[0], oy = slot.offset[1];
    // 0x1000 "snap to direction": the slot is the object's point (plus its
    // offset) facing the object's direction turned by the slot's facing (a
    // direction 0-7), or by its lowest direction bit (none: N). The global
    // SLOT's "CT - Snap Me" slots (0x0F, 0x10, 0x1F, 0x20) turn an object in
    // place this way: "Align Trashcan To Sim" snaps the can a step at a time
    // until it faces the Sim, and spun forever when the snap moved it one
    // tile off without turning it (N001_Lot36). As FreeSO's VMSlotParser
    // (inferred for the Sims 2).
    if (slot.flags & 0x1000) {
        // At the bones of the model (flag 0x20000): each bone's point with
        // its facing, as the slots that stand there (the chairs' get-out
        // snaps the Sim to the corner bone it gets up at: snapping it to the
        // chair's own point left it standing inside the chair).
        if ((slot.unknown10 & 0x20000) && ox == 0 && oy == 0) {
            const std::vector<SlotBone> bones = slotBones(slot, target);
            if (!bones.empty()) {
                const double yaw = (90.0 - 45.0 * boneDir) * M_PI / 180.0;
                for (const SlotBone& b : bones) {
                    const float px = bcx + float(b.x * std::cos(yaw) - b.y * std::sin(yaw));
                    const float py = bcy + float(b.x * std::sin(yaw) + b.y * std::cos(yaw));
                    if (out.goals.empty())
                        out.onTarget = int(std::floor(px)) == centre.x && int(std::floor(py)) == centre.y;
                    out.goals.push_back({int(std::floor(px)), int(std::floor(py)), 0});
                    out.points.push_back({px, py});
                    out.facings.push_back((boneDir + b.facing) % 8);
                    out.multi.push_back(b.index);
                }
                out.chairs.assign(out.goals.size(), boneChair);
                return out;
            }
        }
        int turnBy = 0;
        if (slot.facing >= 0)
            turnBy = slot.facing % 8;
        else
            for (int d = 0; d < 8; ++d)
                if (dirs & (1u << d)) {
                    turnBy = d;
                    break;
                }
        const double yaw = (90.0 - 45.0 * objDir) * M_PI / 180.0;
        const float px = cx + float(ox * std::cos(yaw) - oy * std::sin(yaw));
        const float py = cy + float(ox * std::sin(yaw) + oy * std::cos(yaw));
        out.onTarget = int(std::floor(px)) == centre.x && int(std::floor(py)) == centre.y;
        out.goals.push_back({int(std::floor(px)), int(std::floor(py)), 0});
        out.points.push_back({px, py});
        out.facings.push_back((objDir + turnBy) % 8);
        return out;
    }
    // A slot at the bones of the model (flag 0x20000) stands at its bones
    // whatever its directions say (GetPossibleSlotsFromModelRoutingSlot: one
    // goal per bone): the fridge's slot 0 has direction 0 and 2 to 4 tiles,
    // and its "route_0" bone a tile in front of it; scanning 2 tiles out put
    // the Sims of N002_Lot22 on the counter across the kitchen and they
    // starved.
    const bool atBones = (slot.unknown10 & 0x20000) && ox == 0 && oy == 0 && !slotBones(slot, target).empty();
    if (dirs == 0 || ox != 0 || oy != 0 || atBones) {
        const double yaw = (90.0 - 45.0 * objDir) * M_PI / 180.0;
        auto turn = [&](float x, float y) {
            return std::array<float, 2>{cx + float(x * std::cos(yaw) - y * std::sin(yaw)),
                                        cy + float(x * std::sin(yaw) + y * std::cos(yaw))};
        };
        // A slot at the bones of the model (flag 0x20000 of the u32 after the
        // proximities, Slots::Slot::unknown10) stands there: one goal per
        // bone, facing the object's direction plus the bone's
        // (GetPossibleSlotsFromModelRoutingSlot: score 100 less the
        // penalties; slot_bones.cpp).
        std::vector<SlotBone> bones;
        if ((slot.unknown10 & 0x20000) && ox == 0 && oy == 0)
            bones = slotBones(slot, target);
        if (bones.empty()) {
            const auto [px, py] = turn(ox, oy);
            out.onTarget = int(std::floor(px)) == centre.x && int(std::floor(py)) == centre.y;
            add(px, py, 0);
            return out;
        }
        // Each goal keeps its bone's index in the slot's list, the goal's
        // multi slot index (+0x14; person data 38 once the way is planned).
        auto addFacing = [&](float px, float py, float cost, int facing, int16_t multi) {
            if (!add(px, py, cost))
                return;
            out.facings.back() = facing;
            out.multi.resize(out.goals.size() - 1, 0);
            out.multi.push_back(multi);
        };
        const double boneYaw = (90.0 - 45.0 * boneDir) * M_PI / 180.0;
        auto turnBone = [&](float x, float y) {
            return std::array<float, 2>{bcx + float(x * std::cos(boneYaw) - y * std::sin(boneYaw)),
                                        bcy + float(x * std::sin(boneYaw) + y * std::cos(boneYaw))};
        };
        bool first = true;
        for (const SlotBone& b : bones) {
            const auto [px, py] = turnBone(b.x, b.y);
            if (first)
                out.onTarget = int(std::floor(px)) == centre.x && int(std::floor(py)) == centre.y;
            first = false;
            addFacing(px, py, 0, (boneDir + b.facing) % 8, b.index);
        }
        // (Until round n18 a ring of points half a tile and three quarters
        // out around each bone, ours, stood in for a bone point that could
        // not be reached for want of the models' footprints; the binary has
        // the bone points only: a Sim poured coffee from the counter's side,
        // 0.75 off its "route_counter_std_0_N" bone, with its hand 0.13 short
        // of the machine. OS2_BONE_RING=1 puts it back, to compare.)
        static const bool ring = std::getenv("OS2_BONE_RING") != nullptr;
        if (world && ring)
            for (const SlotBone& b : bones) {
                const auto [px, py] = turnBone(b.x, b.y);
                for (const float r : {0.5f, 0.75f})
                    for (int k = 0; k < 16; ++k) {
                        const double a = k * M_PI / 8;
                        const float qx = px + r * float(std::cos(a)), qy = py + r * float(std::sin(a));
                        const TilePos t{int(std::floor(qx)), int(std::floor(qy)), out.level};
                        if (!world->contains(t) || !world->lineOfSight(centre, t))
                            continue;
                        addFacing(qx, qy, 1 + r, (boneDir + b.facing) % 8, b.index);
                    }
            }
        out.chairs.assign(out.goals.size(), boneChair);
        return out;
    }
    if (!world)
        return out;
    // The binary's goal scan (docs/routing-mac.md §8.2; BV 0x478d2c, the
    // scan 0x4778b4 and its standing callback 0x4740d8): one origin per
    // allowed direction (the slot's offset turned that way), and from each,
    // rows across the direction at distances from the preferred one outwards
    // (preferred, then a step nearer, a step further, ...: the row count is
    // the larger of max - preferred and preferred - min in tiles, rounded
    // up, not in steps, as the binary has it), each row as wide as a cone of
    // 45 degrees (|lateral| <= distance * tan 22.5 degrees), on the slot's
    // grid (its resolution, at least 1/16 tile). The distances must be
    // ordered (0 <= min <= preferred <= max, max at most 32) or there are no
    // goals. It stops after 100 candidates or 48 accepted ones. A candidate's
    // cost (the binary keeps a score, 100 less this) is |d - preferred| * 16
    // * the slot's gradient + EvalTileForGoal's penalties + 0.001 * gradient
    // * |lateral| (a tie-break) + 10 per routing cost of its floor above 1.
    // Not done: the router's own radius added to the distances (vtable 0xB4
    // of the router: its "slot displacement", 0 for the base game's Sims),
    // The +25 for goals away from the target's walls is scanStanding's
    // (below). The +20 is HasStationaryBonus (below). With a sitting weight the
    // chairs in the cone give sitting goals too (scanSitting below).
    const float minD = slot.minDistance, optimal = slot.optimalDistance;
    const float maxD = std::min(slot.maxDistance, 32.0f);
    if (minD < 0 || maxD < 0 || maxD < minD || optimal < minD || maxD < optimal)
        return out;
    const float res = std::max(slot.resolutionTiles, 0.0625f);
    constexpr float kScanCone = 0.7853982f; // the scan context's +0x40 (BV 0x274b59c)
    const float spread = std::tan(kScanCone * 0.5f);
    const float weight = slot.gradient;
    const int rows = int(std::ceil(std::max(maxD - optimal, optimal - minD)));
    std::vector<int> entries; // absolute directions
    for (int d = 0; d < 8; ++d)
        if (dirs & (1u << d))
            entries.push_back(absolute ? d : (d + objDir) % 8);
    const double yaw = (90.0 - 45.0 * objDir) * M_PI / 180.0;
    const float ex = float(ox * std::cos(yaw) - oy * std::sin(yaw));
    const float ey = float(ox * std::sin(yaw) + oy * std::cos(yaw));
    // The standing goals (BV 0x478518: the scan with the standing callback,
    // then standing weight x 10 added to each score; here taken off the
    // cost). Limits: 100 candidates and 48 accepted, 16 more each for the
    // second of two scans.
    auto scanStandingBody = [&](int maxCandidates, int maxAccepted, int& lastEntry) {
        int candidates = 0, accepted = 0;
        struct Tile {
            float cost;
            size_t index;
            float lateral;
        };
        std::map<std::pair<int, int>, Tile> tiles; // without `evaluate`: the best point of each tile
        // Slot flag 0x2000 "random scoring" (the slot's vtable 0x78; FreeSO's
        // SLOTFlags.RandomScoring): when the scan has more candidates than the
        // 100 it may look at, each is taken with a chance of 100 in the total
        // (BV 0x4778b4: a random number below the total, at most 100 - the count
        // so far at the start), and a goal's score gets a random 20 x [0, 1) - 10
        // (the standing callback; here its cost, lower is better: + 10 - 20 x).
        const bool randomScoring = slot.flags & 0x2000;
        const Object* stationary = slot.flags & 0x40000 ? nullptr : object(router);
        if (stationary && (stationary->data[objdata::ContainerId] != 0 || stationary->position.level != out.level))
            stationary = nullptr;
        int total = 0;
        if (randomScoring)
            for (int i = 0; i <= rows; ++i)
                for (const int sign : {-1, 1}) {
                    const float r = optimal + float(sign * i) * res;
                    if ((i == 0 && sign == 1) || r < minD || r > maxD)
                        continue;
                    total += (int(std::ceil((r * spread) / res)) - int(std::floor(-(r * spread) / res)) + 1) *
                             int(entries.size());
                }
        for (int i = 0; i <= rows; ++i)
            for (const int sign : {-1, 1}) {
                if (i == 0 && sign == 1)
                    continue;
                const float r = optimal + float(sign * i) * res;
                if (r < minD || r > maxD)
                    continue;
                const int lo = int(std::floor(-(r * spread) / res)), hi = int(std::ceil((r * spread) / res));
                for (int j = lo; j <= hi; ++j) {
                    const float lateral = float(j) * res;
                    for (const int d : entries) {
                        if (candidates >= maxCandidates || accepted >= maxAccepted)
                            return;
                        lastEntry = d; // the scan's context direction (+0x20)
                        if (total > 100 && std::uniform_int_distribution<int>(0, total - 1)(random) > 100)
                            continue;
                        // The cone (FUN_01655af0, before EvalTileForGoal; round n30):
                        // 2 atan2(|lateral| - width, forward) within the scan's
                        // 45 degrees (+0x40; the width +0x44 is 0 for standing
                        // goals). The row's ends, a step past r tan 22.5
                        // degrees, are left out.
                        if (2.0 * std::atan2(double(std::fabs(lateral)), double(r)) > double(kScanCone))
                            continue;
                        ++candidates;
                        const auto [sx, sy] = directionStep(d);
                        const float n = std::hypot(float(sx), float(sy));
                        const float fx = float(sx) / n, fy = float(sy) / n; // forward
                        const float px = cx + ex + fx * r - fy * lateral, py = cy + ey + fy * r + fx * lateral;
                        float cost = std::fabs(std::hypot(r, lateral) - optimal) * 16 * weight +
                                     0.001f * weight * std::fabs(lateral) - float(standWeight);
                        if (randomScoring)
                            cost += 10 - 20 * std::uniform_real_distribution<float>(0, 1)(random);
                        // HasStationaryBonus (round n03; BV 0x478518, the
                        // slot's vtable 0x80: RS flag 0x40000 off): +20 for
                        // the goal right where the router stands, unless it
                        // is contained (vtable 0x11c IsContained).
                        if (stationary && std::fabs(px - stationary->position.x) < 1e-3f &&
                            std::fabs(py - stationary->position.y) < 1e-3f)
                            cost -= 20;
                        if (!evaluate) {
                            const TilePos t{int(std::floor(px)), int(std::floor(py)), out.level};
                            if (!world->contains(t) || (t.x == centre.x && t.y == centre.y))
                                continue;
                            if (!ignoreRooms && world->room(t) != world->room(centre))
                                continue;
                            if (!world->lineOfSight(centre, t))
                                continue;
                            // The tile's best point: the cheapest, then the nearest the row's middle.
                            auto it = tiles.find({t.x, t.y});
                            if (it != tiles.end()) {
                                if (cost < it->second.cost ||
                                    (cost == it->second.cost && std::fabs(lateral) < it->second.lateral)) {
                                    const size_t k = it->second.index;
                                    it->second = {cost, k, std::fabs(lateral)};
                                    out.goals[k].cost = cost;
                                    out.points[k] = {px, py};
                                    out.facings[k] = facingAt(px, py);
                                }
                                continue;
                            }
                            tiles[{t.x, t.y}] = {cost, out.goals.size(), std::fabs(lateral)};
                        }
                        const TilePos t{int(std::floor(px)), int(std::floor(py)), out.level};
                        const float floorCost = world->contains(t) ? world->cost(t, px, py) : 1;
                        if (add(px, py, cost + (floorCost > 1 ? (floorCost - 1) * 10 : 0)))
                            ++accepted;
                    }
                }
            }
    };
    // The +25 (FUN_00478518, after the scan; docs/routing-mac.md §9.1):
    // unless the slot has RS flag 0x80000, the walls of the target's tile
    // (cEdithObjectModule::GetWallBlockFlagsAtTile: bit d for a wall on the
    // tile's edge toward direction d, the diagonals not), turned into the
    // target's frame and each spread to its two neighbours; every standing
    // goal of the scan scores 25 more when that mask does not hold the
    // scan context's direction, which the scan leaves at the direction of
    // the last origin it went through (absolute: the binary compares it
    // with the turned mask as it is). Here a cost, 25 less.
    auto scanStanding = [&](int maxCandidates, int maxAccepted) {
        const size_t first = out.goals.size();
        int lastEntry = 0;
        scanStandingBody(maxCandidates, maxAccepted, lastEntry);
        if (slot.flags & 0x80000)
            return;
        int walls = 0;
        for (int d = 0; d < 8; d += 2) {
            const auto [sx, sy] = directionStep(d);
            if (world->wallBetween(centre, sx, sy, true))
                walls |= 1 << ((d - objDir + 8) % 8);
        }
        int mask = 0;
        for (int d = 0; d < 8; ++d)
            if (walls & (1 << d))
                mask |= (1 << d) | (1 << ((d + 1) % 8)) | (1 << ((d + 7) % 8));
        if (!(mask & (1 << (lastEntry & 7))))
            for (size_t k = first; k < out.goals.size(); ++k)
                out.goals[k].cost -= 25;
    };
    // The sitting goals (ScanConeForSittingGoals, BV 0x4788cc; docs/seat-mac.md
    // §A): the same cone, whole tiles (resolution 1) and a tile wider each
    // side (+0x44 = 1: the distances one tile further each way, each row's
    // half width d x tan 22.5 degrees + 1); every object on a tile of it
    // (ScanConeForChairRoutingSlotsCB, BV 0x4776fc) that is a chair (OBJf
    // "sit", 26, has a tree) facing back along the cone's direction (its
    // direction = the cone's + 4), not for sale (od 106), with its
    // interactions on (AreInteractionsDisabled: od 87 < 1, od 82 bit 2 off)
    // and nobody but the router in its slot 0, gives the goals of its first
    // bone routing slot with more than one bone and no containment flag
    // (GetPossibleSittingSlotsFromModel, BV 0x4763e4; only for a chair with
    // a "stand" tree, OBJf 27), each sitting in it. A goal's score: 100 less
    // EvalTileForGoal's penalties from the chair's point and from the
    // target's, + 25 (the scan passes no direction mask), + sitting weight
    // x 10, - |d - preferred| x 16 x gradient - 0.01 x gradient x |lateral|
    // (d and lateral from the cone's origin); here the cost, 100 less that.
    // The binary does not count these against the candidate limits. A chair
    // another Sim sits in: the route fails "destination occupied" (7) when
    // no goal is left. Ours: a chair met again (a second point of the cone
    // on its tile) is not taken twice (the binary adds the same goals again).
    auto scanSitting = [&] {
        const float wide = 1;
        const float sMin = std::max(minD - wide, 0.0f), sMax = maxD + wide;
        const int sRows = int(std::ceil(std::max(sMax - optimal, optimal - sMin)));
        std::set<ObjectId> seen;
        bool occupied = false;
        for (int i = 0; i <= sRows; ++i)
            for (const int sign : {-1, 1}) {
                if (i == 0 && sign == 1)
                    continue;
                const float r = optimal + float(sign * i);
                if (r < sMin || r > sMax)
                    continue;
                const float half = r * spread + wide;
                const int lo = int(std::floor(-half)), hi = int(std::ceil(half));
                for (int j = lo; j <= hi; ++j) {
                    const float lateral = float(j);
                    for (const int d : entries) {
                        const auto [sx, sy] = directionStep(d);
                        const float n = std::hypot(float(sx), float(sy));
                        const float fx = float(sx) / n, fy = float(sy) / n;
                        const float px = cx + ex + fx * r - fy * lateral, py = cy + ey + fy * r + fx * lateral;
                        const TilePos t{int(std::floor(px)), int(std::floor(py)), out.level};
                        for (const ObjectId id : objectsOnTile(t)) {
                            const Object* chair = object(id);
                            if (!chair || chair->person || seen.count(id))
                                continue;
                            if (std::getenv("OS2_DEBUG_SIT_GOALS")) // the objects the sitting cone meets
                                std::fprintf(stderr, "sit cone of %d (dir %d): %d %s at (%.1f, %.1f) dir %d, sit %u stand %u\n", target.id, d, id,
                                             chair->definition ? chair->definition->fileName.c_str() : "?", chair->position.x,
                                             chair->position.y, chair->data[objdata::Direction], functionTree(*chair, 26),
                                             functionTree(*chair, 27));
                            if (!functionTree(*chair, 26) || ((chair->data[objdata::Direction] % 8) + 8) % 8 != (d + 4) % 8)
                                continue;
                            if (chair->data[106] != 0 || chair->data[87] >= 1 || (chair->data[82] & 0x04))
                                continue;
                            seen.insert(id);
                            if (const ObjectId in = containedIn(id, 0); in && in != router) {
                                occupied = true;
                                continue;
                            }
                            if (!functionTree(*chair, 27))
                                continue;
                            const Slots::Slot* seat = nullptr;
                            for (const Slots::Slot* s : routingSlots(slotTable(*this, *chair)))
                                if ((s->unknown10 & 0x20000) && !(s->flags & 0x200000) && slotBones(*s, *chair).size() > 1) {
                                    seat = s;
                                    break;
                                }
                            if (!seat)
                                continue;
                            // The chair's model: the lead's for a part of a multi-tile one.
                            float lx = chair->position.x, ly = chair->position.y;
                            int ldir = ((chair->data[objdata::Direction] % 8) + 8) % 8;
                            if (const ObjectId lead = multiTileGroup(*chair); lead != chair->id)
                                if (const Object* l = object(lead); l && l->position.inWorld())
                                    lx = l->position.x, ly = l->position.y, ldir = ((l->data[objdata::Direction] % 8) + 8) % 8;
                            const double cyaw = (90.0 - 45.0 * ldir) * M_PI / 180.0;
                            for (const SlotBone& b : slotBones(*seat, *chair)) {
                                const float gx = lx + float(b.x * std::cos(cyaw) - b.y * std::sin(cyaw));
                                const float gy = ly + float(b.x * std::sin(cyaw) + b.y * std::cos(cyaw));
                                float penalty = 0;
                                if (evaluate && world) {
                                    float p1 = 0, p2 = 0;
                                    const int c1 = evalGoal(*this, *world, *chair, tileOf(*chair), gx, gy, out.level,
                                                            ignoreRooms, router, p1);
                                    const int c2 = c1 == 7 ? evalGoal(*this, *world, target, centre, gx, gy, out.level,
                                                                      ignoreRooms, router, p2)
                                                           : c1;
                                    if (c2 != 7) {
                                        if (const int16_t why = refusedResult(c2))
                                            out.refused = why;
                                        continue;
                                    }
                                    penalty = p1 + p2;
                                }
                                // d and lateral from the cone's origin, along and across its direction.
                                const float vx = gx - (cx + ex), vy = gy - (cy + ey);
                                const float along = vx * fx + vy * fy, across = -vx * fy + vy * fx;
                                float cost = penalty - 25 - float(sitWeight);
                                if (weight > 0)
                                    cost += std::fabs(std::hypot(along, across) - optimal) * 16 * weight +
                                            0.01f * weight * std::fabs(across);
                                out.goals.push_back({int(std::floor(gx)), int(std::floor(gy)), cost});
                                out.points.push_back({gx, gy});
                                out.facings.push_back((ldir + b.facing) % 8);
                                out.multi.resize(out.goals.size() - 1, 0);
                                out.multi.push_back(b.index);
                                out.chairs.resize(out.goals.size() - 1, 0);
                                out.chairs.push_back(id);
                            }
                        }
                    }
                }
            }
        if (occupied && out.goals.empty() && !out.refused)
            out.refused = int16_t(RouteFail::DestinationOccupied);
    };
    // Both weights: the heavier scan first (sitting when it weighs at least
    // as much), with the limits; one weight: that scan alone (BV 0x4797b8).
    if (!world)
        return out;
    if (standWeight > 0 && sitWeight > 0) {
        if (sitWeight >= standWeight) {
            scanSitting();
            scanStanding(116, 64);
        } else {
            scanStanding(100, 48);
            scanSitting();
        }
    } else if (standWeight > 0) {
        scanStanding(100, 48);
    } else {
        scanSitting();
    }
    if (!out.chairs.empty())
        out.chairs.resize(out.goals.size(), 0);
    if (!out.multi.empty())
        out.multi.resize(out.goals.size(), 0);
    return out;
}

} // namespace os2::vm

namespace os2::vm {

// The binary keeps the destinations as managed destination objects of the
// router and flushes them (cTSRoutingObjectAggregate::
// FlushManagedDestinationObjects, BV 0x4f9eb4: kept while in the person's
// queue, current or sub-queue interaction, or in its routing state); when
// it runs is inferred (each routing step, and when a pie menu closes).
void flushDestinations(VM& vm) {
    std::vector<ObjectId> done;
    vm.checkContained();
    for (const ObjectId did : vm.destinationObjects()) { // by ID, as objects() is
        const Object* po = vm.object(did);
        if (!po)
            continue;
        const Object& o = *po;
        const ObjectId id = o.id;
        if (!(o.syntheticSlot || o.managedDestination) || o.destinationHeld)
            continue;
        bool used = false;
        for (const Object* sim : vm.objectTable())
            if (sim && sim->person)
                for (const auto& q : sim->queue)
                    used = used || q.object == id;
        if (!used)
            done.push_back(id);
    }
    for (const ObjectId id : done)
        vm.removeObject(id);
}

namespace {

// A footprint: rectangles x0, y0, x1, y1 in world tiles.
using Footprint = std::vector<std::array<float, 4>>;

// The OBJD field of an object (file offset), 0 without a definition.
uint16_t objdField(const Object& o, size_t offset) { return o.definition ? o.definition->field(offset) : 0; }
constexpr size_t kObjdTileWidth = 0xC6;     // field 0x43 (TSObjectBaseFootprintWidth)
constexpr size_t kObjdFootprintMask = 0xDA; // field 0x4D
constexpr int kTypePerson = 2, kTypeDoor = 8;

int objectType(const Object& o) { return o.person ? kTypePerson : int(objdField(o, 0x52)); }

// TSObjectWorldSpaceFootprint's box (BV 0x8aa40c): a square of half side
// 0.5 * the base width (a person 0.2, else OBJD field 0x43 / 16, 1 when 0)
// about the object's position, each side moved in by a nibble of OBJD
// field 0x4D / 16 (out by person data 73 for a person), the nibbles turned
// with the facing: (8 - direction) / 2 picks which nibble each side takes.
// GetFootprint works it out at direction 0 (with the person's nibbles) and
// turns it by the direction & 6 (BV FUN_0191fa14, base FUN_00b43bfc;
// docs/routing-mac.md §9.5): an odd direction turns as the even one below
// it. Inferred: the direction index is object data 1. The model's aggregate
// footprint comes first (routingFootprint); this box stands in without one.
// `at` puts it at another point (GetFootprint's location; the object's own
// when null).
Footprint footprintOf(const Object& o, const std::array<float, 2>* at = nullptr) {
    const float half = 0.5f * (o.person ? 0.2f : (objdField(o, kObjdTileWidth) ? float(objdField(o, kObjdTileWidth)) / 16 : 1.0f));
    std::array<float, 4> c{0, 0, 0, 0};
    const int dir = ((o.data[objdata::Direction] % 8) + 8) % 8 & 6;
    if (const uint16_t m = objdField(o, kObjdFootprintMask); m && !o.person)
        for (int k = 0; k < 4; ++k)
            c[size_t(k)] = float((m >> (4 * k)) & 0xF) / 16;
    if (o.person && o.person->data.size() > 73)
        if (const uint16_t m = uint16_t(o.person->data[73]))
            for (int k = 0; k < 4; ++k)
                c[size_t(k)] -= float((m >> (4 * k)) & 0xF) / 16;
    const int u = (8 - dir) / 2;
    const float px = at ? (*at)[0] : o.position.x, py = at ? (*at)[1] : o.position.y;
    float x0 = px - half, y0 = py - half, x1 = px + half, y1 = py + half;
    x0 += c[size_t(u % 4)];
    y1 -= c[size_t((u + 1) % 4)];
    x1 -= c[size_t((u + 2) % 4)];
    y0 += c[size_t((u + 3) % 4)];
    return {{x0, y0, x1, y1}};
}

// A door's indexed footprints 0 and 2 (TSObjectIndexedWorldSpaceFootprint;
// the model's footprint data is not read). Inferred: the leaf's swing, the
// half tile on each side of the wall the door cuts (0 in front, 2 behind).
Footprint doorFootprint(const Object& o, int index) {
    const TilePos t = VM::tileOf(o);
    const auto [dx, dy] = directionStep(o.data[objdata::Direction] + 4); // towards the wall behind
    float x0 = float(t.x), y0 = float(t.y), x1 = x0 + 1, y1 = y0 + 1;
    const float side = index == 0 ? 0.0f : 1.0f; // 0: the door's tile, 2: the one behind
    if (dx > 0)
        x0 = float(t.x) + 0.5f + side, x1 = x0 + 0.5f;
    else if (dx < 0)
        x1 = float(t.x) + 0.5f - side, x0 = x1 - 0.5f;
    else if (dy > 0)
        y0 = float(t.y) + 0.5f + side, y1 = y0 + 0.5f;
    else if (dy < 0)
        y1 = float(t.y) + 0.5f - side, y0 = y1 - 0.5f;
    else
        return {};
    return {{x0, y0, x1, y1}};
}

// The model's footprint (the CRES's cDataListExtension "footprint", read
// by the scene graph object's Footprint, BV 0x658ab8): lists named by the
// footprint index ("0", ...), each with a 16x16 bitmask (32 bytes, a u16 a
// row, bit 0 the low x) per tile "(x,y)" of the model. The binary halves
// it to 8x8 cells of 1/8 tile (BV 0x6537a4: a cell is set when any of its
// four is) and makes each run of set cells in a row a rectangle (BV
// 0x656cc0; runs of equal rows merge, which changes nothing here) of
// x = tx + c / 8 - 0.5 + 0.01 .. tx + c' / 8 - 0.5 - 0.01, y alike from the
// rows, in the model's space.
struct ModelFootprints {
    // list name -> tile -> 8 rows of 8 cells
    std::map<std::string, std::map<std::pair<int, int>, std::array<uint8_t, 8>>> lists;
    // Without a list: the model's box in its xy plane (model space, tiles):
    // its shapes' bounds; box[0] > box[2] when the model has none.
    std::array<float, 4> box{1, 1, 0, 0};
};

} // namespace

std::array<uint8_t, 8> halveFootprintMask(std::span<const uint8_t> m) {
    std::array<uint8_t, 8> out{};
    if (m.size() < 32)
        return out;
    auto half = [](uint8_t b) {
        uint8_t h = 0;
        for (int j = 0; j < 4; ++j)
            if (b & (3u << (2 * j)))
                h |= uint8_t(1u << j);
        return h;
    };
    for (size_t r = 0; r < 8; ++r)
        out[r] = uint8_t(half(m[4 * r]) | half(m[4 * r + 2]) | ((half(m[4 * r + 1]) | half(m[4 * r + 3])) << 4));
    return out;
}

std::vector<std::array<float, 4>> footprintMaskRects(const std::array<uint8_t, 8>& rows, int tx, int ty) {
    std::vector<std::array<float, 4>> out;
    constexpr float kCell = 0.125f, kShrink = 0.01f;
    for (int row = 0; row < 8; ++row) {
        const uint8_t bits = rows[size_t(row)];
        for (int c0 = 0; c0 < 8;) {
            if (!(bits & (1u << c0))) {
                ++c0;
                continue;
            }
            int c1 = c0;
            while (c1 < 8 && (bits & (1u << c1)))
                ++c1;
            out.push_back({float(tx) + float(c0) * kCell - 0.5f + kShrink, float(ty) + float(row) * kCell - 0.5f + kShrink,
                           float(tx) + float(c1) * kCell - 0.5f - kShrink,
                           float(ty) + float(row + 1) * kCell - 0.5f - kShrink});
            c0 = c1;
        }
    }
    return out;
}

namespace {

// The footprints of the object's model, read once per OBJD; null without a
// "footprint" data list (or without a model).
const ModelFootprints* modelFootprints(VM& vm, const Object& o) {
    constexpr uint32_t kTypeCresId = 0xE519C933, kTypeObjdId = 0x4F424A44;
    static std::map<std::pair<uint32_t, uint16_t>, std::shared_ptr<ModelFootprints>> cache;
    const auto key = std::pair{o.group, o.definitionInstance};
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second.get();
    std::shared_ptr<ModelFootprints> fp;
    try {
        if (ResourceIndex* index = vm.code().index()) {
            const std::string cres = objectCresName(*index, Tgi{kTypeObjdId, o.group, o.definitionInstance, 0});
            if (auto loc = cres.empty() ? std::nullopt : index->findByName(cres + "_cres", kTypeCresId)) {
                const auto bytes = index->read(*loc);
                const Rcol rcol(bytes);
                for (const Rcol::Block& b : rcol.blocks) {
                    if (b.className != "cDataListExtension")
                        continue;
                    const DataListExtension d = DataListExtension::parse(b.body);
                    if (d.root.name != "footprint")
                        continue;
                    fp = std::make_shared<ModelFootprints>();
                    for (const ExtensionValue& list : d.root.items) {
                        if (list.type != ExtensionValue::Array)
                            continue;
                        auto& tiles = fp->lists[list.name];
                        for (const ExtensionValue& t : list.items) {
                            int x = 0, y = 0;
                            if (t.type == ExtensionValue::Binary && t.bytes.size() == 32 &&
                                std::sscanf(t.name.c_str(), "(%d,%d)", &x, &y) == 2)
                                tiles[{x, y}] = halveFootprintMask(t.bytes);
                        }
                    }
                    break;
                }
                // No "footprint" list: the cObject's box of the model (BV
                // 0x658ab8 -> FUN_00a45e54 with -1, every node: the union of
                // the boxes of the nodes with flag 4, the cShapeRefNodes (the
                // bit cShapeRefNodeT's constructor sets, BV 0x1b1ab78), not
                // hidden (0x114) and bounded (0x180), in the object's space:
                // the shapes' bounds, the GMDCs' bounding meshes,
                // ObjectModel::bounds; docs/routing-mac.md §9.5), cut to the
                // tile below.
                if (!fp) {
                    fp = std::make_shared<ModelFootprints>();
                    const ObjectModel model = loadObjectModel(*index, cres);
                    auto& b = fp->box;
                    b = {1e9f, 1e9f, -1e9f, -1e9f};
                    for (const ObjectModel::Bound& bound : model.bounds)
                        if (!bound.hidden)
                            for (const auto& v : bound.points)
                                b = {std::min(b[0], v[0]), std::min(b[1], v[1]), std::max(b[2], v[0]), std::max(b[3], v[1])};
                }
            }
        }
    } catch (const std::exception& err) {
        os2::logOnce("footprint", std::to_string(o.group) + ":" + std::to_string(o.definitionInstance), err);
        fp.reset();
    }
    return cache.emplace(key, fp).first->second.get();
}

} // namespace

// The rectangles an object puts in the path planner (cTSObject::
// AddThisToPathPlanner, BV 0xaf68e0: the object's GetFootprint, each
// rectangle given to AddObjectRectangle, which inflates it by 0.12499).
// GetFootprint is TSObjectWorldSpaceFootprint (BV 0x8aa40c): unless flag
// 0x80 "Do NOT Use Maya Model Footprint" (object data 8), the model's
// footprint (AggregateFootprint with object data 9, the footprint index:
// that list by name, the first by name when 0) for the part's tile of the
// model, else the OBJD box (footprintOf). The model's space is the lead
// part's, as for the bone slots: the part's tile is its normalized offset
// from the lead (the binary asks the object, vtable 0x150: the OBJD's sub
// index, partTileInModel), and the rectangles turn with the lead's facing as the bones do (the
// binary's FUN_0191fa14 turns by direction & 6). A list without the part's
// tile gives no rectangle (the bench of a piano has a tile with part of a
// row). A model without a footprint list gives its box (the xy bounds of
// its shapes' bounds, round n49) cut to the part's tile, as the scene
// graph's cObject does (BV 0x658ab8); without a model, the OBJD box.
namespace {

// The part's tile in the model: cEdithMTObject::GetLocalOffsetFromLeader
// (vtable 0x150, BV 0xaf2410), (part x - leader x, leader y - part y) of the
// parts' normalized offsets (GetNormalizedOffset), which cEdithMTObject::
// OnInit (base 0xb23280, BV 0xaef22c) sets from the OBJD's multi-tile sub
// index (GetMultiTileOffsets, base 0xbb4834: field 0x56, x the high byte
// with its sign, y the low byte without; docs/routing-mac.md §9.5);
// without definitions, the world offset turned back.
std::pair<int, int> partTileInModel(const Object& part, const Object& lead, float turnedX, float turnedY) {
    if (&part != &lead && part.definition && lead.definition) {
        const uint16_t p = part.definition->field(0x56), l = lead.definition->field(0x56);
        return {int(int8_t(p >> 8)) - int(int8_t(l >> 8)), int(l & 0xFF) - int(p & 0xFF)};
    }
    return {int(std::lround(turnedX)), int(std::lround(turnedY))};
}

} // namespace

std::array<float, 4> snapFootprintRect(const std::array<float, 4>& r) {
    // As the binary computes it: in doubles from the floats, each bound back
    // to a float.
    auto low = [](float v) {
        const float s = float(double(std::floor(float(double(v) * 8.0 + 0.5))) * 0.125 + double(0.01f));
        return double(s) <= double(v) ? v : s;
    };
    auto high = [](float v) {
        const float s = float(double(std::ceil(float(double(v) * 8.0 - 0.5))) * 0.125 - double(0.01f));
        return double(v) <= double(s) ? v : s;
    };
    std::array<float, 4> out{low(r[0]), low(r[1]), high(r[2]), high(r[3])};
    for (size_t a = 0; a < 2; ++a)
        if (r[a] < r[a + 2] && float(double(out[a + 2]) - double(out[a])) < 0.0625f)
            out[a + 2] = float(double(float(double(out[a]) + 0.125)) - double(0.02f));
    return out;
}

std::array<float, 4> VM::personFootprintAt(const Object* person, float x, float y) {
    const std::array<float, 2> at{x, y};
    if (person && person->person)
        return snapFootprintRect(footprintOf(*person, &at)[0]);
    return snapFootprintRect({x - 0.1f, y - 0.1f, x + 0.1f, y + 0.1f});
}

std::vector<std::array<float, 4>> VM::routingFootprint(const Object& o) {
    std::vector<std::array<float, 4>> out = worldFootprint(o);
    for (auto& r : out)
        r = snapFootprintRect(r);
    return out;
}

std::vector<std::array<float, 4>> VM::worldFootprint(const Object& o) {
    if (!o.person && !(o.data[objdata::Flags] & 0x80) && o.definition) {
        const ModelFootprints* mf = modelFootprints(*this, o);
        if (mf && mf->lists.empty() && o.data[9] == 0 && mf->box[0] <= mf->box[2]) {
            // The model's box cut to the part's tile of the model (±0.5 about
            // it), turned with the lead as the masks are.
            const Object* lead = &o;
            if (const ObjectId l = multiTileGroup(o); l != o.id)
                if (const Object* lo = object(l); lo && lo->position.inWorld())
                    lead = lo;
            const int dir = ((lead->data[objdata::Direction] % 8) + 8) % 8 & 6;
            const double yaw = (90.0 - 45.0 * dir) * M_PI / 180.0;
            const float c = float(std::lround(std::cos(yaw))), s = float(std::lround(std::sin(yaw)));
            const float wx = o.position.x - lead->position.x, wy = o.position.y - lead->position.y;
            const auto [tx, ty] = partTileInModel(o, *lead, wx * c + wy * s, -wx * s + wy * c);
            const float mx0 = std::max(mf->box[0], tx - 0.5f), my0 = std::max(mf->box[1], ty - 0.5f);
            const float mx1 = std::min(mf->box[2], tx + 0.5f), my1 = std::min(mf->box[3], ty + 0.5f);
            if (!(mx0 < mx1 && my0 < my1))
                return {};
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (const float mx : {mx0, mx1})
                for (const float my : {my0, my1}) {
                    const float px = lead->position.x + mx * c - my * s;
                    const float py = lead->position.y + mx * s + my * c;
                    x0 = std::min(x0, px), y0 = std::min(y0, py), x1 = std::max(x1, px), y1 = std::max(y1, py);
                }
            return {{x0, y0, x1, y1}};
        }
        if (mf && !mf->lists.empty()) {
            const int16_t index = o.data[9];
            const std::map<std::pair<int, int>, std::array<uint8_t, 8>>* tiles = nullptr;
            if (index != 0) {
                if (auto f = mf->lists.find(std::to_string(index)); f != mf->lists.end())
                    tiles = &f->second;
            } else {
                tiles = &mf->lists.begin()->second; // the smallest name (the binary keeps the least by strcmp)
            }
            if (!tiles) // no list "%d" for the footprint index: the OBJD box
                return footprintOf(o);
            const Object* lead = &o;
            if (const ObjectId l = multiTileGroup(o); l != o.id)
                if (const Object* lo = object(l); lo && lo->position.inWorld())
                    lead = lo;
            const int dir = ((lead->data[objdata::Direction] % 8) + 8) % 8 & 6;
            const double yaw = (90.0 - 45.0 * dir) * M_PI / 180.0;
            const float c = float(std::lround(std::cos(yaw))), s = float(std::lround(std::sin(yaw)));
            // The part's tile in the model: its offset from the lead turned back.
            const float wx = o.position.x - lead->position.x, wy = o.position.y - lead->position.y;
            const auto [tx, ty] = partTileInModel(o, *lead, wx * c + wy * s, -wx * s + wy * c);
            const auto t = tiles->find({tx, ty});
            if (t == tiles->end())
                return {};
            std::vector<std::array<float, 4>> out;
            for (const auto& r : footprintMaskRects(t->second, tx, ty)) {
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                for (const float mx : {r[0], r[2]})
                    for (const float my : {r[1], r[3]}) {
                        const float px = lead->position.x + mx * c - my * s;
                        const float py = lead->position.y + mx * s + my * c;
                        x0 = std::min(x0, px), y0 = std::min(y0, py), x1 = std::max(x1, px), y1 = std::max(y1, py);
                    }
                out.push_back({x0, y0, x1, y1});
            }
            return out;
        }
    }
    return footprintOf(o);
}

bool VM::conformsToCeiling(const Object& o) {
    constexpr uint32_t kTypeCresId = 0xE519C933, kTypeObjdId = 0x4F424A44;
    if (o.person || !o.definition)
        return false;
    static std::map<std::pair<uint32_t, uint16_t>, bool> cache;
    const auto key = std::pair{o.group, o.definitionInstance};
    if (const auto it = cache.find(key); it != cache.end())
        return it->second;
    bool attached = false;
    try {
        if (ResourceIndex* index = code().index()) {
            const std::string cres = objectCresName(*index, Tgi{kTypeObjdId, o.group, o.definitionInstance, 0});
            if (auto loc = cres.empty() ? std::nullopt : index->findByName(cres + "_cres", kTypeCresId)) {
                const Rcol rcol(index->read(*loc));
                for (const Rcol::Block& b : rcol.blocks)
                    if (b.className == "cTransformNode" && TransformNode::parse(b.body).name() == "post_trans")
                        attached = true;
            }
        }
    } catch (const std::exception& e) {
        logOnce("conformsToCeiling", o.definition->fileName, e);
    }
    return cache[key] = attached;
}

std::optional<std::vector<std::array<float, 4>>> VM::indexedFootprint(const Object& o, int index) {
    if (o.person || !o.definition)
        return std::nullopt;
    const ModelFootprints* mf = modelFootprints(*this, o);
    if (!mf)
        return std::nullopt;
    const auto list = mf->lists.find(std::to_string(index));
    if (list == mf->lists.end())
        return std::nullopt;
    const Object* lead = &o;
    if (const ObjectId l = multiTileGroup(o); l != o.id)
        if (const Object* lo = object(l); lo && lo->position.inWorld())
            lead = lo;
    const int dir = ((lead->data[objdata::Direction] % 8) + 8) % 8 & 6;
    const double yaw = (90.0 - 45.0 * dir) * M_PI / 180.0;
    const float c = float(std::lround(std::cos(yaw))), s = float(std::lround(std::sin(yaw)));
    std::vector<std::array<float, 4>> out;
    for (const auto& [tile, rows] : list->second)
        for (const auto& r : footprintMaskRects(rows, tile.first, tile.second)) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            for (const float mx : {r[0], r[2]})
                for (const float my : {r[1], r[3]}) {
                    const float px = lead->position.x + mx * c - my * s;
                    const float py = lead->position.y + mx * s + my * c;
                    x0 = std::min(x0, px), y0 = std::min(y0, py), x1 = std::max(x1, px), y1 = std::max(y1, py);
                }
            out.push_back({x0, y0, x1, y1});
        }
    return out;
}

namespace {

// FUN_0191f6d0 (docs/routing-mac.md §8.4): `p` out of the footprint
// inflated by d, along the shorter axis unless both moves are short.
void pushOutOf(const Footprint& fp, float d, Point2& p) {
    constexpr float kEps = 0.0001f;
    float dx = 0, dy = 0;
    for (const auto& r : fp) {
        if (!(r[0] - d <= p[0] && p[0] <= d + r[2] && r[1] - d <= p[1] && p[1] <= d + r[3]))
            continue;
        if ((r[0] + r[2]) * 0.5f < p[0]) {
            if (p[0] < d + r[2])
                dx = std::max(dx, d + r[2] + kEps - p[0]);
        } else if (const float v = r[0] - d - kEps - p[0]; v < dx) {
            dx = v;
        }
        if ((r[1] + r[3]) * 0.5f < p[1]) {
            if (p[1] < d + r[3])
                dy = std::max(dy, d + r[3] + kEps - p[1]);
        } else if (const float v = r[1] - d - kEps - p[1]; v < dy) {
            dy = v;
        }
    }
    if (dx == 0 && dy == 0)
        return;
    const float far = d + kEps + kEps;
    if (std::fabs(dy) <= std::fabs(dx)) {
        if (std::fabs(dx) >= far) {
            p[1] += dy;
            return;
        }
    } else if (std::fabs(dy) >= far) {
        p[0] += dx;
        return;
    }
    p[0] += dx, p[1] += dy;
}

bool canIntersectObjects(const Object& o) { return (o.data[objdata::Flags] & 0x06) != 0; }
bool canIntersectPeople(const Object& o) { return (o.data[objdata::Flags] & 0x14) != 0; }

// cTSObject::RunTree(5, other, ...): the "allow intersection" tree (OBJf
// entry 5) of `o` about `other`; false without one. Inferred: the other's
// ID goes in parameter 0 and as the stack object.
bool allowsIntersection(VM& vm, const Object& o, ObjectId other) {
    const uint16_t tree = vm.functionTree(o, 5);
    if (!tree)
        return false;
    const std::array<int16_t, 1> args{int16_t(other)};
    Thread t = vm.start(o.id, tree, args, other);
    for (int turn = 0; t.status == Thread::Status::Running && turn < 4; ++turn)
        vm.run(t, vm.instructionsPerTurn);
    dropThreadRecords(vm, t); // records.cpp
    return t.status == Thread::Status::Finished && t.result;
}

// IsDiagonalDoor: a door (type 8) whose wall placement flags (object data
// 13) have 0x50.
bool isDiagonalDoor(const Object& o) { return objectType(o) == kTypeDoor && (o.data[13] & 0x50) != 0; }

} // namespace

Point2 moveAwayFromOtherObjects(VM& vm, const Object& me, Point2 p, int level, float d, bool skipPeople, bool doors) {
    if (me.person && me.person->data.size() > 68 && (me.person->data[68] & 0x02))
        return p; // a ghost that walks through things (person data 68 "ghost flags")
    if (canIntersectObjects(me))
        return p;
    const World& w = *vm.world;
    const TilePos c{int(std::floor(p[0])), int(std::floor(p[1])), level};
    const bool mePerson = objectType(me) == kTypePerson;
    for (int ty = c.y - 1; ty <= c.y + 1; ++ty)
        for (int tx = c.x - 1; tx <= c.x + 1; ++tx) {
            const TilePos t{tx, ty, level};
            if (!w.contains(t))
                continue;
            for (const ObjectId id : vm.objectsOnTile(t)) {
                const Object* o = vm.object(id);
                if (!o)
                    continue;
                if (doors && objectType(*o) == kTypeDoor) {
                    pushOutOf(doorFootprint(*o, 0), d, p);
                    pushOutOf(doorFootprint(*o, 2), d, p);
                }
                const bool oPerson = objectType(*o) == kTypePerson;
                if ((mePerson || oPerson) && (canIntersectPeople(*o) || canIntersectPeople(me)))
                    continue;
                if (canIntersectObjects(*o) || (skipPeople && oPerson))
                    continue;
                if (allowsIntersection(vm, me, o->id) || allowsIntersection(vm, *o, me.id))
                    continue;
                pushOutOf(footprintOf(*o), d, p);
            }
        }
    if (doors)
        for (int ty = c.y - 2; ty <= c.y + 2; ++ty)
            for (int tx = c.x - 2; tx <= c.x + 2; ++tx) {
                if (std::abs(tx - c.x) <= 1 && std::abs(ty - c.y) <= 1)
                    continue;
                const TilePos t{tx, ty, level};
                if (!w.contains(t))
                    continue;
                for (const ObjectId id : vm.objectsOnTile(t))
                    if (const Object* o = vm.object(id); o && isDiagonalDoor(*o)) {
                        pushOutOf(doorFootprint(*o, 0), d, p);
                        pushOutOf(doorFootprint(*o, 2), d, p);
                    }
            }
    return p;
}

Point2 movePointAwayFromWalls(const World& w, Point2 at, int level, float d) {
    constexpr float kEps = 0.001f;
    const float r2 = std::sqrt(2.0f);
    const TilePos t{int(std::floor(at[0])), int(std::floor(at[1])), level};
    const int diag = w.diagonalSides(t);
    for (int side = 1; side <= 0x20; side <<= 1) {
        const bool wall = side == 1   ? w.wallBetween(t, 0, -1)
                          : side == 2 ? w.wallBetween(t, -1, 0)
                          : side == 4 ? w.wallBetween(t, 0, 1)
                          : side == 8 ? w.wallBetween(t, 1, 0)
                                      : (diag & side) != 0;
        if (!wall)
            continue;
        float& x = at[0];
        float& y = at[1];
        switch (side) {
        case 1:
            if (y - std::floor(y) < d)
                y += d - (y - std::floor(y)) + kEps;
            break;
        case 2:
            if (x - std::floor(x) < d)
                x += d - (x - std::floor(x)) + kEps;
            break;
        case 4:
            if (std::ceil(y) - y < d)
                y -= d - (std::ceil(y) - y) + kEps;
            break;
        case 8:
            if (std::ceil(x) - x < d)
                x -= d - (std::ceil(x) - x) + kEps;
            break;
        case 0x10: { // as the binary has it: measured to fx + fy = 1, moved along it
            const float s = ((y - std::floor(y)) - (std::ceil(x) - x)) / r2;
            if (s >= 0 && s < d) {
                const float v = d - s + kEps;
                y += v / -r2, x += v / r2;
            } else if (s < 0 && s > -d) {
                const float v = d + s + kEps;
                y += v / r2, x += v / -r2;
            }
            break;
        }
        case 0x20: { // measured to fx = fy, moved along it
            const float s = ((y - std::floor(y)) - (x - std::floor(x))) / r2;
            if (s >= 0 && s < d) {
                const float v = (d - s + kEps) / r2;
                y += v, x += v;
            } else if (s < 0 && s > -d) {
                const float v = (s + d + kEps) / -r2;
                y += v, x += v;
            }
            break;
        }
        }
    }
    return at;
}

std::optional<Position> goHerePoint(VM& vm, ObjectId simId, float x, float y, int level) {
    if (!vm.world)
        return std::nullopt;
    const World& w = *vm.world;
    const TilePos tile{int(std::floor(x)), int(std::floor(y)), level};
    if (!w.contains(tile))
        return std::nullopt;
    const Object* sim = vm.object(simId);
    Object none;
    // GoHere (docs/routing-mac.md §8.4): 0.4 away from the objects around
    // (the Sim's view of them: TSObjectMoveAwayFromOtherObjects with the
    // Sims counted and the doors), then 0.45 away from the walls of its tile.
    Point2 at = moveAwayFromOtherObjects(vm, sim ? *sim : none, {x, y}, level, 0.4f, false, true);
    at = movePointAwayFromWalls(w, at, level, 0.45f);
    // TSObjectCanSetPosition, inferred as: in the lot, something to stand
    // on (a pool tile for the swim destination) and nothing that blocks.
    const TilePos t{int(std::floor(at[0])), int(std::floor(at[1])), level};
    if (!w.contains(t))
        return std::nullopt;
    if (w.pool(t))
        return Position{at[0], at[1], level};
    if (!w.walkable(t))
        return std::nullopt;
    PathPlanner& planner = plannerOf(vm);
    addObstacles(vm, planner, sim ? *sim : none, {});
    if (!planner.free(level, at))
        return std::nullopt;
    return Position{at[0], at[1], level};
}

ObjectId createGoHereDestination(VM& vm, ObjectId simId, float x, float y, int level) {
    const auto p = goHerePoint(vm, simId, x, y, level);
    if (!p)
        return 0;
    const TilePos tile{int(std::floor(p->x)), int(std::floor(p->y)), level};
    const bool swim = vm.world->pool(tile);
    const uint32_t guid = swim ? kSwimHereGuid : kGoHereGuid;
    // A swim destination goes on the pool's floor, the level below the
    // water, where swimmers walk (the pool is a level of its own, §8.5):
    // on the water's level "Entrar" failed at once with "altitudes don't
    // match" (route result 10) and the Sim never went to the ladder (live QA
    // 16, N001_Lot68). Inferred: the game's ground point under the mouse
    // meets the pool's floor, the water has none.
    const TilePos below{tile.x, tile.y, level - 1};
    if (swim && vm.world->pool(below)) // the water's level (the bottom is a pool tile too)
        level -= 1;
    std::vector<ObjectId> existing;
    for (const auto& [id, o] : vm.eachObject())
        existing.push_back(id);
    Object* dest = vm.instantiateGuid(guid);
    if (!dest)
        return 0;
    const ObjectId id = dest->id;
    auto place = [&](Object& d) {
        moveTo(vm, d, p->x, p->y, level);
        d.managedDestination = true;
        d.destinationHeld = true;
        vm.noteDestination(d);
    };
    place(*dest);
    vm.initialize(id, existing);
    dest = vm.object(id);
    if (!dest)
        return 0;
    place(*dest); // its Init may move it
    return id;
}

bool objectsMayIntersect(VM& vm, const Object& a, const Object& b) { return intersectionAllowed(vm, a, b); }

bool isGoHereRug(VM& vm, ObjectId id) {
    const Object* o = vm.object(id);
    return o && !o->person && objdField(*o, 0x52) == 0x13 && o->data[objdata::Category] != 0xA7;
}

ClickedMenu objectClick(VM& vm, ObjectId sim, ObjectId object, std::optional<Position> ground, bool hasInteractions) {
    if (!isGoHereRug(vm, object))
        return {hasInteractions ? object : ObjectId(0), 0};
    const ObjectId dest = ground ? createGoHereDestination(vm, sim, ground->x, ground->y, ground->level) : 0;
    if (hasInteractions)
        return {object, dest};
    return {dest, 0};
}

void releaseDestination(VM& vm, ObjectId id) {
    if (Object* o = vm.object(id); o && o->managedDestination) {
        o->destinationHeld = false;
        flushDestinations(vm);
    }
}

} // namespace os2::vm
