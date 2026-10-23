// World occupancy, pathfinding and the routing primitives on synthetic lots.
#include "vm/animation.hpp"
#include "vm/go_here.hpp"
#include "vm/interactions.hpp"
#include "vm/locomotion.hpp"
#include "vm/lot.hpp"
#include "vm/path_planner.hpp"
#include "vm/routing_internal.hpp"
#include "vm/vm.hpp"
#include "vm/world.hpp"
#include "formats/lot.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <set>
#include <string>

using namespace os2;
using namespace os2::vm;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr uint32_t kGroup = 0x7F000001;

using Target = Behavior::Target;
using Kind = Behavior::TargetKind;
static const Target T{Kind::True, 0}, F{Kind::False, 0}, E{Kind::Error, 0};
static Target go(uint16_t i) { return {Kind::Instruction, i}; }

static Behavior::Instruction instr(uint16_t opcode, Target onTrue, Target onFalse,
                                   std::initializer_list<uint8_t> operands = {}) {
    Behavior::Instruction in;
    in.opcode = opcode;
    in.onTrue = onTrue;
    in.onFalse = onFalse;
    size_t i = 0;
    for (auto b : operands)
        in.operands[i++] = b;
    return in;
}

static Behavior::Instruction expr(uint8_t lo, int16_t ld, uint8_t op, uint8_t ro, int16_t rd, Target t, Target f) {
    return instr(0x02, t, f,
                 {uint8_t(ld), uint8_t(uint16_t(ld) >> 8), uint8_t(rd), uint8_t(uint16_t(rd) >> 8), 0, op, lo, ro});
}

static Behavior tree(std::vector<Behavior::Instruction> code, uint8_t args = 0, uint8_t locals = 0) {
    Behavior b;
    b.fileName = "test";
    b.signature = 0x8009;
    b.argCount = args;
    b.localCount = locals;
    b.instructions = std::move(code);
    return b;
}

enum : uint8_t { Temp = 0x08, Lit = 0x07, StackId = 0x0A, Local = 0x19, Data = 0x03 };
enum : uint8_t { Assign = 5, Eq = 2 };

// An object placed at the centre of a tile.
static Object& place(VM& vm, int x, int y, int level = 1) {
    Object& o = vm.createObject(kGroup);
    vm.setPosition(o, {float(x) + 0.5f, float(y) + 0.5f, level});
    return o;
}

// A model footprint tile (the CRES "footprint" mask) as the planner's
// rectangles: the piano bench's tile (1,1) of pianoUprightSaloon, rows 0-4
// of 16 with columns 0-6 set ("7f00"), halves to rows 0-2 (any of four
// subcells) and columns 0-3: three rectangles x 0.51..0.99, y 0.51..0.865.
static void testFootprintMask() {
    std::array<uint8_t, 32> mask{};
    for (int row = 0; row < 5; ++row)
        mask[size_t(2 * row)] = 0x7F;
    const auto rows = halveFootprintMask(mask);
    check(rows[0] == 0x0F && rows[1] == 0x0F && rows[2] == 0x0F && rows[3] == 0, "the bench's mask halves to 3 rows of 4 cells");
    const auto rects = footprintMaskRects(rows, 1, 1);
    check(rects.size() == 3, "one rectangle per row's run");
    if (rects.size() == 3) {
        check(std::fabs(rects[0][0] - 0.51f) < 1e-4f && std::fabs(rects[0][2] - 0.99f) < 1e-4f, "x from the tile's centre, shrunk 0.01");
        check(std::fabs(rects[0][1] - 0.51f) < 1e-4f && std::fabs(rects[2][3] - 0.865f) < 1e-4f, "y over rows 0-2 of 8");
    }
    // A whole row: one rectangle across the tile.
    std::array<uint8_t, 32> full{};
    full[0] = full[1] = 0xFF;
    const auto one = footprintMaskRects(halveFootprintMask(full), 0, 0);
    check(one.size() == 1 && std::fabs(one[0][0] + 0.49f) < 1e-4f && std::fabs(one[0][2] - 0.49f) < 1e-4f,
          "a full row spans the tile less 0.01 a side");
}

static void testDirections() {
    check(directionStep(0) == std::array<int, 2>{-1, 0} && directionStep(2) == std::array<int, 2>{0, 1} &&
              directionStep(4) == std::array<int, 2>{1, 0} && directionStep(6) == std::array<int, 2>{0, -1},
          "direction steps");
    bool roundTrip = true;
    for (int d = 0; d < 8; ++d) {
        const auto [dx, dy] = directionStep(d);
        roundTrip &= directionOf(float(dx), float(dy)) == d;
    }
    check(roundTrip && directionOf(0, 0) == -1 && directionStep(-2) == directionStep(6), "directions round trip");
}

static void testWorldWalls() {
    World w(6, 6);
    w.addWall(1, 2, 0, 2, 4); // a wall along x = 2, from y = 0 to 4: separates column 1 from column 2
    check(w.wallBetween({1, 1, 1}, 1, 0) && w.wallBetween({2, 3, 1}, -1, 0), "wall between columns");
    check(!w.wallBetween({1, 4, 1}, 1, 0) && !w.wallBetween({1, 1, 1}, 0, 1), "wall ends");
    BlockedGrid open(36, 0);
    check(!canStep(w, open, {1, 1, 1}, 1, 0) && canStep(w, open, {1, 4, 1}, 1, 0), "steps across walls");
    check(!canStep(w, open, {1, 3, 1}, 1, 1) && canStep(w, open, {1, 4, 1}, 1, 1), "no cutting the wall's end");
    BlockedGrid blocked = open;
    blocked[w.index(4, 5)] = 1;
    check(!canStep(w, blocked, {3, 4, 1}, 1, 1) && !canStep(w, blocked, {4, 4, 1}, 0, 1), "objects block, also corners");
    check(!w.lineOfSight({0, 1, 1}, {4, 2, 1}) && w.lineOfSight({0, 5, 1}, {5, 4, 1}), "line of sight");
    w.openDoor({1, 1, 1}, 0); // facing -X: its back is the wall at x = 2
    check(!w.wallBetween({1, 1, 1}, 1, 0) && w.wallBetween({1, 2, 1}, 1, 0), "a door opens its wall edge");
    w.setWalkable({3, 3, 1}, false);
    check(!canStep(w, open, {3, 2, 1}, 0, 1) && !w.contains({6, 0, 1}) && !w.contains({0, 0, 2}), "floors and bounds");
}

// Diagonal walls cut tiles in half: walkers go around the tiles they cross
// and do not slip through the line diagonally; a door in one opens it.
static void testDiagonalWalls() {
    World w(8, 8);
    w.addWall(1, 1, 1, 5, 5); // "/" through tiles (1, 1) .. (4, 4)
    w.addWall(1, 7, 1, 6, 2); // "\\" through tile (6, 1)
    check(w.diagonalWall({2, 2, 1}) && !w.walkable({2, 2, 1}) && !w.walkable({6, 1, 1}) && w.walkable({2, 3, 1}),
          "the tiles a diagonal wall crosses");
    BlockedGrid open(64, 0);
    check(!canStep(w, open, {3, 2, 1}, -1, 1), "no slipping between the halves");
    check(!w.lineOfSight({3, 1, 1}, {1, 3, 1}) && w.lineOfSight({3, 1, 1}, {5, 3, 1}), "a diagonal wall hides");
    Path p;
    check(findPath(w, open, {3, 1, 1}, {{1, 3, 0}}, p) && p.cost > 5, "around the diagonal wall");
    w.openDoor({3, 3, 1}, 1);
    check(w.walkable({3, 3, 1}) && findPath(w, open, {4, 2, 1}, {{2, 4, 0}}, p) && p.tiles.size() <= 4,
          "through a door in it");
}

// Set to Next 8 (same tile, after the target), 9 (adjacent to the object in
// a local: the last object of each side, +X, +Y, -X, -Y) and 34 (within one
// tile).
static void testSetToNextTiles() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(8, 8);
    Object& a = place(vm, 3, 3);    // 1
    Object& b = place(vm, 3, 3);    // 2
    Object& east = place(vm, 4, 3); // 3: direction 4 (+X)
    Object& west = place(vm, 2, 3); // 4: direction 0 (-X)
    Object& diag = place(vm, 4, 4); // 5
    Object& held = place(vm, 3, 3); // 6: in a slot of b
    vm.setContainer(held, b.id, held.data[objdata::SlotNumber]);
    place(vm, 6, 6); // 7: far away
    (void)east, (void)west, (void)diag;
    auto stn = [](uint8_t type, uint8_t local, Target t, Target f) {
        return instr(0x1F, t, f, {0, 0, 0, 0, uint8_t(0x80 | type), StackId, local, 0, 0});
    };
    // Same tile, from a: b, what is in b's slot, then a again (the binary's
    // tile walk goes into the slots, and wraps around: the game's trees loop
    // until they are back at the start).
    code.add(kGroup, 0x1000,
             tree({expr(StackId, 0, Assign, Lit, a.id, go(1), E), stn(8, 0, go(2), E),
                   expr(Temp, 0, Assign, StackId, 0, go(3), E), stn(8, 0, go(4), E),
                   expr(Temp, 1, Assign, StackId, 0, go(5), E), stn(8, 0, go(6), E),
                   expr(Temp, 2, Assign, StackId, 0, T, E)}));
    Thread t = vm.start(a.id, 0x1000);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == b.id && t.temps[1] == held.id && t.temps[2] == a.id,
          "Set to Next 8 goes through the tile's objects after the target, into their slots, and wraps");
    // Adjacent to a (local 0): +X (east) comes before -X (west); then none.
    code.add(kGroup, 0x1001,
             tree({expr(Local, 0, Assign, Lit, a.id, go(1), E), expr(StackId, 0, Assign, Lit, 0, go(2), E),
                   stn(9, 0, go(3), E), expr(Temp, 0, Assign, StackId, 0, go(4), E), stn(9, 0, go(5), E),
                   expr(Temp, 1, Assign, StackId, 0, go(6), E), stn(9, 0, E, T)},
                  0, 1));
    t = vm.start(a.id, 0x1001);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == east.id && t.temps[1] == west.id,
          "Set to Next 9: +X, +Y, -X, -Y");
    // Within one tile of a: b, east, west, diag (by ID), not a itself nor the far one.
    code.add(kGroup, 0x1002,
             tree({expr(Local, 0, Assign, Lit, a.id, go(1), E), expr(StackId, 0, Assign, Lit, 0, go(2), E),
                   stn(34, 0, go(3), T), expr(Temp, 0, 3 /* add */, Lit, 1, go(2), E)},
                  0, 1));
    t = vm.start(a.id, 0x1002);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.temps[0] == 4, "Set to Next 34 within one tile");
    // One object a side, the tile's last (TrySetToNext: LastValidObjectOnTile),
    // and on from the side of the target: from b, on a's own tile, none.
    Object& east2 = place(vm, 4, 3);
    code.add(kGroup, 0x1003,
             tree({expr(Local, 0, Assign, Lit, a.id, go(1), E), expr(StackId, 0, Assign, Lit, 0, go(2), E),
                   stn(9, 0, go(3), E), expr(Temp, 0, Assign, StackId, 0, go(4), E), stn(9, 0, go(5), E),
                   expr(Temp, 1, Assign, StackId, 0, go(6), E), expr(StackId, 0, Assign, Lit, b.id, go(7), E),
                   stn(9, 0, E, T)},
                  0, 1));
    t = vm.start(a.id, 0x1003);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && t.temps[0] == east2.id && t.temps[1] == west.id,
          "Set to Next 9: the last object of a side; a target on the anchor's tile ends it");
    // The tiles round a point in one walk: each tile's list is objectsOnTile's.
    bool sameAround = true;
    for (const TilePos at : {TilePos{3, 3, 1}, TilePos{0, 0, 1}, TilePos{5, 5, 1}}) {
        const auto around = vm.objectsAround(at, 1);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                sameAround = sameAround && around[size_t(dx + 1) * 3 + size_t(dy + 1)] ==
                                               vm.objectsOnTile({at.x + dx, at.y + dy, at.level});
    }
    check(sameAround, "objectsAround: the tiles' objects as objectsOnTile has them");

    check(VM::blocksRouting(a) && !VM::blocksRouting(held), "objects in slots do not block");
    a.data[objdata::Flags] = 0x10;
    check(!VM::blocksRouting(a) && !vm.tileFree({3, 3, 1}) && vm.tileFree({3, 3, 1}, std::vector<ObjectId>{b.id}),
          "person intersection lets Sims through; ignored objects");
}

// A* on synthetic grids: around walls, through a door, past objects, to the
// cheapest of several goals, and no path.
static void testPathfinder() {
    World w(10, 10);
    for (int y = 0; y < 9; ++y)
        w.addWall(1, 5, y, 5, y + 1); // a wall at x = 5 with a gap at the top row
    BlockedGrid open(100, 0);
    Path p;
    check(findPath(w, open, {1, 1, 1}, {{8, 1, 0}}, p) && p.tiles.back() == TilePos{8, 1, 1}, "path around a wall");
    bool crosses = false;
    for (size_t i = 0; i + 1 < p.tiles.size(); ++i)
        crosses |= p.tiles[i].x == 4 && p.tiles[i + 1].x == 5 && p.tiles[i].y < 9;
    check(!crosses && p.cost > 14, "the path goes through the gap");
    for (size_t i = 0; i < p.tiles.size(); ++i) {
        const TilePos from = i ? p.tiles[i - 1] : TilePos{1, 1, 1};
        crosses |= !canStep(w, open, from, p.tiles[i].x - from.x, p.tiles[i].y - from.y);
    }
    check(!crosses, "every step of the path is allowed");
    w.openDoor({4, 1, 1}, 0); // a door in the wall, facing -X
    check(findPath(w, open, {1, 1, 1}, {{8, 1, 0}}, p) && p.tiles.size() == 7 && std::fabs(p.cost - 7) < 1e-4f,
          "straight through the door");
    check(findPath(w, open, {1, 1, 1}, {{3, 3, 0}}, p) && p.tiles.size() == 2 && std::fabs(p.cost - 2 * M_SQRT2) < 1e-4f,
          "diagonal steps");
    check(findPath(w, open, {1, 1, 1}, {{8, 1, 5}, {1, 8, 0}}, p) && p.goal == 1, "the cheapest goal wins");
    check(findPath(w, open, {1, 1, 1}, {{1, 1, 0}}, p) && p.tiles.empty(), "already there");
    BlockedGrid wall(100, 0);
    for (int x = 0; x < 10; ++x)
        wall[w.index(x, 5)] = 1;
    check(!findPath(w, wall, {1, 1, 1}, {{1, 8, 0}}, p), "no path past a row of objects");
    check(!findPath(w, wall, {1, 1, 1}, {{3, 5, 0}}, p), "a blocked goal");
    wall[w.index(1, 1)] = 1;
    check(findPath(w, wall, {1, 1, 1}, {{2, 2, 0}}, p), "leaving a blocked start tile");
}

// Routing slot goals: a ring of directions around the object, rotated with
// it; a point slot on the object's tile.
static void testRoutingGoals() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(10, 10);
    Object& o = place(vm, 5, 5);
    o.data[objdata::Direction] = 2; // faces +Y
    Slots::Slot front;
    front.type = 3;
    front.flags = 0x01; // direction 0 relative to the object: its front
    front.minDistance = front.maxDistance = front.optimalDistance = 1;
    front.facing = -2;
    auto g = vm.routingGoals(front, o);
    check(g.goals.size() == 1 && g.goals[0].x == 5 && g.goals[0].y == 6 && g.facings[0] == 6,
          "in front of an object facing +Y, facing it");
    front.flags = 0x55;
    g = vm.routingGoals(front, o);
    check(g.goals.size() == 4, "four sides");
    vm.world->addWall(1, 5, 5, 6, 5); // a wall just behind it (y = 5)
    g = vm.routingGoals(front, o);
    check(g.goals.size() == 3, "not through a wall");
    Slots::Slot point;
    point.type = 3;
    point.facing = -2;
    g = vm.routingGoals(point, o);
    check(g.goals.size() == 1 && g.onTarget && g.facings[0] == 2, "a bone slot is on the object, facing its way");
    // The +25 (round n18): with no wall of the target's tile in the scan's
    // last direction (spread), the standing goals score 25 more (cost -25);
    // RS flag 0x80000 has none.
    vm.world = World(10, 10);
    front.flags = 0x01;
    front.standing = 1;
    g = vm.routingGoals(front, o);
    const float open = g.goals.empty() ? 0 : g.goals[0].cost;
    front.flags = 0x01 | 0x80000;
    g = vm.routingGoals(front, o);
    const float none = g.goals.empty() ? 0 : g.goals[0].cost;
    vm.world->addWall(1, 5, 5, 6, 5); // behind it: its tile's edge at -Y (direction 6; in its frame 4)
    front.flags = 0x01;
    g = vm.routingGoals(front, o);
    const float walled = g.goals.empty() ? 0 : g.goals[0].cost;
    if (std::getenv("DEBUG_GOALS"))
        std::fprintf(stderr, "+25: open %.2f, flag %.2f, walled %.2f\n", open, none, walled);
    check(std::fabs(open - (none - 25)) < 1e-3f && std::fabs(walled - open) < 1e-3f,
          "+25 for standing goals away from the target's walls (the scan's last direction, 2 here, off the wall's 3-5)");
}

