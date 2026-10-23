#include "lot.hpp"
#include "formats/log_once.hpp"

#include "formats/build_catalog.hpp"

#include "formats/lot_arrivals.hpp"

#include "formats/binreader.hpp"
#include "formats/dbpf.hpp"
#include "formats/lot.hpp"
#include "formats/lot_objects.hpp"
#include "formats/resindex.hpp"
#include "formats/simantics.hpp"
#include "engine.hpp"
#include "environment.hpp"
#include "interactions.hpp"

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <set>

namespace os2::vm {

namespace {

// Groups given to the code of a package loaded on its own (a Sim's character
// file): 0xFE000000 | CRC24 of the file name, out of the way of the game's
// object (0x7F......) and scenegraph groups.
uint32_t characterGroup(const std::string& stem) { return 0xFE000000 | (nameInstanceHash(stem) & 0x00FFFFFF); }

const std::string kObjtSuffix = "_objt";

} // namespace

std::filesystem::path charactersDirOf(const std::filesystem::path& lotPackage) {
    auto dir = std::filesystem::absolute(lotPackage).parent_path();
    if (dir.filename() != "Lots")
        return {};
    auto characters = dir.parent_path() / "Characters";
    return std::filesystem::is_directory(characters) ? characters : std::filesystem::path{};
}

std::set<uint32_t> currentCObjects(CodeLibrary& code, const Lot& lot, const LotObjects& saved) {
    std::set<uint32_t> out;
    std::map<uint32_t, std::string> names; // GUID -> OBJD file name
    for (const auto& o : lot.objects) {
        const auto id = int16_t(o.instance & 0xFFFF);
        const LotObjects::State* s = saved.state(id);
        if (!s || (o.instance >> 16))
            continue;
        if (s->person) {
            if (o.person)
                out.insert(o.instance);
            continue;
        }
        const uint32_t guid = saved.guidOf(id);
        auto it = names.find(guid);
        if (it == names.end()) {
            std::string name;
            if (const auto def = code.definitionByGuid(guid))
                if (const ObjectDefinition* d = code.definition(def->first, def->second))
                    name = d->fileName;
            it = names.emplace(guid, name).first;
        }
        std::string name = o.definition;
        if (name.ends_with(kObjtSuffix))
            name.resize(name.size() - kObjtSuffix.size());
        if (!it->second.empty() && it->second == name)
            out.insert(o.instance);
    }
    return out;
}

size_t dropLeftoverCObjects(Lot& lot, DbpfPackage& lotPackage, ResourceIndex& index) {
    LotObjects saved = LotObjects::load(lotPackage);
    if (saved.pairs.empty())
        return 0;
    // An evicted family's things are not drawn either: the load removes
    // them (formats/lot_evict.hpp, through the arrivals of a neighborhood lot).
    addArrivingSims(saved, lot, lotPackage.path(), &index);
    CodeLibrary code(&index);
    const auto keep = currentCObjects(code, lot, saved);
    return std::erase_if(lot.objects, [&](const Lot::Object& o) { return !keep.count(o.instance); });
}

void setLotRooms(VM& vm, const Lot& lot) {
    vm.rooms.clear();
    // Rooms: their level (as the saved objects number levels: the arrays'
    // first level is 1) and area in tiles (a quarter tile counts a quarter;
    // inferred). Light, outside, pool and windows are not known.
    if (!lot.rooms.empty()) {
        std::map<int16_t, std::pair<int, size_t>> rooms; // level, quarter tiles
        for (int li = 0; li < lot.levelCount; ++li)
            for (uint32_t x = 0; x < lot.width; ++x)
                for (uint32_t y = 0; y < lot.height; ++y)
                    for (uint16_t room : lot.rooms[(size_t(li) * lot.width + x) * lot.height + y])
                        if (room) {
                            auto& r = rooms[int16_t(room)];
                            r.first = li + 1;
                            ++r.second;
                        }
        // Pool: cTSObject::InterpValue (owner 0x17, value 4) asks the room
        // manager for one floor tile of the room (FloorTileInRoom, room
        // definition 2) and tests that tile alone (nTSWorld::IsPool: a deck
        // foundation of type 3 or 5). Pools share the yard's room (the saved
        // room arrays give the pool's tiles the terrain's room), so the yard
        // is not a pool unless the tile the manager returns is one. The
        // manager returns the first tile of the room's list
        // (cPartitionManager::FloorTileInRoom BV 0xc314f8 -> the room
        // manager of the definition, vtable 0x40 BV 0xc28ef4: element 0, an
        // invalid tile when the room has none), and BuildRoomTileListMap (BV
        // 0xc29c74) fills the lists walking the WorldDB's floor tiles level
        // by level, x outer and y inner (FUN_0015b9c8), a tile going into
        // the list of each room one of its quarters has: the first tile in
        // the arrays' order below, as read. Marking the whole yard a pool
        // made "Swim Here?" true on every lawn of a lot with a pool, and the
        // Go Here pie showed the swim names ("Entrar") in place of Vir Aqui.
        // A pool's floor is a room of its own on the level below the pool's
        // (the saved room arrays of N001_Lot68: a pool at level 0 has its 40
        // floor tiles in room 1 at level -1; the surface tiles have no room),
        // so the pool record matches the room's level or the one above it
        // (without that "Swim Here?" was false over every pool: the click on
        // the water gave Vir Aqui, live QA 15).
        // Outside (inferred): a pool, or a room that reaches the edge of the
        // lot on the ground (the terrain's room).
        std::set<int16_t> outside, pool, seen;
        for (int li = 0; li < lot.levelCount; ++li)
            for (uint32_t x = 0; x < lot.width; ++x)
                for (uint32_t y = 0; y < lot.height; ++y)
                    for (uint16_t room : lot.rooms[(size_t(li) * lot.width + x) * lot.height + y]) {
                        if (!room)
                            continue;
                        const int level = li + lot.minLevel;
                        if (level == 0 && (x == 0 || y == 0 || x + 1 == lot.width || y + 1 == lot.height))
                            outside.insert(int16_t(room));
                        if (!seen.insert(int16_t(room)).second)
                            continue; // the room's first tile decides
                        for (const auto& p : lot.pools)
                            if ((p.level == level || p.level == level + 1) && float(x) >= p.x && float(x) < p.x + float(p.width) &&
                                float(y) >= p.y && float(y) < p.y + float(p.height))
                                pool.insert(int16_t(room)), outside.insert(int16_t(room));
                    }
        for (const auto& [id, r] : rooms) {
            VM::Room& room = vm.rooms[id];
            room.values[1] = int16_t(outside.count(id));
            room.values[2] = int16_t(r.first);
            room.values[3] = int16_t(r.second / 4);
            room.values[4] = int16_t(pool.count(id));
            room.known = 0x1E;
        }
    }
    // The environment score manager's walls and floors (environment.hpp).
    setLotEnvironment(vm, lot);
}

std::function<std::filesystem::path(uint32_t)> useCharacterFiles(CodeLibrary& code,
                                                                  const std::filesystem::path& charactersDir) {
    // The character files' OBJD GUIDs, read once (when first needed).
    auto characterByGuid = std::make_shared<std::map<uint32_t, std::filesystem::path>>();
    auto characterFileOf = [characterByGuid, charactersDir](uint32_t guid) -> std::filesystem::path {
        if (characterByGuid->empty() && !charactersDir.empty() && std::filesystem::exists(charactersDir))
            for (const auto& f : std::filesystem::directory_iterator(charactersDir)) {
                if (f.path().extension() != ".package")
                    continue;
                try {
                    DbpfPackage pkg(f.path());
                    for (const auto& e : pkg.entries())
                        if (e.tgi.type == kTypeObjd) {
                            const auto bytes = pkg.read(e);
                            characterByGuid->emplace(ObjectDefinition::parse(bytes).guid(), f.path());
                        }
                } catch (const std::exception& err) {
                    os2::logOnce("character files", f.path().filename().string(), err);
                }
            }
        auto it = characterByGuid->find(guid);
        return it == characterByGuid->end() ? std::filesystem::path{} : it->second;
    };
    if (!charactersDir.empty())
        code.setGuidFallback([characterFileOf](CodeLibrary& lib, uint32_t guid) -> std::optional<std::pair<uint32_t, uint16_t>> {
            const auto file = characterFileOf(guid);
            if (file.empty())
                return std::nullopt;
            try {
                DbpfPackage pkg(file);
                const uint32_t group = characterGroup(file.stem().string());
                for (uint16_t instance : lib.addPackage(pkg, group))
                    if (const ObjectDefinition* d = lib.definition(group, instance); d && d->guid() == guid)
                        return std::pair{group, instance};
            } catch (const std::exception& err) {
                os2::logOnce("character file", file.filename().string(), err);
            }
            return std::nullopt;
        });
    return characterFileOf;
}

LotLoad loadLot(VM& vm, const Lot& lot, const LotObjects& saved, const std::filesystem::path& charactersDir) {
    CodeLibrary& code = vm.code();
    std::map<uint32_t, const Lot::Object*> cObjects;
    for (const auto& o : lot.objects)
        cObjects[o.instance] = &o;

    setLotRooms(vm, lot);

    LotLoad out;
    // Sims whose cObject does not name their character file are found by
    // their GUID among the neighbourhood's character files.
    auto characterFileOf = useCharacterFiles(code, charactersDir);
    // Every object of the object module (saved.objects), of the OBJD its
    // save type's GUID names; Sims from their character files.
    for (const auto& s : saved.objects) {
        LotLoad::Entry e;
        e.id = s.id;
        e.person = s.person;
        std::optional<std::pair<uint32_t, uint16_t>> def;
        std::string cName;
        const auto c = cObjects.find(uint32_t(uint16_t(s.id)));
        if (c != cObjects.end()) {
            cName = c->second->definition;
            if (cName.size() > kObjtSuffix.size() &&
                cName.compare(cName.size() - kObjtSuffix.size(), kObjtSuffix.size(), kObjtSuffix) == 0)
                cName.resize(cName.size() - kObjtSuffix.size());
        }
        e.name = cName;
        const uint32_t guid = saved.guidOf(s.id);
        try {
            std::filesystem::path file;
            if (s.person) {
                // "N001_User00011 - Don": the character file is the part
                // before " - "; without a cPerson naming it, the file whose
                // OBJD has the save type's GUID.
                if (c != cObjects.end() && c->second->person && !cName.empty()) {
                    if (charactersDir.empty())
                        throw VMError("no character directory for " + cName);
                    file = charactersDir / (cName.substr(0, cName.find(" - ")) + ".package");
                }
                if (file.empty() || !std::filesystem::exists(file))
                    file = characterFileOf(guid);
                // A Sim with no character file (an NPC created during play,
                // saved by us) is the OBJD of its GUID in the game.
                if (file.empty() && !code.definitionByGuid(guid))
                    throw VMError("no character file with GUID " + hexString(guid));
            }
            if (!file.empty()) {
                DbpfPackage pkg(file);
                const std::string stem = file.stem().string();
                const uint32_t group = characterGroup(stem);
                const auto definitions = code.addPackage(pkg, group);
                if (definitions.size() != 1)
                    throw VMError("character file " + file.filename().string() + " has " +
                                  std::to_string(definitions.size()) + " OBJDs");
                def = std::pair{group, definitions.front()};
                e.name = stem;
            } else if (guid) {
                def = code.definitionByGuid(guid);
                if (!def)
                    throw VMError("no OBJD with GUID " + hexString(guid));
            } else {
                throw VMError("the object module does not say what object it is");
            }
            if (const ObjectDefinition* d = code.definition(def->first, def->second); d && !s.person) {
                e.name = d->fileName;
                if (!cName.empty() && d->fileName != cName)
                    e.cObject = cName; // a leftover cObject with this ID
            }
            Object& o = vm.instantiatePart(def->first, def->second, s.id);
            vm.setPosition(o, {s.x, s.y, s.level});
            // Its elevation (cTSObject +0x20c, ReadHeader; vtable 0xb0): a
            // chimney cap's height over the story, a modular step's. The
            // height stays the floor's (kNoHeight).
            o.position.elevation = s.elevation;
            // Its model's bones as the lot saved them (a dining chair pushed in).
            if (c != cObjects.end() && !s.person)
                o.pose = c->second->nodes;
            for (int16_t field : {objdata::Direction, objdata::ContainerId, objdata::SlotNumber, objdata::Room})
                if (size_t(field) < s.data.size())
                    o.data[size_t(field)] = s.data[size_t(field)];
            vm.noteContainer(o);
            // A part of a multi-tile object leads to its lead object
            // (cEdithMTObject::Read); the others lead themselves.
            if (s.kind == LotObjects::Kind::MultiTile && s.leadObject)
                o.data[objdata::LeadTileObjectId] = s.leadObject;
            e.created = true;
            ++out.created;
        } catch (const std::exception& ex) {
            e.error = ex.what();
        }
        out.objects.push_back(std::move(e));
    }
    // The object module's list as saved (ObjM's pairs): Set to Next walks it.
    for (size_t i = 0; i < saved.pairs.size(); ++i)
        if (Object* o = vm.object(ObjectId(saved.pairs[i].first & 0xFFFF)))
            vm.setListOrder(*o, int64_t(i));
    // The world: walls, floors, rooms and stairs, from the lot's current
    // cObjects only (the packages keep stale copies of the stairs), with the
    // walls opened where the doors (and gates) are: each part of a door
    // object opens the wall behind it when the tile behind is another part
    // of the same door (a door's parts stand on both sides of its wall,
    // facing the same way; the direction is object data 1).
    {
        Lot current = lot;
        const auto keep = currentCObjects(code, lot, saved);
        std::erase_if(current.objects, [&](const Lot::Object& o) { return !keep.count(o.instance); });
        // The low fences Sims walk over (once per run).
        static bool walkOverRead = false;
        if (!walkOverRead && code.index()) {
            walkOverRead = true;
            setWalkOverFences(walkOverFenceGuids(*code.index()));
            setFloorRoutingCosts(floorRoutingCosts(*code.index()));
        }
        vm.world = World::fromLot(current);
        // The straight (multi-tile) staircases, which lotStairs does not
        // know: their bottom landing is the "Lo ... Pad" part on the lower
        // level, the top one the "Hi ... Pad" part on the level above, and
        // the steps fill the tiles between them (the "Lo" parts).
        std::map<ObjectId, std::pair<const Object*, const Object*>> pads; // lead -> bottom, top
        for (const auto& e : out.objects) {
            const Object* o = e.created ? vm.object(e.id) : nullptr;
            if (!o || !o->definition || !o->position.inWorld() || !e.name.starts_with("Stairs - Straight") ||
                !e.name.ends_with(" Pad"))
                continue;
            auto& p = pads[multiTileGroup(*o)];
            (e.name.find(" - Lo ") != std::string::npos ? p.first : p.second) = o;
        }
        for (const auto& [lead, p] : pads) {
            const auto [bottom, top] = p;
            if (!bottom || !top || top->position.level != bottom->position.level + 1)
                continue;
            const float dx = top->position.x - bottom->position.x, dy = top->position.y - bottom->position.y;
            const float length = std::hypot(dx, dy);
            if (length < 1.5f || (std::fabs(dx) > 0.01f && std::fabs(dy) > 0.01f))
                continue; // along an axis, with a step between the landings
            Stair st;
            st.up = {dx / length, dy / length};
            st.bottomEdge = {bottom->position.x + st.up[0] * 0.5f, bottom->position.y + st.up[1] * 0.5f};
            st.steps = int(std::lround((length - 1) * 4));
            st.direction = directionOf(st.up[0], st.up[1]);
            st.bottom = {int(std::floor(bottom->position.x)), int(std::floor(bottom->position.y)), bottom->position.level};
            st.top = {int(std::floor(top->position.x)), int(std::floor(top->position.y)), top->position.level};
            st.bottomZ = lot.elevationAt(st.bottom.level - 1, bottom->position.x, bottom->position.y);
            st.topZ = lot.elevationAt(st.top.level - 1, top->position.x, top->position.y);
            vm.world->addStair(st);
        }
        out.stairs = vm.world->stairs().size();
    }
    for (const auto& e : out.objects) {
        const Object* o = e.created ? vm.object(e.id) : nullptr;
        if (!o || !o->definition || o->definition->objectType() != kObjectTypeDoor || !o->position.inWorld())
            continue;
        const int dir = o->data[objdata::Direction];
        const auto [dx, dy] = directionStep(dir + 4);
        const TilePos t{int(std::floor(o->position.x)), int(std::floor(o->position.y)), o->position.level};
        if (vm.world->diagonalWall(t)) {
            // A diagonal door ("Door - 1 Tile - Diagonal - ...") is one part
            // standing in the diagonal wall that crosses its tile: nothing is
            // behind it to pair with (the bedrooms upstairs in N001_Lot68
            // were shut).
            vm.world->openDoor(t, dir);
            ++out.doors;
            continue;
        }
        for (const auto& f : out.objects) {
            const Object* p = f.created && f.id != e.id ? vm.object(f.id) : nullptr;
            if (p && multiTileGroup(*p) == multiTileGroup(*o) && p->position.level == t.level &&
                int(std::floor(p->position.x)) == t.x + dx && int(std::floor(p->position.y)) == t.y + dy) {
                vm.world->openDoor(t, dir);
                ++out.doors;
                break;
            }
        }
    }
    // The objects' own portals (the pool ladder).
    vm.refreshObjectPortals();

    // The lot's relationships: each object's table (its XOBJ) lists its
    // keys and the entries are the SRELs `id << 16 | key`, as
    // cTSRelationshipTable reads them (docs/relationships-mac.md §1): an
    // SREL no table lists is not read; a key without a readable SREL
    // (missing, or not version 2) is an entry with no variables.
    std::map<uint32_t, const LotObjects::Relationship*> srel;
    for (const auto& r : saved.relationships)
        srel[uint32_t(uint16_t(r.from)) << 16 | uint16_t(r.to)] = &r;
    for (const auto& s : saved.objects) {
        if (!vm.object(s.id))
            continue;
        for (uint32_t key : s.relationshipKeys) {
            auto& values = vm.relationships[VM::objectRelationship(s.id, int16_t(key))];
            values.clear();
            if (s.relationshipVersion < 0) {
                if (auto it = s.relationshipInline.find(key); it != s.relationshipInline.end())
                    for (int32_t v : it->second)
                        values.push_back(int16_t(v));
            } else if (auto it = srel.find(uint32_t(uint16_t(s.id)) << 16 | uint16_t(key));
                       it != srel.end() && it->second->version == 2) {
                for (int32_t v : it->second->values)
                    values.push_back(int16_t(v));
            }
            if (values.size() > kMaxRelationshipVars)
                values.resize(kMaxRelationshipVars);
            ++out.relationships;
        }
    }
    return out;
}

ResetLotResult runResetLot(VM& vm, const std::vector<std::filesystem::path>& characters) {
    ResetLotResult out;
    CodeLibrary& code = vm.code();
    for (const auto& file : characters) {
        try {
            DbpfPackage pkg(file);
            code.addPackage(pkg, characterGroup(file.stem().string()));
        } catch (const std::exception& e) {
            out.errors.push_back(file.filename().string() + ": " + e.what());
        }
    }
    std::set<ObjectId> before;
    std::vector<ObjectId> ids;
    for (const auto& [id, o] : vm.objects()) {
        before.insert(id);
        ids.push_back(id);
    }
    static const std::string kName = "reset lot";
    for (ObjectId id : ids) {
        const Object* o = vm.object(id);
        if (!o)
            continue;
        std::optional<uint16_t> tree;
        for (uint32_t group : {o->group, o->semiGroup}) {
            if (!group)
                continue;
            tree = code.behaviorByName(group, kName);
            if (tree && vm.resolveTree(*o, *tree).first == group)
                break;
            tree.reset();
        }
        if (!tree)
            continue;
        ++out.trees;
        std::array<int16_t, kTempCount> temps{};
        const std::array<int16_t, 4> params{};
        try {
            runOneTickTree(vm, temps, id, id, id, *tree, params);
        } catch (const std::exception& e) {
            out.errors.push_back("object " + std::to_string(id) + ": " + e.what());
        }
    }
    for (auto& [id, o] : vm.objects())
        if (o.person && !before.count(id)) {
            // The queue's person data (109 "Current Interaction GUID" and
            // the rest) as the queue has it: the neighbor's copy holds the
            // last ID it ran with (as a load resumes them, lot_resume.cpp).
            syncQueueData(*vm.object(id));
            setInteractionData(*vm.object(id));
            out.sims.push_back(id);
            std::filesystem::path file;
            for (const auto& c : characters)
                if (o.group == characterGroup(c.stem().string()))
                    file = c;
            out.simFiles.push_back(file);
        }
    return out;
}

bool runLotLoaded(VM& vm, std::string* error) {
    constexpr uint32_t kGlobalGroup = 0x7FD46CD0;
    const auto tree = vm.code().behaviorByName(kGlobalGroup, "CT - Lot Loaded");
    if (!tree || vm.objects().empty())
        return false;
    const ObjectId first = vm.objects().begin()->first;
    std::array<int16_t, kTempCount> temps{};
    const std::array<int16_t, 4> params{};
    try {
        return runOneTickTree(vm, temps, first, first, 0, *tree, params);
    } catch (const std::exception& e) {
        if (error)
            *error = e.what();
        return false;
    }
}

} // namespace os2::vm
