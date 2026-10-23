// Build mode on a synthetic lot: walls into the graphs, rooms from the
// walls, floors, wall coverings, prices, undo and the VM's world.
#include "app/undo_order.hpp"
#include "formats/build_catalog.hpp"
#include "formats/design_catalog.hpp"
#include "formats/lot.hpp"
#include "formats/lot_edit.hpp"
#include "vm/build_edit.hpp"
#include "vm/lot.hpp"
#include "vm/rooms.hpp"
#include "vm/placement.hpp"
#include "vm/vm.hpp"

#include <cmath>
#include <cstdio>

using namespace os2;
using namespace os2::vm;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// A flat 12x10 lot with one level (the ground), the outdoors one room (1),
// and the game's empty wall graphs.
static Lot flatLot() {
    Lot lot;
    lot.width = 12, lot.height = 10, lot.levelCount = 1;
    lot.elevations.assign(13 * 11, 0.0f);
    lot.floors.assign(12 * 10, {});
    lot.rooms.assign(12 * 10, {1, 1, 1, 1});
    lot.roomLayers[0x0A] = lot.rooms;
    lot.roomLayers[0x0B] = lot.rooms;
    for (uint32_t instance : {5u, 6u, 7u, 8u, 0x13u, 0x16u, 0x17u, 0x18u}) {
        Lot::WallGraph g;
        g.instance = instance;
        g.header = {0, 0, 0, 12, 10, 1, 0, 0};
        g.rooms = {0, 1};
        lot.wallGraphs.push_back(g);
    }
    lot.floorPatterns[1] = "sidewalk";
    return lot;
}

static uint16_t roomAt(const Lot& lot, int x, int y, int q = 0) { return lot.rooms[size_t(x) * lot.height + size_t(y)][size_t(q)]; }

static void testSections() {
    check(wallSections(0, 0, 3, 0).size() == 3, "straight run splits into tiles");
    check(wallSections(2, 2, 0, 0).size() == 2, "diagonal run splits into tiles");
    check(wallSections(0, 0, 2, 1).empty(), "other slopes make no walls");
}

static void testRooms() {
    Lot lot = flatLot();
    // A 3x3 room from (2,2) to (5,5).
    for (const auto& s : {std::array<int, 4>{2, 2, 5, 2}, {5, 2, 5, 5}, {5, 5, 2, 5}, {2, 5, 2, 2}})
        for (const auto& t : wallSections(s[0], s[1], s[2], s[3]))
            check(addWall(lot, 0, t[0], t[1], t[2], t[3], 1, {0xFFFF, 0xFFFF}).has_value(), "wall added");
    check(!addWall(lot, 0, 2, 2, 3, 2, 1, {0xFFFF, 0xFFFF}), "no second wall on a wall");
    check(lot.walls.size() == 12, "12 wall sections");
    size_t inGraph7 = 0, inGraph17 = 0;
    for (const auto& g : lot.wallGraphs)
        (g.instance == 7 ? inGraph7 : g.instance == 0x17 ? inGraph17 : inGraph7) += g.instance == 7 || g.instance == 0x17 ? g.lines.size() : 0;
    check(inGraph7 == 12 && inGraph17 == 0, "walls go into the light graph, not the attic one");
    updateRooms(lot);
    const uint16_t inside = roomAt(lot, 3, 3), outside = roomAt(lot, 0, 0);
    check(outside == 1, "the outdoors keeps its room");
    check(inside != 0 && inside != outside, "the closed room gets a new ID");
    check(roomAt(lot, 4, 4, 2) == inside && roomAt(lot, 2, 2, 0) == inside, "all of the inside shares it");
    check(roomAt(lot, 1, 3, 2) == outside, "the quarter outside the wall is outdoors");
    // The graph lines know their rooms: wall (2,2)->(3,2) has the room on its left (+y).
    for (const auto& g : lot.wallGraphs)
        if (g.instance == 5)
            for (const auto& l : g.lines)
                if (l.id == lot.walls[0].id)
                    check(l.roomLeft == inside && l.roomRight == outside, "line rooms on their sides");
    // A diagonal splits it in two; the bigger part keeps the ID.
    check(addWall(lot, 0, 2, 2, 3, 3, 1, {0xFFFF, 0xFFFF}).has_value(), "diagonal wall");
    check(addWall(lot, 0, 3, 3, 4, 4, 1, {0xFFFF, 0xFFFF}).has_value(), "diagonal wall 2");
    check(addWall(lot, 0, 4, 4, 5, 5, 1, {0xFFFF, 0xFFFF}).has_value(), "diagonal wall 3");
    updateRooms(lot);
    check(roomAt(lot, 3, 3, QNorth) != roomAt(lot, 3, 3, QEast), "the diagonal separates the quarters");
    check(roomAt(lot, 3, 3, QNorth) == roomAt(lot, 2, 4, QEast), "west half is one room");
    // Taking the walls down joins everything again.
    for (const auto& t : wallSections(2, 2, 5, 5))
        check(removeWall(lot, 0, t[0], t[1], t[2], t[3]).has_value(), "diagonal removed");
    check(removeWall(lot, 0, 3, 2, 2, 2).has_value(), "wall removed either way round");
    updateRooms(lot);
    check(roomAt(lot, 3, 3) == outside, "an open room is outdoors again");
}

static void testEditor() {
    Lot lot = flatLot();
    // The street along y = 0, locked (3ARY 0x03).
    lot.locked.assign(12 * 10, 0);
    for (int x = 0; x < 12; ++x)
        lot.floors[size_t(x) * 10] = {1, 1, 1, 1}, lot.locked[size_t(x) * 10] = 1;
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetDiv10000] = 0;
    vm.globals[global::BudgetMod10000] = 2000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    auto r = ed.drawWalls(1, 2, 0, 2, 3);
    check(!r.ok && funds(vm) == 2000, "no walls on the street");
    r = ed.drawRoom(1, 2, 2, 6, 5);
    check(r.ok && r.changed == 14 && r.cost == 14 * kWallSectionPrice, "room tool: 14 sections at §70");
    check(funds(vm) == 2000 - 980, "the walls are paid for");
    check(vm.world->wallBetween({2, 2, 1}, 0, -1), "the world has the wall for routing");
    check(vm.world->room({3, 3, 1}) != vm.world->room({0, 3, 1}), "the world has the new room");
    BuildPattern carpet;
    carpet.guid = 0x1234, carpet.floor = true, carpet.price = 8, carpet.name = "carpet";
    r = ed.paintFloor(1, 2, 2, 3, 2, &carpet);
    check(r.ok && r.changed == 8 && r.cost == 16, "two tiles of carpet at §8 a tile");
    check(lot.floorPatterns.count(2) && lot.floorPatterns[2] == "1234", "the pattern gets a reference by GUID");
    r = ed.paintFloor(1, 3, 2, 3, 2, nullptr);
    check(r.ok && r.cost == -6, "erasing a tile gives back floor(8 x 0.8) a tile");
    BuildPattern paint;
    paint.guid = 0xabc, paint.price = 3, paint.name = "paint";
    r = ed.paintRoom(1, 3, 3, &paint);
    check(r.ok && r.changed == 14 && r.cost == 42, "the room's 14 wall faces at §3");
    r = ed.paintWalls(1, 2, 2, 6, 2, -1, &paint);
    check(r.ok && r.changed == 0, "the inside faces are painted already");
    BuildPattern paper, tile;
    paper.guid = 0xdef, paper.price = 2, paper.name = "paper";
    tile.guid = 0x987, tile.price = 1, tile.name = "tile";
    r = ed.paintWalls(1, 2, 2, 6, 2, -1, &paper);
    check(r.ok && r.changed == 4, "the south wall's inside papered");
    r = ed.paintFlood(1, 6, 2, 6, 3, -1, &tile);
    check(r.ok && r.changed == 10 && r.cost == 10, "Shift covers only the connected faces with the clicked pattern");
    r = ed.paintFlood(1, 3, 2, 2, 2, 1, &paint);
    check(r.ok && r.changed == 4, "Shift on the papered faces takes those four");
    r = ed.paintWalls(1, 2, 2, 6, 2, 1, &paint);
    check(r.ok && r.changed == 4, "the outside faces of a run");
    r = ed.paintWalls(1, 2, 2, 6, 2, 1, nullptr);
    check(r.ok && r.changed == 4 && r.cost == -8, "taking paint off gives back int(3 x 0.8) a face");
    const int before = funds(vm);
    r = ed.deleteWalls(1, 2, 2, 6, 2);
    check(r.ok && r.changed == 4 && r.cost == -224, "walls come back at int(70 x 0.8) a section");
    check(!vm.world->wallBetween({2, 2, 1}, 0, -1), "the wall is gone from the world");
    check(ed.undo() && funds(vm) == before && vm.world->wallBetween({2, 2, 1}, 0, -1), "undo puts it back");
    check(ed.redo() && funds(vm) == before + 224, "redo takes it again");
    while (ed.undo()) {
    }
    check(funds(vm) == 2000 && lot.walls.empty(), "undoing everything restores the lot and the funds");
}