static ObjectDefinition definition(uint32_t guid, uint16_t slotsId = 0, bool person = false) {
    ObjectDefinition def;
    def.fields.resize(60);
    def.fields[(0x5C - 0x44) / 2] = uint16_t(guid);
    def.fields[(0x5E - 0x44) / 2] = uint16_t(guid >> 16);
    def.fields[(0x68 - 0x44) / 2] = slotsId;
    if (person)
        def.fields[(0x52 - 0x44) / 2] = kObjectTypePerson;
    return def;
}

// A Sim walks to an object over ticks at walking speed, around a wall, and
// ends facing it; a goal it cannot reach is false; out of the world is the
// game's error.
static void testWalking() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(12, 12);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0x80));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Slots table;
    table.slots.resize(3);
    table.slots[0].type = 0; // a container slot
    table.slots[1].type = 3; // routing slot 0: in front, 1 tile
    table.slots[1].flags = 0x01;
    table.slots[1].minDistance = table.slots[1].maxDistance = table.slots[1].optimalDistance = 1;
    table.slots[1].facing = -2;
    table.slots[1].resolutionTiles = 1; // three points a row (laterals -1, 0, 1)
    table.slots[1].gradient = 0.2f;      // the middle one is the best
    table.slots[2].type = 3; // routing slot 1: on the object (a bone, in the game)
    table.slots[2].facing = -2;
    code.add(kGroup, 0x80, table);
    Object& chair = vm.instantiate(kGroup, 0x80);
    vm.setPosition(chair, {8.5f, 2.5f, 1});
    chair.data[objdata::Direction] = 4; // faces +X: its front is (9, 2)
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {1.5f, 2.5f, 1});
    vm.world->addWall(1, 5, 0, 5, 6); // in the way up to y = 6
    // Go To Routing Slot, literal slot 0.
    code.add(kGroup, 0x1000, tree({instr(0x2D, T, F, {0, 0, 1, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, chair.id);
    vm.run(t);
    check(t.status == Thread::Status::Running && sim.route && sim.position.x > 1.5f, "walking yields");
    int ticks = 1;
    while (t.status == Thread::Status::Running && ticks < 2000)
        vm.run(t), ++ticks;
    // Past the wall's end (x = 5, y = 6) with 1/8 tile to spare, and back
    // down: a straight line each way, not tile centres.
    const float walked = std::hypot(5 - 1.5f, 6.125f - 2.5f) + std::hypot(9.5f - 5, 6.125f - 2.5f);
    check(t.status == Thread::Status::Finished && t.result && !sim.route, "the walk ends true");
    check(std::fabs(sim.position.x - 9.5f) < 1e-4f && std::fabs(sim.position.y - 2.5f) < 1e-4f &&
              sim.data[objdata::Direction] == 0,
          "in front of the chair, facing it");
    check(ticks > int(walked / vm.walkTilesPerTick) * 0.95 && ticks < int(walked / vm.walkTilesPerTick) * 1.15,
          "at walking speed");
    // Routing slot 1 is on the chair itself, which does not block it.
    code.add(kGroup, 0x1001, tree({instr(0x2D, T, F, {1, 0, 1, 0, 0})}));
    t = vm.start(sim.id, 0x1001, {}, chair.id);
    for (int i = 0; i < 100 && t.status == Thread::Status::Running; ++i)
        vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && std::fabs(sim.position.x - 8.5f) < 1e-4f &&
              sim.data[objdata::Direction] == 4,
          "a slot on the object");
    code.add(kGroup, 0x1002, tree({instr(0x2D, T, F, {7, 0, 1, 0, 0})}));
    t = vm.start(sim.id, 0x1002, {}, chair.id);
    vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("Invalid routing slot") == 0, "missing routing slot");

    // Go To Relative Position: behind the chair (location 4), any direction;
    // walled in, it cannot get there.
    vm.setPosition(sim, {1.5f, 9.5f, 1});
    code.add(kGroup, 0x1003, tree({instr(0x1B, T, F, {0, 0, 4, 0xFF, 0, 0, 2})}));
    t = vm.start(sim.id, 0x1003, {}, chair.id);
    for (int i = 0; i < 1000 && t.status == Thread::Status::Running; ++i)
        vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && std::fabs(sim.position.x - 7.5f) < 1e-4f &&
              std::fabs(sim.position.y - 2.5f) < 1e-4f,
          "Go To Relative Position behind");
    for (int x = 0; x < 12; ++x)
        vm.world->addWall(1, x, 8, x + 1, 8);
    vm.setPosition(sim, {1.5f, 9.5f, 1});
    t = vm.start(sim.id, 0x1003, {}, chair.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result, "no way there: false");
    vm.setPosition(sim, {-1, -1, 1});
    t = vm.start(sim.id, 0x1003, {}, chair.id);
    vm.run(t);
    check(t.status == Thread::Status::Failed && t.error.find("out of world") != std::string::npos,
          "routing out of the world");
}

// Snap Into puts an object in a containment slot; Find Location For places
// objects near another one, or out of the world.
static void testSnapAndPlace() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(8, 8);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0x80));
    Slots table;
    table.slots.resize(3);
    table.slots[1].type = 3; // containers: slots 0 and 2 of the table, numbered 0 and 1
    table.slots[0].height = table.slots[2].height = 4; // counter height: bit 3 of the allowed heights
    code.add(kGroup, 0x80, table);
    Object& counter = vm.instantiate(kGroup, 0x80);
    vm.setPosition(counter, {3.5f, 3.5f, 1});
    counter.data[objdata::Direction] = 4;
    Object& plate = place(vm, 6, 6);
    Object& cup = place(vm, 6, 5);
    plate.data[4] = cup.data[4] = 0x08;
    // Snap the stack object (plate) into me (counter), slot 1; then the cup into the same slot.
    code.add(kGroup, 0x1000, tree({instr(0x71, T, F, {StackId, 0, 0, Data, 11, 0, 1, 0, 0, 0})}));
    Thread t = vm.start(counter.id, 0x1000, {}, plate.id);
    vm.run(t);
    // PlaceInSlot: the container's position and direction (docs/seat-mac.md §B).
    check(t.status == Thread::Status::Finished && t.result && vm.containedIn(counter.id, 1) == plate.id &&
              plate.position.x == 3.5f && plate.data[objdata::Direction] == 4 && counter.data[52] == 0,
          "Snap Into a slot");
    t = vm.start(counter.id, 0x1000, {}, cup.id);
    vm.run(t);
    // TestPlaceInSlot's errors + 2 in my od 52: 0x0C taken, 0x0E no slot, 0x25 the wrong height, 0x0F too heavy.
    check(t.status == Thread::Status::Finished && !t.result && counter.data[52] == 0x0E, "the slot is taken");
    code.add(kGroup, 0x1001, tree({expr(Temp, 0, Assign, Lit, 2, go(1), E),
                                   instr(0x71, T, F, {StackId, 0, 0, Data, 11, 0, 0, 0, 0, 1})}));
    t = vm.start(counter.id, 0x1001, {}, cup.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result && counter.data[52] == 0x10, "slot from temp 0 (no slot 2)");
    code.add(kGroup, 0x1005, tree({instr(0x71, T, F, {StackId, 0, 0, Data, 11, 0, 0, 0, 0, 0})}));
    cup.data[4] = 0x01; // the floor only
    t = vm.start(counter.id, 0x1005, {}, cup.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result && counter.data[52] == 0x27, "the wrong height");
    cup.data[4] = 0x08;
    cup.data[27] = 5; // heavier than the counter holds (od 28 = 0)
    t = vm.start(counter.id, 0x1005, {}, cup.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result && counter.data[52] == 0x11, "too heavy");
    cup.data[27] = 0;

    // Find Location For, normal: the cup next to the counter (whose tile is taken).
    code.add(kGroup, 0x1002, tree({instr(0x10, T, F, {0, 0, 0})}));
    vm.setContainer(cup, 0, cup.data[objdata::SlotNumber]);
    t = vm.start(counter.id, 0x1002, {}, cup.id);
    vm.run(t);
    const TilePos at = VM::tileOf(cup);
    check(t.status == Thread::Status::Finished && t.result && std::max(std::abs(at.x - 3), std::abs(at.y - 3)) == 1,
          "Find Location For next to the reference");
    // Out of world.
    code.add(kGroup, 0x1003, tree({instr(0x10, T, F, {1, 0, 0})}));
    t = vm.start(counter.id, 0x1003, {}, cup.id);
    vm.run(t);
    check(t.status == Thread::Status::Finished && !cup.position.inWorld(), "Find Location For out of world");
    // Explicit objects (flag 0x08): the plate (local 0) near the cup's... the
    // counter (me), along its vector (mode 3): in front of it first.
    counter.data[objdata::Direction] = 2; // faces +Y
    code.add(kGroup, 0x1004, tree({expr(Local, 0, Assign, Lit, plate.id, go(1), E),
                                   instr(0x10, T, F, {3, 0, 8, 0, Local, 0, 0, Data, 11, 0})},
                                  0, 1));
    t = vm.start(counter.id, 0x1004, {}, 0);
    vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && VM::tileOf(plate) == TilePos{3, 4, 1} &&
              plate.data[objdata::ContainerId] == 0,
          "Find Location For along the vector takes it out of its slot");
}

// Stairs: read off the steps' cObjects, then walked by the routing
// primitives from one level to the next, phase by phase.
static void testStairs() {
    Lot lot;
    lot.width = lot.height = 10;
    lot.minLevel = 0;
    lot.levelCount = 2; // no elevations: level l is 3 * l high
    auto piece = [&](const char* kind, float y, float z) {
        Lot::Object o;
        o.definition = std::string("Stair Modular - Test - ") + kind + "_objt";
        o.position = {4.5f, y, z};
        o.rotation = {0, 0, -1, 0}; // yaw -180: the steps face -Y (down)
        lot.objects.push_back(o);
    };
    piece("Portal - A", 8.75f, 2.8125f);
    for (int k = 1; k < 15; ++k)
        if (k != 7) // a step lost from the save: bridged
            piece("Bridge", 8.75f - 0.25f * float(k), 2.8125f - 0.1875f * float(k));
    piece("End", 5.0f, 0);
    piece("Portal - A", 8.75f, 2.8125f); // a stale copy
    const auto stairs = lotStairs(lot);
    check(stairs.size() == 1, "one stair from the steps");
    if (stairs.size() == 1) {
        const Stair& st = stairs[0];
        check(st.bottom == TilePos{4, 4, 1} && st.top == TilePos{4, 9, 2} && st.steps == 16 && st.direction == 2 &&
                  std::fabs(st.bottomZ) < 1e-4f && std::fabs(st.topZ - 3) < 1e-4f,
              "the stair's landings, steps and heights");
    }

    // A stair up +X from (2, 1) on level 1 to (5, 1) on level 2.
    CodeLibrary code;
    VM vm(code);
    vm.world = World(10, 4, 1, 2);
    World& w = *vm.world;
    for (int x = 3; x <= 4; ++x)
        w.setWalkable({x, 1, 2}, false); // the stairwell
    Stair st;
    st.bottom = {2, 1, 1}, st.top = {5, 1, 2};
    st.bottomEdge = {3.0f, 1.5f};
    st.up = {1, 0};
    st.steps = 8;
    st.bottomZ = 0, st.topZ = 1.5f;
    st.direction = 4;
    w.addStair(st);
    check(!w.walkable({3, 1, 1}) && !w.walkable({4, 1, 1}) && w.walkable({2, 1, 1}), "no walking under the steps");
    std::vector<BlockedGrid> blocked(2, BlockedGrid(40, 0));
    Path p;
    std::vector<int> via;
    check(findPath(w, blocked, {0, 1, 1}, 2, {{8, 1, 0}}, p, via) && p.tiles.back() == TilePos{8, 1, 2},
          "a path up the stair");
    size_t climbs = 0;
    for (size_t i = 0; i < p.tiles.size(); ++i)
        climbs += via[i] == 0 && p.tiles[i] == st.top && i > 0 && p.tiles[i - 1] == st.bottom;
    check(climbs == 1, "the path takes the stair once, landing to landing");
    check(!findPath(w, blocked, {0, 1, 1}, 2, {{3, 1, 0}}, p, via), "no goal in the stairwell");

    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {8.5f, 1.5f, 2});
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {0.5f, 1.5f, 1});
    // Go To Relative Position: in front of the lamp (direction 0, -X), facing it.
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0x04})})); // 0x04: another level
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    std::set<std::string> phases;
    float maxZ = 0;
    int ticks = 0;
    for (; ticks < 3000 && t.status == Thread::Status::Running; ++ticks) {
        vm.run(t);
        if (sim.route && !sim.route->climb.animation.empty()) {
            phases.insert(sim.route->climb.animation);
            maxZ = std::max(maxZ, sim.route->climb.z);
        }
    }
    check(t.status == Thread::Status::Finished && t.result && sim.position.level == 2 &&
              sim.position.x > 6 && sim.position.x < 8 && sim.data[objdata::Direction] == 4,
          "walked upstairs to the lamp");
    check(phases.count("a-stairs-up-start-R") && phases.count("a-stairs-up-loop-L") &&
              phases.count("a-stairs-up-loop-R") && phases.count("a-stairs-up-stop-R") && std::fabs(maxZ - 1.5f) < 1e-4f,
          "the climb's phases");
    // And back down.
    vm.setPosition(lamp, {1.5f, 2.5f, 1});
    t = vm.start(sim.id, 0x1000, {}, lamp.id);
    phases.clear();
    for (int i = 0; i < 3000 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        if (sim.route && !sim.route->climb.animation.empty())
            phases.insert(sim.route->climb.animation);
    }
    check(t.status == Thread::Status::Finished && t.result && sim.position.level == 1 &&
              phases.count("a-stairs-down-start-R") && phases.count("a-stairs-down-stop-R"),
          "and back down");
}

// The continuous path planner (path_planner.hpp): inflated obstacles,
// diagonal walls cut tiles in half, doors are portals, smoothing, costs and
// the failure codes.
static float pathLength(Point2 start, const PlannedPath& p) {
    float len = 0;
    Point2 a = start;
    for (const auto& w : p.points) {
        len += std::hypot(w.x - a[0], w.y - a[1]);
        a = {w.x, w.y};
    }
    return len;
}

