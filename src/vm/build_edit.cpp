// Build mode's tools over a lot in the VM (build_edit.hpp): the editor's
// core (the geometry kept for undo, the world synced from the lot, commit,
// undo and redo), the floor and wall coverings, and picking the lot's
// ground. The other tools: build_edit_walls.cpp (walls, fences, roofs),
// build_edit_ground.cpp (foundations and decks, terrain, pools),
// build_edit_stairs.cpp (staircases and modular stairs);
// build_edit_internal.hpp holds what they share.

#include "build_edit.hpp"
#include "build_edit_internal.hpp"

#include "formats/lot_edit.hpp"
#include "lot.hpp"
#include "placement.hpp"
#include "rooms.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>

namespace os2::vm {

using namespace build_edit_internal;

namespace build_edit_internal {

std::string money(int v) { return "§" + std::to_string(v); }

std::string point(int x, int y) { return "(" + std::to_string(x) + "," + std::to_string(y) + ")"; }

// The streets the lot packages carry (named floor patterns of
// floorPatterns.txt): nothing is built over them (inferred: the game keeps
// the lot's buildable area off them).
bool street(const std::string& pattern) { return pattern.rfind("road_", 0) == 0 || pattern == "sidewalk"; }

bool sameSection(const std::array<int, 4>& a, const Lot::Wall& w) {
    auto at = [](const Lot::WallVertex& v, int x, int y) { return std::lround(v.x) == x && std::lround(v.y) == y; };
    return (at(w.a, a[0], a[1]) && at(w.b, a[2], a[3])) || (at(w.a, a[2], a[3]) && at(w.b, a[0], a[1]));
}

// The partitions round the raised tiles: foundation 23, lattice deck 16,
// column deck skirt 4 (deckfoundations.txt wallStyle).
bool raisedEdge(uint32_t type) { return type == Lot::Foundation || type == 16 || type == Lot::DeckSkirt; }

bool levelRoomDeletes(uint32_t type) {
    return type == Lot::Foundation || type == 16 || type == 24 || type == Lot::DeckSkirt || type == Lot::Pool;
}

} // namespace build_edit_internal

std::vector<std::array<int, 4>> wallSections(int x0, int y0, int x1, int y1) {
    std::vector<std::array<int, 4>> out;
    const int dx = x1 - x0, dy = y1 - y0;
    if (dx && dy && std::abs(dx) != std::abs(dy))
        return out;
    const int sx = dx > 0 ? 1 : dx < 0 ? -1 : 0, sy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
    for (int x = x0, y = y0; x != x1 || y != y1; x += sx, y += sy)
        out.push_back({x, y, x + sx, y + sy});
    return out;
}

BuildEditor::BuildEditor(VM& vm, Lot& lot, const BuildCatalog* catalog) : vm_(vm), lot_(lot), catalog_(catalog) {
    if (catalog_ && catalog_->railingPrice())
        vm_.railingPrice = catalog_->railingPrice();
    editable_ = inferredRoadStripArea(lot_);
    // The stairs the world was loaded with (the lot's modular stair
    // cObjects), less the staircase objects that are among them.
    if (vm_.world) {
        const auto objects = objectStairs(vm_, lot_);
        for (const Stair& s : vm_.world->stairs())
            if (std::none_of(objects.begin(), objects.end(),
                             [&](const Stair& o) { return o.bottom == s.bottom && o.top == s.top; }))
                loadedStairs_.push_back(s);
    }
}

BuildEditor::Geometry BuildEditor::capture() const {
    Geometry g;
    g.elevations = lot_.elevations;
    g.floorVisibility = lot_.floorVisibility;
    g.floorRotations = lot_.floorRotations;
    g.objects = lot_.objects;
    g.fencePosts = lot_.fencePosts;
    g.paints = lot_.paints;
    g.roofs = lot_.roofs;
    g.pools = lot_.pools;
    g.water = lot_.water;
    g.minLevel = lot_.minLevel, g.levelCount = lot_.levelCount;
    g.walls = lot_.walls;
    g.wallGraphs = lot_.wallGraphs;
    g.floors = lot_.floors;
    g.rooms = lot_.rooms;
    g.roomLayers = lot_.roomLayers;
    g.floorPatterns = lot_.floorPatterns;
    g.wallPatterns = lot_.wallPatterns;
    g.staleWallLayer = lot_.staleWallLayer;
    g.deckFoundationStyles = lot_.deckFoundationStyles;
    g.deckFoundationBase = lot_.deckFoundationBase;
    return g;
}

void BuildEditor::restore(const Geometry& g) {
    merges_.clear();
    lot_.elevations = g.elevations;
    lot_.floorVisibility = g.floorVisibility;
    lot_.floorRotations = g.floorRotations;
    lot_.objects = g.objects;
    lot_.fencePosts = g.fencePosts;
    lot_.paints = g.paints;
    lot_.roofs = g.roofs;
    lot_.pools = g.pools;
    lot_.water = g.water;
    lot_.minLevel = g.minLevel, lot_.levelCount = g.levelCount;
    lot_.walls = g.walls;
    lot_.wallGraphs = g.wallGraphs;
    lot_.floors = g.floors;
    lot_.rooms = g.rooms;
    lot_.roomLayers = g.roomLayers;
    lot_.floorPatterns = g.floorPatterns;
    lot_.wallPatterns = g.wallPatterns;
    lot_.staleWallLayer = g.staleWallLayer;
    lot_.deckFoundationStyles = g.deckFoundationStyles;
    lot_.deckFoundationBase = g.deckFoundationBase;
}

void BuildEditor::syncWorld() {
    // The world from the lot's walls, floors and rooms; the stairs (the
    // lot's own and the staircase objects now in the world) and doors as
    // the VM has them.
    Lot shape = lot_;
    shape.objects.clear();
    World w = World::fromLot(shape);
    // The lot's modular stairs while their top step is still in the world
    // (the sledgehammer sells flights), then the ones Build mode laid and
    // the staircase objects.
    std::vector<Stair> stairs;
    auto known = [&](const Stair& s) {
        return std::any_of(stairs.begin(), stairs.end(), [&](const Stair& o) { return o.bottom == s.bottom && o.top == s.top; });
    };
    for (const Stair& s : loadedStairs_) {
        const auto top = s.at(0.25f * float(s.steps - 1));
        const bool there = std::any_of(vm_.objects().begin(), vm_.objects().end(), [&](const auto& kv) {
            const Object& o = kv.second;
            return o.position.inWorld() && o.definition && o.definition->fileName.rfind("Stair Modular - ", 0) == 0 &&
                   o.definition->fileName.find(" - Portal - A") != std::string::npos &&
                   std::fabs(o.position.x - top[0]) < 0.05f && std::fabs(o.position.y - top[1]) < 0.05f;
        });
        if (there && !known(s))
            stairs.push_back(s);
    }
    for (const Stair& s : modularStairs(vm_, lot_))
        if (!known(s))
            stairs.push_back(s);
    for (const Stair& s : objectStairs(vm_, lot_))
        if (!known(s))
            stairs.push_back(s);
    for (const Stair& s : stairs)
        w.addStair(s);
    // Each part of a door opens the wall behind it when the tile behind is
    // another part of the same door (vm::loadLot).
    for (const auto& [id, o] : vm_.objects()) {
        if (!o.definition || o.definition->objectType() != kObjectTypeDoor || !o.position.inWorld())
            continue;
        const int dir = o.data[objdata::Direction];
        const auto [dx, dy] = directionStep(dir + 4);
        const TilePos t = VM::tileOf(o);
        if (w.diagonalWall(t)) { // a diagonal door: one part in the wall across its tile
            w.openDoor(t, dir);
            continue;
        }
        for (const auto& [pid, p] : vm_.objects())
            if (pid != id && p.position.inWorld() && multiTileGroup(p) == multiTileGroup(o) &&
                VM::tileOf(p) == TilePos{t.x + dx, t.y + dy, t.level}) {
                w.openDoor(t, dir);
                break;
            }
    }
    // cEdithObjectModule::UpdateObjectsInRoom (BV 0x52e170), for each room
    // the map changed (the 0x2B4D18F6 message of definition 2): every object
    // on the room's tiles gets the room (object data 29, SetRoom with its
    // contained objects) and OnRoomChanged, which runs the "room changed"
    // entry only for those whose room is new (and diagonal doors).
    std::set<int16_t> changedRooms;
    if (vm_.world)
        for (int level = w.firstLevel(); level < w.firstLevel() + w.levelCount(); ++level)
            for (int y = 0; y < int(w.height()); ++y)
                for (int x = 0; x < int(w.width()); ++x)
                    if (const TilePos t{x, y, level}; vm_.world->contains(t) && vm_.world->room(t) != w.room(t))
                        changedRooms.insert(w.room(t)), changedRooms.insert(vm_.world->room(t));
    std::vector<std::pair<ObjectId, int16_t>> moved;
    if (!changedRooms.empty())
        for (const auto& [id, o] : vm_.objects()) {
            if (!o.position.inWorld() || o.data[objdata::ContainerId])
                continue;
            const TilePos t = VM::tileOf(o);
            if (w.contains(t) && changedRooms.count(w.room(t)))
                moved.push_back({id, w.room(t)});
        }
    std::optional<World> before = std::move(vm_.world);
    vm_.world = std::move(w);
    vm_.planner.reset();
    vm_.refreshObjectPortals();
    setLotRooms(vm_, lot_);
    for (const auto& [id, room] : moved)
        if (Object* o = vm_.object(id)) {
            o->data[objdata::Room] = room;
            vm_.noteRoomChanged(id);
        }
    vm_.followContainers(); // SetRoom(..., true): the contained objects too
    for (const auto& [id, room] : moved)
        if (vm_.object(id))
            finishMove(vm_, id);
    // The wall messages (0x0B4CF313, 0x2B4CF319): UpdateObjectsAdjacentToWall.
    if (before)
        updateObjectsAdjacentToWalls(vm_, *before);
    ++revision_;
}

void BuildEditor::changed() {
    updateRooms(lot_, merges_);
    merges_.clear();
    syncWorld();
}

void BuildEditor::commit(Geometry before, int cost, std::vector<Sold> sold, std::vector<Sold> created) {
    Op op;
    op.before = std::move(before);
    op.after = capture();
    op.cost = cost;
    op.sold = std::move(sold);
    op.created = std::move(created);
    undo_.push_back(std::move(op));
    dropRedo();
}

void BuildEditor::dropRedo() {
    for (const Op& op : redo_)
        for (const Sold& x : op.created)
            if (const Object* o = vm_.object(x.id); o && !o->position.inWorld()) {
                if (onForget)
                    for (ObjectId part : groupOf(vm_, x.id))
                        onForget(part);
                deleteGroup(vm_, x.id);
            }
    redo_.clear();
}

// PartitionStyleBearsLoad (BV 0x8b5bf0): the style's product says
// (walls.txt / fences.txt "bearsLoad"): walls, foundations, decks and pools
// do; attic walls (3) and fences do not (docs/buy-build-mac.md §7.3).
static bool bearsLoad(uint32_t type) { return !Lot::isFence(type) && type != Lot::Attic; }

int BuildEditor::stairCeilingError(const Placement& p) const {
    if (!p.ok || p.container)
        return 0;
    const int W = int(lot_.width), H = int(lot_.height);
    for (const auto& part : p.parts) {
        const Object* o = vm_.object(part.id);
        if (!o || !o->definition || o->definition->objectType() != 10 ||
            (o->data[objdata::PlacementFlags2] & 0x20))
            continue;
        const TilePos t = part.tile;
        const int li = t.level - 1;
        if (t.x < 0 || t.y < 0 || t.x >= W || t.y >= H || !lot_.hasLevel(li) || !lot_.hasLevel(li + 1) ||
            !hasSupport(t.level + 1, t.x, t.y, 0xF))
            continue;
        for (int cx = t.x; cx <= t.x + 1; ++cx)
            for (int cy = t.y; cy <= t.y + 1; ++cy)
                if (lot_.elevation(li + 1, cx, cy) - lot_.elevation(li, cx, cy) > 3.0f + 0.001f)
                    return 0x30;
    }
    return 0;
}

bool BuildEditor::supportedUnderneath(int level, int x, int y, int quarters, ObjectId ignore) const {
    const int li = level - 1;
    if (li <= 0)
        return true;
    if (x < 0 || y < 0 || x >= int(lot_.width) || y >= int(lot_.height) || !lot_.hasLevel(li - 1))
        return false;
    // FUN_00164d30: a quarter inside a room of the level below in the
    // load-bearing room map (cPartitionManager vtable 0x13c on the
    // quarter's centre, room definition 1 = 3ARY 0x0A): walls that bear load
    // all round it, not just a floor.
    if (auto it = lot_.roomLayers.find(0x0A); it != lot_.roomLayers.end() && !it->second.empty()) {
        const size_t i = (size_t(li - 1 - lot_.minLevel) * lot_.width + uint32_t(x)) * lot_.height + uint32_t(y);
        if (i < it->second.size())
            for (int q = 0; q < 4; ++q)
                if ((quarters & (1 << q)) && it->second[i][size_t(q)])
                    return true;
    }
    // HasObjectSupport: a "support" object (OBJD type 5) on the tile below.
    for (ObjectId id : vm_.objectsOnTile({x, y, level - 1}))
        if (const Object* o = vm_.object(id); o && id != ignore && o->definition &&
                                              o->definition->objectType() == kObjectTypeSupport)
            return true;
    return false;
}

bool BuildEditor::wallSupported(int level, int x, int y, int quarters) const {
    const int li = level - 1;
    auto bears = [&](int ax, int ay, int bx, int by) {
        const Lot::Wall* w = findWall(lot_, li - 1, ax, ay, bx, by);
        return w && bearsLoad(w->type);
    };
    // The two diagonals, then the edge of each quarter (1 west, 2 north,
    // 4 east, 8 south).
    if (bears(x, y, x + 1, y + 1) || bears(x + 1, y, x, y + 1))
        return true;
    return ((quarters & 1) && bears(x, y, x, y + 1)) || ((quarters & 2) && bears(x, y + 1, x + 1, y + 1)) ||
           ((quarters & 4) && bears(x + 1, y + 1, x + 1, y)) || ((quarters & 8) && bears(x + 1, y, x, y));
}

bool BuildEditor::roofSupported(int level, int x, int y) const {
    const int li = level - 1;
    constexpr int kInset = 3; // int(StandardLevelHeight), WorldDB vtable 0x15c
    for (const auto& r : lot_.roofs) {
        // GetRoofIDs(level - 1, 6): the mansards (tRoofType 6) of the level below.
        if (r.level != li - 1 || r.type != Lot::Mansard)
            continue;
        const int x0 = int(std::min(r.x0, r.x1)), x1 = int(std::max(r.x0, r.x1));
        const int y0 = int(std::min(r.y0, r.y1)), y1 = int(std::max(r.y0, r.y1));
        if (x0 + kInset <= x && y0 + kInset <= y && x < x1 - kInset && y < y1 - kInset)
            return true;
    }
    return false;
}

bool BuildEditor::floorable(int level, int x, int y) const {
    const int li = level - 1;
    if (li <= 0)
        return true; // HasSupport: SupportedUnderneath holds the ground
    const int W = int(lot_.width), H = int(lot_.height);
    auto onLot = [&](int tx, int ty) { return tx >= 0 && ty >= 0 && tx < W && ty < H; };
    // Above the ground the tile must be flat and in bounds.
    if (!onLot(x, y) || !lot_.hasLevel(li))
        return false;
    const float e = lot_.elevation(li, x, y);
    if (std::fabs(lot_.elevation(li, x + 1, y) - e) > 1e-4f || std::fabs(lot_.elevation(li, x, y + 1) - e) > 1e-4f ||
        std::fabs(lot_.elevation(li, x + 1, y + 1) - e) > 1e-4f)
        return false;
    if (hasSupport(level, x, y, 0xF))
        return true;
    // The four neighbours (directions 1 -x, 2 +y, 4 +x, 8 -y): the quarter
    // of each that faces this tile supported from below, or floored on
    // this level.
    bool nextToFloor = false;
    for (const auto& [dx, dy, facing] : std::array<std::array<int, 3>, 4>{{{-1, 0, 4}, {0, 1, 8}, {1, 0, 1}, {0, -1, 2}}}) {
        const int nx = x + dx, ny = y + dy;
        if (!onLot(nx, ny))
            continue;
        if (supportedUnderneath(level, nx, ny, facing))
            return true;
        for (int q = 0; q < 4; ++q)
            if ((facing & (1 << q)) && lot_.floor(li, uint32_t(nx), uint32_t(ny))[size_t(q)])
                nextToFloor = true;
    }
    if (!nextToFloor)
        return false;
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy)
            if (std::abs(dx) + std::abs(dy) < 4 && onLot(x + dx, y + dy) &&
                supportedUnderneath(level, x + dx, y + dy, 0xF))
                return true;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (onLot(x + dx, y + dy) && wallSupported(level, x + dx, y + dy, 0xF))
                return true;
    return false;
}