// Round 9: the ground levelled under walls and floors, fences and their
// posts, floor rotation, a click's quarters, the upper floors' support.
static void testRound9() {
    Lot lot = flatLot();
    // A slope along x: elevation = x / 4.
    for (int x = 0; x <= 12; ++x)
        for (int y = 0; y <= 10; ++y)
            lot.elevations[size_t(x) * 11 + size_t(y)] = float(x) * 0.25f;
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetDiv10000] = 0;
    vm.globals[global::BudgetMod10000] = 5000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    auto r = ed.drawWalls(1, 2, 3, 6, 3);
    check(r.ok && r.changed == 4, "a wall across a slope");
    check(lot.elevation(0, 6, 3) == 0.5f && lot.elevation(0, 4, 3) == 0.5f, "the ground under it is levelled to the start");
    check(lot.elevation(0, 6, 4) == 1.5f, "the ground beside it is not");
    check(ed.undo() && lot.elevation(0, 6, 3) == 1.5f, "undo puts the ground back");
    // Floors level the rectangle to the first tile's height.
    BuildPattern tile;
    tile.guid = 0x77, tile.floor = true, tile.price = 4, tile.name = "tile";
    r = ed.paintFloor(1, 8, 5, 9, 6, &tile, 0xF, 4);
    check(r.ok && r.changed == 16, "a floor on the slope");
    check(lot.elevation(0, 10, 7) == lot.elevation(0, 8, 5) && lot.elevation(0, 8, 5) == 2.125f,
          "the ground under the floor is level at the first tile's mean height");
    check(floorRotation(lot, 0, 8, 5, 0) == 4, "the floor keeps its rotation");
    r = ed.paintFloor(1, 8, 5, 8, 5, &tile, 0xF, 4);
    check(r.ok && r.changed == 0, "the same pattern and rotation is no change");
    r = ed.paintFloor(1, 8, 5, 8, 5, &tile, 0xF, 2);
    check(r.ok && r.changed == 4 && r.cost == 4, "another rotation pays the pattern again");
    // A diagonal wall: a click floors the half under the mouse.
    ed.drawWalls(1, 2, 6, 3, 7);
    check(ed.clickQuarters(1, 2.2f, 6.8f) == 0x3 && ed.clickQuarters(1, 2.8f, 6.2f) == 0xC, "a click's half of a diagonal");
    check(ed.clickQuarters(1, 5.5f, 5.5f) == 0xF, "a click floors a whole tile");
    // Fences: their cost a section, a post at each point, the refund.
    BuildFence picket;
    picket.guid = 0x12345678, picket.price = 10, picket.name = "picket";
    BuildCatalog none;
    const int before = funds(vm);
    r = ed.drawFences(1, 1, 8, 4, 8, picket);
    check(r.ok && r.changed == 3 && r.cost == 30, "three fence sections at their cost");
    check(lot.fencePosts.size() == 4, "a post at each grid point of the run");
    check(funds(vm) == before - 30, "the fence is paid for");
    r = ed.deleteFences(1, 3, 8, 4, 8);
    check(r.ok && r.changed == 1 && lot.fencePosts.size() == 3, "the post left with no fence goes");
    check(ed.undo() && lot.fencePosts.size() == 4 && funds(vm) == before - 30, "undo puts the fence back");
    // Upper floors: supported from below or near a supported tile.
    Lot two = flatLot();
    two.levelCount = 2;
    two.elevations.resize(13 * 11 * 2, 3.0f);
    two.floors.resize(12 * 10 * 2);
    two.rooms.resize(12 * 10 * 2);
    for (auto& [k, l] : two.roomLayers)
        l.resize(12 * 10 * 2);
    // A load-bearing room (3ARY 0x0A) under tiles 2-4: what holds a floor
    // above (SupportedUnderneath), not the ground floor itself.
    for (int x = 2; x < 5; ++x)
        for (int y = 2; y < 5; ++y)
            two.floors[size_t(x) * 10 + size_t(y)] = {1, 1, 1, 1};
    for (const auto& s : std::array<std::array<int, 4>, 4>{{{2, 2, 5, 2}, {5, 2, 5, 5}, {5, 5, 2, 5}, {2, 5, 2, 2}}})
        for (const auto& t : wallSections(s[0], s[1], s[2], s[3]))
            addWall(two, 0, t[0], t[1], t[2], t[3], 1, {0xFFFF, 0xFFFF});
    for (auto* m : {&two.rooms, &two.roomLayers[0x0A], &two.roomLayers[0x0B]})
        m->assign(12 * 10 * 2, {0, 0, 0, 0}); // outdoors 0
    updateRooms(two);
    two.floors[size_t(9) * 10 + 2] = {1, 1, 1, 1}; // a ground floor outdoors holds nothing
    VM vm2(code);
    vm2.globals[global::BudgetMod10000] = 5000;
    vm2.families[1], vm2.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm2.world = World::fromLot(two);
    BuildEditor ed2(vm2, two);
    check(ed2.floorable(2, 3, 3), "a tile over a load-bearing room is floorable");
    check(!ed2.floorable(2, 9, 2), "a tile over an outdoor floor is not");
    check(!ed2.floorable(2, 9, 9), "a tile over nothing is not");
    r = ed2.paintFloor(2, 9, 9, 9, 9, &tile);
    check(!r.ok && r.error == 0x52, "flooring in the air is refused (0x52)");
    r = ed2.paintFloor(2, 2, 2, 5, 2, &tile);
    check(r.ok && ed2.floorable(2, 6, 2) && !ed2.floorable(2, 8, 8), "a floor reaches out next to a supported tile");
    // IsFloorable (BV 0x16a990): a load-bearing wall below holds the tiles
    // on its edge (HasSupport) and, next to a floor, the tiles one away;
    // two away it no longer does. An attic wall bears no load.
    addWall(two, 0, 8, 6, 8, 7, Lot::Attic, {0xFFFF, 0xFFFF});
    check(!ed2.floorable(2, 8, 6), "an attic wall holds nothing up");
    addWall(two, 0, 8, 8, 8, 9, Lot::Normal, {0xFFFF, 0xFFFF});
    check(ed2.floorable(2, 8, 8) && ed2.floorable(2, 7, 8) && !ed2.floorable(2, 9, 8),
          "a wall below holds the tiles on both sides of it");
    r = ed2.paintFloor(2, 8, 8, 8, 8, &tile);
    check(r.ok && ed2.floorable(2, 9, 8), "next to a floor, a wall within one tile holds it");
    r = ed2.paintFloor(2, 9, 8, 9, 8, &tile);
    check(r.ok && !ed2.floorable(2, 10, 8), "two tiles from the wall nothing holds it");
    check(ed2.supportedUnderneath(2, 3, 3, 0x1) && !ed2.supportedUnderneath(2, 8, 8, 0xF) &&
              ed2.supportedUnderneath(1, 9, 9, 0),
          "SupportedUnderneath: a load-bearing room below in the quarters; the ground always");
    // Walls upstairs (FUN_0016b740): over the floor below, over a wall
    // below, but not in the air (0x48).
    r = ed2.drawWalls(2, 3, 3, 3, 4);
    check(r.ok, "a wall over a room of the level below stands");
    r = ed2.drawWalls(2, 5, 6, 5, 7);
    check(!r.ok && r.error == 0x48, "a wall in the air needs something to support it (0x48)");
    r = ed2.drawWalls(2, 8, 6, 8, 7);
    check(r.ok, "a wall over an attic wall stands");
}

// Room IDs as cWallGraph makes them: outdoors 0, a new room the smallest
// free ID, opening a room onto the outdoors gives it back to room 0.
static void testRoomIds() {
    Lot lot = flatLot();
    for (auto* m : {&lot.rooms, &lot.roomLayers[0x0A], &lot.roomLayers[0x0B]})
        m->assign(12 * 10, {0, 0, 0, 0});
    auto room = [&](int x0, int y0, int x1, int y1) {
        for (const auto& s : std::array<std::array<int, 4>, 4>{{{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}}})
            for (const auto& t : wallSections(s[0], s[1], s[2], s[3]))
                addWall(lot, 0, t[0], t[1], t[2], t[3], 1, {0xFFFF, 0xFFFF});
        updateRooms(lot);
    };
    room(1, 1, 3, 3);
    room(5, 1, 7, 3);
    check(roomAt(lot, 2, 2) == 1 && roomAt(lot, 6, 2) == 2 && roomAt(lot, 0, 0) == 0, "rooms 1 and 2, outdoors 0");
    removeWall(lot, 0, 1, 1, 2, 1);
    updateRooms(lot);
    check(roomAt(lot, 2, 2) == 0 && roomAt(lot, 6, 2) == 2, "an opened room goes back to the outdoors");
    room(1, 5, 3, 7);
    check(roomAt(lot, 2, 6) == 1, "a new room takes the smallest free ID");
    // A fence splits the placement rooms (0x09) only.
    for (const auto& t : wallSections(8, 0, 8, 10))
        addWall(lot, 0, t[0], t[1], t[2], t[3], 0x12345678, {0xFFFF, 0xFFFF});
    updateRooms(lot);
    const auto& light = lot.roomLayers[0x0B];
    check(roomAt(lot, 10, 5) == 0 && light[size_t(10) * 10 + 5][0] == 0, "fences cut no room off the lot's edge");

    // Removing the wall between two rooms keeps the right-hand room's ID
    // (SetRoomsPreDetach), whatever their sizes.
    Lot two = flatLot();
    for (auto* m : {&two.rooms, &two.roomLayers[0x0A], &two.roomLayers[0x0B]})
        m->assign(12 * 10, {0, 0, 0, 0});
    auto box = [&](int x0, int y0, int x1, int y1) {
        for (const auto& s : std::array<std::array<int, 4>, 4>{{{x0, y0, x1, y0}, {x1, y0, x1, y1}, {x1, y1, x0, y1}, {x0, y1, x0, y0}}})
            for (const auto& t : wallSections(s[0], s[1], s[2], s[3]))
                addWall(two, 0, t[0], t[1], t[2], t[3], 1, {0xFFFF, 0xFFFF});
        updateRooms(two);
    };
    box(1, 1, 3, 4); // room 1, the larger
    box(3, 1, 5, 3); // room 2, east of it: the wall (3,1)-(3,3) has room 2 on its right
    check(roomAt(two, 2, 2) == 1 && roomAt(two, 4, 2) == 2, "rooms 1 and 2 side by side");
    std::vector<RoomMerge> merges;
    removeWall(two, 0, 3, 1, 3, 2, &merges);
    removeWall(two, 0, 3, 2, 3, 3, &merges);
    updateRooms(two, merges);
    check(roomAt(two, 2, 2) == 2 && roomAt(two, 4, 2) == 2 && roomAt(two, 2, 3) == 2,
          "the joined room keeps the right-hand side's ID");
    merges.clear();
    removeWall(two, 0, 1, 1, 2, 1, &merges);
    updateRooms(two, merges);
    check(roomAt(two, 2, 2) == 0 && roomAt(two, 4, 2) == 0, "joined with the outdoors it is room 0");

    // SetRoomsPostAttach (docs/buy-build-mac.md §7.25): a wall splitting a
    // room gives the new ID to the left side of the line that closes the
    // cycle (seen from its a to its b), whatever the sizes; the outdoors
    // split gives it to the enclosed side, whichever way the walls run.
    Lot split = flatLot();
    for (auto* m : {&split.rooms, &split.roomLayers[0x0A], &split.roomLayers[0x0B]})
        m->assign(12 * 10, {0, 0, 0, 0});
    for (const auto& s : std::array<std::array<int, 4>, 4>{{{1, 1, 1, 5}, {1, 5, 7, 5}, {7, 5, 7, 1}, {7, 1, 1, 1}}})
        for (const auto& t : wallSections(s[0], s[1], s[2], s[3]))
            addWall(split, 0, t[0], t[1], t[2], t[3], 1, {0xFFFF, 0xFFFF});
    updateRooms(split);
    check(roomAt(split, 4, 3) == 1 && roomAt(split, 0, 0) == 0, "a room drawn clockwise is the enclosed side's");
    for (const auto& t : wallSections(3, 5, 3, 1)) // down -y: its left is +x
        addWall(split, 0, t[0], t[1], t[2], t[3], 1, {0xFFFF, 0xFFFF});
    updateRooms(split);
    check(roomAt(split, 5, 3) == 2 && roomAt(split, 2, 3) == 1,
          "the closing line's left side (the larger part here) takes the new ID");
    const auto& light2 = split.roomLayers[0x0B];
    check(light2[size_t(5) * 10 + 3][0] == 2 && light2[size_t(2) * 10 + 3][0] == 1, "so do the light rooms (3ARY 0x0B)");
}