static void testPathPlanner() {
    World w(10, 10);
    PathPlanner planner(w);
    PlannedPath p;
    RouteFail why;
    // Open ground: one straight line.
    check(planner.plan({1.5f, 1.5f}, 1, 1, {{8.5f, 6.5f, 0}}, p, why) && p.points.size() == 1 &&
              std::fabs(p.points[0].x - 8.5f) < 1e-4f,
          "planner: a straight line in the open");
    // An object's tile, inflated by 1/8 tile: the walk keeps clear of it,
    // and is shorter than going round tile centres.
    planner.addObstacle(1, 4, 0, 5, 9, PathPlanner::kObject); // a row of objects up to y = 9
    check(!planner.free(1, {3.9f, 3.5f}) && planner.free(1, {3.85f, 3.5f}), "planner: obstacles inflated 1/8 tile");
    check(planner.plan({1.5f, 1.5f}, 1, 1, {{8.5f, 1.5f, 0}}, p, why), "planner: around a row of objects");
    bool clear = true;
    Point2 a{1.5f, 1.5f};
    for (const auto& q : p.points) {
        clear &= planner.clear(1, a, {q.x, q.y});
        a = {q.x, q.y};
    }
    const float len = pathLength({1.5f, 1.5f}, p);
    // Up to (3.875, 9.125), across and down: about 17.8 tiles.
    check(clear && len > 17.5f && len < 18.5f,
          "planner: the path hugs the inflated corner");
    // A gap narrower than a Sim (two objects 0.2 apart, inflated, close it).
    planner.clearObstacles();
    planner.addObstacle(1, 4, 0, 5, 4.9f, PathPlanner::kObject);
    planner.addObstacle(1, 4, 5.1f, 5, 10, PathPlanner::kObject);
    check(!planner.plan({1.5f, 5.0f}, 1, 1, {{8.5f, 5.0f, 0}}, p, why) && why == RouteFail::PathNotFound,
          "planner: no squeezing through a gap, path not found (2)");
    // Failure codes: no goals (6), a goal under an object (7).
    planner.clearObstacles();
    check(!planner.plan({1.5f, 1.5f}, 1, 1, {}, p, why) && why == RouteFail::NoValidGoals, "planner: no valid goals (6)");
    planner.addObstacle(1, 8, 8, 9, 9, PathPlanner::kObject);
    check(!planner.plan({1.5f, 1.5f}, 1, 1, {{8.5f, 8.5f, 0}}, p, why) && why == RouteFail::DestinationOccupied,
          "planner: destination occupied (7)");
    // A start inside an object (getting up from a chair) steps out first.
    check(planner.plan({8.5f, 8.5f}, 1, 1, {{1.5f, 1.5f, 0}}, p, why) && p.escaped, "planner: out of a footprint");
    // Another Sim is a cost, not a wall: the walk goes round it if that is cheaper.
    planner.clearObstacles();
    planner.addRouter(1, 5.0f, 5.0f, 0, false);
    check(planner.plan({1.0f, 5.0f}, 1, 1, {{9.0f, 5.0f, 0}}, p, why), "planner: past another Sim");
    float closest = 9;
    a = {1.0f, 5.0f};
    for (const auto& q : p.points) {
        for (int k = 0; k <= 20; ++k) {
            const float u = float(k) / 20, x = a[0] + (q.x - a[0]) * u, y = a[1] + (q.y - a[1]) * u;
            closest = std::min(closest, std::hypot(x - 5.0f, y - 5.0f));
        }
        a = {q.x, q.y};
    }
    check(closest > 0.3f, "planner: round another Sim, not through");

    // Diagonal walls cut tiles in half: both halves are places to stand.
    World d(8, 8);
    d.addWall(1, 0, 4, 4, 0); // from (0, 4) to (4, 0)
    PathPlanner dp(d);
    check(dp.free(1, {2.75f, 1.75f}) && dp.free(1, {2.25f, 1.25f}) && !dp.free(1, {2.5f, 1.5f}),
          "planner: the halves of a diagonal tile");
    check(dp.plan({6.5f, 6.5f}, 1, 1, {{2.75f, 1.75f, 0}}, p, why), "planner: to the outer half of a diagonal tile");
    check(!dp.plan({6.5f, 6.5f}, 1, 1, {{2.2f, 1.2f, 0}}, p, why), "planner: not through the diagonal wall");

    // A door is a portal: the walk goes through its two points, straight.
    World h(10, 6);
    for (int y = 0; y < 6; ++y)
        h.addWall(1, 5, y, 5, y + 1); // a wall at x = 5
    h.openDoor({4, 2, 1}, 0);          // facing -X, its back to the wall
    PathPlanner hp(h);
    check(hp.portals().size() == 1 && !hp.free(1, {5.0f, 2.5f}), "planner: the door's opening is closed, a portal opens it");
    check(hp.plan({1.5f, 4.5f}, 1, 1, {{8.5f, 4.5f, 0}}, p, why), "planner: through the door");
    bool through = false;
    for (size_t i = 0; i < p.points.size(); ++i)
        through |= p.points[i].portal == 0 && std::fabs(p.points[i].x - 5.5f) < 1e-4f && i > 0 &&
                   std::fabs(p.points[i - 1].x - 4.5f) < 1e-4f && std::fabs(p.points[i - 1].y - 2.5f) < 1e-4f;
    check(through, "planner: in front of the door, then behind it");

    // The rooms a walker reaches (GetAccessibleRooms): a walled-off room
    // is not reached until a door opens it.
    World r(10, 6);
    for (int y = 0; y < 6; ++y)
        r.addWall(1, 5, y, 5, y + 1);
    for (int x = 0; x < 10; ++x)
        for (int y = 0; y < 6; ++y)
            r.setRoom({x, y, 1}, int16_t(x < 5 ? 1 : 2));
    {
        PathPlanner rp(r);
        check(rp.accessibleRooms(1) == std::vector<int16_t>{1} && rp.accessibleRooms(2) == std::vector<int16_t>{2},
              "accessible rooms: a wall keeps the other room out");
    }
    r.openDoor({4, 2, 1}, 0);
    PathPlanner rp2(r);
    check(rp2.accessibleRooms(1) == (std::vector<int16_t>{1, 2}) && rp2.accessibleRooms(7).empty(),
          "accessible rooms: through a door's portal; an unknown room reaches nothing");

    // Floor costs: a costly band is walked round when that is cheaper.
    World c(12, 6);
    for (int y = 0; y < 5; ++y)
        c.setCost({6, y, 1}, 10);
    PathPlanner cp(c);
    check(cp.plan({1.5f, 1.5f}, 1, 1, {{10.5f, 1.5f, 0}}, p, why) && pathLength({1.5f, 1.5f}, p) > 9.5f,
          "planner: round a costly floor");
}

// The planner kept between plans brings its grids and quadtree up to date
// only where the obstacles and Sims changed (path_planner.hpp, Stamp and
// Pyramid): its plans must be those of a planner made anew for each set of
// obstacles, and those of the summed-area tables (a cost outside [0.5, 16],
// which changes no cell, sends a planner to them).
static void testPlannerKept() {
    World w(24, 20, 1, 2);
    for (int y = 0; y < 14; ++y)
        w.addWall(1, 12, y, 12, y + 1);
    w.openDoor({11, 6, 1}, 0);
    for (int x = 0; x < 24; ++x)
        w.setQuarterCosts({x, 17, 1}, {1.0f, 1.0f, 4.0f, 2.0f});
    for (int y = 0; y < 10; ++y)
        w.setCost({5, y, 1}, 7.5f);
    PathPlanner kept(w);
    uint32_t rng = 12345;
    auto next = [&](int n) {
        rng = rng * 1103515245u + 12345u;
        return int((rng >> 8) % uint32_t(n));
    };
    // Fixed objects, and Sims walking about.
    std::vector<std::array<float, 4>> objects;
    for (int i = 0; i < 25; ++i) {
        const float x = float(next(23)), y = float(next(19));
        objects.push_back({x, y, x + 0.4f + float(next(8)) * 0.1f, y + 0.4f + float(next(8)) * 0.1f});
    }
    std::array<Point2, 6> sims{};
    for (auto& s : sims)
        s = {float(next(230)) / 10, float(next(190)) / 10};
    bool same = true, sameGrid = true;
    int planned = 0;
    for (int round = 0; round < 120; ++round) {
        // One to three Sims move a step; now and then an object goes or comes.
        for (int m = 1 + next(3); m > 0; --m) {
            Point2& s = sims[size_t(next(int(sims.size())))];
            s = {std::clamp(s[0] + float(next(11) - 5) * 0.1f, 0.2f, 23.8f),
                 std::clamp(s[1] + float(next(11) - 5) * 0.1f, 0.2f, 19.8f)};
        }
        if (next(10) == 0)
            objects[size_t(next(int(objects.size())))][0] += 1.0f;
        PathPlanner fresh(w), tables(w);
        for (PathPlanner* p : {&kept, &fresh, &tables}) {
            p->clearObstacles();
            for (const auto& o : objects)
                p->addObstacle(1, o[0], o[1], o[2], o[3], PathPlanner::kObject);
            for (size_t i = 1; i < sims.size(); ++i)
                p->addRouter(1, sims[i][0], sims[i][1], 0, (round + int(i)) % 3 == 0);
        }
        tables.addCost(1, 0.0f, 0.0f, 1.0f, 1.0f, 0.25f);
        for (int x = 0; x < 24 * 8 && sameGrid; ++x)
            for (int y = 0; y < 20 * 8; ++y) {
                const Point2 c{(float(x) + 0.5f) / 8, (float(y) + 0.5f) / 8};
                if (kept.at(1, c) != fresh.at(1, c) || kept.costAt(1, c) != fresh.costAt(1, c)) {
                    sameGrid = false;
                    break;
                }
            }
        const Point2 from = sims[0];
        const std::vector<PlanGoal> goals{{float(next(230)) / 10, float(next(190)) / 10, 0},
                                          {float(next(230)) / 10, float(next(190)) / 10, 0.5f}};
        PlannedPath a, b, c;
        RouteFail wa = RouteFail::Success, wb = RouteFail::Success, wc = RouteFail::Success;
        kept.seed(uint32_t(round + 1)), fresh.seed(uint32_t(round + 1)), tables.seed(uint32_t(round + 1));
        const bool ra = kept.plan(from, 1, 1, goals, a, wa), rb = fresh.plan(from, 1, 1, goals, b, wb),
                   rc = tables.plan(from, 1, 1, goals, c, wc);
        planned += ra;
        auto equal = [](const PlannedPath& x, const PlannedPath& y) {
            if (x.points.size() != y.points.size() || x.goal != y.goal || x.cost != y.cost || x.escaped != y.escaped)
                return false;
            for (size_t i = 0; i < x.points.size(); ++i)
                if (x.points[i].x != y.points[i].x || x.points[i].y != y.points[i].y || x.points[i].portal != y.points[i].portal)
                    return false;
            return true;
        };
        same = same && ra == rb && rb == rc && wa == wb && wb == wc && equal(a, b) && equal(b, c) &&
               kept.noise() == fresh.noise() && fresh.noise() == tables.noise();
    }
    check(sameGrid, "kept planner: its grids are those of a new planner");
    check(same && planned > 60, "kept planner: its plans are those of a new planner and of the summed-area tables");
}

// A failed route writes the route result (person data 62) and the
// primitive returns false.
static void testRouteResult() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(10, 10);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {8.5f, 8.5f, 1});
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {1.5f, 1.5f, 1});
    check(sim.person.has_value(), "a person");
    for (int x = 0; x < 10; ++x)
        vm.world->addWall(1, x, 5, x + 1, 5); // walled off
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    for (int i = 0; i < 10 && t.status == Thread::Status::Running; ++i)
        vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result && sim.person && sim.person->data[62] == 2,
          "route result 2: path not found");
}

// Person data 39 "route status" as the base game's router keeps it
// (docs/primitives-mac.md §4j): 1 from the start of the route (the pre-route
// tree already sees it) while it walks, 0 once it is over; 0 when no way is
// found (InitRoute) and after the route that failed.
static void testRouteStatus() {
    for (const bool walled : {false, true}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(10, 10);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        ObjectFunctions f;
        f.functions.resize(0x26);
        f.functions[0x25] = {0, 0x1010};
        code.add(kGroup, 0x81, f);
        constexpr uint8_t Global = 0x06, MyPersonData = 0x12;
        // The pre-route tree: global 10 = my person data 39, true.
        code.add(kGroup, 0x1010, tree({expr(Global, 10, Assign, MyPersonData, 39, T, E)}));
        vm.globals[10] = -1;
        Object& lamp = vm.instantiate(kGroup, 0x80);
        vm.setPosition(lamp, {8.5f, 8.5f, 1});
        Object& sim = vm.instantiate(kGroup, 0x81);
        vm.setPosition(sim, {1.5f, 1.5f, 1});
        if (walled) {
            for (int x = 0; x < 10; ++x)
                vm.world->addWall(1, x, 5, x + 1, 5);
            sim.person->data[39] = 1;
        }
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0x02})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        bool walking = false, steady = true;
        for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i) {
            vm.run(t);
            if (t.status == Thread::Status::Running && sim.route) {
                walking = true;
                steady = steady && sim.person->data[39] == 1;
            }
        }
        if (walled)
            check(t.status == Thread::Status::Finished && !t.result && !walking && vm.globals[10] == -1 &&
                      sim.person->data[39] == 0,
                  "no way: route status 0");
        else
            check(t.status == Thread::Status::Finished && t.result && walking && steady && vm.globals[10] == 1 &&
                      sim.person->data[39] == 0,
                  "route status 1 from the pre-route tree through the walk, 0 after it");
    }
}

// Round n46: UpdatePersonDataFields (base 1.0 FUN_00c64a70) at every plan:
// person data 40 "route goal" (the target), 37 "route slot" (-1 without a
// routing slot of the target's or the global SLOT), 55 "route start slot"
// (-1: from where the walker stands) and 38 "route multi slot index" (the
// current goal's; -1 when the plan found no way), for any route, not only
// Go To Routing Slot's; the pre-route tree already sees them.
static void testRoutePersonDataFields() {
    for (const bool walled : {false, true}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(10, 10);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        ObjectFunctions f;
        f.functions.resize(0x26);
        f.functions[0x25] = {0, 0x1010};
        code.add(kGroup, 0x81, f);
        constexpr uint8_t Global = 0x06, MyPersonData = 0x12;
        // The pre-route tree: global 10 = my person data 40.
        code.add(kGroup, 0x1010, tree({expr(Global, 10, Assign, MyPersonData, 40, T, E)}));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        vm.setPosition(lamp, {8.5f, 8.5f, 1});
        Object& sim = vm.instantiate(kGroup, 0x81);
        vm.setPosition(sim, {1.5f, 1.5f, 1});
        for (int k : {37, 38, 40, 55})
            sim.person->data[size_t(k)] = 77;
        if (walled)
            for (int x = 0; x < 10; ++x)
                vm.world->addWall(1, x, 5, x + 1, 5);
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0x02})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        const auto& d = sim.person->data;
        if (walled)
            check(t.status == Thread::Status::Finished && !t.result && d[40] == lamp.id && d[37] == -1 && d[55] == -1 &&
                      d[38] == -1,
                  "no way: 40 the target, 37 and 55 -1, 38 -1 (no current goal)");
        else
            check(t.status == Thread::Status::Finished && t.result && vm.globals[10] == lamp.id && d[40] == lamp.id &&
                      d[37] == -1 && d[55] == -1 && d[38] == 0,
                  "a Go To Relative Position route: 40 the target (the pre-route tree sees it), 37 and 55 -1, 38 the "
                  "goal's 0");
    }
}

// Person data 37 of a global routing slot is its index + 1000
// (WorkOutSlotIndex, round n49: "Turn toward incoming Sim's target slot"
// compares it with 1033, the social approach's global slot 33); a slot of
// the target's own SLOT is its index.
static void testRouteSlotIndex() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(10, 10);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0x80));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Slots table;
    table.slots.resize(2);
    for (auto& sl : table.slots) {
        sl.type = 3;
        sl.flags = 0x01;
        sl.minDistance = sl.maxDistance = sl.optimalDistance = 1;
        sl.resolutionTiles = 1;
    }
    code.add(kGroup, 0x80, table);
    code.add(kGlobalGroup, 0x64, table); // the global SLOT
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {6.5f, 5.5f, 1});
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {1.5f, 1.5f, 1});
    for (const auto& [scope, want] : {std::pair{uint8_t(1), int16_t(1)}, std::pair{uint8_t(2), int16_t(1001)}}) {
        code.add(kGroup, 0x1000 + scope, tree({instr(0x2D, T, F, {1, 0, scope, 0, 0})}));
        Thread t = vm.start(sim.id, uint16_t(0x1000 + scope), {}, lamp.id);
        vm.run(t);
        check(sim.person->data[37] == want, scope == 2 ? "route slot: a global slot's index + 1000" : "route slot: a local slot's index");
        for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        vm.setPosition(sim, {1.5f, 1.5f, 1});
    }
}