int BuildEditor::clickQuarters(int level, float px, float py) const {
    const int x = int(std::floor(px)), y = int(std::floor(py));
    const float fx = px - float(x), fy = py - float(y);
    // "/" separates west+north from east+south, "\" north+east from
    // south+west (lot_edit.hpp quarters: bit 1 west, 2 north, 4 east, 8 south).
    if (findWall(lot_, level - 1, x, y, x + 1, y + 1))
        return fy > fx ? 0x3 : 0xC;
    if (findWall(lot_, level - 1, x + 1, y, x, y + 1))
        return fx + fy > 1 ? 0x6 : 0x9;
    return 0xF;
}

BuildEditor::Result BuildEditor::paintFloor(int level, int x0, int y0, int x1, int y1, const BuildPattern* pattern,
                                            int quarters, int rotation) {
    Result r;
    const int li = level - 1;
    if (!lot_.hasLevel(li)) {
        r.message = "no such level";
        return r;
    }
    if (rotation != 1 && rotation != 2 && rotation != 4 && rotation != 8)
        rotation = 1;
    const int W = int(lot_.width), H = int(lot_.height);
    const int ax = std::max(0, std::min(x0, x1)), bx = std::min(W - 1, std::max(x0, x1));
    const int ay = std::max(0, std::min(y0, y1)), by = std::min(H - 1, std::max(y0, y1));
    Geometry before = capture();
    // Painting on the ground levels the terrain under the rectangle to the
    // first tile's height first (the floor tool's release: LevelRect;
    // inferred: the mean of that tile's corners). Points a wall holds stay.
    if (pattern && li == 0 && !lot_.elevations.empty()) {
        const int sx = std::clamp(x0, 0, W - 1), sy = std::clamp(y0, 0, H - 1);
        const float height = (lot_.elevation(0, sx, sy) + lot_.elevation(0, sx + 1, sy) +
                              lot_.elevation(0, sx, sy + 1) + lot_.elevation(0, sx + 1, sy + 1)) / 4;
        const size_t base = size_t(0 - lot_.minLevel) * size_t(W + 1) * size_t(H + 1);
        for (int x = ax; x <= bx + 1; ++x)
            for (int y = ay; y <= by + 1; ++y) {
                bool held = false;
                for (const auto& w : lot_.walls)
                    held |= w.a.level == 0 && ((std::lround(w.a.x) == x && std::lround(w.a.y) == y) ||
                                               (std::lround(w.b.x) == x && std::lround(w.b.y) == y));
                if (!held)
                    lot_.elevations[base + size_t(x) * size_t(H + 1) + size_t(y)] = height;
            }
    }
    std::map<uint16_t, std::string> names = lot_.floorPatterns;
    const uint16_t ref = pattern ? patternRef(names, pattern->lotName(), 1) : 0;
    // cTSUserToolUndoableFloorPattern::ChangeTile: per quarter whose pattern
    // or rotation changes, + new price (placing) or - refund of the old
    // (erasing), a quarter each.
    float cost = 0;
    std::vector<std::pair<std::array<int, 3>, uint16_t>> changes;
    int refused = 0;
    for (int x = ax; x <= bx; ++x)
        for (int y = ay; y <= by; ++y) {
            if (!editableTile(x, y)) {
                refused = 0x45;
                continue;
            }
            // UserMayPlaceFloorPattern: terrain not flat on the ground
            // (0x53), a tile above it that is not floorable (0x52).
            if (pattern && li == 0 &&
                (std::fabs(lot_.elevation(0, x, y) - lot_.elevation(0, x + 1, y + 1)) > 0.01f ||
                 std::fabs(lot_.elevation(0, x + 1, y) - lot_.elevation(0, x, y + 1)) > 0.01f ||
                 std::fabs(lot_.elevation(0, x, y) - lot_.elevation(0, x + 1, y)) > 0.01f)) {
                refused = 0x53;
                continue;
            }
            if (pattern && !floorable(level, x, y)) {
                refused = 0x52;
                continue;
            }
            const auto& q = lot_.floor(li, uint32_t(x), uint32_t(y));
            for (int k = 0; k < 4; ++k) {
                if (!(quarters & (1 << k)))
                    continue;
                const bool sameRotation = !pattern || floorRotation(lot_, li, x, y, k) == rotation;
                if (q[size_t(k)] == ref && sameRotation)
                    continue;
                const std::string old = q[size_t(k)] ? (names.count(q[size_t(k)]) ? names[q[size_t(k)]] : "") : "";
                if (li == 0 && street(old))
                    continue; // the street stays
                // FloorPatternCost (BV 0x9a3e50): floor(price x (refund ? 0.8 : 1)).
                if (pattern) {
                    cost += float(pattern->price) * 0.25f;
                } else if (q[size_t(k)]) {
                    cost -= std::floor(float(priceOf(old, true)) * kBuildRefundRate) * 0.25f;
                }
                changes.push_back({{x, y, k}, ref});
            }
        }
    if (changes.empty()) {
        restore(before);
        r.ok = !refused;
        r.error = refused;
        r.message = refused == 0x53 ? "the ground is not flat" : refused == 0x52 ? "nothing holds the floor up"
                                                                                : "nothing to change";
        return r;
    }
    r.cost = int(std::lround(cost));
    if (!canAfford(vm_, r.cost)) {
        restore(before);
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    lot_.floorPatterns = names;
    if (pattern)
        prices_[pattern->lotName()] = pattern->price;
    for (const auto& [t, v] : changes) {
        setFloor(lot_, li, t[0], t[1], t[2], v);
        setFloorRotation(lot_, li, t[0], t[1], t[2], uint8_t(v ? rotation : 1));
    }
    r.changed = int(changes.size());
    chargeFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = std::to_string(r.changed) + " quarter tiles " + (pattern ? "floored with " + pattern->name : "cleared") +
                (r.cost >= 0 ? " for " + money(r.cost) : ", " + money(-r.cost) + " back");
    return r;
}

// ChangeWallPattern (BV 0x394914): + the new pattern's price a face;
// taking a covering off (back to the style's "blank") gives back
// int(old price x 0.8); covering over another gives nothing back.
BuildEditor::Result BuildEditor::cover(std::vector<std::pair<uint32_t, int>> faces, const BuildPattern* pattern,
                                       const std::string& what) {
    Result r;
    Geometry before = capture();
    std::map<uint16_t, std::string> names = lot_.wallPatterns;
    const uint16_t ref = patternRef(names, pattern ? pattern->lotName() : kDefaultWallPattern, 0);
    float cost = 0;
    std::erase_if(faces, [&](const std::pair<uint32_t, int>& f) {
        for (const auto& w : lot_.walls)
            if (w.id == f.first) {
                const uint16_t old = w.patterns[size_t(f.second)];
                if (old == ref)
                    return true;
                if (pattern)
                    cost += float(pattern->price);
                else if (auto it = names.find(old); it != names.end())
                    cost -= float(int(float(priceOf(it->second, false)) * kBuildRefundRate));
                return false;
            }
        return true;
    });
    if (faces.empty()) {
        r.ok = true;
        r.message = "no wall faces to change " + what;
        return r;
    }
    r.cost = int(std::lround(cost));
    if (!canAfford(vm_, r.cost)) {
        r.message = "not enough money (" + money(r.cost) + ")";
        r.cost = 0;
        return r;
    }
    lot_.wallPatterns = names;
    if (pattern)
        prices_[pattern->lotName()] = pattern->price;
    for (const auto& [id, k] : faces)
        for (auto& w : lot_.walls)
            if (w.id == id)
                w.patterns[size_t(k)] = ref, w.layered = true;
    r.changed = int(faces.size());
    chargeFunds(vm_, -r.cost);
    changed();
    commit(std::move(before), r.cost);
    r.ok = true;
    r.message = std::to_string(r.changed) + " wall sides " + what + " " +
                (pattern ? "covered with " + pattern->name : std::string("uncovered")) +
                (r.cost >= 0 ? " for " + money(r.cost) : ", " + money(-r.cost) + " back");
    return r;
}

BuildEditor::Result BuildEditor::paintWalls(int level, int x0, int y0, int x1, int y1, int side,
                                            const BuildPattern* pattern) {
    std::vector<std::pair<uint32_t, int>> faces; // wall ID, pattern index
    for (const auto& s : wallSections(x0, y0, x1, y1)) {
        const Lot::Wall* w = findWall(lot_, level - 1, s[0], s[1], s[2], s[3]);
        if (!w || Lot::isFence(w->type) || w->type == Lot::DeckSkirt)
            continue; // mayChangeSurface false
        // The wall may run the other way: its right is the run's left.
        const bool same = std::lround(w->a.x) == s[0] && std::lround(w->a.y) == s[1];
        for (int k = 0; k < 2; ++k) {
            const int runSide = (k == 0) == same ? 1 : -1;
            if (side == 0 || side == runSide)
                faces.push_back({w->id, k});
        }
    }
    return cover(std::move(faces), pattern, "along the run");
}

BuildEditor::Result BuildEditor::paintRoom(int level, int x, int y, const BuildPattern* pattern) {
    Result r;
    const int li = level - 1;
    if (lot_.rooms.empty() || !lot_.hasLevel(li) || x < 0 || y < 0 || x >= int(lot_.width) || y >= int(lot_.height)) {
        r.message = "no room there";
        return r;
    }
    uint16_t room = 0;
    for (uint16_t q : lot_.rooms[(size_t(li - lot_.minLevel) * lot_.width + size_t(x)) * lot_.height + size_t(y)])
        room = room ? room : q;
    if (!room) {
        r.message = "no room there";
        return r;
    }
    std::vector<std::pair<uint32_t, int>> faces;
    for (const auto& w : lot_.walls) {
        if (w.a.level != li || Lot::isFence(w.type) || w.type == Lot::DeckSkirt)
            continue;
        for (int k = 0; k < 2; ++k)
            if (wallSideRoom(lot_, w, k) == room)
                faces.push_back({w.id, k});
    }
    return cover(std::move(faces), pattern, "of room " + std::to_string(room));
}

BuildEditor::Result BuildEditor::paintFlood(int level, int ax, int ay, int bx, int by, int side,
                                            const BuildPattern* pattern) {
    Result r;
    const int li = level - 1;
    const Lot::Wall* start = findWall(lot_, li, ax, ay, bx, by);
    if (!start || Lot::isFence(start->type) || start->type == Lot::DeckSkirt) {
        r.message = "no wall face there";
        return r;
    }
    // The run's right (+1) is the wall's right (patterns[0]) when the wall
    // runs the same way.
    const bool same = std::lround(start->a.x) == ax && std::lround(start->a.y) == ay;
    const int k0 = (side >= 0) == same ? 0 : 1;
    // UserSetWallPatternFlood: from the clicked face round the ring of faces
    // of its room both ways, while a face has the pattern the clicked one
    // had. The ring here: faces of the same room (the light rooms, 0x0B,
    // which fences do not split) on the level that share an end point.
    const uint32_t which = lot_.roomLayers.count(0x0B) && !lot_.roomLayers.at(0x0B).empty() ? 0x0B : 0x09;
    const int32_t room = wallSideRoom(lot_, *start, k0, which);
    const uint16_t had = start->patterns[size_t(k0)];
    auto key = [](const Lot::Wall& w) {
        return std::array<long, 4>{std::lround(w.a.x), std::lround(w.a.y), std::lround(w.b.x), std::lround(w.b.y)};
    };
    std::vector<std::pair<uint32_t, int>> faces{{start->id, k0}};
    std::set<std::pair<uint32_t, int>> seen{{start->id, k0}};
    std::vector<const Lot::Wall*> stack{start};
    while (!stack.empty()) {
        const Lot::Wall* w = stack.back();
        stack.pop_back();
        const auto e = key(*w);
        for (const auto& o : lot_.walls) {
            if (o.a.level != li || Lot::isFence(o.type) || o.type == Lot::DeckSkirt)
                continue;
            const auto f = key(o);
            const bool touches = (f[0] == e[0] && f[1] == e[1]) || (f[0] == e[2] && f[1] == e[3]) ||
                                 (f[2] == e[0] && f[3] == e[1]) || (f[2] == e[2] && f[3] == e[3]);
            if (!touches)
                continue;
            for (int k = 0; k < 2; ++k) {
                if (seen.count({o.id, k}) || o.patterns[size_t(k)] != had || wallSideRoom(lot_, o, k, which) != room)
                    continue;
                seen.insert({o.id, k});
                faces.push_back({o.id, k});
                stack.push_back(&o);
            }
        }
    }
    return cover(std::move(faces), pattern, "round room " + std::to_string(room));
}

bool BuildEditor::undo() {
    if (undo_.empty())
        return false;
    Op op = std::move(undo_.back());
    undo_.pop_back();
    restore(op.before);
    // What was sold with the walls goes back where it was (its refund is
    // in op.cost).
    for (const Sold& x : op.sold)
        applyPlacement(vm_, x.id, x.where);
    for (const Sold& x : op.created)
        pickUp(vm_, x.id);
    for (const auto& m : op.models)
        setRailingModel(vm_, ObjectId(m[0]), m[1]);
    for (const auto& l : op.links)
        for (ObjectId part : groupOf(vm_, ObjectId(l[0])))
            linkFireplaceObject(vm_, part, ObjectId(l[1]), ObjectId(l[2]));
    chargeFunds(vm_, op.cost); // the price back, or the refund paid back
    syncWorld();
    redo_.push_back(std::move(op));
    return true;
}

bool BuildEditor::redo() {
    if (redo_.empty())
        return false;
    // RedoIt (Mac BV 0x9b519c) charges again through AdjustFunds
    // (cTSSimulator::Spend) without asking the funds, nor does the manager's
    // RedoLastCommand: a redo is never refused for money and the funds may go
    // below zero (docs/buy-build-mac.md §7.25).
    Op op = std::move(redo_.back());
    redo_.pop_back();
    restore(op.after);
    for (const Sold& x : op.sold)
        pickUp(vm_, x.id);
    for (const Sold& x : op.created)
        applyPlacement(vm_, x.id, x.where);
    for (const auto& m : op.models)
        setRailingModel(vm_, ObjectId(m[0]), m[2]);
    for (const auto& l : op.links)
        for (ObjectId part : groupOf(vm_, ObjectId(l[0])))
            linkFireplaceObject(vm_, part, ObjectId(l[3]), ObjectId(l[4]));
    chargeFunds(vm_, -op.cost);
    syncWorld();
    undo_.push_back(std::move(op));
    return true;
}

void BuildEditor::resetHistory(const std::function<void(ObjectId)>& forget) {
    auto die = [&](const Sold& x) {
        if (const Object* o = vm_.object(x.id); o && !o->position.inWorld()) {
            if (forget)
                for (ObjectId part : groupOf(vm_, x.id))
                    forget(part);
            deleteGroup(vm_, x.id);
        }
    };
    for (const Op& op : undo_)
        for (const Sold& x : op.sold)
            die(x);
    for (const Op& op : redo_) // undone decks: their columns
        for (const Sold& x : op.created)
            die(x);
    undo_.clear(), redo_.clear();
}

int BuildEditor::priceOf(const std::string& lotName, bool floor) const {
    if (auto it = prices_.find(lotName); it != prices_.end())
        return it->second;
    if (catalog_)
        try {
            if (const BuildPattern* p = catalog_->byGuid(uint32_t(std::stoul(lotName, nullptr, 16))); p && p->floor == floor)
                return p->price;
        } catch (const std::logic_error&) { // not a GUID in hex (std::stoul): no catalog price
        }
    return 0;
}

} // namespace os2::vm