static void testFoundation() {
    Lot lot = flatLot();
    lot.levelCount = 3;
    lot.elevations.resize(13 * 11 * 3);
    for (size_t i = 0; i < 13 * 11 * 3; ++i)
        lot.elevations[i] = 3.0f * float(i / (13 * 11));
    lot.floors.resize(12 * 10 * 3);
    lot.rooms.resize(12 * 10 * 3);
    for (auto& [k, l] : lot.roomLayers)
        l.resize(12 * 10 * 3);
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    auto r = ed.drawFoundation(2, 2, 4, 3);
    check(r.ok && r.changed == 6 && r.cost == 6 * kFoundationTilePrice, "six foundation tiles at §4");
    check(lot.elevation(1, 3, 3) == kFoundationHeight && lot.elevation(2, 3, 3) == 3.0f + kFoundationHeight,
          "the stories above sit a foundation higher");
    check(lot.elevation(1, 8, 8) == 3.0f, "elsewhere they do not");
    check(ed.foundationTile(3, 2) && !ed.foundationTile(5, 2), "the tiles are foundation tiles");
    // SetDeckFoundationMarkers (docs/buy-build-mac.md §7.25): the product on
    // every quarter of the rectangle, on the ground; nothing elsewhere.
    check(lot.deckFoundationStyle(0, 2, 2, 0) == 0 && lot.deckFoundationStyle(0, 4, 3, 3) == 0 &&
              lot.deckFoundationStyle(0, 5, 3, 0) == -1 && lot.deckFoundationStyle(1, 3, 3, 0) == -1,
          "the foundation's markers (3ARY 0x14) on the ground");
    check(lot.deckFoundationStyles.size() == 12 * 10 * 2 && lot.deckFoundationBase == 0,
          "a plane a level, the top one left out");
    size_t walls = 0;
    for (const auto& w : lot.walls)
        walls += w.type == Lot::Foundation;
    check(walls == 10, "foundation walls round its edge");
    check(lot.floor(1, 3, 3)[0] != 0, "the foundation has its floor");
    r = ed.deleteFoundation(4, 2, 4, 3);
    check(r.ok && r.changed == 2 && r.cost == -2 * 3, "two tiles back at int(4 x 0.8)");
    walls = 0;
    for (const auto& w : lot.walls)
        walls += w.type == Lot::Foundation;
    check(walls == 8 && lot.elevation(1, 5, 2) == 3.0f && lot.elevation(1, 4, 2) == kFoundationHeight,
          "the walls and heights follow what is left");
    check(lot.deckFoundationStyle(0, 4, 2, 1) == -1 && lot.deckFoundationStyle(0, 3, 2, 1) == 0,
          "the deleted tiles lose their markers");
    check(ed.undo() && ed.undo() && lot.walls.empty() && lot.elevation(1, 3, 3) == 3.0f, "undo takes it all back");
    check(lot.deckFoundationStyles.empty(), "the markers too");
}

static void testTerrain() {
    Lot lot = flatLot();
    lot.floors[size_t(1) * 10] = {1, 1, 1, 1}; // the sidewalk on tile (1, 0)
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    std::vector<uint8_t> profile(32 * 32, 255);
    auto r = ed.terrainStroke(BuildEditor::TerrainOp::Raise, 6, 5, 10, 1, profile);
    check(r.ok && r.changed > 0 && lot.elevation(0, 6, 5) > 0.4f, "raising the ground");
    check(lot.elevation(0, 0, 0) == 0.0f, "only under the brush");
    const float h = lot.elevation(0, 6, 5);
    check(std::fabs(h / 0.00625f - std::round(h / 0.00625f)) < 1e-3f, "heights stay on the terrain step");
    check(r.cost == funds(vm) * 0 + r.cost && r.cost > 0 && funds(vm) == 5000 - r.cost, "the change is paid for");
    check(ed.undo() && lot.elevation(0, 6, 5) == 0.0f && funds(vm) == 5000, "undo flattens it again");
    r = ed.terrainStroke(BuildEditor::TerrainOp::Raise, 1.5f, 0.5f, 3, 1, profile);
    check(!r.ok && r.error == 0x8B, "a floored tile is not exposed terrain");
    // The water tool digs a pond and fills it 0.25 under its rim.
    r = ed.terrainStroke(BuildEditor::TerrainOp::Water, 6, 5, 200, 2, profile);
    size_t wet = 0;
    for (int x = 0; x <= 12; ++x)
        for (int y = 0; y <= 10; ++y)
            wet += lot.waterLevel(x, y) > lot.elevation(0, x, y);
    check(r.ok && lot.elevation(0, 6, 5) < -0.3f && wet > 0, "a pond: the ground dug and water over it");
    check(std::fabs(lot.waterLevel(6, 5) - (lot.elevation(0, 0, 0) - 0.25f)) < 1e-4f, "the water 0.25 under the rim");
    check(ed.undo() && lot.water.empty() && lot.elevation(0, 6, 5) == 0.0f, "undo fills the pond in");
    r = ed.terrainStroke(BuildEditor::TerrainOp::Paint, 6, 5, 4, 1, profile, "terrain-grass-b-paint");
    check(r.ok && lot.paints.size() == 1 && lot.maskWidth == 49, "painting adds a paint and its mask");
    // The height limits (FUN_009e7700): +/-150 x 0.1875.
    check(terrainLimitError(28.125f) == 0 && terrainLimitError(28.2f) == 0x66 && terrainLimitError(-28.2f) == 0x65,
          "the terrain's height limits are +/-28.125");
    for (float& e : lot.elevations)
        e = 28.125f;
    r = ed.terrainStroke(BuildEditor::TerrainOp::Raise, 6, 5, 10, 1, profile);
    check(!r.ok && r.error == 0x66 && r.changed == 0 && lot.elevation(0, 6, 5) == 28.125f, "can't raise terrain further");
    for (float& e : lot.elevations)
        e = -28.125f;
    r = ed.terrainStroke(BuildEditor::TerrainOp::Lower, 6, 5, 10, 1, profile);
    check(!r.ok && r.error == 0x65 && lot.elevation(0, 6, 5) == -28.125f, "can't lower terrain further");
    // The brush's vertex test (flags 0x2BF7, round n20): a wall's end stays
    // (it needs a flat bottom, 0x5B), a fence's end moves (fences say no),
    // a point by a floor stays (0x5A).
    for (float& e : lot.elevations)
        e = 0.0f;
    addWall(lot, 0, 6, 5, 7, 5, Lot::Normal, {0xFFFF, 0xFFFF});
    addWall(lot, 0, 9, 5, 10, 5, 0x2CB2A001, {0xFFFF, 0xFFFF});
    lot.floors[size_t(3) * 10 + 5] = {1, 1, 1, 1};
    check(ed.vertexMoveError(0, 6, 5, 0.5f, vertextest::kBrush) == 0x5B, "a wall's end may not move (0x5B)");
    check(ed.vertexMoveError(0, 9, 5, 0.5f, vertextest::kBrush) == 0, "a fence's end may");
    check(ed.vertexMoveError(0, 4, 6, 0.5f, vertextest::kBrush) == 0x5A, "a point by a floor may not (0x5A)");
    check(ed.vertexMoveError(0, 0, 3, 0.5f, vertextest::kBrush) == 0x62, "nor one on the lot's edge (0x62)");
    r = ed.terrainStroke(BuildEditor::TerrainOp::Raise, 6.5f, 6.5f, 10, 1, profile);
    check(r.ok && lot.elevation(0, 6, 5) == 0.0f && lot.elevation(0, 6, 6) > 0.0f, "the brush leaves the wall's end where it is");
}

static void testRoof() {
    Lot lot = flatLot();
    lot.levelCount = 2;
    lot.elevations.resize(13 * 11 * 2, 3.0f);
    lot.floors.resize(12 * 10 * 2);
    lot.rooms.assign(12 * 10 * 2, {0, 0, 0, 0}); // outdoors (auto-roof reads the rooms)
    for (auto& [k, l] : lot.roomLayers)
        l = lot.rooms;
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    ed.drawRoom(1, 2, 2, 8, 5);
    auto r = ed.drawRoof(1, 2, 2, 8, 5, Lot::LongGable);
    check(r.ok && lot.roofs.size() == 1 && lot.roofs[0].level == 1 && r.cost == 0, "a free roof over the walls");
    size_t attic = 0;
    for (const auto& w : lot.walls)
        attic += w.type == Lot::Attic && w.a.level == 1;
    check(attic == 6, "attic walls on the two gable ends (the short sides)");
    r = ed.drawRoof(1, 2, 2, 8, 5, Lot::Hip);
    r = ed.deleteRoof(1, 5, 3);
    check(r.ok && lot.roofs.size() == 1, "deleting the roof on top");
    r = ed.deleteRoof(1, 5, 3);
    attic = 0;
    for (const auto& w : lot.walls)
        attic += w.type == Lot::Attic;
    check(r.ok && lot.roofs.empty() && attic == 0, "the attic walls go with the roof");

    // Auto-roof: the room (6 x 3 tiles, wider than deep) gets one rectangle.
    r = ed.autoRoof(3, false);
    check(r.ok && lot.roofs.size() == 1 && lot.roofs[0].type == Lot::Hip && lot.roofs[0].x0 == 2 &&
              lot.roofs[0].x1 == 8 && lot.roofs[0].y0 == 2 && lot.roofs[0].y1 == 5,
          "tool 3 puts a hip roof over a room no deeper than wide");
    r = ed.autoRoof(4, false);
    check(r.ok && lot.roofs.size() == 1 && lot.roofs[0].type == Lot::ShortGable,
          "the tool's earlier roofs go; tool 4 makes a short gable over a wide room");
    r = ed.autoRoof(1, true);
    check(r.ok && lot.roofs.size() == 1, "preserving: the roofed room gets nothing more");
    r = ed.removeRoofs();
    check(r.ok && lot.roofs.empty(), "C300 removes the roofs");

    // Dormers: walls of the box on the attic level, a roof over them one level up.
    ed.drawRoof(1, 2, 2, 8, 5, Lot::LongGable);
    const size_t walls = lot.walls.size(), roofs = lot.roofs.size();
    const int before = funds(vm);
    r = ed.placeDormer(7, 2, 4, 2, 0);
    check(r.ok && r.cost == 12 * kWallSectionPrice && funds(vm) == before - 840, "a 2-wide dormer: 12 sections at §70 (§840)");
    check(lot.levelCount == 3 && lot.roofs.size() == roofs + 1 && lot.roofs.back().level == 2 &&
              lot.roofs.back().type == Lot::LongGable && lot.roofs.back().x1 == 6 && lot.roofs.back().y1 == 6,
          "its gable roof over the box, on a new level");
    check(lot.walls.size() >= walls + 12, "the box's walls (and the gable ends' attic walls)");
    r = ed.placeDormer(9, 2, 5, 2, 0);
    check(!r.ok && r.error == 0x08, "a dormer on another's walls (0x08)");
    check(ed.undo() && lot.levelCount == 2 && lot.roofs.size() == roofs && funds(vm) == before, "undo takes the dormer back");

    // cTSUserToolRoof::IsRoofValid (BV 0x1d9fd4).
    r = ed.removeRoofs();
    r = ed.drawRoof(1, 0, 2, 4, 5, Lot::Hip);
    check(!r.ok && r.error == 0x7E && lot.roofs.empty(), "a roof corner on the lot's edge (0x7E)");
    Object& chair = vm.createObject(0x7F000001);
    vm.setPosition(chair, {2.5f, 2.5f, 2});
    r = ed.drawRoof(1, 2, 2, 8, 5, Lot::LongGable);
    check(!r.ok && r.error == 0x7F && lot.roofs.empty(), "a roof low over an object of its level (0x7F)");
    vm.setPosition(chair, {5.5f, 3.5f, 2});
    r = ed.drawRoof(1, 2, 2, 8, 5, Lot::Hip);
    check(!r.ok && r.error == 0x7F, "the hip's ridge is still under a level's height");
    vm.removeObject(chair.id);
    r = ed.drawRoof(1, 2, 2, 8, 5, Lot::LongGable);
    check(r.ok, "with the object gone the roof goes on");
    // RoofIntersectWithObjects walks the roof's box with the eaves, and the
    // tool's cWorldRoof pre-test (IsAnyRoofAtLocation + clearance) frees a
    // tile under a roof already there (docs/buy-build-mac.md §7.22).
    r = ed.removeRoofs();
    Object& eaveChair = vm.createObject(0x7F000001);
    vm.setPosition(eaveChair, {2.5f, 1.5f, 2});
    r = ed.drawRoof(1, 2, 2, 8, 5, Lot::LongGable);
    check(!r.ok && r.error == 0x7F, "an object under the new roof's eave, outside the walls (0x7F)");
    vm.removeObject(eaveChair.id);
    r = ed.drawRoof(1, 2, 2, 8, 5, Lot::LongGable);
    Object& eaveChair2 = vm.createObject(0x7F000001);
    vm.setPosition(eaveChair2, {2.5f, 1.5f, 2});
    r = ed.drawRoof(1, 2, 2, 8, 5, Lot::Hip);
    check(r.ok && lot.roofs.size() == 2, "the eave of a roof already there clears that tile for another roof");
    vm.removeObject(eaveChair2.id);
}