// A Sim standing in a one-tile corridor blocks another: the walker waits
// (asking it to move), plans again, and after five tries fails with
// "blocked by person" (14).
static void testBlockedBySim() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(12, 3);
    for (int x = 0; x < 12; ++x) {
        vm.world->addWall(1, x, 1, x + 1, 1);
        vm.world->addWall(1, x, 2, x + 1, 2);
    }
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {11.5f, 1.5f, 1});
    Object& walker = vm.instantiate(kGroup, 0x81);
    vm.setPosition(walker, {0.5f, 1.5f, 1});
    // Two Sims across the corridor: one Sim alone in a one-tile corridor is
    // passed (the centres only have to stay 0.283 apart and the walls let a
    // centre 0.375 off the middle).
    Object& other = vm.instantiate(kGroup, 0x81);
    vm.setPosition(other, {6.5f, 1.6f, 1});
    Object& wallSim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(wallSim, {6.5f, 1.15f, 1});
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(walker.id, 0x1000, {}, lamp.id);
    int ticks = 0, waited = 0;
    for (; ticks < 3000 && t.status == Thread::Status::Running; ++ticks) {
        vm.run(t);
        waited += walker.route && walker.route->waiting && walker.route->blocker == other.id;
    }
    // DoBlockedGoalStillAvailable: it cannot be asked (one goal), so the
    // walker steps aside itself once (TryGetSelfOutOfWay), is blocked again
    // and gives up with "blocked by person" (the binary waits only after a
    // request the other took).
    if (std::getenv("DEBUG_BLOCK"))
        std::fprintf(stderr, "corridor: status %d result %d rr %d waited %d ticks %d x %.2f\n", int(t.status), t.result,
                     walker.person ? walker.person->data[62] : -1, waited, ticks, walker.position.x);
    check(t.status == Thread::Status::Finished && !t.result && walker.person && walker.person->data[62] == 14 &&
              ticks < 300 && walker.position.x < 6,
          "blocked by a Sim in a corridor: steps aside once, then fails with 14");
    // The Sim steps aside (out of the corridor): the way is free.
    vm.setPosition(other, {-1, -1, 1});
    t = vm.start(walker.id, 0x1000, {}, lamp.id);
    for (int i = 0; i < 3000 && t.status == Thread::Status::Running; ++i)
        vm.run(t);
    check(t.status == Thread::Status::Finished && t.result && walker.position.x > 9, "and walks on once it is gone");

    // Two Sims walking at each other in the open pass each other (each
    // pauses, "CT - Route Blocked Animation" here a 30-tick wait, and plans
    // round the other: the blocked walk's try counter goes on with the same
    // Sim since round n18, so without the pause's time the second block
    // gave up). Each starts a tile short of the other's goal: on it, its
    // slot's only point there (the scan's cone, round n30, leaves out the
    // row's ends, a tile to either side) would be taken from the start.
    Behavior pauseTree = tree({expr(0x09, 2, Assign, Lit, 30, go(1), E), instr(0x00, T, E, {2, 0})}, 4);
    pauseTree.fileName = "CT - Route Blocked Animation";
    code.add(kGlobalGroup, 0x1BB, pauseTree);
    vm.world = World(12, 12);
    Object& a = walker;
    Object& b = other;
    vm.setPosition(a, {2.5f, 6.5f, 1});
    vm.setPosition(b, {9.5f, 6.5f, 1});
    Object& lampB = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {11.5f, 6.5f, 1});
    vm.setPosition(lampB, {0.5f, 6.5f, 1});
    code.add(kGroup, 0x1001, tree({instr(0x1B, T, F, {0, 0, 4, 0xFE, 0, 0, 0})})); // behind the lamp (its +X side)
    vm.threads.insert_or_assign(a.id, vm.start(a.id, 0x1000, {}, lamp.id));
    vm.threads.insert_or_assign(b.id, vm.start(b.id, 0x1001, {}, lampB.id));
    float closest = 99;
    for (int i = 0; i < 2000; ++i) {
        vm.tick();
        closest = std::min(closest, std::hypot(a.position.x - b.position.x, a.position.y - b.position.y));
        if (vm.threads.at(a.id).status != Thread::Status::Running && vm.threads.at(b.id).status != Thread::Status::Running)
            break;
    }
    if (std::getenv("DEBUG_BLOCK"))
        std::fprintf(stderr, "pass: a %d/%d at %.2f,%.2f (pd62 %d) b %d/%d at %.2f,%.2f (pd62 %d) closest %.2f\n",
                     int(vm.threads.at(a.id).status), vm.threads.at(a.id).result, a.position.x, a.position.y,
                     a.person->data[62], int(vm.threads.at(b.id).status), vm.threads.at(b.id).result, b.position.x,
                     b.position.y, b.person->data[62], closest);
    check(vm.threads.at(a.id).status == Thread::Status::Finished && vm.threads.at(a.id).result &&
              vm.threads.at(b.id).status == Thread::Status::Finished && vm.threads.at(b.id).result && a.position.x > 9 &&
              b.position.x < 3 && closest > 0.25f,
          "two Sims walking at each other pass");
}

// Through a door: the walker stops at the door's portal (waiting for it to
// open: the route's wait point names the door object), then goes through.
static void testDoorWait() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(10, 6);
    for (int y = 0; y < 6; ++y)
        vm.world->addWall(1, 5, y, 5, y + 1);
    vm.world->openDoor({4, 2, 1}, 0);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    ObjectDefinition doorDef = definition(0xAAAA0003, 0);
    doorDef.fields[(0x52 - 0x44) / 2] = kObjectTypeDoor;
    code.add(kGroup, 0x82, doorDef);
    Object& door = vm.instantiate(kGroup, 0x82);
    vm.setPosition(door, {4.5f, 2.5f, 1});
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {9.5f, 2.5f, 1});
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {1.5f, 4.5f, 1});
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    int waitTicks = 0;
    bool named = false;
    for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        if (sim.route && sim.route->waiting) {
            ++waitTicks;
            named |= std::fabs(sim.position.x - 4.5f) < 1e-3f && !sim.route->points.empty() &&
                     sim.route->points.front().door == door.id;
        }
    }
    if (std::getenv("DEBUG_DOOR"))
        std::fprintf(stderr, "door test: status %d result %d x %.2f named %d wait %d type %d\n", int(t.status), t.result,
                     sim.position.x, named, waitTicks, door.definition ? door.definition->objectType() : -1);
    check(t.status == Thread::Status::Finished && t.result && sim.position.x > 7 && named && waitTicks >= 10,
          "a door: wait at its portal, then through");
}

// A door with a portal function (OBJf entry 15): planning runs its guard
// as the TEST tree (false locks the door: no way through), arriving runs
// its action on the walker's thread with the door as the stack object and
// the portal's parameters (6 = the routing slot on the walker's side), and
// the walk goes on through when it returns true.
static void testDoorPortalTree() {
    for (const bool locked : {false, true}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(10, 6);
        for (int y = 0; y < 6; ++y)
            vm.world->addWall(1, 5, y, 5, y + 1);
        vm.world->openDoor({4, 2, 1}, 0);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        ObjectDefinition doorDef = definition(0xAAAA0003, 0);
        doorDef.fields[(0x52 - 0x44) / 2] = kObjectTypeDoor;
        code.add(kGroup, 0x82, doorDef);
        ObjectFunctions f;
        f.functions.resize(16);
        f.functions[15] = {0x1010, 0x1011};
        code.add(kGroup, 0x82, f);
        constexpr uint8_t Global = 0x06, Param = 0x09;
        // TEST: open while global 12 is 0.
        code.add(kGroup, 0x1010, tree({expr(Global, 12, Eq, Lit, 0, T, F)}));
        // The portal: global 10 = parameter 6 + 100, global 11 = the stack object.
        code.add(kGroup, 0x1011, tree({expr(Global, 10, Assign, Param, 6, go(1), E),
                                       expr(Global, 10, 3, Lit, 100, go(2), E),
                                       expr(Global, 11, Assign, StackId, 0, T, E)}, 8));
        vm.globals[12] = locked ? 1 : 0;
        Object& door = vm.instantiate(kGroup, 0x82);
        vm.setPosition(door, {4.5f, 2.5f, 1});
        Object& lamp = vm.instantiate(kGroup, 0x80);
        vm.setPosition(lamp, {9.5f, 2.5f, 1});
        Object& sim = vm.instantiate(kGroup, 0x81);
        vm.setPosition(sim, {1.5f, 4.5f, 1});
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0x02})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        if (locked)
            check(t.status == Thread::Status::Finished && !t.result && sim.position.x < 5 && vm.globals[11] == 0 &&
                      sim.person && sim.person->data[62] == int16_t(RouteFail::PathNotFound),
                  "a door whose portal TEST is false is locked");
        else
            check(t.status == Thread::Status::Finished && t.result && sim.position.x > 7 && vm.globals[11] == door.id &&
                      vm.globals[10] == 100,
                  "a door's portal tree runs on arrival, with the door as stack object and its side's slot");
    }
}

// The walker's pre-route tree (OBJf entry 0x25) runs once a way is found,
// on its thread with itself as the stack object and the target in
// parameter 0 (here it counts its runs); the route is planned again after it.
// False fails the route with "pre-route failed" (11).
static void testPreRouteTree() {
    for (const bool refuse : {false, true}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(10, 6);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        ObjectFunctions f;
        f.functions.resize(0x26);
        f.functions[0x25] = {0, 0x1010};
        code.add(kGroup, 0x81, f);
        constexpr uint8_t Global = 0x06, Param = 0x09;
        code.add(kGroup, 0x1010, tree({expr(Global, 10, 3, Lit, 1, go(1), E),          // global 10 += 1
                                       expr(Global, 11, Assign, Param, 0, go(2), E),   // global 11 = the target
                                       expr(Global, 12, Assign, StackId, 0, go(3), E), // global 12 = stack object
                                       expr(Data, 1, Assign, Lit, 0, refuse ? F : T, E)}, 6));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        vm.setPosition(lamp, {8.5f, 2.5f, 1});
        Object& sim = vm.instantiate(kGroup, 0x81);
        vm.setPosition(sim, {1.5f, 2.5f, 1});
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0x02})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        if (refuse)
            check(t.status == Thread::Status::Finished && !t.result && sim.position.x < 2 && vm.globals[10] == 1 &&
                      sim.person && sim.person->data[62] == int16_t(RouteFail::PreRouteFailed),
                  "a pre-route tree's false fails the route with code 11");
        else
            check(t.status == Thread::Status::Finished && t.result && sim.position.x > 6 && vm.globals[10] == 1 &&
                      vm.globals[11] == lamp.id && vm.globals[12] == sim.id,
                  "the walker's pre-route tree runs once, with the target and itself as stack object");
    }
}

// A goal check tree that routes the walker again (a social's "Goal Check -
// Update Dynamic Route?" -> "Route - Refresh route" -> "Route - Dynamic
// w/Lead in" from inside the walk's goal check): the inner route runs with
// the outer one kept under it (the route stack of cTSRoutingObjectAggregate),
// and back from the tree its false fails the outer route with 13 "goal not
// available". Before, the inner route replaced the outer one, and the outer
// instruction took the tree's false for its pre-route's (11).
static void testRouteInGoalCheck() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(12, 6);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    ObjectFunctions f;
    f.functions.resize(0x28);
    f.functions[0x27] = {0, 0x1011}; // the goal check of the Go To's code owner (the Sim's own tree here)
    code.add(kGroup, 0x81, f);
    constexpr uint8_t Global = 0x06;
    // The goal check: the first time it routes again (to the same slot) and
    // then says no; inside that route it says yes.
    code.add(kGroup, 0x1011, tree({expr(Global, 10, 3, Lit, 1, go(1), E),     // global 10 += 1
                                   expr(Global, 10, Eq, Lit, 1, go(2), T),    // the first time?
                                   instr(0x1B, go(3), go(3), {0, 0, 0, 0xFE, 0, 0, 0x02}),
                                   expr(Global, 11, Assign, Lit, 1, F, E)}, 4)); // global 11: back from the inner route
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {9.5f, 2.5f, 1});
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {1.5f, 2.5f, 1});
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0x02})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    vm.run(t);
    // One goal check a tick (a turn of the thread; docs/routing-mac.md §9.4):
    // the inner route's first one, due in the same turn as the outer's that
    // started it, is not run (DoWalking stands that turn).
    check(t.status == Thread::Status::Running && vm.globals[10] == 1 && vm.globals[11] == 0,
          "one goal check a tick: the inner route's first check, in the outer's turn, is not run");
    for (int i = 0; i < 3000 && t.status == Thread::Status::Running; ++i)
        vm.run(t);
    check(t.status == Thread::Status::Finished && !t.result && vm.globals[11] == 1 && vm.globals[10] >= 2 &&
              sim.position.x > 7 && sim.person && sim.person->data[62] == int16_t(RouteFail::GoalNotAvailable) &&
              !sim.route && sim.outerRoutes.empty(),
          "a goal check that routes again: the inner route walks, the outer one fails with 13 (not 11) after it");
}

// Blocked, the goal check says go on: DoBlockedWaitingForGoalCheckTree (BV
// 0x56da50) goes straight to DoBlockedGoalStillAvailable with the blocker
// the locomotor stored, without trying the step again (docs/routing-mac.md
// §9.4). An object put in the walker's way after it planned blocks a step;
// the blocked goal check takes a few ticks, and meanwhile the object goes
// out of the world: the route still fails with 15 "blocked by object".
// Before, the step was tried again with the walker standing, went free and
// the walk went on (the stuck socials of round n31: the next tick the step
// went into the object again, and the try counter never rose).
static void testBlockedGoalCheck() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(12, 6);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    ObjectFunctions f;
    f.functions.resize(0x28);
    f.functions[0x27] = {0, 0x1011}; // the goal check of the Go To's code owner (the Sim's own tree here)
    code.add(kGroup, 0x81, f);
    constexpr uint8_t Global = 0x06, Param = 0x09;
    // The goal check: true; from its second run (the first is the route's
    // first step) it waits 5 ticks first.
    code.add(kGroup, 0x1011, tree({expr(Global, 10, 3, Lit, 1, go(1), E),  // global 10 += 1
                                   expr(Global, 10, Eq, Lit, 1, T, go(2)), // the first time: true
                                   expr(Param, 3, Assign, Lit, 5, go(3), E),
                                   instr(0x00, T, E, {3, 0})}, // sleep param 3 ticks
                                  4));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {9.5f, 2.5f, 1});
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {1.5f, 2.5f, 1});
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0x02})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    vm.run(t);
    Object& box = vm.instantiate(kGroup, 0x80);
    vm.setPosition(box, {3.5f, 2.5f, 1}); // in the way, after the plan
    bool moved = false;
    for (int i = 0; i < 3000 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        if (!moved && sim.route && sim.route->check == 3) { // the blocked goal check runs
            vm.setPosition(box, {-1, -1, 1});
            moved = true;
        }
    }
    check(moved && t.status == Thread::Status::Finished && !t.result && sim.person &&
              sim.person->data[62] == int16_t(RouteFail::BlockedByObject) && sim.position.x < 3,
          "blocked by an object, the goal check says go on: blocked by object (15), the step not tried again");
}

// The move-out-of-way slot (CreateMoveOutOfWayDestination type 0,
// docs/routing-mac.md §9.3): one direction 45 degrees off the asker's, on
// the side the mover stands; distances 0.0625 to 0.3, optimal 0.3,
// gradient 100, spacing 1/16: two rows (0.3 and 0.2375 out) of three points
// (the cone leaves the rows' ends out), the cheapest 0.3 straight out; the
// facing towards the asker; none when the two are more than 4 tiles apart.
static void testMoveOutOfWaySlot() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(10, 10);
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& asker = vm.instantiate(kGroup, 0x81);
    vm.setPosition(asker, {3.5f, 5.5f, 1});
    asker.data[objdata::Direction] = 4; // +X
    Object& mover = vm.instantiate(kGroup, 0x81);
    vm.setPosition(mover, {4.5f, 5.7f, 1}); // ahead of it, on its +Y side (direction 2, d + 6)
    int direction = -1;
    const auto slot = routing_internal::moveOutOfWaySlot(mover, asker, 0, direction);
    check(slot.has_value() && (slot->flags & 0xFF) == (1u << 3), "move-out-of-way slot: direction d + 7 on the mover's side");
    Object spot;
    spot.position = mover.position;
    spot.data[objdata::Direction] = int16_t(direction);
    const VM::RouteGoals g = slot ? vm.routingGoals(*slot, spot, true, mover.id) : VM::RouteGoals{};
    size_t best = 0;
    for (size_t i = 1; i < g.goals.size(); ++i)
        if (g.goals[i].cost < g.goals[best].cost)
            best = i;
    const float s2 = 0.3f / std::sqrt(2.0f);
    check(g.goals.size() == 6 && std::fabs(g.points[best][0] - (4.5f + s2)) < 1e-3f &&
              std::fabs(g.points[best][1] - (5.7f + s2)) < 1e-3f,
          "move-out-of-way slot: six places, the cheapest 0.3 out at 45 degrees");
    check(!g.facings.empty() && g.facings[best] == directionOf(asker.position.x - g.points[best][0], asker.position.y - g.points[best][1]),
          "move-out-of-way slot: facing the asker");
    // Type 1 (round n49): directions 1, 2, 6 and 7 relative to the
    // destination's own direction (SetPosition does not turn it: -1, keep
    // it), facing -3 (0x28), RS 0x100000 (0x70(1)) and 0x8000 (0x8c(1)).
    const auto one = routing_internal::moveOutOfWaySlot(mover, asker, 1, direction);
    check(one && (one->flags & 0xFF) == 0xC6 && direction == -1 && one->facing == -3 && (one->flags & 0x108000) == 0x108000 &&
              !(one->flags & 0x1000000),
          "move-out-of-way slot: type 1 relative to the destination's own direction");
    vm.setPosition(mover, {9.5f, 9.5f, 1});
    check(!routing_internal::moveOutOfWaySlot(mover, asker, 0, direction), "move-out-of-way slot: none past 4 tiles");
}