namespace os2::vm {

float lotPickHeight(const Lot& lot, int level, float x, float y, bool poolWater) {
    const float W = float(lot.width), H = float(lot.height);
    const int l = std::clamp(level - 1, lot.minLevel, lot.minLevel + lot.levelCount - 1);
    const float cx = std::clamp(x, 0.0f, W), cy = std::clamp(y, 0.0f, H);
    const float z = lot.elevationAt(l, cx, cy);
    if (level == 1 && lot.hasLevel(-1)) {
        const int tx = int(std::floor(x)), ty = int(std::floor(y));
        for (const auto& p : lot.pools)
            if (p.level == 0 && tx >= int(std::lround(p.x)) && tx < int(std::lround(p.x)) + int(p.width) &&
                ty >= int(std::lround(p.y)) && ty < int(std::lround(p.y)) + int(p.height))
                return poolWater ? z - p.depth : lot.elevationAt(-1, cx, cy);
    }
    return z;
}

std::optional<std::array<float, 2>> pickLotGround(const Lot& lot, int level, std::array<float, 3> eye,
                                                  std::array<float, 3> dir, bool poolWater) {
    if (!lot.hasLevel(0) || std::fabs(dir[2]) < 1e-5f)
        return std::nullopt;
    const float W = float(lot.width), H = float(lot.height);
    const int l = std::clamp(level - 1, lot.minLevel, lot.minLevel + lot.levelCount - 1);
    auto terrain = [&](float x, float y) { return lot.elevationAt(l, std::clamp(x, 0.0f, W), std::clamp(y, 0.0f, H)); };
    auto surface = [&](float x, float y) { return lotPickHeight(lot, level, x, y, poolWater); };
    float zHi = -1e9f, zLo = 1e9f;
    for (float f : lot.elevations)
        zHi = std::max(zHi, f), zLo = std::min(zLo, f);
    if (zHi < zLo)
        zHi = zLo = 0;
    const float tA = std::max(0.0f, (zHi + 0.5f - eye[2]) / dir[2]), tB = (zLo - 0.5f - eye[2]) / dir[2];
    if (tB <= 0 || !(tB > tA)) {
        // Level ground, or a ray that never goes down: the plain plane hit.
        const float t = (terrain(W / 2, H / 2) - eye[2]) / dir[2];
        if (t <= 0)
            return std::nullopt;
        return std::array<float, 2>{eye[0] + dir[0] * t, eye[1] + dir[1] * t};
    }
    auto above = [&](float t) { return eye[2] + dir[2] * t > surface(eye[0] + dir[0] * t, eye[1] + dir[1] * t); };
    constexpr int kSteps = 1024;
    float lo = tA;
    for (int i = 1; i <= kSteps; ++i) {
        const float t = tA + (tB - tA) * float(i) / kSteps;
        if (above(t)) {
            lo = t;
            continue;
        }
        float hi = t;
        for (int k = 0; k < 24; ++k) {
            const float m = (lo + hi) / 2;
            (above(m) ? lo : hi) = m;
        }
        return std::array<float, 2>{eye[0] + dir[0] * hi, eye[1] + dir[1] * hi};
    }
    return std::nullopt;
}

} // namespace os2::vm