// The editable area: a road along an edge takes its 10-tile strip out.
static void testEditableArea() {
    Lot lot;
    lot.width = 24, lot.height = 30, lot.levelCount = 1;
    lot.elevations.assign(25 * 31, 0.0f);
    lot.floors.assign(24 * 30, {});
    lot.rooms.assign(24 * 30, {1, 1, 1, 1});
    lot.roomLayers[0x0A] = lot.rooms;
    lot.roomLayers[0x0B] = lot.rooms;
    lot.floorPatterns[1] = "road_center";
    for (int x = 0; x < 24; ++x)
        lot.floors[size_t(x) * 30 + 25] = {1, 1, 1, 1};
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    check(ed.tileLocked(0, 3, 20) && !ed.tileLocked(0, 3, 19), "without 3ARY 0x03, the road edge's strip is locked (inferred)");
    auto r = ed.drawWalls(1, 2, 22, 5, 22);
    // The walls' leveller runs first (TryPlacePartition): its edge test
    // (FUN_009e8780) refuses the points by a locked tile, 0x62, before
    // UserMayPlacePartition's 0x45 (both read "Location out of bounds").
    check(!r.ok && r.error == 0x62, "no walls in the road strip (0x62)");
    r = ed.drawWalls(1, 2, 18, 5, 18);
    check(r.ok && r.changed == 3, "walls inside the area");
}

// The locked tiles (3ARY 0x03, cWorldDB::IsLocked): partitions refuse a
// locked side (0x45, both sides for a fence), a wall needs both sides on
// the lot and a fence one (0x40), grid points with a locked tile round them
// stay (0x62 in the game), and Buy mode refuses a locked tile (0x02).
static void testLockedTiles() {
    Lot lot;
    lot.width = 20, lot.height = 20, lot.levelCount = 1;
    lot.elevations.assign(21 * 21, 0.0f);
    lot.floors.assign(20 * 20, {});
    lot.rooms.assign(20 * 20, {});
    lot.roomLayers[0x0A] = lot.rooms;
    lot.roomLayers[0x0B] = lot.rooms;
    lot.locked.assign(20 * 20, 0);
    for (int x = 0; x < 20; ++x)
        for (int y = 15; y < 20; ++y)
            lot.locked[size_t(x) * 20 + size_t(y)] = 1;
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    check(vm.world->locked({3, 15, 1}) && !vm.world->locked({3, 14, 1}), "the world knows the locked tiles");
    auto r = ed.drawWalls(1, 2, 16, 5, 16);
    check(!r.ok && r.error == 0x62, "no walls on locked tiles (0x62, the leveller's edge test)");
    r = ed.drawWalls(1, 2, 15, 5, 15);
    check(!r.ok && r.error == 0x62, "a wall with one locked side (0x62)");
    BuildFence fence;
    fence.guid = 0x2CB2A001, fence.price = 10;
    r = ed.drawFences(1, 2, 15, 5, 15, fence);
    check(r.ok && r.changed == 3, "a fence needs one side free");
    r = ed.drawWalls(1, 0, 2, 0, 5);
    check(!r.ok && r.error == 0x62, "a wall on the lot's edge: its points are on the world's edge (0x62)");
    r = ed.drawFences(1, 0, 2, 0, 5, fence);
    check(r.ok && r.changed == 3, "a fence on the lot's edge");
    check(!ed.editablePoint(4, 15) && ed.editablePoint(4, 14) && !ed.editablePoint(0, 4),
          "grid points next to a locked tile or on the edge do not move");
}

// Fence arches: on a fence, not beside another, gone with the fence.
// Round n20 (docs/buy-build-mac.md §7.24): the room tool levels the whole
// rectangle (FUN_009a7dd8), the walls' leveller (FUN_009a5a68 with the
// vertex test's 0x2BB7 flags) refuses water (0x47) and floors round a point
// (0x5A), and a drag that meets a refusal builds nothing.
static void testRoundN20() {
    Lot lot = flatLot();
    for (int x = 0; x <= 12; ++x) // a slope along x: elevation = x / 4
        for (int y = 0; y <= 10; ++y)
            lot.elevations[size_t(x) * 11 + size_t(y)] = float(x) * 0.25f;
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1;
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    const std::vector<float> slope = lot.elevations;
    auto r = ed.drawRoom(1, 2, 2, 5, 5);
    bool flat = true;
    for (int x = 2; x <= 5; ++x)
        for (int y = 2; y <= 5; ++y)
            flat &= lot.elevation(0, x, y) == 0.5f;
    check(r.ok && r.changed == 12 && flat, "a room levels every grid point of its rectangle to the start's height");
    check(lot.elevation(0, 6, 3) == 1.5f && lot.elevation(0, 1, 3) == 0.25f, "and nothing outside it");
    check(ed.undo() && lot.elevations == slope && lot.walls.empty(), "undo gives the slope back");
    // A floor inside on the slope holds its points: the room is refused.
    lot.floors[size_t(3) * 10 + 3] = {1, 1, 1, 1};
    r = ed.drawRoom(1, 2, 2, 6, 6);
    check(!r.ok && r.error == 0x5A && lot.walls.empty() && lot.elevations == slope,
          "a floor round a point to level refuses the room (0x5A), nothing built");
    lot.floors[size_t(3) * 10 + 3] = {};
    // A point under water (IsWaterVertex) refuses, even at the height: a
    // pond in a dip at grid point (4, 4) as UpdateWaterElevations leaves one
    // (its water 0.25 under its lowest rim, 0.75; its neighbours' 0.1 under
    // that; the others' 1 under the ground). The run's first section moves
    // (3, 4), the pond's rim: its water follows, 0.25 under the new rim.
    std::vector<float> dipped = slope;
    dipped[size_t(4) * 11 + 4] = 0.0f;
    lot.elevations = dipped;
    lot.water.assign(13 * 11, 0.0f);
    for (int y = 0; y <= 10; ++y)
        for (int x = 0; x <= 12; ++x)
            lot.water[size_t(y) * 13 + size_t(x)] =
                std::abs(x - 4) <= 1 && std::abs(y - 4) <= 1 ? 0.4f : lot.elevation(0, x, y) - 1.0f;
    lot.water[size_t(4) * 13 + 4] = 0.5f;
    const std::vector<float> pond = lot.water;
    r = ed.drawRoom(1, 4, 2, 6, 6);
    check(!r.ok && r.error == 0x47 && lot.walls.empty(), "a point under water refuses the room (0x47)");
    r = ed.drawWalls(1, 2, 4, 6, 4);
    check(!r.ok && r.error == 0x47 && lot.walls.empty() && lot.elevations == dipped && lot.water == pond,
          "a wall over water is refused whole: the sections before it are not built either");
    // A wall that lowers a point of the pond's rim (3, 5) to 0.5: the pond's
    // water goes 0.25 under it (ChooseWaterElevationsForPondWithVertex), the
    // rim's points 0.1 under the pond's (AdjustWaterElevationsAtNonPondVertices).
    r = ed.drawWalls(1, 2, 5, 3, 5);
    check(r.ok && lot.elevation(0, 3, 5) == 0.5f && lot.waterLevel(4, 4) == 0.25f && lot.waterLevel(3, 5) == 0.25f - 0.1f &&
              lot.waterLevel(5, 3) == 0.25f - 0.1f && lot.waterLevel(8, 8) == lot.elevation(0, 8, 8) - 1.0f,
          "the ground's water follows a moved point (UpdateWaterElevations with the floor flag)");
    check(ed.undo() && lot.water == pond && lot.elevations == dipped, "undo gives the pond back");
    lot.water.clear();
    lot.elevations = slope;
    // A wall whose end meets a wall on another height: that wall's point
    // cannot move (0x5B); the drag builds nothing.
    r = ed.drawWalls(1, 8, 1, 8, 3);
    check(r.ok && r.changed == 2, "a wall across the slope");
    r = ed.drawWalls(1, 6, 2, 9, 2);
    check(!r.ok && r.error == 0x5B && lot.walls.size() == 2,
          "a run that would level the point of another wall is refused whole (0x5B)");
}

// The game's one undo manager over Buy and Build mode (app/undo_order.hpp,
// docs/buy-build-mac.md §7.24): the last change of either is undone first,
// a new change in one releases the other's redo list, Live mode flushes.
static void testUndoOrder() {
    UndoOrder order;
    std::vector<int> buy, build, buyRedo, buildRedo; // each mode's own lists
    int flushed = 0;
    auto hooks = [&](std::vector<int>& u, std::vector<int>& r) {
        UndoOrder::Hooks h;
        h.undo = [&] { return u.empty() ? false : (r.push_back(u.back()), u.pop_back(), true); };
        h.redo = [&] { return r.empty() ? false : (u.push_back(r.back()), r.pop_back(), true); };
        h.dropRedo = [&] { r.clear(); };
        h.flush = [&] { u.clear(), r.clear(), ++flushed; };
        return h;
    };
    order.attach(UndoOrder::Buy, hooks(buy, buyRedo));
    order.attach(UndoOrder::Build, hooks(build, buildRedo));
    buy.push_back(1), order.pushed(UndoOrder::Buy);     // a chair bought
    build.push_back(2), order.pushed(UndoOrder::Build); // then a wall
    check(order.undo() && build.empty() && buy.size() == 1, "undo takes the last change back, Build mode's wall");
    check(order.undo() && buy.empty() && buyRedo == std::vector<int>{1}, "then Buy mode's chair, from Build mode");
    check(order.redo() && buy == std::vector<int>{1} && buildRedo == std::vector<int>{2}, "redo in the same order");
    buy.push_back(3), order.pushed(UndoOrder::Buy); // a new change releases both redo lists
    check(buildRedo.empty() && !order.canRedo() && !order.redo(), "a new change in Buy mode drops Build mode's redo");
    order.flush();
    check(flushed == 2 && buy.empty() && build.empty() && !order.canUndo(), "Live mode flushes both");
}