// A Sim standing in a corridor is asked to move (MoveOutOfWay): a
// destination object (GUID 0x7C4) is made on it with that slot whether or
// not the slot will give a place (CreateMoveOutOfWayDestination always makes
// it), and its "Interaction - Move Out Of Way" (action 0x100B: Go To Routing
// Slot 0 of the destination) goes on the blocker's queue with the walker's
// priority less one; the blocker's Main runs it: its 0.3-tile step at 45
// degrees, on its side of the walker's way, opens the corridor, and the
// walker, told as soon as the blocker's route ends, plans again and gets
// through; the destination goes. With the blocker's
// priority as high, it is not asked. (Until round n30 the VM searched a
// row up to 6 tiles wide for a free place, ours, and the blocker walked a
// tile into an alcove.)
static void testMoveOutOfWay() {
    for (const bool lowPriority : {true, false}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(12, 4);
        for (int x = 0; x < 12; ++x) {
            vm.world->addWall(1, x, 1, x + 1, 1);
            vm.world->addWall(1, x, 2, x + 1, 2);
        }
        constexpr uint32_t kDestGroup = 0x7F000002;
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        ObjectDefinition simDef = definition(0xAAAA0002, 0, true);
        code.add(kGroup, 0x81, simDef);
        ObjectFunctions mainOnly;
        mainOnly.functions.resize(2);
        mainOnly.functions[1].action = 0x1002;
        code.add(kGroup, 0x81, mainOnly);
        // The Sims' Main: Idle for Input with "allow push", over and over.
        code.add(kGroup, 0x1002, tree({expr(0x09, 0, Assign, Lit, 1, go(1), E), instr(0x11, go(0), E, {0, 0, 1, 0})}, 1));
        // The destination: the move-out-of-way interaction.
        ObjectDefinition destDef = definition(0x000007C4, 0x80);
        destDef.fields[(0x4E - 0x44) / 2] = 1; // its TTAB
        code.add(kDestGroup, 0x80, destDef);
        InteractionTable ttab;
        ttab.version = 0x4D;
        InteractionTable::Interaction e;
        e.action = 0x100B;
        ttab.interactions = {e};
        code.add(kDestGroup, 1, ttab);
        code.add(kDestGroup, 0x100B, tree({instr(0x2D, T, F, {0, 0, 1, 0, 0x01})}));
        // Its Init lets Sims through it (object flag 5, "allow person intersection", as the game's).
        ObjectFunctions destInit;
        destInit.functions.resize(1);
        destInit.functions[0].action = 0x1001;
        code.add(kDestGroup, 0x80, destInit);
        code.add(kDestGroup, 0x1001, tree({expr(Data, 8, 9, Lit, 5, T, E)}));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        vm.setPosition(lamp, {11.5f, 1.5f, 1});
        Object& walker = vm.instantiate(kGroup, 0x81);
        vm.setPosition(walker, {0.5f, 1.5f, 1});
        walker.person->data[33] = 50; // user driven
        Object& other = vm.instantiate(kGroup, 0x81);
        vm.setPosition(other, {6.5f, 1.6f, 1});
        other.data[objdata::Direction] = 0; // facing the walker
        other.person->data[33] = lowPriority ? 0 : 50;
        vm.startMain(other.id);
        // A Sim of high priority against the far wall: with it the way past
        // the other is too narrow (0.283 between centres).
        Object& wallSim = vm.instantiate(kGroup, 0x81);
        vm.setPosition(wallSim, {6.5f, 1.15f, 1});
        wallSim.person->data[33] = 100;
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0xFF, 0xFE, 0, 0, 0x02})})); // anywhere near: goals
        vm.threads.insert_or_assign(walker.id, vm.start(walker.id, 0x1000, {}, lamp.id));
        bool asked = false, destination = false;
        for (int i = 0; i < 3000 && vm.threads.at(walker.id).status == Thread::Status::Running; ++i) {
            vm.tick();
            for (const auto& q : other.queue)
                asked = asked || q.action == 0x100B;
            for (const auto& [id, o] : vm.objects())
                destination = destination || (o.syntheticSlot && o.syntheticSlot->minDistance == 0.0625f);
        }
        const Thread t = vm.threads.at(walker.id);
        for (int i = 0; i < 10; ++i) // the destination goes at the next flush
            vm.tick();
        if (std::getenv("DEBUG_MOVE"))
            std::fprintf(stderr, "move: status %d result %d walker %.2f,%.2f other %.2f,%.2f queue %zu objects %zu main %d %s rr %d asked %d\n",
                         int(t.status), t.result, walker.position.x, walker.position.y, other.position.x, other.position.y,
                         other.queue.size(), vm.objects().size(), int(vm.threads.at(other.id).status),
                         vm.threads.at(other.id).error.c_str(), walker.person->data[62], int(asked));
        if (lowPriority) {
            check(asked && destination && t.status == Thread::Status::Finished && t.result && walker.position.x > 9,
                  "a Sim in the way is asked to move (its destination has the move-out-of-way slot): it steps 0.3 aside and the walker gets through");
            check(vm.objects().size() == 4, "the move-out-of-way destination is gone once its interaction is");
        } else {
            check(!asked && t.status == Thread::Status::Finished && other.queue.empty(), "a Sim of the same priority is not asked to move");
        }
    }
}

// BeginFollow's shape of a walk (docs/routing-mac.md §6.1): legs cut every
// 0.2 tile, and a Sim facing away from its way turns in place first (the
// TurnInPlace animation of the 45-degree steps, mirrored counter-clockwise);
// up to 40 degrees off it walks on at once.
static void testTurnInPlace() {
    for (const int facing : {0, 4, 2}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(12, 12);
        for (const char* n : {"a-TurnInPlace-45", "a-TurnInPlace-90", "a-TurnInPlace-135", "a-TurnInPlace-180"})
            vm.animations.add(n, 1.0f);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        vm.setPosition(lamp, {10.5f, 5.5f, 1});
        Object& sim = vm.instantiate(kGroup, 0x81);
        vm.setPosition(sim, {2.5f, 5.5f, 1});
        sim.person->data[58] = 27; // an adult
        sim.data[objdata::Direction] = int16_t(facing);
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        vm.run(t);
        const bool turning = sim.route && sim.route->climb.turnSteps != 0;
        const int steps = turning ? sim.route->climb.turnSteps : 0;
        const std::string anim = turning ? sim.route->climb.animation : "";
        const float x0 = sim.position.x;
        // Each walking leg on the floor at most 0.2 tile (+ the remainder).
        float longest = 0;
        if (sim.route) {
            Point2 at{sim.position.x, sim.position.y};
            for (const auto& p : sim.route->points)
                if (p.ticks == 0) {
                    longest = std::max(longest, std::hypot(p.x - at[0], p.y - at[1]));
                    at = {p.x, p.y};
                }
        }
        for (int i = 0; i < 25 && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        const float x25 = sim.position.x;
        for (int i = 0; i < 2000 && t.status == Thread::Status::Running; ++i)
            vm.run(t);
        if (std::getenv("DEBUG_TURN"))
            std::fprintf(stderr, "facing %d: steps %d %s x0 %.2f x25 %.2f longest %.3f result %d\n", facing, steps,
                         anim.c_str(), x0, x25, longest, t.result);
        check(t.status == Thread::Status::Finished && t.result, "turn in place: the walk ends");
        check(longest < 0.41f, "the legs are cut every 0.2 tile");
        if (facing == 4) { // direction 4 faces +X, the way: no turn, walking at once
            check(!turning && x25 > 4, "facing the way: no turn in place");
        } else {
            const int want = facing == 2 ? -2 : -4; // +Y to +X: 90 degrees clockwise; -X to +X: 180
            check(steps == want && anim == "a-TurnInPlace-" + std::to_string(45 * -want) && x0 == 2.5f && x25 == 2.5f,
                  "facing away: turns in place (TurnInPlace of its steps) before walking");
        }
    }
}

// The locomotion's clips (docs/routing-mac.md §6.1): a standing Sim starts
// with the turnAndGo of its first leg's 45-degree steps (mirrored the other
// way round), its frame covering the clip's distance in the clip's length
// while it stands where the clip's root is; then the gait's walk cycle at its
// speed, and the turnAndStop at the end over what is left, which leaves it
// facing the final way.
static StringTable strings(std::initializer_list<const char*> values) {
    StringTable t;
    for (const char* v : values)
        t.entries.push_back({lang::EnglishUS, v, "", 0});
    return t;
}

// `extra`: also the walkShort cycle (half the walk's speed and stride) and
// the long "step" clips (0.6 over 1.2 s).
static void addLocoSet(CodeLibrary& code, VM& vm, bool extra = false) {
    code.add(kGlobalGroup, 0x84,
             strings({extra ? "turnInPlace-alt(turnInPlace),turnAndGoShort-alt(turnAndGoShort),turnAndGo-alt(turnAndGo),"
                              "turnAndStopShort-alt(turnAndStopShort),turnAndStop-alt(turnAndStop),stepShort-alt(stepShort),"
                              "step-alt(step)"
                            : "turnInPlace-alt(turnInPlace),turnAndGoShort-alt(turnAndGoShort),turnAndGo-alt(turnAndGo),"
                              "turnAndStopShort-alt(turnAndStopShort),turnAndStop-alt(turnAndStop),stepShort-alt(stepShort)",
                      extra ? "stepInPlace-alt(stepInPlace),walkShort-alt0-normal(walkShort),walk-alt0-normal(walk)"
                            : "stepInPlace-alt(stepInPlace),walk-alt0-normal(walk)",
                      "",
                      "turnAndGo-alt@2(turnAndGo@2),turnAndStop-alt@2(turnAndStop@2)", "run-alt0-normal(run)", ""}));
    // Clips at 0-180 degrees: a start 0.75 s over 0.85 tile whose root lags
    // 0.1 behind its frame and turns from where the Sim faced to the way;
    // a stop 0.5 s over 0.34 turning to the final facing; a step 0.6 s.
    for (int k = 0; k <= 4; ++k) {
        const float turn = -float(k) * float(M_PI / 4);
        const std::string deg = std::to_string(45 * k);
        auto clip = [&](const std::string& name, int ms, float distance, float yaw0, float yaw1, float lag) {
            LocoClip c;
            c.ticks = ms, c.distance = distance, c.velocity = distance / (float(ms) / 1000), c.turn = turn;
            c.cycle = 0.5f, c.type = 6;
            const int n = int(std::ceil(float(ms) / 1000 * 30));
            for (int i = 0; i <= n; ++i) {
                const float u = float(i) / float(n);
                c.root.push_back({0, lag * (1 - u), yaw0 + (yaw1 - yaw0) * u});
            }
            vm.animations.add(name, float(ms) / 1000, c.velocity);
            vm.animations.addLoco(name, c);
        };
        clip("a-turnAndGo-alt-" + deg, 750, 0.85f, -turn, 0, -0.1f);
        clip("a-turnAndGoShort-alt-" + deg, 750, 0.35f, -turn, 0, 0);
        clip("a-turnAndStop-alt-" + deg, 500, 0.34f, 0, turn, 0);
        clip("a-turnAndStopShort-alt-" + deg, 500, 0.16f, 0, turn, 0);
        clip("a-stepShort-" + deg, 600, 0.3f, -turn, -turn, 0);
        if (extra)
            clip("a-step-" + deg, 1200, 0.6f, -turn, -turn, 0);
    }
    if (extra) {
        LocoClip still;
        still.ticks = 600, still.type = 10;
        still.root.assign(19, {0, 0, 0});
        vm.animations.add("a-stepInPlace", 0.6f, 0);
        vm.animations.addLoco("a-stepInPlace", still);
        LocoClip shorter;
        shorter.ticks = 690, shorter.distance = 0.69f, shorter.velocity = 1.0f, shorter.type = 11;
        shorter.root.assign(22, {0, 0, 0});
        vm.animations.add("a-male-walkShort-alt0-normal", 0.69f, 1.0f);
        vm.animations.addLoco("a-male-walkShort-alt0-normal", shorter);
    }
    LocoClip walk;
    walk.ticks = 690, walk.distance = 1.38f, walk.velocity = 2.0f, walk.type = 11;
    walk.root.assign(22, {0, 0, 0});
    vm.animations.add("a-male-walk-alt0-normal", 0.69f, 2.0f);
    vm.animations.addLoco("a-male-walk-alt0-normal", walk);
    vm.animations.add("a-male-run-alt0-normal", 0.5f, 8.0f);
}

// ComputeLocoStartTicks (docs/anim-mac.md §13.6): a route begun within 140
// ms of the last blocking primitive's action end (an Animate Sim blending
// out) starts its first phase 70 ms after it; within 140 ms before that
// request's end (else), at its end less 70; the next route after a route's
// hand-off waits for the stop clip still playing. The ends are used up.
static void testLocoStartWindow() {
    using Loco = Object::Route::Loco;
    for (int c = 0; c < 3; ++c) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(14, 12);
        addLocoSet(code, vm);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        vm.setPosition(lamp, {11.5f, 5.5f, 1});
        Object& sim = vm.instantiate(kGroup, 0x81);
        vm.setPosition(sim, {2.5f, 5.5f, 1});
        sim.person->data[58] = 27;
        sim.data[objdata::Direction] = 4;
        vm.ticks = 300;
        const double now = animationClockMs(vm);
        AnimationState& a = sim.animation;
        float want = -1;
        if (c == 0) { // the action ended 10 ms ago: starts 60 ms from now
            a.previousActionEndMs = now - 10, a.previousEndMs = now + 223.3;
            want = -1 - 60.0f * 30 / 1000;
        } else if (c == 1) { // 200 ms ago, its end 33.3 ms ago: begun 36.7 ms ago (e - 70)
            a.previousActionEndMs = now - 200, a.previousEndMs = now + 33.3;
            want = -1 - (33.3f - 70) * 30 / 1000;
        } else { // a route's stop plays on 150 ms more
            a.previousActionEndMs = now - 83.3, a.previousEndMs = now + 150, a.locoEndingUntilMs = now + 150;
            want = -1 - 150.0f * 30 / 1000;
        }
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        float lead = 0;
        for (int i = 0; i < 1000 && t.status == Thread::Status::Running && lead == 0; ++i) {
            vm.run(t);
            if (sim.route && sim.route->loco.kind == Loco::Start)
                lead = sim.route->loco.lead;
        }
        check(std::fabs(lead - want) < 0.01f, "loco start window: the first phase's start");
        check(a.previousEndMs == AnimationState::kNoTicks && a.previousActionEndMs == AnimationState::kNoTicks,
              "loco start window: the last primitive's ends used up");
    }
}

static void testLocoPhases() {
    using Loco = Object::Route::Loco;
    // Facing +X (direction 4, the way), +Y (2: 90 degrees clockwise to the
    // way) and -Y (6: 90 degrees counter-clockwise).
    for (const int facing : {4, 2, 6}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(14, 12);
        addLocoSet(code, vm);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        vm.setPosition(lamp, {11.5f, 5.5f, 1});
        Object& sim = vm.instantiate(kGroup, 0x81);
        vm.setPosition(sim, {2.5f, 5.5f, 1});
        sim.person->data[58] = 27;
        sim.data[objdata::Direction] = int16_t(facing);
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
        Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
        std::vector<std::string> clips;
        std::string firstClip;
        bool firstMirror = false;
        int startTicks = 0, stopTicks = 0;
        float lagMid = 0, walkStep = 0, startStep = 0;
        float prevFx = sim.position.x;
        int lastDir = -1;
        for (int i = 0; i < 1000 && t.status == Thread::Status::Running; ++i) {
            vm.run(t);
            if (!sim.route)
                continue;
            const Loco& L = sim.route->loco;
            if (L.animation.empty() || L.kind == Loco::None)
                continue;
            if (clips.empty() || clips.back() != L.animation)
                clips.push_back(L.animation);
            if (L.kind == Loco::Start) {
                if (firstClip.empty())
                    firstClip = L.animation, firstMirror = L.mirror;
                ++startTicks;
                if (L.tick == L.ticks / 2) {
                    lagMid = std::hypot(sim.position.x - L.fx, sim.position.y - L.fy);
                    startStep = L.fx - prevFx;
                }
            } else if (L.kind == Loco::Walk && L.fx - prevFx > 0.01f) {
                walkStep = L.fx - prevFx;
            } else if (L.kind == Loco::Stop) {
                ++stopTicks;
            }
            prevFx = L.fx;
            lastDir = sim.data[objdata::Direction];
        }
        if (std::getenv("DEBUG_LOCO")) {
            std::fprintf(stderr, "facing %d: start %s%s (%d ticks, lag %.3f, step %.4f) walk step %.4f stop %d ticks, at %.2f,%.2f dir %d result %d clips:",
                         facing, firstClip.c_str(), firstMirror ? " mirrored" : "", startTicks, lagMid, startStep, walkStep,
                         stopTicks, sim.position.x, sim.position.y, sim.data[objdata::Direction], t.result);
            for (const auto& c : clips)
                std::fprintf(stderr, " %s", c.c_str());
            std::fprintf(stderr, "\n");
        }
        const std::string want = facing == 4 ? "a-turnAndGo-alt-0" : "a-turnAndGo-alt-90";
        check(t.status == Thread::Status::Finished && t.result, "loco: the walk ends there");
        check(firstClip == want && firstMirror == (facing == 6), "loco: the start turns by the first leg's steps, mirrored counter-clockwise");
        // (Started now, standing at its start the first tick:
        // ComputeLocoStartTicks and FollowOneStep; the 5/100 ms are the next
        // request's head start, docs/anim-mac.md §8.2-8.3.)
        check(startTicks >= 23 && startTicks <= 24, "loco: the start lasts its clip (0.75 s) after a tick at its start");
        check(std::fabs(lagMid - 0.05f) < 0.02f, "loco: in the start the Sim stands at the clip's root, behind its frame");
        check(std::fabs(startStep - 0.85f / 23) < 0.01f || facing != 4, "loco: the start's frame covers its distance in its length");
        check(std::fabs(walkStep - 2.0f / 30) < 0.002f, "loco: the walk cycle at the gait's speed");
        // (The walk is done as the stop starts to fade out, 233.3 ms before
        // its 0.5 s end: FollowOneStep's hand-off.)
        check(clips.size() >= 3 && clips[clips.size() - 2] == "a-male-walk-alt0-normal" &&
                  clips.back().starts_with("a-turnAndStop-alt-") && stopTicks >= 7 && stopTicks <= 9,
              "loco: start, walk, stop, done as the stop fades out");
        check(std::fabs(sim.position.x - 10.5f) < 0.05f && lastDir == 4, "loco: stopped in front of the lamp, facing it");
    }
}