static void testFenceArch() {
    Lot lot = flatLot();
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    BuildFence fence;
    fence.guid = 0x2CB2A001, fence.price = 10, fence.rail = "rail", fence.post = "post";
    BuildFenceArch arch;
    arch.id = 26, arch.price = 50, arch.name = "arch";
    // UserMayPlacePartition: no fence needed (0x79 and 0x88 are the errors
    // of objects that hang on fences).
    auto r = ed.placeFenceArch(1, 3, 6, 4, 6, arch);
    check(r.ok && fenceArches(lot).size() == 1, "an arch stands without a fence");
    ed.drawFences(1, 1, 2, 7, 2, fence);
    r = ed.placeFenceArch(1, 3, 2, 4, 2, arch);
    check(r.ok && r.cost == 50 && fenceArches(lot).size() == 2, "an arch on the fence for its price");
    r = ed.placeFenceArch(1, 4, 2, 5, 2, arch);
    check(!r.ok && r.error == 0x0C, "not next to another arch (MayPlaceFenceArchWithRespectToObjects: 0x0C)");
    check(ed.fenceArchBlocked(0, 0, 3, 0, 4) == 0x45, "an edge on the lot's outer ring is off the lot (0x45)");
    check(ed.fenceArchBlocked(0, 6, 2, 7, 2) == 0, "an edge clear of arches and objects may take one");
    check(vm.world->wallBetween({3, 1, 1}, 0, 1, true), "the fence still blocks under the arch");
    // FUN_009a6b20: a wall ending at an arch's post (0x4D), an arch next to
    // a wall (0x50) or on one (0x41).
    r = ed.drawWalls(1, 4, 6, 4, 8);
    check(!r.ok && r.error == 0x4D, "no wall next to an arch (0x4D)");
    r = ed.drawWalls(1, 8, 4, 8, 8);
    r = ed.placeFenceArch(1, 8, 5, 8, 6, arch);
    check(!r.ok && r.error == 0x41, "no arch on a wall (0x41)");
    r = ed.placeFenceArch(1, 7, 5, 8, 5, arch);
    check(!r.ok && r.error == 0x50, "no arch next to a wall (0x50)");
    r = ed.placeFenceArch(1, 3, 4, 4, 5, arch);
    check(!r.ok && r.error == 0x4C, "no diagonal arch (0x4C)");
    ed.deleteFences(1, 3, 2, 4, 2);
    check(fenceArches(lot).size() == 2, "the arch stays when its fence goes");
    check(ed.undo() && fenceArches(lot).size() == 2, "undo brings the fence back");
}

// The auto-roof's rectangles (ComputeRoofRects, 0x9a0a08).
static void testAutoRoofRects() {
    const int W = 8, H = 6;
    std::vector<uint8_t> grid(W * H, 0);
    auto mark = [&](int x0, int y0, int x1, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
                grid[size_t(y * W + x)] = 1;
    };
    mark(0, 0, 6, 2); // an L: two rows six wide, then two wide down to row 5
    mark(0, 2, 2, 5);
    auto r = autoRoofRects(grid, W, H);
    check(r.size() == 2, "an L gives two rectangles");
    bool wide = false, tall = false;
    for (const auto& q : r) {
        wide |= q == std::array<int, 4>{0, 0, 6, 2};
        tall |= q == std::array<int, 4>{0, 0, 2, 5}; // the overlap grows down from the top
    }
    check(wide && tall, "the narrower part overlaps the wider one from its top");
    std::fill(grid.begin(), grid.end(), 0);
    mark(1, 1, 4, 4);
    r = autoRoofRects(grid, W, H);
    check(r.size() == 1 && r[0] == std::array<int, 4>({1, 1, 4, 4}), "a square room is one rectangle");
}

static void testPoolOverFloor() {
    // A pool on flat floored ground: MayPlaceQuadrilateral tests the floor
    // only through the vertex test of grid points off the tool's height, so
    // the pool goes over it and the floor is hidden; deleting the pool shows
    // it again.
    Lot lot = flatLot();
    CodeLibrary code;
    VM vm(code);
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    for (int q = 0; q < 4; ++q)
        setFloor(lot, 0, 4, 4, q, 1);
    auto r = ed.drawPool(3, 3, 5, 5);
    check(r.ok && lot.floor(0, 4, 4)[0] == 1 && !floorVisible(lot, 0, 4, 4, 2), "a pool over a ground floor keeps it hidden");
    r = ed.deletePool(4, 4);
    check(r.ok && lot.pools.empty() && floorVisible(lot, 0, 4, 4, 2), "the deleted pool's ground shows again");
}

static void testPool() {
    Lot lot = flatLot();
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    auto r = ed.drawPool(3, 3, 5, 4);
    check(r.ok && r.cost == 6 * kPoolTilePrice && lot.pools.size() == 1, "a pool of six tiles at §75");
    check(lot.minLevel == -1 && lot.levelCount == 2 && lot.elevation(-1, 4, 4) == -3.0f, "the lot gets its underground level");
    check(lot.floor(-1, 4, 3)[0] != 0 && lot.floor(0, 4, 3)[0] == 0, "the pool's floor is a level down");
    check(lot.deckFoundationBase == -1 && lot.deckFoundationStyle(-1, 3, 3, 0) == 3 &&
              lot.deckFoundationStyle(-1, 5, 4, 2) == 3 && lot.deckFoundationStyle(-1, 6, 4, 2) == -1 &&
              lot.deckFoundationStyle(0, 4, 4, 0) == -1,
          "the pool's markers (product 3) a level down");
    size_t walls = 0;
    for (const auto& w : lot.walls)
        walls += w.type == Lot::Pool && w.a.level == -1;
    check(walls == 10, "pool walls round it");
    check(vm.world->pool({4, 3, 1}) && !vm.world->walkable({4, 3, 1}), "nobody walks over the water");
    check(!floorVisible(lot, 0, 4, 3, 0) && !floorVisible(lot, 0, 5, 4, 3) && floorVisible(lot, 0, 6, 4, 0) &&
              floorVisible(lot, -1, 4, 3, 0),
          "ModifyFloorVisibility: the ground over the pool hidden (3ARY 0x15), the bottom seen");
    // cWorldPool::AreExpansionBoundsLegal: a drag over the pool, or beside
    // it without making one rectangle with it, is refused; one along a
    // whole side extends it (a POOL entry of its own); 900 tiles at most.
    r = ed.drawPool(5, 4, 6, 6);
    check(!r.ok && r.error == 0x86, "a drag over a pool's corner is refused (0x86)");
    r = ed.drawPool(6, 4, 7, 6);
    check(!r.ok && r.error == 0x86, "a drag beside the pool that would make an L is refused (0x86)");
    r = ed.drawPool(6, 3, 8, 4);
    check(r.ok && r.changed == 6 && r.cost == 6 * kPoolTilePrice && lot.pools.size() == 2,
          "a drag along the pool's whole side extends it, a POOL entry of its own");
    size_t walls2 = 0;
    for (const auto& w : lot.walls)
        walls2 += w.type == Lot::Pool && w.a.level == -1;
    check(walls2 == 16 && vm.world->pool({8, 4, 1}), "pool walls round the whole rectangle");
    // The cursor on a dug pool: the ray lands on the bottom the player sees
    // (3 under the lawn), not the lawn tile 2 tiles behind; off the pool it
    // is the plain ground. A pool drag off the lot, over the road or onto an
    // object is refused with the game's code, which the preview paints red.
    {
        const std::array<float, 3> dir{0.5f, 0.0f, -0.866f}; // 60 degrees down, along x
        const float bx = 6.5f, by = 3.5f, bz = -3.0f;          // a point of the pool's bottom
        const float tt = 20.0f;
        const auto hit = pickLotGround(lot, 1, {bx - dir[0] * tt, by - dir[1] * tt, bz - dir[2] * tt}, dir);
        check(hit && std::fabs((*hit)[0] - bx) < 0.05f && std::fabs((*hit)[1] - by) < 0.05f,
              "a click inside a dug pool picks the tile on its bottom");
        const float lz = 0.0f, lx = 10.5f, ly = 8.5f;
        const auto lawn = pickLotGround(lot, 1, {lx - dir[0] * tt, ly - dir[1] * tt, lz - dir[2] * tt}, dir);
        check(lawn && std::fabs((*lawn)[0] - lx) < 0.05f && std::fabs((*lawn)[1] - ly) < 0.05f,
              "off the pool it is the lawn");
        // The pool tool picks the water (0.25 under the ground), where its
        // cursor is drawn: under the mouse over the water (QA pass 11).
        const float wz = -0.25f;
        const auto water = pickLotGround(lot, 1, {bx - dir[0] * tt, by - dir[1] * tt, wz - dir[2] * tt}, dir, true);
        check(water && std::fabs((*water)[0] - bx) < 0.05f && std::fabs((*water)[1] - by) < 0.05f &&
                  std::fabs(lotPickHeight(lot, 1, bx, by, true) - wz) < 1e-4f &&
                  std::fabs(lotPickHeight(lot, 1, bx, by) - bz) < 1e-4f,
              "the pool tool's pick and cursor height are the water's");
    }
    {
        using namespace placementerror;
        check(stringNumber(0x02) == 1 && stringNumber(0x86) == 78 && stringNumber(0x8A) == 90 && stringNumber(0x81) == 12 &&
                  stringNumber(0x87) == 95 && stringNumber(0x2D) == 44 && stringNumber(0x2E) == 45 &&
                  stringNumber(0x0C) == 11 && stringNumber(0x16) == 21 && stringNumber(0x19) == 91 &&
                  stringNumber(0x4B) == 75 && stringNumber(0x2F) == 8 && stringNumber(0x13) == 32,
              "PlacementErrorString's table: code -> STR# 0x89 entry");
        check(stringNumber(0x1E) == 0 && textIndex(0x1E) == -1 && stringNumber(0xFE) == 1 && textIndex(0x86) == 77,
              "empty texts and codes out of the table");
    }
    check(ed.poolDigError(-1, 3, 2, 4) == 0x02 && ed.poolDigError(3, 3, 12, 4) == 0x02, "a pool drag off the lot is refused (0x02)");
    check(ed.poolDigError(10, 6, 11, 7) == 0 && ed.poolDigError(5, 4, 6, 5) == 0x86, "the preview's check agrees with drawPool");
    // Ctrl-drag: IsQuadLegalForPoolErase / QuadListOnPoolErase.
    r = ed.erasePool(4, 3, 4, 3);
    check(!r.ok && r.error == 0x87, "an erase must cross a pool through (0x87)");
    r = ed.erasePool(4, 3, 4, 4);
    check(!r.ok && r.error == 0x87, "an erase may not leave a strip one tile wide (0x87)");
    r = ed.erasePool(8, 3, 8, 4);
    check(r.ok && r.changed == 2 && !vm.world->pool({8, 4, 1}) && lot.pools.size() == 2, "erasing the pool's end");
    r = ed.deletePool(4, 4);
    check(r.ok && lot.pools.empty() && r.cost == -10 * 60, "deleting it takes the whole pool, int(75 x 0.8) a tile");
    {
        // Room value 4 (pool): the room's one tile from FloorTileInRoom is
        // tested, not every tile. A pool in the yard (the yard's room, as the
        // saved room arrays have it) leaves the yard no pool; a room that
        // starts on a pool tile is one.
        Lot yard = flatLot();
        Lot::SwimmingPool p;
        p.x = 3, p.y = 3, p.width = 3, p.height = 2, p.level = 0;
        yard.pools.push_back(p);
        VM v(code);
        setLotRooms(v, yard);
        check(roomValue(v, 1, 4) == 0, "a pool in the yard: the yard's room is no pool");
        for (uint32_t x = 3; x < 6; ++x)
            for (uint32_t y = 3; y < 5; ++y)
                yard.rooms[size_t(x) * yard.height + y] = {2, 2, 2, 2};
        setLotRooms(v, yard);
        check(roomValue(v, 2, 4) == 1 && roomValue(v, 1, 4) == 0, "a room of the pool's tiles is a pool");
    }
    check(ed.undo() && ed.undo() && ed.undo() && ed.undo() && lot.minLevel == 0 && lot.levelCount == 1 && funds(vm) == 5000,
          "undo takes the level away again");
    Lot big = flatLot();
    big.width = 40, big.height = 40;
    big.elevations.assign(41 * 41, 0.0f);
    big.floors.assign(40 * 40, {});
    big.rooms.assign(40 * 40, {1, 1, 1, 1});
    big.roomLayers[0x0A] = big.rooms;
    big.roomLayers[0x0B] = big.rooms;
    VM vm2(code);
    vm2.globals[global::BudgetDiv10000] = 10;
    vm2.world = World::fromLot(big);
    BuildEditor ed2(vm2, big);
    r = ed2.drawPool(1, 1, 30, 30);
    check(r.ok, "a pool of 900 tiles");
    r = ed2.drawPool(31, 1, 31, 30);
    check(!r.ok && r.error == 0x86, "no pool over 900 tiles (0x86)");
}