// The start and stop clips take the 45-degree clip nearest the angle (no
// blend of two angles: that is CreateStepCycle's, for sidesteps), the walk
// follows the route's B-spline (its heading changes little from tick to
// tick), and the stop leaves the Sim at the final facing.
static void testLocoBlend() {
    using Loco = Object::Route::Loco;
    CodeLibrary code;
    VM vm(code);
    vm.world = World(14, 12);
    addLocoSet(code, vm);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {11.5f, 8.5f, 1});
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {2.5f, 5.5f, 1});
    sim.person->data[58] = 27;
    sim.data[objdata::Direction] = 4;
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    bool oneAngle = true, sawStart = false, sawStop = false;
    float maxTurn = 0, lastHeading = 0;
    bool walking = false;
    for (int i = 0; i < 1000 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        if (!sim.route)
            continue;
        const Loco& L = sim.route->loco;
        if (L.kind == Loco::Start || L.kind == Loco::Stop) {
            (L.kind == Loco::Start ? sawStart : sawStop) = true;
            for (const auto& l : L.layers)
                oneAngle = oneAngle && l.animation.substr(l.animation.rfind('-')) ==
                                           L.layers.front().animation.substr(L.layers.front().animation.rfind('-'));
        }
        if (L.kind == Loco::Walk) {
            if (walking)
                maxTurn = std::max(maxTurn, std::fabs(std::remainder(L.heading - lastHeading, float(2 * M_PI))));
            walking = true;
            lastHeading = L.heading;
        } else {
            walking = false;
        }
    }
    if (std::getenv("DEBUG_LOCO"))
        std::fprintf(stderr, "blend: one angle %d max turn %.3f at %.2f,%.2f dir %d\n", oneAngle, maxTurn, sim.position.x,
                     sim.position.y, sim.data[objdata::Direction]);
    check(t.status == Thread::Status::Finished && t.result, "loco blend: the walk ends there");
    check(sawStart && sawStop && oneAngle, "loco: a start or a stop takes the 45-degree clip nearest the angle, no angle blend");
    check(maxTurn < 0.5f, "loco blend: the walk's frame follows a curve (no turn of 30 degrees in a tick)");
}

// The frame's curve: a uniform cubic B-spline with the ends tripled
// (cPathStrategyCubic::UpdatePathData): it starts and ends at the ends, cuts
// a corner, and each segment is the matrix's blend of four control points.
static void testLocoCurve() {
    const LocoCurve c(0, 0, {{1, 0}, {1, 1}});
    const auto a = c.at(0, 0), z = c.at(c.segments() - 1, 1), s1 = c.at(1, 0);
    float nearest = 9;
    for (size_t j = 0; j < c.segments(); ++j)
        for (int k = 0; k <= 20; ++k) {
            const auto p = c.at(j, float(k) / 20);
            nearest = std::min(nearest, std::hypot(p[0] - 1, p[1]));
        }
    check(c.segments() == 4 && std::hypot(a[0], a[1]) < 1e-6f && std::hypot(z[0] - 1, z[1] - 1) < 1e-6f,
          "loco curve: starts and ends at the route's ends (tripled)");
    check(std::hypot(s1[0] - 1.0f / 6, s1[1]) < 1e-6f, "loco curve: a segment is 1/6 (P0 + 4 P1 + P2) at its start");
    check(nearest > 0.1f && c.total() > std::sqrt(2.0f) && c.total() < 2, "loco curve: it cuts the corner");
    check(c.passedAfter(0) == 0 && c.passedAfter(1) == 1 && c.passedAfter(2) == 1 && c.passedAfter(3) == 2,
          "loco curve: the route's points are passed as the segments ending near them are done");
}

// walk and walkShort blend by stride (InitChannelNetwork): the walk between
// the start and the stop is a whole number of half cycles, the start and the
// stop blended with their short clips by the same weight.
// The "*Blend" poses' blend (InitChannelNetwork's +0xec): 0.2, at most
// 0.95 of the start's and the stop's distances, in seconds at the speed.
static void testLocoPoseBlend() {
    check(std::fabs(locoPoseBlend(0, 0, 2) - 0.1f) < 1e-5f, "pose blend: 0.2 at 2 per second");
    check(std::fabs(locoPoseBlend(0.1f, 0, 1) - 0.095f) < 1e-5f, "pose blend: 0.95 of a short start");
    check(std::fabs(locoPoseBlend(1, 0.15f, 1) - 0.1425f) < 1e-5f, "pose blend: 0.95 of a short stop");
}

// fitStride as InitChannelNetwork fits a way (walk 1.393 / walkShort 0.683
// a cycle, turnAndGo 0.853 / Short 0.347, turnAndStop 0.337 / 0.16).
static void testFitStride() {
    std::function<std::pair<float, float>(bool)> stop = [](bool) { return std::pair<float, float>{0.337f, 0.16f}; };
    // 5 tiles: 3.81 left, 6 half cycles, w = (5.369 - 5) / (5.369 - 2.556).
    StrideFit f = fitStride(5, 1.393f, 0.683f, 0.853f, 0.347f, &stop, false, 0, 0);
    check(std::fabs(f.cycles - 3) < 1e-4f && std::fabs(f.weight - 0.369f / 2.813f) < 2e-3f &&
              std::fabs(f.stopWeight - f.weight) < 1e-5f,
          "fit stride: whole half cycles, one short weight for start, cycles and stop");
    // 1.2 tiles: one half cycle would want w = 0.66 > 0.3: half a cycle less,
    // the last quarter and the stop at v = 1, w from the rest.
    f = fitStride(1.2f, 1.393f, 0.683f, 0.853f, 0.347f, &stop, false, 0, 0);
    check(std::fabs(f.cycles) < 1e-4f && std::fabs(f.stopWeight - 1) < 1e-5f && std::fabs(f.weight - 0.4857f) < 2e-3f,
          "fit stride: over 0.3, half a cycle less and the stop short");
    const float startD = f.weight * 0.347f + (1 - f.weight) * 0.853f;
    const float stride = f.weight * 0.683f + (1 - f.weight) * 1.393f;
    check(std::fabs(startD + 0.25f * stride + 0.25f * 0.683f + 0.16f - 1.2f) < 2e-3f, "fit stride: the rest covers the way");
    // Walking on (no start): what is left of the half cycle counts first.
    f = fitStride(3, 1.393f, 0.683f, 0, 0, &stop, false, 0.3f, 0);
    check(std::fabs(f.cycles - (0.3f + 0.5f * 4)) < 1e-4f, "fit stride: f0 + half cycles");
    // A short way (0.9 <= 0.9 x (0.853 + 0.337)): no cycles, one short
    // weight (1.19 - 0.9) / (1.19 - 0.507) for the start and the stop.
    f = fitStride(0.9f, 1.393f, 0.683f, 0.853f, 0.347f, &stop, false, 0, 0);
    const float ws = 0.29f / 0.683f;
    check(f.shortWay && f.cycles == 0 && std::fabs(f.weight - ws) < 1e-3f && std::fabs(f.stopWeight - ws) < 1e-3f &&
              std::fabs(f.startDistance - (ws * 0.347f + (1 - ws) * 0.853f)) < 1e-3f &&
              std::fabs(f.startDistance + f.stopDistance - 0.9f) < 1e-4f,
          "fit stride: a short way blends start and stop by its length, no cycles");
    // Under the short clips' 0.507: squeezed, the start 0.4 x 0.347 / 0.507.
    f = fitStride(0.4f, 1.393f, 0.683f, 0.853f, 0.347f, &stop, false, 0, 0);
    check(f.shortWay && f.weight == 1 && std::fabs(f.startDistance - 0.4f * 0.347f / 0.507f) < 1e-4f &&
              std::fabs(f.stopDistance - (0.4f - f.startDistance)) < 1e-5f,
          "fit stride: under the short clips the two are squeezed");
    // The stop's first foot by the request's cycle portion (getter 0x178).
    check(!stopFootOther(0.5f) && !stopFootOther(0.2f) && stopFootOther(0.7f) && stopFootOther(0),
          "fit stride: the normal stop for a portion in (0, 0.5], else the R one");
}

static void testLocoStride() {
    using Loco = Object::Route::Loco;
    CodeLibrary code;
    VM vm(code);
    vm.world = World(14, 12);
    addLocoSet(code, vm, true);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {6.5f, 5.5f, 1});
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {2.5f, 5.5f, 1});
    sim.person->data[58] = 27;
    sim.data[objdata::Direction] = 4;
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    float startShort = -1, walkShort = -1, weight = -1;
    int walkTicks = 0;
    float walked = 0;
    for (int i = 0; i < 1000 && t.status == Thread::Status::Running; ++i) {
        const float fx = sim.route ? sim.route->loco.fx : sim.position.x;
        vm.run(t);
        if (!sim.route)
            continue;
        const Loco& L = sim.route->loco;
        weight = L.strideWeight;
        for (const auto& l : L.layers) {
            if (L.kind == Loco::Start && l.animation.find("turnAndGoShort") != std::string::npos)
                startShort = l.weight;
            if (L.kind == Loco::Walk && l.animation.find("walkShort") != std::string::npos)
                walkShort = l.weight;
        }
        if (L.kind == Loco::Walk) {
            ++walkTicks;
            walked += L.fx - fx;
        }
    }
    // 3 tiles: start 0.85 + stop 0.34 long, 0.35 + 0.16 short; strides 1.38
    // and 0.69: 1.81 left, 3 half cycles, w = 0.26 / 1.715.
    const float want = 0.26f / 1.715f;
    const float halfCycles = walked / (0.5f * ((1 - weight) * 1.38f + weight * 0.69f));
    if (std::getenv("DEBUG_LOCO"))
        std::fprintf(stderr, "stride: weight %.3f start short %.3f walk short %.3f walk %d ticks %.3f tiles = %.2f half cycles at %.2f\n",
                     weight, startShort, walkShort, walkTicks, walked, halfCycles, sim.position.x);
    check(t.status == Thread::Status::Finished && t.result, "loco stride: the walk ends there");
    check(std::fabs(weight - want) < 0.02f && std::fabs(startShort - weight) < 1e-3f && std::fabs(walkShort - weight) < 1e-3f,
          "loco stride: walk and walkShort, turnAndGo and turnAndGoShort blend by the same stride weight");
    check(std::fabs(halfCycles - 3) < 0.25f, "loco stride: the walk covers a whole number of half cycles");
}

// A straight way to the facing the Sim already has is a sidestep
// (CreateStepCycle): the "step" clips blended by distance, played 1.4 times
// as fast, the body keeping its facing.
static void testSidestep() {
    using Loco = Object::Route::Loco;
    CodeLibrary code;
    VM vm(code);
    vm.world = World(14, 12);
    addLocoSet(code, vm, true);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {3.5f, 3.5f, 1});
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {2.5f, 4.0f, 1});
    sim.person->data[58] = 27;
    sim.data[objdata::Direction] = 4; // facing +X, the lamp's west side's facing
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(sim.id, 0x1000, {}, lamp.id);
    bool sidestep = false, kept = true, walked = false;
    int ticks = 0;
    std::string clips;
    for (int i = 0; i < 400 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        if (!sim.route)
            continue;
        const Loco& L = sim.route->loco;
        if (L.sidestep && L.kind == Loco::Start) {
            sidestep = true;
            ++ticks;
            kept = kept && sim.data[objdata::Direction] == 4;
            if (clips.empty())
                for (const auto& l : L.layers)
                    clips += l.animation + "*" + std::to_string(l.weight).substr(0, 4) + " ";
        }
        walked = walked || L.kind == Loco::Walk;
    }
    if (std::getenv("DEBUG_LOCO"))
        std::fprintf(stderr, "sidestep: %d (%d ticks, %s) kept %d walked %d at %.2f,%.2f dir %d result %d\n", sidestep, ticks,
                     clips.c_str(), kept, walked, sim.position.x, sim.position.y, sim.data[objdata::Direction], t.result);
    check(t.status == Thread::Status::Finished && t.result && std::fabs(sim.position.y - 3.5f) < 0.05f,
          "sidestep: the walk ends there");
    // 0.5 between stepShort (0.3) and step (0.6): 1/3 short, 2/3 long, at
    // (0.6/1.2 * 2/3 + 0.3/0.6 * 1/3) * 1.4 = 0.7 m/s: 0.71 s.
    check(sidestep && kept && !walked && ticks >= 17 && ticks <= 23 && clips.find("a-step-90") != std::string::npos,
          "sidestep: steps sideways without turning (step/stepShort by distance, 1.4 times as fast)");
}

// A walker that comes on a Sim standing in its way slows to a stop with a
// stop clip (StopLocomotion) before the blocked handling starts; an object
// put in its way (the per-step test against objects) fails the route with
// "blocked by object" (15).
static void testLocoHalt() {
    using Loco = Object::Route::Loco;
    for (const bool object : {false, true}) {
        CodeLibrary code;
        VM vm(code);
        vm.world = World(16, 5);
        for (int x = 0; x < 16; ++x) {
            vm.world->addWall(1, x, 1, x + 1, 1);
            vm.world->addWall(1, x, 2, x + 1, 2);
        }
        addLocoSet(code, vm);
        code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
        code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
        Object& lamp = vm.instantiate(kGroup, 0x80);
        vm.setPosition(lamp, {15.5f, 1.5f, 1});
        Object& walker = vm.instantiate(kGroup, 0x81);
        vm.setPosition(walker, {1.5f, 1.5f, 1});
        walker.person->data[58] = 27;
        walker.data[objdata::Direction] = 4;
        if (!object) { // two Sims across the corridor (one alone is passed)
            Object& other = vm.instantiate(kGroup, 0x81);
            vm.setPosition(other, {8.5f, 1.6f, 1});
            other.person->data[58] = 27;
            Object& wallSim = vm.instantiate(kGroup, 0x81);
            vm.setPosition(wallSim, {8.5f, 1.15f, 1});
            wallSim.person->data[58] = 27;
        }
        code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
        Thread t = vm.start(walker.id, 0x1000, {}, lamp.id);
        int haltTicks = 0, halts = 0, firstHalt = 0;
        bool wasHalting = false;
        float closest = 99;
        for (int i = 0; i < 600 && t.status == Thread::Status::Running; ++i) {
            if (object && i == 40) { // a box dropped two tiles ahead
                Object& box = vm.instantiate(kGroup, 0x80);
                vm.setPosition(box, {std::floor(walker.position.x) + 2.5f, 1.5f, 1});
            }
            vm.run(t);
            if (!walker.route)
                continue;
            const bool halting = walker.route->loco.halting && walker.route->loco.kind == Loco::Stop;
            if (halting) {
                ++haltTicks;
                if (!wasHalting)
                    ++halts;
                if (halts == 1)
                    ++firstHalt;
            }
            wasHalting = halting;
            if (!object && vm.object(3))
                closest = std::min(closest, std::hypot(walker.position.x - vm.object(3)->position.x,
                                                       walker.position.y - vm.object(3)->position.y));
        }
        if (std::getenv("DEBUG_LOCO"))
            std::fprintf(stderr, "halt (%s): %d halts, first %d ticks (%d in all), closest %.3f, result %d route result %d at %.2f\n",
                         object ? "object" : "Sim", halts, firstHalt, haltTicks, closest, t.result, walker.person->data[62],
                         walker.position.x);
        if (object) {
            // The per-step test: LocoPlaceFromSceneGraph fails -> EndFollow(1),
            // blocked at once with no clip.
            check(halts == 0, "loco halt: the next step taken blocks at once, no stop clip");
            check(t.status == Thread::Status::Finished && !t.result && walker.person->data[62] == 15,
                  "loco halt: an object put in the way fails the route (blocked by object)");
        } else {
            // CollisionCheck: the first collision in the horizon (half a tile
            // of travel, the point 0.35 ahead): the stop (the half cycle and
            // the clip) does not end 2 steps before it, so the walk ends at
            // once (EndFollow), well short of the Sims (round n18).
            check(halts == 0 && closest >= 0.9f, "loco halt: a walker blocked ahead stops at once, short of the Sims");
            check(t.status == Thread::Status::Finished && !t.result && walker.person->data[62] == 14,
                  "loco halt: and fails blocked by person");
        }
    }
}