// Modular stairs (§7.12): how many steps a flight gets, and the refusals.
// (The objects need the game's OBJDs: lot-save --stairs checks those.)
static void testStairs() {
    Lot lot = flatLot();
    lot.levelCount = 3;
    lot.elevations.resize(13 * 11 * 3);
    for (size_t i = 0; i < 13 * 11 * 3; ++i)
        lot.elevations[i] = 3.0f * float(i / (13 * 11));
    lot.floors.resize(12 * 10 * 3);
    lot.rooms.resize(12 * 10 * 3);
    for (auto& [k, l] : lot.roomLayers)
        l.resize(12 * 10 * 3);
    lot.floorPatterns[2] = "ccc26098";
    for (int x = 2; x <= 4; ++x)
        for (int y = 2; y <= 3; ++y)
            for (int q = 0; q < 4; ++q)
                setFloor(lot, 1, x, y, q, 2);
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetMod10000] = 5000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1; // a family's lot: buy and build charge
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    int error = 0;
    check(ed.stairsSteps({4, 2, 2}, 4, std::nullopt, false, &error) == 16 && error == 0,
          "a story down to the ground in 16 steps, the end found by itself");
    check(ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{9.0f, 2.5f}, false) == 16, "dragged to where the last step goes");
    check(ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{6.2f, 2.5f}, true) == 8, "whole tiles round up to 4 a tile");
    // A short flight: the tile past its last step is brought to it (the
    // binary sets that tile's corners to the last step's height).
    check(ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{6.2f, 2.5f}, false, &error) == 5 && error == 0,
          "a short drag makes a short flight");
    ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{4.6f, 2.5f}, false, &error);
    check(error == 0x78, "no steps on the top tile itself (0x78)");
    ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{7.5f, 5.5f}, false, &error);
    check(error == 0x71, "the end off the line of the flight (0x71)");
    ed.stairsSteps({7, 7, 2}, 4, std::nullopt, false, &error);
    check(error == 0x6C, "no floor on the top tile (0x6C)");
    // FindEnd (BV 0x9ece38): ground raised to 1.5 from x = 5 on meets the
    // flight in its second tile; the end is that tile's far edge, 8 steps.
    {
        const std::vector<float> keep = lot.elevations;
        for (int x = 5; x <= 12; ++x)
            for (int y = 0; y <= 10; ++y)
                lot.elevations[size_t(x) * 11 + size_t(y)] = 1.5f;
        check(ed.stairsSteps({4, 2, 2}, 4, std::nullopt, false, &error) == 8 && error == 0,
              "the end found where the raised ground meets the flight");
        for (int x = 5; x <= 12; ++x)
            for (int y = 0; y <= 10; ++y)
                lot.elevations[size_t(x) * 11 + size_t(y)] = 2.25f;
        // Ground four steps under the top: the first tile meets it and goes
        // on (its far corners are there already), the second takes
        // round((2.25 - 1.5) / 0.1875) = 4 steps off.
        check(ed.stairsSteps({4, 2, 2}, 4, std::nullopt, false, &error) == 4 && error == 0,
              "a floor four steps down: the first tile's four steps");
        lot.elevations = keep;
    }
    // A hill under the flight: the ground over the steps is lowered.
    for (int x = 6; x <= 7; ++x)
        for (int y = 0; y <= 10; ++y)
            lot.elevations[size_t(x) * 11 + size_t(y)] = 0.5f;
    check(ed.stairsSteps({4, 2, 2}, 4, std::nullopt, false, &error) > 0 && error == 0, "a flight down a slope");
    ed.drawWalls(1, 7, 1, 7, 4);
    ed.stairsSteps({4, 2, 2}, 4, std::array<float, 2>{9.0f, 2.5f}, false, &error);
    check(error == 0x76 || error == 0x6F, "a wall across the flight stops it");
}

// The Design tool's choices (MMAT property sets): the model's first two
// subsets with choices, clean materials only, the default first.
static void testDesignCatalog() {
    DesignCatalog c;
    // (defaultMaterial is a bool property: readPropertySet writes "true"/"false".)
    auto mmat = [&](const char* subset, const char* name, int def, int state) {
        c.add({{"type", "modelMaterial"}, {"modelName", "sofaTest_cres"}, {"subsetName", subset}, {"name", name},
               {"defaultMaterial", def ? "true" : "false"}, {"materialStateFlags", std::to_string(state)}, {"family", name}});
    };
    mmat("wood", "sofatest_wood_dark", 0, 0);
    mmat("cushion", "sofatest_cushion_red", 1, 0);
    mmat("cushion", "sofatest_cushion_blue", 0, 0);
    mmat("cushion", "sofatest_cushion_blue_dirty", 0, 2);
    mmat("legs", "sofatest_legs_steel", 1, 0);
    const auto subsets = c.subsets("SofaTest", {"groundshadow", "Cushion", "wood", "legs"});
    check(subsets.size() == 2 && subsets[0] == "cushion" && subsets[1] == "wood", "the first two subsets with choices, in the model's order");
    const auto opts = c.options("sofatest_cres", "cushion");
    check(opts.size() == 2 && opts[0]->material == "sofatest_cushion_red" && opts[1]->material == "sofatest_cushion_blue",
          "clean choices, the default first");
    check(c.subsets("other", {}).empty(), "a model without choices is not designable");
}

// Low fences (maywalkover) bound placement but Sims walk over them.
static void testWalkOver() {
    Lot lot = flatLot();
    addWall(lot, 0, 2, 2, 3, 2, 0x1234567, {0xFFFF, 0xFFFF}); // a walk-over fence
    addWall(lot, 0, 5, 2, 6, 2, 0x7654321, {0xFFFF, 0xFFFF}); // a tall one
    setWalkOverFences({0x1234567});
    const World w = World::fromLot(lot);
    check(!w.wallBetween({2, 2, 1}, 0, -1) && w.wallBetween({2, 2, 1}, 0, -1, true),
          "a walk-over fence does not block routing but blocks placement");
    check(w.wallBetween({5, 2, 1}, 0, -1), "another fence blocks routing");
    setWalkOverFences({});
}

// A lot with no family (an empty lot or a house for sale entered from the
// neighborhood): building costs nothing and gives nothing back (the
// undoables' "no charge", Mac BV 0x9b47a4; TotalCost 0); a selected Sim on
// the lot charges again.
static void testNoCharge() {
    Lot lot = flatLot();
    CodeLibrary code;
    VM vm(code);
    vm.world = World::fromLot(lot);
    check(noCharge(vm) && canAfford(vm, 1000000) && funds(vm) == 0, "no family: no charge");
    BuildEditor ed(vm, lot);
    auto r = ed.drawRoom(1, 2, 2, 6, 5);
    check(r.ok && r.changed == 14 && funds(vm) == 0, "a room with §0 on a lot with no family, funds unchanged");
    BuildPattern carpet;
    carpet.guid = 0x1234, carpet.floor = true, carpet.price = 8, carpet.name = "carpet";
    r = ed.paintFloor(1, 2, 2, 3, 2, &carpet);
    check(r.ok && r.changed == 8 && funds(vm) == 0, "floors too");
    r = ed.deleteWalls(1, 2, 2, 6, 2);
    check(r.ok && r.changed == 4 && funds(vm) == 0, "deleting walls gives nothing back");
    std::vector<uint8_t> profile(32 * 32, 255);
    r = ed.terrainStroke(BuildEditor::TerrainOp::Raise, 8, 8, 10, 1, profile);
    check(r.ok && r.changed > 0 && funds(vm) == 0, "terrain too");
    check(ed.undo() && ed.undo() && funds(vm) == 0, "undo pays nothing back");
    vm.families[3];
    vm.globals[global::CurrentFamily] = 3;
    check(!noCharge(vm) && !canAfford(vm, 1) && canAfford(vm, 0), "a family's lot charges (CanAfford: cost <= 0 or <= the funds)");
}

// Round n37 (docs/buy-build-mac.md §7.25): undo gives the whole price back
// and redo charges it again without a funds test (AdjustFunds ->
// cTSSimulator::Spend; RedoIt asks nothing): the funds may go below zero.
static void testRedoFunds() {
    Lot lot = flatLot();
    CodeLibrary code;
    VM vm(code);
    vm.globals[global::BudgetDiv10000] = 0;
    vm.globals[global::BudgetMod10000] = 1000;
    vm.families[1], vm.globals[global::CurrentFamily] = 1;
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    auto r = ed.drawWalls(1, 2, 2, 6, 2);
    check(r.ok && r.cost == 4 * kWallSectionPrice && funds(vm) == 1000 - 280, "four walls paid");
    check(ed.undo() && funds(vm) == 1000, "undo gives the whole price back");
    addFunds(vm, -900);
    check(funds(vm) == 100 && !canAfford(vm, 280), "the funds spent elsewhere");
    check(ed.redo() && funds(vm) == 100 - 280, "redo is not refused for money: the funds go below zero");
    check(lot.walls.size() == 4, "the walls are back");
}