// A runner (steps longer than the predicted blocking looks ahead) does not
// step into a Sim standing in its way: the locomotor's placement test refuses
// the step and the Sim becomes the blocker.
static void testRunnerStep() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(16, 5);
    for (int x = 0; x < 16; ++x) {
        vm.world->addWall(1, x, 1, x + 1, 1);
        vm.world->addWall(1, x, 2, x + 1, 2);
    }
    vm.walkTilesPerTick = 0.45f; // longer steps than the prediction looks ahead
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {15.5f, 1.5f, 1});
    Object& runner = vm.instantiate(kGroup, 0x81);
    vm.setPosition(runner, {1.5f, 1.5f, 1});
    runner.person->data[58] = 27;
    Object& other = vm.instantiate(kGroup, 0x81); // two across the corridor (one alone is passed)
    vm.setPosition(other, {6.5f, 1.6f, 1});
    other.person->data[58] = 27;
    Object& wallSim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(wallSim, {6.5f, 1.15f, 1});
    wallSim.person->data[58] = 27;
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    Thread t = vm.start(runner.id, 0x1000, {}, lamp.id);
    float closest = 99;
    bool blocked = false;
    for (int i = 0; i < 400 && t.status == Thread::Status::Running; ++i) {
        vm.run(t);
        closest = std::min(closest, std::hypot(runner.position.x - other.position.x, runner.position.y - other.position.y));
        blocked = blocked || (runner.route && runner.route->waiting);
    }
    if (std::getenv("DEBUG_LOCO"))
        std::fprintf(stderr, "runner: closest %.3f blocked %d result %d at %.2f\n", closest, blocked, t.result, runner.position.x);
    (void)blocked;
    check(t.status == Thread::Status::Finished && !t.result && runner.person->data[62] == 14 && closest >= 0.283f,
          "a runner is blocked (blocked by person) before it steps into a Sim in its way");
}

// Two Sims walking at each other down a corridor with a bay block each
// other: the binary's DoBlockedGoalStillAvailable sees the blocker blocked
// by the walker and the walker steps aside itself (TryGetSelfOutOfWay), so
// both get through at once instead of asking and waiting.
// The predicted blocking (CollisionCheck) looks where the walker's frame
// will really go: the cubic path it follows (GlobalLocationEstimate), which
// cuts inside the route's corners. A Sim standing on the inside of a corner,
// 0.29 or more off the straight legs but under 0.283 from the curve, is
// predicted: the walker stops for it instead of stepping within 0.283 of it
// unpredicted (the per-step test), which left two Sims 0.285 apart blocking
// each other for good at N001_Lot11's stair top (round n18, OS2_SEED=2).
static void testPredictionOnCurve() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(8, 6);
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& a = vm.instantiate(kGroup, 0x81);
    Object& b = vm.instantiate(kGroup, 0x81);
    vm.setPosition(a, {1.8f, 1.5f, 1});
    Object::Route r;
    std::vector<std::array<float, 2>> through;
    auto add = [&](float x, float y) {
        Object::Route::Point p;
        p.x = x, p.y = y, p.level = 1;
        r.points.push_back(p);
        through.push_back({x, y});
    };
    add(2.0f, 1.5f); // east, then north round the corner at (2, 1.5)
    add(2.0f, 3.0f);
    a.route = r;
    const LocoCurve curve(a.position.x, a.position.y, through);
    auto curveDistance = [&](float px, float py) {
        float d = 1e9f;
        for (size_t j = 0; j < curve.segments(); ++j)
            for (int k = 0; k <= 32; ++k) {
                const auto q = curve.at(j, float(k) / 32);
                d = std::min(d, std::hypot(q[0] - px, q[1] - py));
            }
        return d;
    };
    // On the corner's inner diagonal: off both legs by t.
    float t = 0.29f;
    for (; t < 0.40f; t += 0.005f)
        if (curveDistance(2.0f - t, 1.5f + t) < 0.27f)
            break;
    vm.setPosition(b, {2.0f - t, 1.5f + t, 1});
    check(t < 0.40f && t >= 0.29f, "prediction on the curve: a place off the legs by 0.29 or more and under 0.283 from the curve");
    check(routing_internal::simAhead(vm, a, *a.route).id == b.id,
          "the predicted blocking follows the frame's curve: the Sim inside the corner is seen");
}

static void testMutualBlock() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(14, 5);
    for (int x = 0; x < 14; ++x) {
        vm.world->addWall(1, x, 1, x + 1, 1);
        if (x < 6 || x > 8)
            vm.world->addWall(1, x, 2, x + 1, 2);
    }
    vm.world->addWall(1, 6, 2, 6, 4); // a bay three tiles wide, two deep
    vm.world->addWall(1, 9, 2, 9, 4);
    vm.world->addWall(1, 6, 4, 9, 4);
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& east = vm.instantiate(kGroup, 0x80);
    vm.setPosition(east, {13.5f, 1.5f, 1});
    Object& west = vm.instantiate(kGroup, 0x80);
    vm.setPosition(west, {0.5f, 1.5f, 1});
    Object& a = vm.instantiate(kGroup, 0x81);
    vm.setPosition(a, {2.5f, 1.5f, 1});
    Object& b = vm.instantiate(kGroup, 0x81);
    vm.setPosition(b, {11.5f, 1.5f, 1});
    code.add(kGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0, 0xFE, 0, 0, 0})}));
    code.add(kGroup, 0x1001, tree({instr(0x1B, T, F, {0, 0, 4, 0xFE, 0, 0, 0})}));
    // The pause (DoPause): "CT - Route Blocked Animation", here a 30-tick wait.
    Behavior pauseTree = tree({expr(0x09, 2, Assign, Lit, 30, go(1), E), instr(0x00, T, E, {2, 0})}, 4);
    pauseTree.fileName = "CT - Route Blocked Animation";
    code.add(kGlobalGroup, 0x1BB, pauseTree);
    vm.threads.insert_or_assign(a.id, vm.start(a.id, 0x1000, {}, east.id));
    vm.threads.insert_or_assign(b.id, vm.start(b.id, 0x1001, {}, west.id));
    int ticks = 0;
    for (; ticks < 1500; ++ticks) {
        vm.tick();
        if (vm.threads.at(a.id).status != Thread::Status::Running && vm.threads.at(b.id).status != Thread::Status::Running)
            break;
    }
    if (std::getenv("DEBUG_BLOCK"))
        std::fprintf(stderr, "mutual: %d ticks, a %d/%d at %.2f,%.2f b %d/%d at %.2f,%.2f\n", ticks,
                     int(vm.threads.at(a.id).status), vm.threads.at(a.id).result, a.position.x, a.position.y,
                     int(vm.threads.at(b.id).status), vm.threads.at(b.id).result, b.position.x, b.position.y);
    check(vm.threads.at(a.id).result && vm.threads.at(b.id).result && a.position.x > 11 && b.position.x < 3 && ticks < 335,
          "two Sims blocking each other in a corridor: one steps aside, both get through");
}

// Go Here: a click on the ground makes the destination there, moved 0.45
// tile off the wall of its tile and out of what blocks; it is kept while
// the pie menu is open and while its interaction is queued, and goes once
// the Sim has walked there.
static void testGoHere() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(12, 8);
    vm.world->addWall(1, 3, 0, 3, 8); // between x = 2 and x = 3
    constexpr uint32_t kDestGroup = 0x7F000003;
    code.add(kGroup, 0x80, definition(0xAAAA0001, 0));
    ObjectDefinition simDef = definition(0xAAAA0002, 0, true);
    code.add(kGroup, 0x81, simDef);
    ObjectFunctions mainOnly;
    mainOnly.functions.resize(2);
    mainOnly.functions[1].action = 0x1002;
    code.add(kGroup, 0x81, mainOnly);
    code.add(kGroup, 0x1002, tree({expr(0x09, 0, Assign, Lit, 1, go(1), E), instr(0x11, go(0), E, {0, 0, 1, 0})}, 1));
    ObjectDefinition destDef = definition(kGoHereGuid, 0);
    destDef.fields[(0x4E - 0x44) / 2] = 1; // its TTAB
    code.add(kDestGroup, 0x80, destDef);
    InteractionTable ttab;
    ttab.version = 0x4D;
    InteractionTable::Interaction e;
    e.action = 0x1000;
    ttab.interactions = {e};
    code.add(kDestGroup, 1, ttab);
    // "Go Here": to the destination's spot (Go To Relative Position, on it).
    code.add(kDestGroup, 0x1000, tree({instr(0x1B, T, F, {0, 0, 0xFF, 0xFE, 0, 0, 0x02})}));
    ObjectFunctions destInit;
    destInit.functions.resize(1);
    destInit.functions[0].action = 0x1001;
    code.add(kDestGroup, 0x80, destInit);
    code.add(kDestGroup, 0x1001, tree({expr(Data, 8, 9, Lit, 5, T, E)}));
    Object& lamp = vm.instantiate(kGroup, 0x80);
    vm.setPosition(lamp, {8.5f, 4.5f, 1});
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {6.5f, 1.5f, 1});
    sim.person->data[33] = 0;
    vm.startMain(sim.id);
    const size_t before = vm.objects().size();
    // Next to the wall: 0.45 off it.
    const auto p = goHerePoint(vm, sim.id, 3.1f, 2.5f, 1);
    check(p && std::fabs(p->x - 3.451f) < 1e-3f && std::fabs(p->y - 2.5f) < 1e-4f, "go here: 0.45 tile off the wall");
    // On the lamp: out of its tile.
    const auto q = goHerePoint(vm, sim.id, 8.5f, 4.5f, 1);
    check(q && (std::floor(q->x) != 8 || std::floor(q->y) != 4), "go here: out of what blocks");
    check(!goHerePoint(vm, sim.id, 13.0f, 2.0f, 1), "go here: not out of the lot");
    // Near the lamp's corner: pushed along the shorter way out only
    // (TSObjectMoveAwayFromOtherObjects: y by -0.5001, x left alone).
    const auto q2 = goHerePoint(vm, sim.id, 8.8f, 4.1f, 1);
    check(q2 && std::fabs(q2->x - 8.8f) < 1e-4f && std::fabs(q2->y - 3.5999f) < 1e-3f,
          "go here: out of a footprint along the shorter axis");
    // Next to the Sim: 0.4 off its 0.2-wide footprint.
    const auto q3 = goHerePoint(vm, sim.id, 6.7f, 1.5f, 1);
    check(q3 && std::fabs(q3->x - 7.0001f) < 1e-3f, "go here: 0.4 off the Sim's footprint");
    // The diagonal walls as the binary moves off them: along the wall.
    {
        World dw(8, 8);
        dw.addWall(1, 0, 4, 4, 0); // slope -1: fx + fy = 1 in tile (1, 2)
        dw.addWall(1, 4, 4, 8, 8); // slope +1: fx = fy in tile (5, 5)
        check(dw.diagonalSides({1, 2, 1}) == 0x10 && dw.diagonalSides({5, 5, 1}) == 0x20 &&
                  dw.diagonalSides({1, 1, 1}) == 0,
              "go here: diagonal wall sides");
        const Point2 a = movePointAwayFromWalls(dw, {1.5f, 2.6f}, 1, 0.45f);
        check(std::fabs(a[0] - 1.7689f) < 1e-3f && std::fabs(a[1] - 2.3311f) < 1e-3f,
              "go here: diagonal 0x10 as the binary has it");
        const Point2 b = movePointAwayFromWalls(dw, {5.5f, 5.6f}, 1, 0.45f);
        check(std::fabs((b[0] - 5.5f) - (b[1] - 5.6f)) < 1e-4f && b[0] > 5.7f, "go here: diagonal 0x20 along the wall");
    }
    // A rug (type 0x13): its menu merges a destination at the floor under
    // the mouse; without interactions the destination's alone; another
    // object only its own.
    {
        ObjectDefinition rugDef = definition(0xAAAA0003, 0);
        rugDef.fields[(0x52 - 0x44) / 2] = 0x13;
        code.add(kGroup, 0x82, rugDef);
        Object& rug = vm.instantiate(kGroup, 0x82);
        vm.setPosition(rug, {10.5f, 2.5f, 1});
        const ObjectId rugId = rug.id;
        const Position floor{10.2f, 2.7f, 1};
        check(isGoHereRug(vm, rugId) && !isGoHereRug(vm, lamp.id), "rug: type 0x13");
        auto m = objectClick(vm, sim.id, rugId, floor, true);
        check(m.object == rugId && m.destination && vm.object(m.destination)->guid() == kGoHereGuid,
              "rug: its menu with a go-here destination");
        releaseDestination(vm, m.destination);
        check(!vm.object(m.destination), "rug: the destination goes with the menu");
        m = objectClick(vm, sim.id, rugId, floor, false);
        check(m.object && m.object != rugId && !m.destination, "rug without interactions: the destination's menu");
        releaseDestination(vm, m.object);
        m = objectClick(vm, sim.id, rugId, std::nullopt, true);
        check(m.object == rugId && !m.destination, "rug off the floor: its menu alone");
        vm.object(rugId)->data[objdata::Category] = 0xA7;
        check(!isGoHereRug(vm, rugId), "rug: not with category 0xA7");
        m = objectClick(vm, sim.id, lamp.id, floor, true);
        check(m.object == lamp.id && !m.destination, "another object: its menu");
        vm.removeObject(rugId);
    }
    // Cancelled: the destination goes when the menu closes.
    ObjectId d = createGoHereDestination(vm, sim.id, 9.5f, 6.5f, 1);
    check(d && vm.objects().size() == before + 1, "go here: the destination is made");
    vm.tick();
    check(vm.object(d) != nullptr, "go here: kept while the pie menu is open");
    releaseDestination(vm, d);
    check(!vm.object(d), "go here: gone when the menu closes without a pick");
    // Picked: the Sim walks there and the destination goes after.
    d = createGoHereDestination(vm, sim.id, 9.5f, 6.5f, 1);
    auto in = makeInteraction(vm, d, 0);
    check(d && in.has_value(), "go here: its interaction");
    if (in) {
        in->priority = 50;
        enqueue(vm, sim, *in);
    }
    releaseDestination(vm, d);
    check(vm.object(d) != nullptr, "go here: kept while queued");
    for (int i = 0; i < 600 && vm.object(d); ++i)
        vm.tick();
    check(!vm.object(d) && std::hypot(sim.position.x - 9.5f, sim.position.y - 6.5f) < 1.5f,
          "go here: the Sim walks there, then the destination goes");
    // A click on a pool's water: the swim destination goes on the pool's
    // floor (a level down, where swimmers are), not on the water's level,
    // whose route failed with "altitudes don't match" (live QA 16).
    {
        CodeLibrary pc;
        VM pv(pc);
        pv.world = World(8, 8, 0, 2);
        for (int x = 2; x < 6; ++x)
            for (int y = 2; y < 6; ++y)
                pv.world->setPool({x, y, 1}), pv.world->setPool({x, y, 0});
        pc.add(kGroup, 0x81, simDef);
        ObjectDefinition swimDef = definition(kSwimHereGuid, 0);
        pc.add(kDestGroup, 0x80, swimDef);
        ObjectDefinition walkDef = definition(kGoHereGuid, 0);
        pc.add(kDestGroup, 0x81, walkDef);
        Object& swimmer = pv.instantiate(kGroup, 0x81);
        vm.setPosition(swimmer, {6.5f, 6.5f, 1});
        const ObjectId sd = createGoHereDestination(pv, swimmer.id, 3.5f, 3.5f, 1);
        check(sd && pv.object(sd)->guid() == kSwimHereGuid && pv.object(sd)->position.level == 0,
              "go here: a swim destination on the pool's floor");
        const ObjectId fd = createGoHereDestination(pv, swimmer.id, 3.5f, 3.5f, 0);
        check(fd && pv.object(fd)->position.level == 0, "go here: clicked on the floor, it stays there");
        const ObjectId ld = createGoHereDestination(pv, swimmer.id, 7.2f, 7.2f, 1);
        check(ld && pv.object(ld)->guid() == kGoHereGuid && pv.object(ld)->position.level == 1,
              "go here: the lawn keeps its level");
    }
}