// The vertex test's bit 7 (FUN_009e8e0c, docs/buy-build-mac.md §7.25): the
// objects on the four tiles round a grid point.
static void testVertexObjects() {
    Lot lot = flatLot();
    lot.levelCount = 2;
    lot.elevations.resize(13 * 11 * 2);
    for (size_t i = 0; i < 13 * 11 * 2; ++i)
        lot.elevations[i] = 3.0f * float(i / (13 * 11));
    lot.floors.resize(12 * 10 * 2);
    lot.rooms.resize(12 * 10 * 2);
    for (auto& [k, l] : lot.roomLayers)
        l.resize(12 * 10 * 2);
    CodeLibrary code;
    VM vm(code);
    vm.world = World::fromLot(lot);
    BuildEditor ed(vm, lot);
    using namespace vertextest;
    Object& chair = vm.createObject(0x7F000001);
    vm.setPosition(chair, {3.5f, 3.5f, 1});
    check(ed.vertexMoveError(0, 3, 3, 0.5f, kWalls) == 0x61, "an object pins the ground round it (0x61)");
    check(ed.vertexMoveError(0, 5, 5, 0.5f, kWalls) == 0, "two points off it does not");
    check(ed.vertexMoveError(0, 3, 3, 0.0f, kWalls) == 0, "a point already at the height is not tested");
    check(ed.vertexMoveError(0, 3, 3, 0.5f, kWalls & ~Objects) == 0, "only with the flags' bit 7");
    chair.data[objdata::PlacementFlags] = 0x100;
    check(ed.vertexMoveError(0, 3, 3, 0.5f, kWalls) == 0, "od 42 bit 0x100 lets the ground move");
    // The story above brought within a story of the ground: the objects
    // under it must conform to the ceiling (0x60).
    check(ed.vertexMoveError(1, 3, 3, 0.75f, kWalls) == 0x60, "a ceiling brought down onto an object (0x60)");
    check(ed.vertexMoveError(1, 3, 3, 3.5f, kWalls) == 0, "raised, it does not mind");
    // od 42 bit 0x80: the floor may not rise over it (0x68).
    chair.data[objdata::PlacementFlags] = 0x180;
    chair.position.elevation = 0.1f;
    check(ed.vertexMoveError(0, 3, 3, 1.0f, kWalls) == 0x68, "the floor raised over an od 42 0x80 object (0x68)");
    check(ed.vertexMoveError(0, 3, 3, -1.0f, kWalls) == 0, "lowered under it, fine");
    chair.person.emplace();
    check(ed.vertexMoveError(0, 3, 3, -1.0f, kWalls) == 0x61, "a person always pins the ground");
    vm.removeObject(chair.id);
}

// Foundations, decks and pools on a slope as the game's deck tool levels
// them (docs/buy-build-mac.md §7.25): the tool's height is the start's
// ground + 0.75 (a pool: + 0), every grid point of the rectangle not at it
// must pass the vertex test (0x81), the raised level goes to it one grid
// point past the rectangle too, and the ground under a deck only comes down.
static void testDeckLevelling() {
    auto sloped = [](int levels) {
        Lot lot = flatLot();
        lot.levelCount = levels;
        lot.elevations.resize(13 * 11 * size_t(levels));
        for (int k = 0; k < levels; ++k)
            for (int x = 0; x <= 12; ++x)
                for (int y = 0; y <= 10; ++y)
                    lot.elevations[size_t(k) * 13 * 11 + size_t(x) * 11 + size_t(y)] = 0.1875f * float(x) + 3.0f * float(k);
        lot.floors.resize(12 * 10 * size_t(levels));
        lot.rooms.resize(12 * 10 * size_t(levels));
        for (auto& [k, l] : lot.roomLayers)
            l.resize(12 * 10 * size_t(levels));
        return lot;
    };
    {
        Lot lot = sloped(2);
        CodeLibrary code;
        VM vm(code);
        vm.globals[global::BudgetMod10000] = 5000;
        vm.families[1], vm.globals[global::CurrentFamily] = 1;
        vm.world = World::fromLot(lot);
        BuildEditor ed(vm, lot);
        // A chair on the ground by the rectangle's edge: the edge's points
        // of the level above would come down onto it (0x60): 0x81.
        Object& chair = vm.createObject(0x7F000001);
        vm.setPosition(chair, {5.5f, 2.5f, 1});
        auto r = ed.drawFoundation(2, 2, 4, 3);
        check(!r.ok && r.error == 0x81 && lot.elevation(1, 3, 3) == 3.5625f, "an object under the edge refuses it (0x81)");
        vm.setPosition(chair, {6.5f, 2.5f, 1}); // by the ring: those points stay
        r = ed.drawFoundation(2, 2, 4, 3);
        check(r.ok && r.cost == 6 * kFoundationTilePrice, "the foundation on the slope");
        const float top = 0.375f + 0.75f;
        bool flat = true;
        for (int x = 2; x <= 5; ++x)
            for (int y = 2; y <= 4; ++y)
                flat &= lot.elevation(1, x, y) == top;
        check(flat, "its floor is flat at the start's ground + 0.75");
        check(lot.elevation(1, 1, 3) == top && lot.elevation(1, 6, 1) == top && lot.elevation(1, 6, 5) == top,
              "the level above goes to it one grid point past the rectangle");
        check(lot.elevation(1, 7, 3) == 0.1875f * 7 + 3.0f, "two points past it does not");
        check(lot.elevation(1, 6, 2) != top && lot.elevation(1, 6, 3) != top, "the ring points by the chair stay");
        check(lot.elevation(0, 2, 3) == 0.375f && lot.elevation(0, 1, 3) == 0.1875f, "the ground under its low side stays");
        check(lot.elevation(0, 4, 3) == 0.375f && lot.elevation(0, 5, 3) == 0.375f && lot.elevation(0, 6, 4) == 0.375f &&
                  lot.elevation(0, 6, 3) == 1.125f,
              "the ground under its high side comes down to the floor - 0.75 (not by the chair)");
        check(ed.undo() && lot.elevation(1, 3, 3) == 3.5625f && lot.elevation(0, 5, 3) == 0.9375f, "undo puts the slope back");
        // A deck started on a foundation's edge takes the foundation's height.
        ed.redo();
        vm.removeObject(chair.id);
        r = ed.drawFoundation(5, 2, 6, 3, 1);
        check(r.ok && r.cost == 2 * 2 * kLatticeDeckTilePrice && lot.elevation(1, 7, 3) == top,
              "a deck from the foundation's edge at its height");
        check(lot.deckFoundationStyle(0, 6, 3, 0) == 1 && lot.deckFoundationStyle(0, 4, 3, 0) == 0, "each tile its product");
        // Over the foundation: the quarters that change cost the deck less
        // 0.8 of the foundation (3 - 3) / 4 each.
        r = ed.drawFoundation(4, 2, 4, 3, 1);
        check(r.ok && r.changed == 0, "the deck over foundation tiles lays nothing new");
        // Deleting: the tiles' products x 0.8 back by the quarters.
        r = ed.deleteFoundation(5, 2, 6, 3);
        check(r.ok && r.cost == -4 * 2 && lot.elevation(1, 7, 3) == lot.elevation(0, 7, 3) + 3.0f &&
                  lot.elevation(0, 7, 3) == 0.375f,
              "the deck goes for int(3 x 0.8) a tile, its points back a story over the ground (left cut down)");
        check(lot.elevation(1, 5, 3) == top, "the foundation's edge stays (its floor holds it)");
    }
    {
        Lot lot = sloped(1);
        CodeLibrary code;
        VM vm(code);
        vm.globals[global::BudgetMod10000] = 5000;
        vm.families[1], vm.globals[global::CurrentFamily] = 1;
        vm.world = World::fromLot(lot);
        BuildEditor ed(vm, lot);
        auto r = ed.drawPool(2, 2, 4, 3);
        check(r.ok && r.cost == 6 * kPoolTilePrice, "a pool on the slope");
        check(lot.elevation(0, 5, 3) == 0.375f && lot.elevation(0, 6, 3) == 0.375f && lot.elevation(0, 1, 3) == 0.375f,
              "the ground levelled to the start's, one grid point past the pool too");
        check(lot.elevation(0, 7, 3) == 0.1875f * 7, "not two");
        check(lot.elevation(-1, 3, 3) == 0.375f - 3.0f && lot.elevation(-1, 6, 3) == 0.375f - 3.0f, "the bottom a story down");
        // The start on the far side: the pool takes that corner's ground.
        r = ed.drawPool(9, 6, 8, 7);
        check(r.ok && lot.elevation(0, 8, 6) == 0.1875f * 10, "a drag from its right takes the right corner's height");
        // TestPlacementCommon's floor test (FUN_0016d518 with its pool
        // argument, FUN_00164c40): over a pool's marker there is no floor,
        // even on the ground (0x8E); in the air (0x200) or p2 & 4 it may.
        ObjectDefinition chairDef;
        chairDef.fields.resize(60);
        chairDef.fields[(0x5C - 0x44) / 2] = 0x0002, chairDef.fields[(0x5E - 0x44) / 2] = 0x7F00;
        code.add(0x7F7F7F7F, 0x80, chairDef);
        Object& lawnChair = vm.instantiate(0x7F7F7F7F, 0x80);
        lawnChair.data[objdata::PlacementFlags] = 0x3;
        lawnChair.data[objdata::AllowedHeightFlags] = 0x1;
        vm.placementLot = &lot;
        Placement p = checkPlacement(vm, lawnChair.id, {3, 3, 1}, 0);
        check(!p.ok && p.error == placementerror::NoFloor, "an object over the pool: no floor (0x8E)");
        p = checkPlacement(vm, lawnChair.id, {3, 4, 1}, 0);
        check(p.ok, "beside it, on the ground the pool levelled");
        lawnChair.data[objdata::PlacementFlags2] = 0x4;
        p = checkPlacement(vm, lawnChair.id, {3, 3, 1}, 0);
        check(!p.ok && p.error == 0x8F, "with p2 & 4 the floor test passes; the pool still wants p & 0x800 (0x8F)");
        vm.placementLot = nullptr;
        vm.removeObject(lawnChair.id);
    }
}