// The reset's free place (FUN_00af87c8): the centre of the first tile of a
// square spiral from the object's tile where it fits: its own tile's centre
// when free (not the point it stood on), else the next of the spiral (+X
// first).
static void testPlaceNearFree() {
    CodeLibrary code;
    VM vm(code);
    vm.world = World(8, 8);
    Object& me = place(vm, 3, 3);
    Position at = me.position;
    at.x = 3.2f, at.y = 3.7f;
    vm.setPosition(me, at);
    me.data[objdata::Flags] = 0x10; // lets others through, so it is no obstacle itself
    check(vm.placeNearFree(me) && me.position.x == 3.5f && me.position.y == 3.5f, "free spot: the own tile's centre");
    place(vm, 3, 3); // something on that tile
    check(vm.placeNearFree(me) && me.position.x == 4.5f && me.position.y == 3.5f, "free spot: the spiral's next tile, +X");
}

// A walker standing in a diagonal wall's cells (stopped in a diagonal
// doorway) steps out of them before planning.
static void testEscapeDiagonal() {
    World w(8, 8);
    w.addWall(1, 1, 1, 5, 5); // "/" through tiles (1, 1) .. (4, 4)
    PathPlanner planner(w);
    PlannedPath p;
    RouteFail why{};
    check(planner.plan({3.4f, 3.5f}, 1, 1, {{6.5f, 1.5f, 0}}, p, why) && p.escaped, "planner: out of a diagonal wall's cells");
}

// Objects' portals (World::objectPortals, the pool ladder): a pool a level
// down (VM level 0, its bottom a floor) and the deck; a swimmer leaves by
// the portal, and a one-way portal (a slot whose partner does not name it
// back) is not crossed backwards.
static void testObjectPortals() {
    World w(10, 10, 0, 2);
    for (int x = 0; x < 10; ++x)
        for (int y = 0; y < 10; ++y)
            w.setWalkable({x, y, 0}, x >= 2 && x < 6 && y >= 2 && y < 8); // the pool's bottom
    World::ObjectPortal ladder;
    ladder.levelA = 0, ladder.levelB = 1;
    ladder.a = {5.25f, 3.5f}, ladder.b = {7.0f, 3.5f};
    w.setObjectPortals({ladder});
    {
        PathPlanner planner(w);
        PlannedPath p;
        RouteFail why;
        check(planner.portals().size() == 1 && planner.portals()[0].object == 0, "object portal: in the planner");
        check(planner.plan({3.0f, 6.0f}, 0, 1, {{8.5f, 8.5f, 0}}, p, why) && p.points.size() >= 2 &&
                  std::any_of(p.points.begin(), p.points.end(), [](const auto& q) { return q.portal == 0; }),
              "object portal: out of the pool by the ladder");
        check(planner.plan({8.5f, 8.5f}, 1, 0, {{3.0f, 6.0f, 0}}, p, why), "object portal: into the pool by the ladder");
    }
    ladder.oneWay = true; // the diving board, were its pool slot not to name the deck's
    w.setObjectPortals({ladder});
    {
        PathPlanner planner(w);
        PlannedPath p;
        RouteFail why;
        check(planner.plan({3.0f, 6.0f}, 0, 1, {{8.5f, 8.5f, 0}}, p, why), "object portal: one way, a to b");
        check(!planner.plan({8.5f, 8.5f}, 1, 0, {{3.0f, 6.0f, 0}}, p, why), "object portal: one way, not b to a");
    }
}

// The floors' routing costs (WorkOutTileCostAndPassability, round n18): a
// quarter tile's floor pattern's routingcost (held in [0.5, 10]), the
// terrain (no floor) and an unknown pattern the route manager's 2, a slope
// over half a tile raising it; the planner walks the cheaper floor.
static void testFloorCosts() {
    Lot lot;
    lot.width = 6, lot.height = 3;
    lot.minLevel = 0, lot.levelCount = 1;
    lot.elevations.assign(7 * 4, 0.0f);
    lot.floors.assign(18, {0, 0, 0, 0});
    lot.floorPatterns = {{1, "sidewalk"}, {2, "road_asphalt"}, {3, "cc13f31"}, {4, "nothing_known"}};
    auto at = [&](uint32_t x, uint32_t y) -> std::array<uint16_t, 4>& { return lot.floors[x * 3 + y]; };
    at(1, 0) = {1, 1, 1, 1};
    at(2, 0) = {2, 2, 2, 2};
    at(3, 0) = {3, 3, 0, 0}; // west and north quarters floored
    at(4, 0) = {4, 4, 4, 4};
    for (uint32_t x = 0; x < 6; ++x)
        at(x, 2) = {1, 1, 1, 1}; // a sidewalk along y = 2
    setFloorRoutingCosts({{"sidewalk", 1}, {"road_asphalt", 4}, {"cc13f31", 0}});
    World w = World::fromLot(lot);
    check(w.cost({0, 0, 1}) == 2 && w.cost({1, 0, 1}) == 1 && w.cost({2, 0, 1}) == 4 && w.cost({4, 0, 1}) == 2,
          "floor costs: terrain 2, sidewalk 1, road 4, unknown 2");
    check(w.cost({3, 0, 1}, 3.1f, 0.5f) == 0.5f && w.cost({3, 0, 1}, 3.9f, 0.5f) == 2,
          "floor costs: per quarter, a 0 held at 0.5, the bare quarter the terrain's");
    // From (0.5, 1.5) to (5.5, 1.5) over the grass (2) or along the sidewalk (1).
    PathPlanner planner(w);
    PlannedPath path;
    RouteFail why;
    check(planner.plan({0.5f, 1.5f}, 1, 1, {{5.5f, 1.5f, 0}}, path, why), "floor costs: a way");
    float maxY = 0;
    for (const auto& p : path.points) {
        maxY = std::max(maxY, p.y);
        if (std::getenv("DEBUG_FLOOR"))
            std::fprintf(stderr, "floor path (%.2f, %.2f) cost %.2f\n", p.x, p.y, planner.costAt(1, {p.x, p.y}));
    }
    check(maxY > 2.0f, "floor costs: the walker takes the sidewalk over the grass");
    setFloorRoutingCosts({});
    World plain = World::fromLot(lot);
    check(plain.cost({1, 0, 1}) == 1 && plain.cost({0, 0, 1}) == 2, "floor costs: without the catalog floors 1, terrain 2");
}

// GetFootprint snaps every rectangle to the 1/8 grid (round n49; BV
// FUN_0191fa14, base FUN_00b43bfc): a bound less than half a cell into a
// cell goes back to its line + 0.01, a side under 1/16 grows to 0.105. The
// Sim's 0.2 box too: a step a little into the cell beside a footprint
// (a start's turn next to a table's corner, N001_Lot68 seed 7 of round n31b)
// does not reach it, half a cell in it does.
static void testSnappedFootprints() {
    auto near = [](const std::array<float, 4>& a, std::array<float, 4> b) {
        for (size_t i = 0; i < 4; ++i)
            if (std::fabs(a[i] - b[i]) > 1e-4f)
                return false;
        return true;
    };
    check(near(snapFootprintRect({3.0f, 3.0f, 4.0f, 4.0f}), {3.01f, 3.01f, 3.99f, 3.99f}) &&
              near(snapFootprintRect({2.2f, 2.13f, 2.8f, 2.86f}), {2.26f, 2.135f, 2.74f, 2.86f}) &&
              near(snapFootprintRect({1.01f, 1.0f, 1.24f, 2.0f}), {1.01f, 1.01f, 1.24f, 1.99f}),
          "snapped footprint: bounds back to the 1/8 lines, mask rectangles unchanged");
    check(near(snapFootprintRect({5.02f, 5.0f, 5.05f, 5.5f}), {5.02f, 5.01f, 5.125f, 5.49f}),
          "snapped footprint: a side under 1/16 grows to 0.105");
    check(near(VM::personFootprintAt(nullptr, 4.105f, 3.5f), {4.01f, 3.4f, 4.205f, 3.6f}),
          "snapped footprint: a Sim's box");
    CodeLibrary code;
    VM vm(code);
    vm.world = World(8, 8);
    code.add(kGroup, 0x81, definition(0xAAAA0002, 0, true));
    Object& table = place(vm, 3, 3); // the OBJD box: [3.01, 3.99] once snapped
    check(near(vm.routingFootprint(table)[0], {3.01f, 3.01f, 3.99f, 3.99f}), "snapped footprint: an object's");
    Object& sim = vm.instantiate(kGroup, 0x81);
    vm.setPosition(sim, {4.105f, 3.5f, 1}); // 0.015 off the table's side
    Object::Route r;
    check(routing_internal::stepBlocker(vm, sim, r, 4.07f, 3.5f, 1) == 0,
          "snapped footprint: a step 0.03 into the table's last cell goes");
    check(routing_internal::stepBlocker(vm, sim, r, 3.98f, 3.5f, 1) == table.id,
          "snapped footprint: half a cell into it does not");
}

// The world's fixed cost rectangles (round n49: the cost-10 rectangles at a
// railed stair's ends, PortalAddToPathPlannerImpl): passable, the most of
// the floor and the rectangle in each cell they overlap; a plan goes round
// them when that is cheaper.
static void testCostRects() {
    World w(10, 6);
    w.setCostRects({{1, {4.0f, 0.0f, 5.0f, 4.0f}, 10.0f}});
    PathPlanner planner(w);
    check(planner.free(1, {4.5f, 2.0f}) && planner.costAt(1, {4.5f, 2.0f}) == 10 && planner.costAt(1, {5.5f, 2.0f}) == 1,
          "cost rectangles: passable, costing 10 inside");
    PlannedPath path;
    RouteFail why;
    check(planner.plan({1.5f, 1.0f}, 1, 1, {{8.5f, 1.0f, 0}}, path, why), "cost rectangles: a way");
    float maxY = 0;
    for (const auto& q : path.points)
        maxY = std::max(maxY, q.y);
    check(maxY >= 4.0f, "cost rectangles: the walk goes round");
}

// The terrain under a low floor (round n49; base 1.0's
// WorkOutTileCostAndPassability state 5, FUN_01038900): a quarter of the
// ground whose quarter on the level above has a visible floor standing less
// than 3.0 over it at a corner is closed to the planner; a floor at a full
// level's height (3.0) is not, nor a hidden one.
static void testLowFloorTerrain() {
    Lot lot;
    lot.width = 8, lot.height = 3;
    lot.minLevel = 0, lot.levelCount = 2;
    lot.elevations.assign(size_t(2) * 9 * 4, 0.0f);
    for (int x = 0; x <= 8; ++x)
        for (int y = 0; y <= 3; ++y)
            lot.elevations[(size_t(1) * 9 + size_t(x)) * 4 + size_t(y)] = x <= 4 ? 1.0f : 3.0f; // a deck at 1.0, then 3.0
    lot.floors.assign(size_t(2) * 24, {0, 0, 0, 0});
    lot.floorVisibility.assign(size_t(2) * 24, {1, 1, 1, 1});
    auto above = [&](uint32_t x, uint32_t y) -> std::array<uint16_t, 4>& { return lot.floors[24 + x * 3 + y]; };
    above(2, 1) = {1, 1, 1, 1};  // the deck over (2, 1): 1.0 over the ground
    above(3, 1) = {1, 1, 0, 0};  // its west and north quarters only
    above(6, 1) = {1, 1, 1, 1};  // a floor 3.0 up: a full level
    above(1, 1) = {1, 1, 1, 1};  // 1.0 up but hidden (3ARY 0x15)
    lot.floorVisibility[24 + 1 * 3 + 1] = {0, 0, 0, 0};
    World w = World::fromLot(lot);
    check(w.blockedQuarters({2, 1, 1}) == 0x0F && w.blockedQuarters({3, 1, 1}) == 0x03,
          "low floor: the ground under a deck closed, quarter by quarter");
    check(w.blockedQuarters({6, 1, 1}) == 0 && w.blockedQuarters({1, 1, 1}) == 0 && w.blockedQuarters({2, 1, 2}) == 0,
          "low floor: not under a full level, nor under a hidden floor, nor the deck itself");
    PathPlanner planner(w);
    check(!planner.free(1, {2.5f, 1.5f}) && !planner.free(1, {3.1f, 1.5f}) && planner.free(1, {3.9f, 1.5f}),
          "low floor: the planner's cells in the closed quarters");
    PlannedPath path;
    RouteFail why;
    check(planner.plan({0.5f, 1.5f}, 1, 1, {{7.5f, 1.5f, 0}}, path, why), "low floor: a way around");
    bool under = false;
    Point2 a{0.5f, 1.5f};
    for (const auto& q : path.points) {
        under |= !planner.clear(1, a, {q.x, q.y});
        a = {q.x, q.y};
    }
    check(!under, "low floor: not under the deck");
}

// A pool's rooms: the floor (the level below the pool's) is a room of its own
// and is a pool ("Swim Here?" of the go-here destination); the water over it
// takes the same room; a lawn is not a pool.
static void testPoolRoom() {
    Lot lot;
    lot.width = lot.height = 8;
    lot.minLevel = -1;
    lot.levelCount = 2;
    lot.rooms.assign(size_t(lot.levelCount) * 64, {0, 0, 0, 0});
    lot.elevations.assign(size_t(lot.levelCount) * 81, 0.0f);
    lot.pools.push_back({});
    lot.pools[0].level = 0, lot.pools[0].x = 2, lot.pools[0].y = 2, lot.pools[0].width = 4, lot.pools[0].height = 4;
    for (uint32_t x = 2; x < 6; ++x)
        for (uint32_t y = 2; y < 6; ++y)
            lot.rooms[x * 8 + y] = {1, 1, 1, 1}; // level -1: the pool's floor
    for (uint32_t x = 0; x < 8; ++x)
        for (uint32_t y = 0; y < 8; ++y)
            lot.rooms[64 + x * 8 + y] = {2, 2, 2, 2}; // level 0: the yard (the terrain's room)
    CodeLibrary code;
    VM vm(code);
    setLotRooms(vm, lot);
    check(vm.rooms.count(1) && vm.rooms[1].values[4] == 1, "pool: the floor's room is a pool");
    check(vm.rooms.count(2) && vm.rooms[2].values[4] == 0, "pool: the yard is not");
    World w = World::fromLot(lot);
    check(w.pool({3, 3, 1}), "pool: the water over the floor is a pool tile");
    // (the yard room covers the surface tiles in this fixture, so the water keeps it)
    lot.rooms[64 + 3 * 8 + 3] = {0, 0, 0, 0};
    World w2 = World::fromLot(lot);
    check(w2.room({3, 3, 1}) == 1, "pool: the water without a room takes the floor's");
    check(w2.room({0, 0, 1}) == 2, "pool: the lawn keeps its own");
}

int main() {
    testStairs();
    testWalking();
    testFootprintMask();
    testSnapAndPlace();
    testPathfinder();
    testRoutingGoals();
    testDirections();
    testWorldWalls();
    testDiagonalWalls();
    testSetToNextTiles();
    testPlaceNearFree();
    testPathPlanner();
    testPlannerKept();
    testEscapeDiagonal();
    testObjectPortals();
    testRouteResult();
    testRouteStatus();
    testRoutePersonDataFields();
    testRouteSlotIndex();
    testBlockedBySim();
    testMutualBlock();
    testPredictionOnCurve();
    testTurnInPlace();
    testLocoPhases();
    testLocoStartWindow();
    testRunnerStep();
    testLocoBlend();
    testLocoCurve();
    testLocoStride();
    testFitStride();
    testLocoPoseBlend();
    testSidestep();
    testLocoHalt();
    testDoorWait();
    testDoorPortalTree();
    testPreRouteTree();
    testRouteInGoalCheck();
    testBlockedGoalCheck();
    testMoveOutOfWaySlot();
    testMoveOutOfWay();
    testGoHere();
    testPoolRoom();
    testFloorCosts();
    testLowFloorTerrain();
    testSnappedFootprints();
    testCostRects();
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("all routing tests passed\n");
    return failures ? 1 : 0;
}