// TestPlacementWithRespectToObjects as base 1.0 has it (round n48,
// docs/buy-build-mac.md §7.26): the exclusive placement flags on one tile,
// the intersection flags, the layers, the footprints (strict), the doors and
// windows on one wall (0x17), a "Hide Floor" object's floor, the people.
// The object folder's markers and ConformsToCeiling (ObjectMayBeInFoundation,
// HasNonCeilingConformingObjects; docs/buy-build-mac.md §7.26).
static void testFolderMarkersN48() {
    CodeLibrary code;
    VM vm(code);
    auto def = [&](uint16_t instance, uint16_t type, uint32_t guid) {
        ObjectDefinition d;
        d.fields.resize(0x100);
        d.fields[(0x52 - 0x44) / 2] = type;
        d.fields[(0x5C - 0x44) / 2] = uint16_t(guid), d.fields[(0x5E - 0x44) / 2] = uint16_t(guid >> 16);
        code.add(0x7F7F7F7D, instance, d);
    };
    def(1, 4, 0x4C859106); // come and see
    def(2, 4, 0x000007C4); // go here
    def(3, 4, 0x6CD4A8BC); // grave marker
    def(4, 5, 0x11110005); // a column without a model ("post_trans")
    def(5, 4, 0x11110006);
    for (uint16_t i = 1; i <= 3; ++i) {
        const Object& o = vm.instantiate(0x7F7F7F7D, i);
        check(isFolderMarker(o) && objectMayBeInFoundation(vm, o) && !nonCeilingConforming(vm, o),
              "a come-and-see, go-here or grave marker may be in a foundation and under a low ceiling");
    }
    const Object& column = vm.instantiate(0x7F7F7F7D, 4);
    check(!vm.conformsToCeiling(column) && !objectMayBeInFoundation(vm, column) && nonCeilingConforming(vm, column),
          "a support whose model has no post_trans does not conform to the ceiling");
    const Object& other = vm.instantiate(0x7F7F7F7D, 5);
    check(!isFolderMarker(other) && !objectMayBeInFoundation(vm, other), "another object may not be in a foundation");
}

// MayPlaceQuadrilateral's walls (FUN_009ac8d0): a wall inside the rectangle
// the level room tool deletes (walls.txt deleteTool levelRoom: the deck
// skirt 24 here) lets a foundation be laid; a normal wall is 0x84. (The
// level above is at the tool's height already, 0.75: no point is moved, so
// the vertex test, which comes first, has nothing to refuse.)
static void testLevelRoomWallsN48() {
    for (const uint32_t type : {uint32_t(24), uint32_t(Lot::Normal)}) {
        Lot lot = flatLot();
        lot.levelCount = 2;
        lot.elevations.assign(2 * 13 * 11, 0.0f);
        for (size_t i = 13 * 11; i < lot.elevations.size(); ++i)
            lot.elevations[i] = 0.75f;
        lot.floors.assign(2 * 12 * 10, {});
        lot.rooms.assign(2 * 12 * 10, {});
        for (auto& [k, l] : lot.roomLayers)
            l.resize(2 * 12 * 10);
        addWall(lot, 0, 4, 4, 5, 4, type, {0xFFFF, 0xFFFF});
        lot.deckFoundationStyles.assign(2 * 12 * 10, {-1, -1, -1, -1}); // no foundation yet (the markers say)
        CodeLibrary code;
        VM vm(code);
        vm.world = World::fromLot(lot);
        BuildEditor ed(vm, lot);
        const auto r = ed.drawFoundation(3, 3, 6, 6, 0);
        if (type == 24)
            check(r.ok && r.cost > 0, "a deck skirt inside the rectangle: the level room tool deletes it");
        else
            check(!r.ok && r.error == 0x84, ("a wall inside the rectangle: 0x84 (" + r.message + ")").c_str());
    }
}

static void testObjectsTestN48() {
    Lot lot = flatLot();
    lot.levelCount = 2;
    lot.elevations.assign(2 * 13 * 11, 0.0f);
    for (size_t i = 13 * 11; i < lot.elevations.size(); ++i)
        lot.elevations[i] = 3.0f;
    lot.floors.assign(2 * 12 * 10, {});
    for (size_t i = 12 * 10; i < lot.floors.size(); ++i)
        lot.floors[i] = {1, 1, 1, 1}; // level 1 (VM level 2) floored
    lot.rooms.assign(2 * 12 * 10, {1, 1, 1, 1});
    CodeLibrary code;
    VM vm(code);
    vm.world = World::fromLot(lot);
    vm.placementLot = &lot;
    // An OBJD box footprint: field 0x43 tile width (16 = 1 tile), 0x4D the
    // sides pulled in by nibbles of 1/16.
    auto def = [&](uint16_t instance, uint16_t type, uint16_t width, uint16_t mask, uint32_t guid) {
        ObjectDefinition d;
        d.fields.resize(0x100);
        d.fields[(0x52 - 0x44) / 2] = type;
        d.fields[(0x5C - 0x44) / 2] = uint16_t(guid), d.fields[(0x5E - 0x44) / 2] = uint16_t(guid >> 16);
        d.fields[(0xC6 - 0x44) / 2] = width;
        d.fields[(0xDA - 0x44) / 2] = mask;
        code.add(0x7F7F7F7E, instance, d);
    };
    def(1, 4, 16, 0, 0x11110001);      // a full tile
    def(2, 4, 16, 0x6666, 0x11110002); // a quarter of a tile wide, centred
    def(3, 9, 16, 0, 0x11110003);      // a window
    auto make = [&](uint16_t instance) -> Object& {
        Object& o = vm.instantiate(0x7F7F7F7E, instance);
        o.data[objdata::PlacementFlags] = 0x3;
        o.data[objdata::AllowedHeightFlags] = 0x1;
        return o;
    };
    Object& a = make(1);
    Object& b = make(1);
    vm.setPosition(a, {3.5f, 3.5f, 1});
    check(checkPlacement(vm, b.id, {4, 3, 1}, 0, false).ok, "a full tile beside another: the footprints only touch");
    Placement p = checkPlacement(vm, b.id, {3, 3, 1}, 0, false);
    check(!p.ok && p.error == placementerror::Objects, "two full tiles on one tile cross (0x0C)");
    b.data[objdata::Flags] = 0x02;
    check(checkPlacement(vm, b.id, {3, 3, 1}, 0, false).ok, "allow object intersection (od 8 0x02) lets it");
    b.data[objdata::Flags] = 0;
    b.data[objdata::PlacementFlags] = 0x1003;
    check(checkPlacement(vm, b.id, {3, 3, 1}, 0, false).ok, "one on the ceiling's layer, one on the floor's: no test");
    b.data[objdata::PlacementFlags] = 0x3;
    // Two quarter-tile boxes on one tile: at one centre they cross; side by
    // side they only touch, but both "floor" in od 63 is the exclusive
    // test's 0x0C.
    Object& c = make(2);
    Object& d = make(2);
    vm.setPosition(c, {6.5f, 3.5f, 1});
    check(!checkPlacement(vm, d.id, {6, 3, 1}, 0, false).ok, "quarter tiles at one centre cross");
    vm.setPosition(c, {6.25f, 3.5f, 1});
    check(checkPlacement(vm, d.id, {6, 3, 1}, 0, false).ok, "side by side on one tile they fit");
    vm.removeObject(a.id);
    c.data[objdata::ExclusivePlacementFlags] = 0x1, d.data[objdata::ExclusivePlacementFlags] = 0x1;
    p = checkPlacement(vm, d.id, {6, 3, 1}, 0, false);
    check(!p.ok && p.error == placementerror::Objects, "both od 63 floor on one tile: 0x0C");
    d.data[objdata::ExclusivePlacementFlags] = 0x2;
    check(checkPlacement(vm, d.id, {6, 3, 1}, 0, false).ok, "floor and wall layers share the tile");
    // Two windows needing one wall on one tile: 0x17 "Must be against unused wall".
    vm.world->addWall(1, 9, 5, 10, 5);
    Object& w1 = make(3);
    Object& w2 = make(3);
    w1.data[objdata::WallPlacementFlags] = 0x4, w2.data[objdata::WallPlacementFlags] = 0x4; // wall behind
    w1.data[objdata::Flags] = 0x02, w2.data[objdata::Flags] = 0x02;
    vm.setPosition(w1, {9.5f, 4.5f, 1});
    w1.data[objdata::Direction] = 6; // facing -y: the wall behind is at y = 5
    p = checkPlacement(vm, w2.id, {9, 4, 1}, 6, false);
    check(!p.ok && p.error == 0x17, "two windows on one wall: 0x17");
    p = checkPlacement(vm, w2.id, {9, 4, 1}, 2, false);
    check(!p.ok && p.error == placementerror::NeedsWall, "facing the other way it needs a wall behind (7)");
    // A "Hide Floor" object (od 8 0x4000) above the ground: no floor for another.
    Object& tub = make(1);
    vm.setPosition(tub, {2.5f, 7.5f, 2});
    tub.data[objdata::Flags] = 0x4000 | 0x02;
    Object& lamp = make(2);
    p = checkPlacement(vm, lamp.id, {2, 7, 2}, 0, false);
    check(!p.ok && p.error == placementerror::NoFloor, "on a sunken hot tub's tile the floor is hidden (0x8E)");
    tub.data[objdata::HiddenFlags] = 0x01;
    check(checkPlacement(vm, lamp.id, {2, 7, 2}, 0, false).ok, "with the tub hidden its floor shows again");
    tub.data[objdata::HiddenFlags] = 0;
    lamp.data[objdata::Flags] = 0x4000;
    check(checkPlacement(vm, lamp.id, {2, 7, 2}, 0, false).ok, "an object that hides the floor itself does not test it");
    // The floor / terrain flags on the ground: terrain needs od 42 bit 2.
    lamp.data[objdata::Flags] = 0;
    lamp.data[objdata::PlacementFlags] = 0x1;
    p = checkPlacement(vm, lamp.id, {5, 7, 1}, 0, false);
    check(!p.ok && p.error == 0x8D, "floor-only on the terrain: 0x8D");
    // od 30 "room placement" by the rooms of definition 0 (3ARY 0x0B): tile
    // (8, 7) in room 1, the rest outdoors.
    lot.roomLayers[0x0B].assign(2 * 12 * 10, {0, 0, 0, 0});
    lot.roomLayers[0x0B][size_t(8) * 10 + 7] = {1, 1, 1, 1};
    lamp.data[objdata::PlacementFlags] = 0x3;
    lamp.data[objdata::RoomPlacement] = 1;
    check(checkPlacement(vm, lamp.id, {7, 7, 1}, 0, false).ok, "outside only, outdoors");
    p = checkPlacement(vm, lamp.id, {8, 7, 1}, 0, false);
    check(!p.ok && p.error == 0x0A, "outside only, in a room: 0x0A");
    lamp.data[objdata::RoomPlacement] = 2;
    p = checkPlacement(vm, lamp.id, {7, 7, 1}, 0, false);
    check(!p.ok && p.error == 0x0B, "inside only, outdoors: 0x0B");
    check(checkPlacement(vm, lamp.id, {8, 7, 1}, 0, false).ok, "inside only, in a room");
    vm.placementLot = nullptr;
    check(checkPlacement(vm, lamp.id, {7, 7, 1}, 0, false).ok, "without the lot's geometry, not tested");
}

int main() {
    testObjectsTestN48();
    testFolderMarkersN48();
    testLevelRoomWallsN48();
    testPoolOverFloor();
    testRedoFunds();
    testDeckLevelling();
    testVertexObjects();
    testNoCharge();
    testStairs();
    testAutoRoofRects();
    testFenceArch();
    testEditableArea();
    testLockedTiles();
    testRoundN20();
    testUndoOrder();
    testDesignCatalog();
    testWalkOver();
    testPool();
    testRoof();
    testTerrain();
    testFoundation();
    testRoomIds();
    testSections();
    testRooms();
    testEditor();
    testRound9();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::printf("build: all passed\n");
    return 0;
}
